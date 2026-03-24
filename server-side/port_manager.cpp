/**
 * port_manager.cpp — Server Port Manager
 *
 * Listens for incoming client registration requests on TCP port 8000.
 * For each new client:
 *   1. Receives "REGISTER <client_id>"
 *   2. Checks the registry file — if the client already has an assigned port, reuses it
 *   3. If new, scans for a free port in the configured range and assigns it
 *   4. Saves the mapping to registry/clients.txt
 *   5. Responds with "PORT <number>"
 *
 * Registry file format (registry/clients.txt):
 *   <client_id> <assigned_port> <ip_address> <last_seen_timestamp>
 *
 * Compile (Linux/macOS):
 *   g++ -std=c++17 -o port_manager port_manager.cpp -pthread
 *
 * Compile (Windows MinGW):
 *   g++ -std=c++17 -o port_manager port_manager.cpp -lws2_32 -pthread
 *
 * Compile (Windows MSVC):
 *   cl /EHsc /std:c++17 port_manager.cpp ws2_32.lib
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using SocketType = SOCKET;
    #define INVALID_SOCK  INVALID_SOCKET
    #define CLOSE_SOCKET  closesocket
    #define SOCK_ERR      SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using SocketType = int;
    #define INVALID_SOCK  -1
    #define CLOSE_SOCKET  close
    #define SOCK_ERR      -1
#endif

// ─── Configuration ────────────────────────────────────────────────────────────
static const uint16_t REGISTRATION_PORT  = 8000;   // Port clients connect to for registration
static const uint16_t PORT_RANGE_START   = 10000;  // Start of assignable port range
static const uint16_t PORT_RANGE_END     = 20000;  // End of assignable port range
static const char*    REGISTRY_DIR       = "registry";
static const char*    REGISTRY_FILE      = "registry/clients.txt";
static const char*    LOG_FILE           = "port_manager.log";
// ─────────────────────────────────────────────────────────────────────────────

struct ClientEntry {
    std::string clientId;
    uint16_t    port       = 0;
    std::string ipAddress;
    std::string lastSeen;
};

// In-memory registry: client_id → entry
std::map<std::string, ClientEntry> gRegistry;
std::mutex                         gRegistryMutex;
std::atomic<bool>                  running(true);
std::ofstream                      gLogFile;

// ── Helpers ──────────────────────────────────────────────────────────────────

void signalHandler(int) { running = false; }

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

void log(const std::string& level, const std::string& msg) {
    std::string line = "[" + timestamp() + "] [" + level + "] " + msg;
    std::cout << line << "\n";
    if (gLogFile.is_open()) { gLogFile << line << "\n"; gLogFile.flush(); }
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// Receive a full \n-terminated line from a socket
std::string recvLine(SocketType sock) {
    std::string result;
    char c = 0;
    while (true) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0 || c == '\n') break;
        result += c;
    }
    return trim(result);
}

void sendLine(SocketType sock, const std::string& msg) {
    std::string line = msg + "\n";
    send(sock, line.c_str(), static_cast<int>(line.size()), 0);
}

// ── Registry I/O ─────────────────────────────────────────────────────────────

void saveRegistry() {
    std::error_code ec;
    std::filesystem::create_directories(REGISTRY_DIR, ec);

    std::ofstream f(REGISTRY_FILE, std::ios::trunc);
    if (!f.is_open()) { log("ERROR", "Cannot write registry file."); return; }

    f << "# client_id assigned_port ip_address last_seen\n";
    for (auto& [id, entry] : gRegistry) {
        f << entry.clientId << " "
          << entry.port     << " "
          << entry.ipAddress << " "
          << entry.lastSeen  << "\n";
    }
}

void loadRegistry() {
    std::ifstream f(REGISTRY_FILE);
    if (!f.is_open()) { log("INFO", "No existing registry found. Starting fresh."); return; }

    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        ClientEntry entry;
        ss >> entry.clientId >> entry.port >> entry.ipAddress >> entry.lastSeen;
        if (!entry.clientId.empty() && entry.port != 0) {
            gRegistry[entry.clientId] = entry;
            count++;
        }
    }
    log("INFO", "Loaded " + std::to_string(count) + " client(s) from registry.");
}

// ── Port allocation ───────────────────────────────────────────────────────────

// Checks if a port is already in use by attempting to bind to it
bool isPortFree(uint16_t port) {
    SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCK) return false;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bool free = (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCK_ERR);
    CLOSE_SOCKET(sock);
    return free;
}

// Returns a free port in the configured range, or 0 if none found.
// Also checks that the port isn't already assigned in the registry.
uint16_t allocatePort() {
    // Collect all already-assigned ports
    std::set<uint16_t> usedPorts;
    for (auto& [id, entry] : gRegistry)
        usedPorts.insert(entry.port);

    for (uint16_t p = PORT_RANGE_START; p <= PORT_RANGE_END; p++) {
        if (usedPorts.count(p)) continue;
        if (isPortFree(p)) return p;
    }
    return 0;
}

// ── Connection handler ────────────────────────────────────────────────────────

void handleRegistration(SocketType clientSock, const std::string& clientIp) {
    std::string line = recvLine(clientSock);
    log("IN", "[" + clientIp + "] " + line);

    // Expect: "REGISTER <client_id>"
    if (line.substr(0, 9) != "REGISTER ") {
        sendLine(clientSock, "ERROR expected REGISTER <client_id>");
        CLOSE_SOCKET(clientSock);
        return;
    }

    std::string clientId = trim(line.substr(9));
    if (clientId.empty()) {
        sendLine(clientSock, "ERROR empty client_id");
        CLOSE_SOCKET(clientSock);
        return;
    }

    std::lock_guard<std::mutex> lock(gRegistryMutex);

    // Check if this client already has a port assigned
    auto it = gRegistry.find(clientId);
    if (it != gRegistry.end()) {
        // Returning client — update IP and last seen, reuse port
        it->second.ipAddress = clientIp;
        it->second.lastSeen  = timestamp();
        saveRegistry();
        log("INFO", "Returning client [" + clientId + "] → port " +
            std::to_string(it->second.port));
        sendLine(clientSock, "PORT " + std::to_string(it->second.port));
    } else {
        // New client — allocate a fresh port
        uint16_t port = allocatePort();
        if (port == 0) {
            log("ERROR", "No free ports available for new client: " + clientId);
            sendLine(clientSock, "ERROR no free ports available");
            CLOSE_SOCKET(clientSock);
            return;
        }

        ClientEntry entry;
        entry.clientId  = clientId;
        entry.port      = port;
        entry.ipAddress = clientIp;
        entry.lastSeen  = timestamp();
        gRegistry[clientId] = entry;
        saveRegistry();

        log("INFO", "New client [" + clientId + "] from " + clientIp +
            " → assigned port " + std::to_string(port));
        sendLine(clientSock, "PORT " + std::to_string(port));
    }

    CLOSE_SOCKET(clientSock);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    gLogFile.open(LOG_FILE, std::ios::app);
    if (!gLogFile.is_open())
        std::cerr << "[WARN] Could not open log file: " << LOG_FILE << "\n";

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log("ERROR", "WSAStartup failed."); return 1;
    }
#endif

    loadRegistry();

    SocketType listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCK) {
        log("ERROR", "Failed to create socket."); return 1;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(REGISTRATION_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
        log("ERROR", "bind() failed on port " + std::to_string(REGISTRATION_PORT));
        CLOSE_SOCKET(listenSock); return 1;
    }

    listen(listenSock, 32);
    log("INFO", "=== Port Manager started ===");
    log("INFO", "Listening for registrations on port " + std::to_string(REGISTRATION_PORT));
    log("INFO", "Port range: " + std::to_string(PORT_RANGE_START) +
        " – " + std::to_string(PORT_RANGE_END));

    while (running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval tv{1, 0};

        if (select(static_cast<int>(listenSock + 1), &readSet, nullptr, nullptr, &tv) <= 0)
            continue;

        sockaddr_in clientAddr{};
        socklen_t   clientLen = sizeof(clientAddr);
        SocketType  clientSock = accept(listenSock,
            reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == INVALID_SOCK) continue;

        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));

        std::thread(handleRegistration, clientSock, std::string(ipBuf)).detach();
    }

    log("INFO", "Port Manager shutting down.");
    CLOSE_SOCKET(listenSock);

#ifdef _WIN32
    WSACleanup();
#endif
    gLogFile.close();
    return 0;
}