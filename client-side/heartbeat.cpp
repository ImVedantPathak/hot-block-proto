#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <sstream>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using SocketType = SOCKET;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCKET closesocket
    #define SOCK_ERR SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using SocketType = int;
    #define INVALID_SOCK -1
    #define CLOSE_SOCKET close
    #define SOCK_ERR -1
#endif

std::string loadSeverIP(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open secrets file: " + path);
    }
    std::string ip;
    std::getline(file, ip);
    // Trim whitespace/newlines
    ip.erase(ip.find_last_not_of(" \t\r\n") + 1);
    return ip;
}

// ─── Configuration ────────────────────────────────────────────────────────────
static const uint16_t SERVER_PORT     = 9999;
static const int      INTERVAL_SEC    = 5;
static const int      MAX_RETRIES     = 3;  
static const char*    LOG_FILE        = "heartheartbeat.log";
// ─────────────────────────────────────────────────────────────────────────────

std::atomic<bool> running(true);

// Signal handler for graceful shutdown (Ctrl+C)
void signalHandler(int signum) {
    std::cout << "\n[INFO] Caught signal " << signum << ". Shutting down gracefully...\n";
    running = false;
}

// Returns current timestamp as a string
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

// Logs a message to both stdout and the log file
void log(std::ofstream& logFile, const std::string& level, const std::string& msg) {
    std::string line = "[" + timestamp() + "] [" + level + "] " + msg;
    std::cout << line << "\n";
    if (logFile.is_open()) {
        logFile << line << "\n";
        logFile.flush();
    }
}

int main() {
    // Register signal handler
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string serverIP;
    try{
        serverIP = loadSeverIP("../secrets/config.txt");
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }

    // Open log file
    std::ofstream logFile(LOG_FILE, std::ios::app);
    if (!logFile.is_open()) {
        std::cerr << "[WARN] Could not open log file: " << LOG_FILE << "\n";
    }

#ifdef _WIN32
    // Initialize Winsock on Windows
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log(logFile, "ERROR", "WSAStartup failed.");
        return 1;
    }
#endif

    log(logFile, "INFO", "Heartbeat client starting.");
    log(logFile, "INFO", std::string("Target: ") + serverIP + ":" + std::to_string(SERVER_PORT));
    log(logFile, "INFO", "Interval: " + std::to_string(INTERVAL_SEC) + "s | Retries: " + std::to_string(MAX_RETRIES));

    // Set up destination address
    sockaddr_in serverAddr{};
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_port        = htons(SERVER_PORT);
    if (inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr) <= 0) {
        log(logFile, "ERROR", "Invalid server IP address: " + std::string(serverIP));
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    uint64_t sequenceNumber = 0;

    while (running) {
        sequenceNumber++;

        // Build heartbeat payload
        std::ostringstream payload;
        payload << "HEARTBEAT seq=" << sequenceNumber
                << " ts=" << timestamp();
        std::string message = payload.str();

        bool sent = false;

        // Retry loop
        for (int attempt = 1; attempt <= MAX_RETRIES && !sent && running; attempt++) {

            // Create UDP socket (fresh each attempt for robustness)
            SocketType sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCK) {
                log(logFile, "ERROR", "Failed to create socket (attempt " + std::to_string(attempt) + ").");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            ssize_t bytesSent = sendto(
                sock,
                message.c_str(),
                static_cast<int>(message.size()),
                0,
                reinterpret_cast<sockaddr*>(&serverAddr),
                sizeof(serverAddr)
            );

            CLOSE_SOCKET(sock);

            if (bytesSent == SOCK_ERR) {
                log(logFile, "WARN", "Send failed (attempt " + std::to_string(attempt) +
                    "/" + std::to_string(MAX_RETRIES) + "). Retrying...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } else {
                log(logFile, "OK", "Sent [seq=" + std::to_string(sequenceNumber) +
                    "] (" + std::to_string(bytesSent) + " bytes) → " +
                    std::string(serverIP) + ":" + std::to_string(SERVER_PORT));
                sent = true;
            }
        }

        if (!sent) {
            log(logFile, "ERROR", "All retries exhausted for seq=" + std::to_string(sequenceNumber) + ".");
        }

        // Wait for next interval, checking running flag every 100ms
        for (int i = 0; i < INTERVAL_SEC * 10 && running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    log(logFile, "INFO", "Heartbeat client stopped. Total packets attempted: " + std::to_string(sequenceNumber));

#ifdef _WIN32
    WSACleanup();
#endif

    logFile.close();
    return 0;
}