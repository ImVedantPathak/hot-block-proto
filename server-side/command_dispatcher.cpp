/**
 * command_dispatcher.cpp — Server Command Dispatcher
 *
 * An interactive CLI tool that lets the server operator send commands
 * to any registered client by its client ID.
 *
 * Usage (interactive mode):
 *   ./command_dispatcher
 *
 * Usage (single command, non-interactive):
 *   ./command_dispatcher <client_id> <COMMAND [args...]>
 *
 * Reads from:
 *   registry/clients.txt  — to resolve client_id → IP + port
 *   registry/status.txt   — to show online/offline status
 *
 * Supported commands (forwarded to client receiver):
 *   MOVE <src> <dst>
 *   DELETE <path>
 *   EXEC <command...>
 *   SEND_FILE <local_path> <dest_ip> <dest_port>
 *   RECV_FILE <save_dir>
 *   STOP
 *
 * Also supports dispatcher-only meta-commands:
 *   list                  — show all registered clients and their status
 *   reload                — reload registry from disk
 *   help                  — show command reference
 *   exit / quit           — exit the dispatcher
 *
 * Compile (Linux/macOS):
 *   g++ -std=c++17 -o command_dispatcher command_dispatcher.cpp -pthread
 *
 * Compile (Windows MinGW):
 *   g++ -std=c++17 -o command_dispatcher command_dispatcher.cpp -lws2_32 -pthread
 *
 * Compile (Windows MSVC):
 *   cl /EHsc /std:c++17 command_dispatcher.cpp ws2_32.lib
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <map>
#include <vector>
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
static const char* REGISTRY_FILE  = "registry/clients.txt";
static const char* STATUS_FILE    = "registry/status.txt";
static const char* LOG_FILE       = "dispatcher.log";
static const int   CONNECT_TIMEOUT_SEC = 5;
// ─────────────────────────────────────────────────────────────────────────────

struct ClientEntry {
    std::string clientId;
    uint16_t    port       = 0;
    std::string ipAddress;
    std::string lastSeen;
};

struct ClientStatus {
    std::string status;     // "ONLINE" or "OFFLINE"
    std::string lastBeat;
    uint64_t    totalBeats = 0;
};

std::map<std::string, ClientEntry>  gRegistry;
std::map<std::string, ClientStatus> gStatus;
std::ofstream                       gLogFile;

// ── Helpers ──────────────────────────────────────────────────────────────────

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

std::vector<std::string> splitArgs(const std::string& s, int maxParts = -1) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string token;
    int count = 0;
    while (ss >> token) {
        if (maxParts > 0 && count == maxParts - 1) {
            std::string rest;
            std::getline(ss, rest);
            parts.push_back(token + rest);
            break;
        }
        parts.push_back(token);
        count++;
    }
    return parts;
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

// ── Registry / status loaders ─────────────────────────────────────────────────

void loadRegistry() {
    gRegistry.clear();
    std::ifstream f(REGISTRY_FILE);
    if (!f.is_open()) {
        std::cout << "[WARN] Registry not found: " << REGISTRY_FILE << "\n";
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        ClientEntry e;
        ss >> e.clientId >> e.port >> e.ipAddress >> e.lastSeen;
        if (!e.clientId.empty() && e.port != 0)
            gRegistry[e.clientId] = e;
    }
}

void loadStatus() {
    gStatus.clear();
    std::ifstream f(STATUS_FILE);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        // Format: client_id port ip_address status last_beat total_beats
        std::istringstream ss(line);
        std::string id, portStr, ip, status, lastBeat, beats;
        ss >> id >> portStr >> ip >> status >> lastBeat >> beats;
        if (id.empty()) continue;
        ClientStatus cs;
        cs.status     = status;
        cs.lastBeat   = lastBeat;
        try { cs.totalBeats = std::stoull(beats); } catch (...) {}
        gStatus[id] = cs;
    }
}

// ── Network: send a command to a client ──────────────────────────────────────

// Opens a TCP connection to the client's assigned IP:port,
// sends the command string, reads the response, and returns it.
std::string sendCommand(const ClientEntry& client, const std::string& command) {
    SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCK) return "ERR failed to create socket";

    // Set send/recv timeout
#ifdef _WIN32
    DWORD timeout = CONNECT_TIMEOUT_SEC * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{CONNECT_TIMEOUT_SEC, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(client.port);
    if (inet_pton(AF_INET, client.ipAddress.c_str(), &addr.sin_addr) <= 0) {
        CLOSE_SOCKET(sock);
        return "ERR invalid client IP: " + client.ipAddress;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
        CLOSE_SOCKET(sock);
        return "ERR could not connect to " + client.ipAddress +
               ":" + std::to_string(client.port) + " (is the client online?)";
    }

    // Send command
    std::string msg = command + "\n";
    send(sock, msg.c_str(), static_cast<int>(msg.size()), 0);

    // Read response
    std::string response = recvLine(sock);
    CLOSE_SOCKET(sock);
    return response.empty() ? "ERR no response from client" : response;
}

// ── Meta-commands (dispatcher only) ──────────────────────────────────────────

void cmdList() {
    loadRegistry();
    loadStatus();

    if (gRegistry.empty()) {
        std::cout << "  No clients registered.\n";
        return;
    }

    std::cout << "\n";
    std::cout << "  " << std::string(72, '-') << "\n";
    std::cout << "  " << std::left
              << std::setw(20) << "CLIENT ID"
              << std::setw(8)  << "PORT"
              << std::setw(18) << "IP ADDRESS"
              << std::setw(10) << "STATUS"
              << std::setw(10) << "BEATS"
              << "\n";
    std::cout << "  " << std::string(72, '-') << "\n";

    for (auto& [id, entry] : gRegistry) {
        std::string status = "UNKNOWN";
        std::string beats  = "-";
        auto sit = gStatus.find(id);
        if (sit != gStatus.end()) {
            status = sit->second.status;
            beats  = std::to_string(sit->second.totalBeats);
        }
        std::string tag = (status == "ONLINE") ? "[+]" : "[-]";
        std::cout << "  " << tag << " "
                  << std::left
                  << std::setw(17) << id
                  << std::setw(8)  << entry.port
                  << std::setw(18) << entry.ipAddress
                  << std::setw(10) << status
                  << std::setw(10) << beats
                  << "\n";
    }
    std::cout << "  " << std::string(72, '-') << "\n\n";
}

void cmdHelp() {
    std::cout << R"(
  CLIENT COMMANDS  (usage: <client_id> <COMMAND [args]>)
  ─────────────────────────────────────────────────────
  <id> MOVE <src> <dst>                  Move/rename a file on the client
  <id> DELETE <path>                     Delete a file on the client
  <id> EXEC <command...>                 Run a program or shell command
  <id> SEND_FILE <path> <ip> <port>      Client pushes a file to another host
  <id> RECV_FILE <save_dir>              Server pushes a file to the client
  <id> STOP                              Shut down the client receiver

  BROADCAST  (sends to all ONLINE clients)
  ─────────────────────────────────────────────────────
  all <COMMAND [args]>

  DISPATCHER META-COMMANDS
  ─────────────────────────────────────────────────────
  list                                   Show all clients and their status
  reload                                 Reload registry and status from disk
  help                                   Show this help
  exit / quit                            Exit the dispatcher

)";
}

// ── Broadcast: send to all online clients ────────────────────────────────────

void broadcastCommand(const std::string& command) {
    loadRegistry();
    loadStatus();
    int sent = 0, failed = 0;

    for (auto& [id, entry] : gRegistry) {
        auto sit = gStatus.find(id);
        if (sit == gStatus.end() || sit->second.status != "ONLINE") {
            std::cout << "  [SKIP] " << id << " (offline)\n";
            continue;
        }
        std::string response = sendCommand(entry, command);
        bool ok = (response.substr(0, 2) == "OK");
        std::cout << "  [" << (ok ? "OK  " : "ERR ") << "] " << id << " → " << response << "\n";
        log(ok ? "BROADCAST_OK" : "BROADCAST_ERR",
            "[" + id + "] " + command + " → " + response);
        ok ? sent++ : failed++;
    }
    std::cout << "  Broadcast complete: " << sent << " OK, " << failed << " failed.\n\n";
}

// ── Interactive REPL ──────────────────────────────────────────────────────────

void runInteractive() {
    std::cout << "\n=== Fleet Manager — Command Dispatcher ===\n";
    std::cout << "Type 'help' for available commands, 'list' to see clients.\n\n";

    loadRegistry();
    loadStatus();

    while (true) {
        std::cout << "dispatcher> ";
        std::string input;
        if (!std::getline(std::cin, input)) break;
        input = trim(input);
        if (input.empty()) continue;

        // Meta-commands
        if (input == "exit" || input == "quit") {
            std::cout << "Bye.\n";
            break;
        }
        if (input == "help") { cmdHelp(); continue; }
        if (input == "list") { cmdList(); continue; }
        if (input == "reload") {
            loadRegistry();
            loadStatus();
            std::cout << "  Reloaded. " << gRegistry.size() << " client(s) in registry.\n\n";
            continue;
        }

        // Format: <client_id> <COMMAND [args...]>
        //    or:  all <COMMAND [args...]>
        auto parts = splitArgs(input, 2);
        if (parts.size() < 2) {
            std::cout << "  Usage: <client_id> <COMMAND [args]>  or  all <COMMAND>\n\n";
            continue;
        }

        std::string target  = parts[0];
        std::string command = parts[1];

        if (target == "all") {
            broadcastCommand(command);
            continue;
        }

        // Look up client
        auto it = gRegistry.find(target);
        if (it == gRegistry.end()) {
            std::cout << "  Unknown client ID: " << target << "\n";
            std::cout << "  Type 'list' to see registered clients.\n\n";
            continue;
        }

        const ClientEntry& client = it->second;
        log("SEND", "[" + target + "] " + command);
        std::string response = sendCommand(client, command);
        bool ok = (response.substr(0, 2) == "OK");

        std::cout << "  " << (ok ? "[OK]  " : "[ERR] ") << response << "\n\n";
        log(ok ? "OK" : "ERR", "[" + target + "] → " + response);
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    gLogFile.open(LOG_FILE, std::ios::app);
    if (!gLogFile.is_open())
        std::cerr << "[WARN] Could not open log file: " << LOG_FILE << "\n";

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[ERROR] WSAStartup failed.\n"; return 1;
    }
#endif

    if (argc >= 3) {
        // Non-interactive: ./command_dispatcher <client_id> <COMMAND [args...]>
        loadRegistry();
        std::string target  = argv[1];
        std::string command;
        for (int i = 2; i < argc; i++) {
            if (i > 2) command += " ";
            command += argv[i];
        }

        if (target == "all") {
            broadcastCommand(command);
        } else {
            auto it = gRegistry.find(target);
            if (it == gRegistry.end()) {
                std::cerr << "[ERROR] Unknown client ID: " << target << "\n";
#ifdef _WIN32
                WSACleanup();
#endif
                return 1;
            }
            std::string response = sendCommand(it->second, command);
            std::cout << response << "\n";
            log((response.substr(0, 2) == "OK" ? "OK" : "ERR"),
                "[" + target + "] " + command + " → " + response);
        }
    } else {
        // Interactive mode
        runInteractive();
    }

#ifdef _WIN32
    WSACleanup();
#endif
    gLogFile.close();
    return 0;
}