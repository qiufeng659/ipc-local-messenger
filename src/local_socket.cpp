#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if __has_include(<afunix.h>)
#include <afunix.h>
#else
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif
typedef struct SOCKADDR_UN {
    ADDRESS_FAMILY sun_family;
    char sun_path[UNIX_PATH_MAX];
} SOCKADDR_UN;
#endif
#include "transports.hpp"
#include <memory>
#include <cstring>

namespace lm {
namespace {
struct SocketPeer {
    std::atomic<SOCKET> socket{INVALID_SOCKET};
    std::string name;
    std::mutex writeMutex;
    std::atomic<bool> connected{true};
};
struct SocketServerState {
    std::atomic<bool> running{true};
    std::mutex peersMutex;
    std::vector<std::shared_ptr<SocketPeer>> peers;
    std::vector<std::thread> workers; // joined after the accept thread stops
};

bool sendAll(SOCKET socket, const char* data, std::size_t size) {
    std::size_t sentTotal = 0;
    while (sentTotal < size) {
        const int sent = send(socket, data + sentTotal,
                              static_cast<int>(size - sentTotal), 0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        sentTotal += static_cast<std::size_t>(sent);
    }
    return true;
}

bool sendSocketLine(SOCKET socket, const std::string& message) {
    std::string framed = message;
    if (framed.size() > kMaxMessage) framed.resize(kMaxMessage);
    framed.push_back('\n');
    return sendAll(socket, framed.data(), framed.size());
}

bool receiveSocketLine(SOCKET socket, std::string& line) {
    line.clear();
    char ch = 0;
    while (line.size() < kMaxMessage) {
        const int received = recv(socket, &ch, 1, 0);
        if (received <= 0) return false;
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(ch);
    }
    line = "[message rejected: over 4096 bytes]";
    return true;
}

void closeSocketPeer(const std::shared_ptr<SocketPeer>& peer) {
    peer->connected.store(false);
    std::lock_guard<std::mutex> lock(peer->writeMutex);
    const SOCKET current = peer->socket.exchange(INVALID_SOCKET);
    if (current != INVALID_SOCKET) {
        shutdown(current, SD_BOTH);
        closesocket(current);
    }
}

void socketBroadcast(const std::shared_ptr<SocketServerState>& state,
                     const std::string& message) {
    std::vector<std::shared_ptr<SocketPeer>> peers;
    {
        std::lock_guard<std::mutex> lock(state->peersMutex);
        peers = state->peers;
    }
    for (const auto& peer : peers) {
        if (!peer->connected.load()) continue;
        std::lock_guard<std::mutex> lock(peer->writeMutex);
        const SOCKET socket = peer->socket.load();
        if (peer->connected.load() && socket != INVALID_SOCKET) {
            if (!sendSocketLine(socket, message)) peer->connected.store(false);
        }
    }
}

void socketPeerReader(const std::shared_ptr<SocketServerState>& state,
                      const std::shared_ptr<SocketPeer>& peer) {
    const SOCKET socket = peer->socket.load();
    std::string message;
    while (state->running.load() && peer->connected.load() &&
           receiveSocketLine(socket, message)) {
        if (message.rfind("HELLO\t", 0) == 0) {
            peer->name = sanitizeName(message.substr(6));
            std::cout << "[local socket] " << peer->name << " connected.\n";
            socketBroadcast(state, "SERVER: " + peer->name + " joined the chat.");
            continue;
        }
        if (message == "/quit") break;
        const std::string name = peer->name.empty() ? "Client" : peer->name;
        const std::string formatted = name + ": " + message;
        std::cout << formatted << "\n";
        socketBroadcast(state, formatted);
    }
    closeSocketPeer(peer);
    if (!peer->name.empty()) std::cout << "[local socket] " << peer->name << " disconnected.\n";
}

void socketAcceptLoop(const std::shared_ptr<SocketServerState>& state, SOCKET listenSocket) {
    while (state->running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready == 0) continue;
        if (ready == SOCKET_ERROR) {
            if (!state->running.load()) break;
            std::cerr << "select failed: " << WSAGetLastError() << "\n";
            continue;
        }
        SOCKET client = accept(listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!state->running.load()) break;
            std::cerr << "accept failed: " << WSAGetLastError() << "\n";
            continue;
        }
        auto peer = std::make_shared<SocketPeer>();
        peer->socket.store(client);
        {
            std::lock_guard<std::mutex> lock(state->peersMutex);
            state->peers.push_back(peer);
        }
        state->workers.emplace_back(socketPeerReader, state, peer);
    }
}

bool makeSocketAddress(SOCKADDR_UN& address) {
    ZeroMemory(&address, sizeof(address));
    address.sun_family = AF_UNIX;
    const std::size_t pathLength = std::strlen(kSocketPath);
    if (pathLength >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, kSocketPath, pathLength + 1);
    return true;
}
} // namespace

