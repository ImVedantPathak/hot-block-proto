/**
 * receiver.cpp — Client Command Receiver
 *
 * Reads secrets/config.txt to get the assigned port, then listens
 * on that port for TCP commands from the server.
 *
 * Supported commands (sent as plain-text lines):
 *
 *   MOVE <src_path> <dst_path>
 *     Move/rename a file on the client machine.
 *
 *   DELETE <path>
 *     Delete a file on the client machine.
 *
 *   SEND_FILE <local_path> <dest_ip> <dest_port>
 *     Transfer a local file to another IP:port over TCP.
 *
 *   RECV_FILE <save_path> <expected_bytes>
 *     Receive a file that the server is about to push over the same connection.
 *
 *   EXEC <command...>
 *     Execute a shell command / .exe on the client.
 *
 *   STOP
 *     Gracefully shut down the receiver (and signals heartbeat to stop).
 *
 * Protocol per connection:
 *   Server → one command line (terminated with \n)
 *   Receiver → "OK\n" or "ERR <reason>\n"
 *
 * Cross-platform: Windows (Winsock2) + Linux/macOS (POSIX sockets)
 *
 * Compile (Linux/macOS):
 *   g++ -std=c++17 -o receiver receiver.cpp -pthread
 *
 * Compile (Windows MSVC):
 *   cl /EHsc /std:c++17 receiver.cpp ws2_32.lib
 *
 * Compile (Windows MinGW):
 *   g++ -std=c++17 -o receiver receiver.cpp -lws2_32 -pthread
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <cstdlib>
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

// ─────────────────────────────────────────────────────────────────────────────
static const char* LOG_FILE = "receiver.log";
// ─────────────────────────────────────────────────────────────────────────────

std::atomic<bool> running(true);

void signalHandler(int) {
    running = false;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

std::ofstream gLogFile;

void log(const std::string& level, const std::string& msg) {
    std::string line = "[" + timestamp() + "] [" + level + "] " + msg;
    std::cout << line << "\n";
    if (gLogFile.is_open()) {
        gLogFile << line << "\n";
        gLogFile.flush();
    }
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

// Splits a string by spaces (respects up to maxParts-1 splits)
std::vector<std::string> splitArgs(const std::string& s, int maxParts = -1) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string token;
    int count = 0;
    while (ss >> token) {
        if (maxParts > 0 && count == maxParts - 1) {
            // Rest of string as last token
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

// Send a response string back over the connection socket
void sendResponse(SocketType sock, const std::string& msg) {
    std::string line = msg + "\n";
    send(sock, line.c_str(), static_cast<int>(line.size()), 0);
}

// Receive a full line (up to \n) from a socket
std::string recvLine(SocketType sock) {
    std::string result;
    char c = 0;
    while (true) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        result += c;
    }
    return trim(result);
}

// ── Config loader ─────────────────────────────────────────────────────────────

struct ClientConfig {
    std::string clientId;
    std::string serverIp;
    uint16_t    assignedPort = 0;
};

bool loadConfig(const std::string& path, ClientConfig& cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq  = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key == "client_id")         cfg.clientId     = val;
        else if (key == "server_ip")    cfg.serverIp     = val;
        else if (key == "assigned_port") cfg.assignedPort = static_cast<uint16_t>(std::stoi(val));
    }
    return cfg.assignedPort != 0;
}

// ── Command Handlers ──────────────────────────────────────────────────────────

// MOVE <src> <dst>
std::string cmdMove(const std::vector<std::string>& args) {
    if (args.size() < 3) return "ERR usage: MOVE <src> <dst>";
    std::error_code ec;
    std::filesystem::rename(args[1], args[2], ec);
    if (ec) return "ERR " + ec.message();
    log("CMD", "Moved: " + args[1] + " → " + args[2]);
    return "OK";
}

// DELETE <path>
std::string cmdDelete(const std::vector<std::string>& args) {
    if (args.size() < 2) return "ERR usage: DELETE <path>";
    std::error_code ec;
    std::filesystem::remove(args[1], ec);
    if (ec) return "ERR " + ec.message();
    log("CMD", "Deleted: " + args[1]);
    return "OK";
}

// EXEC <command...>
std::string cmdExec(const std::vector<std::string>& args) {
    if (args.size() < 2) return "ERR usage: EXEC <command>";
    // Reconstruct full command from args[1] onward
    std::string cmd;
    for (size_t i = 1; i < args.size(); i++) {
        if (i > 1) cmd += " ";
        cmd += args[i];
    }
#ifdef _WIN32
    // On Windows, run via cmd.exe so .exe resolution and PATH work
    cmd = "cmd /C " + cmd;
#endif
    log("CMD", "Executing: " + cmd);
    int ret = std::system(cmd.c_str());
    if (ret != 0) return "ERR process exited with code " + std::to_string(ret);
    return "OK";
}

// SEND_FILE <local_path> <dest_ip> <dest_port>
// Opens a TCP connection to dest_ip:dest_port and streams the file.
// The destination (another client or server) must be listening with RECV_FILE.
std::string cmdSendFile(const std::vector<std::string>& args) {
    if (args.size() < 4) return "ERR usage: SEND_FILE <local_path> <dest_ip> <dest_port>";

    const std::string& filePath = args[1];
    const std::string& destIp   = args[2];
    uint16_t destPort            = static_cast<uint16_t>(std::stoi(args[3]));

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return "ERR cannot open file: " + filePath;

    std::streamsize fileSize = file.tellg();
    file.seekg(0);

    SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCK) return "ERR failed to create socket";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(destPort);
    if (inet_pton(AF_INET, destIp.c_str(), &addr.sin_addr) <= 0) {
        CLOSE_SOCKET(sock);
        return "ERR invalid dest IP: " + destIp;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCK_ERR) {
        CLOSE_SOCKET(sock);
        return "ERR could not connect to " + destIp + ":" + std::to_string(destPort);
    }

    // Send filename and filesize header first
    std::string fname = std::filesystem::path(filePath).filename().string();
    std::string header = "FILE " + fname + " " + std::to_string(fileSize) + "\n";
    send(sock, header.c_str(), static_cast<int>(header.size()), 0);

    // Stream file contents
    const size_t BUF = 65536;
    std::vector<char> buf(BUF);
    std::streamsize total = 0;
    while (file.read(buf.data(), BUF) || file.gcount() > 0) {
        std::streamsize chunk = file.gcount();
        send(sock, buf.data(), static_cast<int>(chunk), 0);
        total += chunk;
    }
    CLOSE_SOCKET(sock);

    log("CMD", "Sent file: " + filePath + " (" + std::to_string(total) + " bytes) → " +
        destIp + ":" + std::to_string(destPort));
    return "OK sent " + std::to_string(total) + " bytes";
}

// RECV_FILE <save_dir> <expected_bytes>
// Receives a file pushed over the same command connection.
// The server will push: "FILE <name> <size>\n" followed by raw bytes.
std::string cmdRecvFile(SocketType sock, const std::vector<std::string>& args) {
    if (args.size() < 2) return "ERR usage: RECV_FILE <save_dir>";

    const std::string& saveDir = args[1];
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);

    // Read the FILE header from the server
    std::string header = recvLine(sock);
    // Expected: "FILE <name> <size>"
    auto parts = splitArgs(header);
    if (parts.size() < 3 || parts[0] != "FILE") return "ERR bad file header: " + header;

    std::string filename    = parts[1];
    std::streamsize fileSize = static_cast<std::streamsize>(std::stoll(parts[2]));
    std::string savePath    = saveDir + "/" + filename;

    std::ofstream out(savePath, std::ios::binary);
    if (!out.is_open()) return "ERR cannot create file: " + savePath;

    const size_t BUF = 65536;
    std::vector<char> buf(BUF);
    std::streamsize received = 0;
    while (received < fileSize) {
        std::streamsize toRead = std::min((std::streamsize)BUF, fileSize - received);
        int n = recv(sock, buf.data(), static_cast<int>(toRead), 0);
        if (n <= 0) break;
        out.write(buf.data(), n);
        received += n;
    }
    out.close();

    log("CMD", "Received file: " + savePath + " (" + std::to_string(received) + " bytes)");
    if (received < fileSize)
        return "ERR incomplete transfer: got " + std::to_string(received) +
               "/" + std::to_string(fileSize);
    return "OK saved to " + savePath;
}

// ── Connection handler ─────────────────────────────────────────────────────────

void handleConnection(SocketType clientSock) {
    std::string line = recvLine(clientSock);
    if (line.empty()) {
        CLOSE_SOCKET(clientSock);
        return;
    }

    log("IN", line);

    // Parse command verb and args
    // Split into at most 4 parts; EXEC and SEND_FILE need the tail preserved
    auto args = splitArgs(line, 4);
    if (args.empty()) {
        sendResponse(clientSock, "ERR empty command");
        CLOSE_SOCKET(clientSock);
        return;
    }

    std::string verb = args[0];
    std::string response;

    if (verb == "MOVE") {
        response = cmdMove(args);
    } else if (verb == "DELETE") {
        response = cmdDelete(args);
    } else if (verb == "EXEC") {
        response = cmdExec(args);
    } else if (verb == "SEND_FILE") {
        response = cmdSendFile(args);
    } else if (verb == "RECV_FILE") {
        response = cmdRecvFile(clientSock, args);
    } else if (verb == "STOP") {
        log("CMD", "Received STOP command from server. Shutting down.");
        sendResponse(clientSock, "OK stopping");
        CLOSE_SOCKET(clientSock);
        running = false;
        return;
    } else {
        response = "ERR unknown command: " + verb;
        log("WARN", response);
    }

    log("OUT", response);
    sendResponse(clientSock, response);
    CLOSE_SOCKET(clientSock);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    gLogFile.open(LOG_FILE, std::ios::app);
    if (!gLogFile.is_open())
        std::cerr << "[WARN] Could not open log file: " << LOG_FILE << "\n";

    // Accept config path as optional argument (default: ../secrets/config.txt)
    std::string configPath = (argc > 1) ? argv[1] : "../secrets/config.txt";

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log("ERROR", "WSAStartup failed.");
        return 1;
    }
#endif

    ClientConfig cfg;
    if (!loadConfig(configPath, cfg)) {
        log("ERROR", "Could not load config from: " + configPath);
        log("ERROR", "Run main first to register with the server.");
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    log("INFO", "=== Receiver starting ===");
    log("INFO", "Client ID    : " + cfg.clientId);
    log("INFO", "Listening on : 0.0.0.0:" + std::to_string(cfg.assignedPort));

    // Create TCP listening socket
    SocketType listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCK) {
        log("ERROR", "Failed to create socket.");
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // Allow reuse of the port
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in bindAddr{};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_port        = htons(cfg.assignedPort);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCK_ERR) {
        log("ERROR", "bind() failed on port " + std::to_string(cfg.assignedPort));
        CLOSE_SOCKET(listenSock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (listen(listenSock, 8) == SOCK_ERR) {
        log("ERROR", "listen() failed.");
        CLOSE_SOCKET(listenSock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    log("INFO", "Waiting for commands from server...");

    while (running) {
        // Use select() with a timeout so we can check the running flag
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock, &readSet);
        timeval tv{1, 0}; // 1 second timeout

        int ready = select(static_cast<int>(listenSock + 1), &readSet, nullptr, nullptr, &tv);
        if (ready <= 0) continue; // timeout or error — loop and recheck running

        sockaddr_in clientAddr{};
        socklen_t   clientLen = sizeof(clientAddr);
        SocketType  clientSock = accept(listenSock,
                                        reinterpret_cast<sockaddr*>(&clientAddr),
                                        &clientLen);
        if (clientSock == INVALID_SOCK) continue;

        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        log("INFO", "Connection from: " + std::string(ipBuf));

        // Handle each command in its own thread so we don't block
        std::thread(handleConnection, clientSock).detach();
    }

    log("INFO", "Receiver shut down.");
    CLOSE_SOCKET(listenSock);

#ifdef _WIN32
    WSACleanup();
#endif
    gLogFile.close();
    return 0;
}