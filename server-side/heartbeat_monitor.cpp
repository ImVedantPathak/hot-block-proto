/**
 * heartbeat_monitor.cpp — Server Heartbeat Monitor
 *
 * Listens for UDP heartbeat packets from all registered clients.
 * Parses each packet, updates the client's last-seen timestamp,
 * and periodically checks for clients that have gone silent.
 *
 * A client is marked OFFLINE if no heartbeat is received within
 * TIMEOUT_SEC seconds. It is marked ONLINE again on the next beat.
 *
 * Status is written to registry/status.txt in real time so
 * command_dispatcher can read it before issuing commands.
 *
 * Heartbeat packet format (from client):
 *   "HEARTBEAT seq=<n> ts=<timestamp>"
 *
 * Compile (Linux/macOS):
 *   g++ -std=c++17 -o heartbeat_monitor heartbeat_monitor.cpp -pthread
 *
 * Compile (Windows MinGW):
 *   g++ -std=c++17 -o heartbeat_monitor heartbeat_monitor.cpp -lws2_32 -pthread
 *
 * Compile (Windows MSVC):
 *   cl /EHsc /std:c++17 heartbeat_monitor.cpp ws2_32.lib
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
#include <chrono>
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
static const uint16_t MONITOR_PORT    = 9999;   // UDP port to receive heartbeats on
static const int      TIMEOUT_SEC     = 30;     // Seconds of silence before marking offline
static const int      CHECK_INTERVAL  = 5;      // How often (sec) to run the timeout check
static const char*    REGISTRY_FILE   = "registry/clients.txt";
static const char*    STATUS_FILE     = "registry/status.txt";
static const char*    LOG_FILE        = "heartbeat_monitor.log";
// ─────────────────────────────────────────────────────────────────────────────

struct ClientStatus {
    std::string clientId;
    std::string ipAddress;
    uint16_t    port         = 0;
    bool        online       = false;
    uint64_t    lastSeq      = 0;
    uint64_t    totalBeats   = 0;
    std::chrono::steady_clock::time_point lastBeatTime;
    std::string lastBeatTs;   // human-readable
};

std::map<std::string, ClientStatus> gClients;   // client_id → status
std::map<std::string, std::string>  gIpToId;    // ip_address → client_id
std::mutex                          gMutex;
std::atomic<bool>                   running(true);
std::ofstream                       gLogFile;

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

// ── Registry loader ───────────────────────────────────────────────────────────

// Reads registry/clients.txt (written by port_manager) to know which
// client IDs exist, what ports they are on, and their last known IPs.
void loadRegistry() {
    std::ifstream f(REGISTRY_FILE);
    if (!f.is_open()) {
        log("WARN", "Registry file not found: " + std::string(REGISTRY_FILE));
        log("WARN", "Start port_manager first and register at least one client.");
        return;
    }

    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string id, ip, ts;
        uint16_t port = 0;
        ss >> id >> port >> ip >> ts;
        if (id.empty() || port == 0) continue;

        ClientStatus cs;
        cs.clientId    = id;
        cs.port        = port;
        cs.ipAddress   = ip;
        cs.online      = false;
        cs.lastBeatTime = std::chrono::steady_clock::now() -
                          std::chrono::seconds(TIMEOUT_SEC + 1); // start as timed-out
        gClients[id]   = cs;
        if (!ip.empty() && ip != "-")
            gIpToId[ip] = id;
        count++;
    }
    log("INFO", "Loaded " + std::to_string(count) + " client(s) from registry.");
}

// ── Status file writer ────────────────────────────────────────────────────────

void writeStatusFile() {
    std::error_code ec;
    std::filesystem::create_directories("registry", ec);

    std::ofstream f(STATUS_FILE, std::ios::trunc);
    if (!f.is_open()) { log("ERROR", "Cannot write status file."); return; }

    f << "# Generated: " << timestamp() << "\n";
    f << "# client_id port ip_address status last_beat total_beats\n";
    for (auto& [id, cs] : gClients) {
        f << cs.clientId   << " "
          << cs.port       << " "
          << (cs.ipAddress.empty() ? "-" : cs.ipAddress) << " "
          << (cs.online ? "ONLINE" : "OFFLINE") << " "
          << (cs.lastBeatTs.empty() ? "-" : cs.lastBeatTs) << " "
          << cs.totalBeats << "\n";
    }
}

// ── Heartbeat packet parser ───────────────────────────────────────────────────

// Parses: "HEARTBEAT seq=<n> ts=<timestamp>"
// Returns true if valid, fills seq and ts.
bool parseHeartbeat(const std::string& packet, uint64_t& seq, std::string& ts) {
    if (packet.substr(0, 10) != "HEARTBEAT ") return false;
    std::string rest = packet.substr(10);

    // Extract seq=
    auto seqPos = rest.find("seq=");
    if (seqPos == std::string::npos) return false;
    try { seq = std::stoull(rest.substr(seqPos + 4)); } catch (...) { return false; }

    // Extract ts=
    auto tsPos = rest.find("ts=");
    if (tsPos != std::string::npos)
        ts = rest.substr(tsPos + 3);

    return true;
}

// ── Timeout checker thread ────────────────────────────────────────────────────

void timeoutChecker() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(CHECK_INTERVAL));

        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(gMutex);

        for (auto& [id, cs] : gClients) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               now - cs.lastBeatTime).count();

            if (cs.online && elapsed > TIMEOUT_SEC) {
                cs.online = false;
                log("OFFLINE", "Client [" + id + "] (" + cs.ipAddress +
                    ") went offline — no beat for " + std::to_string(elapsed) + "s");
            }
        }
        writeStatusFile();
    }
}

// ── Main receive loop ─────────────────────────────────────────────────────────

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

    SocketType sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCK) {
        log("ERROR", "Failed to create UDP socket."); return 1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(MONITOR_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
        log("ERROR", "bind() failed on port " + std::to_string(MONITOR_PORT));
        CLOSE_SOCKET(sock); return 1;
    }

    log("INFO", "=== Heartbeat Monitor started ===");
    log("INFO", "Listening for UDP heartbeats on port " + std::to_string(MONITOR_PORT));
    log("INFO", "Offline timeout: " + std::to_string(TIMEOUT_SEC) + "s");

    // Start the background timeout checker thread
    std::thread checker(timeoutChecker);

    char buf[512];
    while (running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv{1, 0};

        if (select(static_cast<int>(sock + 1), &readSet, nullptr, nullptr, &tv) <= 0)
            continue;

        sockaddr_in senderAddr{};
        socklen_t   senderLen = sizeof(senderAddr);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
        if (n <= 0) continue;

        buf[n] = '\0';
        std::string packet = trim(std::string(buf, n));

        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &senderAddr.sin_addr, ipBuf, sizeof(ipBuf));
        std::string senderIp(ipBuf);

        uint64_t    seq = 0;
        std::string ts;
        if (!parseHeartbeat(packet, seq, ts)) {
            log("WARN", "Malformed packet from " + senderIp + ": " + packet);
            continue;
        }

        std::lock_guard<std::mutex> lock(gMutex);

        // Look up client by IP address
        auto idIt = gIpToId.find(senderIp);
        if (idIt == gIpToId.end()) {
            // Unknown IP — could be a new registration that hasn't been reloaded yet
            log("WARN", "Heartbeat from unknown IP: " + senderIp + " (seq=" +
                std::to_string(seq) + "). Is this client registered?");
            continue;
        }

        std::string clientId = idIt->second;
        auto& cs             = gClients[clientId];

        bool wasOffline = !cs.online;
        cs.online       = true;
        cs.lastSeq      = seq;
        cs.totalBeats++;
        cs.lastBeatTime = std::chrono::steady_clock::now();
        cs.lastBeatTs   = ts.empty() ? timestamp() : ts;
        cs.ipAddress    = senderIp;

        if (wasOffline) {
            log("ONLINE", "Client [" + clientId + "] (" + senderIp +
                ") came back online (seq=" + std::to_string(seq) + ")");
        } else {
            log("BEAT",   "Client [" + clientId + "] seq=" + std::to_string(seq) +
                " beats=" + std::to_string(cs.totalBeats));
        }

        writeStatusFile();
    }

    running = false;
    if (checker.joinable()) checker.join();

    log("INFO", "Heartbeat Monitor shutting down.");
    CLOSE_SOCKET(sock);

#ifdef _WIN32
    WSACleanup();
#endif
    gLogFile.close();
    return 0;
}