void runLocalSocketServer() {
    configureConsole();
    WSADATA data{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0) {
        std::cerr << "WSAStartup failed: " << startupResult << "\n";
        return;
    }

    DeleteFileA(kSocketPath); // Remove a stale socket path left by an earlier run.
    SOCKET listenSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "AF_UNIX socket creation failed: " << WSAGetLastError()
                  << ". Check that Windows supports AF_UNIX.\n";
        WSACleanup();
        return;
    }
    SOCKADDR_UN address{};
    if (!makeSocketAddress(address) ||
        bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::cerr << "AF_UNIX bind failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        WSACleanup();
        return;
    }
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen failed: " << WSAGetLastError() << "\n";
        closesocket(listenSocket);
        DeleteFileA(kSocketPath);
        WSACleanup();
        return;
    }

    auto state = std::make_shared<SocketServerState>();
    std::cout << "Local AF_UNIX socket server listening at " << kSocketPath << ".\n"
              << "Start clients with: ipc_messenger.exe --client socket Alice\n"
              << "Type /quit to stop the server.\n\n";
    std::thread acceptThread(socketAcceptLoop, state, listenSocket);

    std::string line;
    while (true) {
        std::cout << "Server> " << std::flush;
        if (!std::getline(std::cin, line) || line == "/quit") break;
        socketBroadcast(state, "SERVER: " + line);
    }

    socketBroadcast(state, "SERVER: shutting down");
    state->running.store(false);
    shutdown(listenSocket, SD_BOTH);
    closesocket(listenSocket);
    if (acceptThread.joinable()) acceptThread.join();

    std::vector<std::shared_ptr<SocketPeer>> peers;
    {
        std::lock_guard<std::mutex> lock(state->peersMutex);
        peers = state->peers;
    }
    for (const auto& peer : peers) closeSocketPeer(peer);
    for (auto& worker : state->workers) {
        if (worker.joinable()) worker.join();
    }
    DeleteFileA(kSocketPath);
    WSACleanup();
}

void runLocalSocketClient(const std::string& rawName) {
    configureConsole();
    const std::string name = sanitizeName(rawName);
    WSADATA data{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0) {
        std::cerr << "WSAStartup failed: " << startupResult << "\n";
        return;
    }

    SOCKET socketHandle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketHandle == INVALID_SOCKET) {
        std::cerr << "AF_UNIX socket creation failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }
    SOCKADDR_UN address{};
    if (!makeSocketAddress(address) ||
        connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::cerr << "Cannot connect to the local socket server. Start it first. Error: "
                  << WSAGetLastError() << "\n";
        closesocket(socketHandle);
        WSACleanup();
        return;
    }
    sendSocketLine(socketHandle, "HELLO\t" + name);

    std::atomic<bool> receiving{true};
    std::thread receiver([&]() {
        std::string message;
        while (receiving.load() && receiveSocketLine(socketHandle, message)) {
            std::cout << "\n" << message << "\n> " << std::flush;
        }
        receiving.store(false);
    });

    std::cout << "Connected to local-socket chat as " << name << ". Type /quit to exit.\n";
    std::string line;
    while (receiving.load()) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line == "/quit") {
            sendSocketLine(socketHandle, "/quit");
            break;
        }
        if (!sendSocketLine(socketHandle, line)) {
            std::cerr << "Sending failed. The server may have closed.\n";
            break;
        }
    }
    receiving.store(false);
    shutdown(socketHandle, SD_BOTH);
    if (receiver.joinable()) receiver.join();
    closesocket(socketHandle);
    WSACleanup();
}

} // namespace lm
