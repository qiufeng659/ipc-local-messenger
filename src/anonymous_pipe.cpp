#include "transports.hpp"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>

namespace lm {
namespace {
struct AnonymousPeer {
    std::string name;
    HANDLE fromChild = INVALID_HANDLE_VALUE; // server reads here
    HANDLE toChild = INVALID_HANDLE_VALUE;   // server writes here
    HANDLE process = nullptr;
    std::mutex writeMutex;
    std::atomic<bool> connected{true};
};

void broadcastAnonymous(const std::vector<std::shared_ptr<AnonymousPeer>>& peers,
                        const std::string& message) {
    for (const auto& peer : peers) {
        if (!peer->connected.load()) continue;
        std::lock_guard<std::mutex> lock(peer->writeMutex);
        if (peer->toChild != INVALID_HANDLE_VALUE &&
            !writeLineToHandle(peer->toChild, message)) {
            peer->connected.store(false);
        }
    }
}

void closeAnonymousPeer(const std::shared_ptr<AnonymousPeer>& peer) {
    std::lock_guard<std::mutex> lock(peer->writeMutex);
    if (peer->fromChild != INVALID_HANDLE_VALUE) {
        CloseHandle(peer->fromChild);
        peer->fromChild = INVALID_HANDLE_VALUE;
    }
    if (peer->toChild != INVALID_HANDLE_VALUE) {
        CloseHandle(peer->toChild);
        peer->toChild = INVALID_HANDLE_VALUE;
    }
    if (peer->process) {
        CloseHandle(peer->process);
        peer->process = nullptr;
    }
}
} // namespace


int runAnonymousChild(HANDLE readHandle, HANDLE writeHandle,
                      const std::string& rawName) {
    configureConsole();

    const std::string name = sanitizeName(rawName);
    std::atomic<bool> receiving{true};
    std::atomic<HANDLE> receiverWinHandle{nullptr};

    std::cout << "Anonymous-pipe client: " << name << "\n"
              << "Messages are sent through inherited anonymous pipe handles.\n"
              << "Type /quit to exit.\n\n";

    std::thread receiver([&]() {
        HANDLE currentThreadHandle = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(),
                GetCurrentThread(),
                GetCurrentProcess(),
                &currentThreadHandle,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS)) {
            receiverWinHandle.store(currentThreadHandle);
        }

        std::string message;
        while (receiving.load() &&
               readLineFromHandle(readHandle, message)) {
            if (message == "__SERVER_SHUTDOWN__") {
                break;
            }

            std::cout << "\n" << message << "\n> " << std::flush;
        }

        receiving.store(false);
    });

    std::string line;

    while (receiving.load()) {
        std::cout << "> " << std::flush;

        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line.size() > kMaxMessage) {
            line.resize(kMaxMessage);
        }

        if (line == "/quit") {
            writeLineToHandle(writeHandle, "/quit");
            break;
        }

        if (!writeLineToHandle(writeHandle, line)) {
            std::cout << "Connection to the server was closed.\n";
            break;
        }
    }

    receiving.store(false);

    if (receiver.joinable()) {
        HANDLE threadHandle = receiverWinHandle.load();

        if (threadHandle != nullptr) {
            CancelSynchronousIo(threadHandle);
        }

        receiver.join();

        if (threadHandle != nullptr) {
            CloseHandle(threadHandle);
        }
    }

    CloseHandle(readHandle);
    CloseHandle(writeHandle);

    return 0;
}

void runAnonymousServer(int childCount) {
    configureConsole();
    childCount = std::max(1, std::min(childCount, 8));
    const std::wstring exePath = getExecutablePath();
    if (exePath.empty()) {
        std::cerr << "Cannot determine the executable path.\n";
        return;
    }

    std::vector<std::shared_ptr<AnonymousPeer>> peers;
    std::vector<std::thread> readers;
    std::cout << "Starting anonymous-pipe chat with " << childCount
              << " child client process(es). Each client opens a separate console.\n";

    for (int i = 1; i <= childCount; ++i) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE childRead = INVALID_HANDLE_VALUE;
        HANDLE serverWrite = INVALID_HANDLE_VALUE;
        HANDLE serverRead = INVALID_HANDLE_VALUE;
        HANDLE childWrite = INVALID_HANDLE_VALUE;

        if (!CreatePipe(&childRead, &serverWrite, &sa, 0) ||
            !CreatePipe(&serverRead, &childWrite, &sa, 0)) {
            std::cerr << "CreatePipe failed: " << GetLastError() << "\n";
            if (childRead != INVALID_HANDLE_VALUE) CloseHandle(childRead);
            if (serverWrite != INVALID_HANDLE_VALUE) CloseHandle(serverWrite);
            if (serverRead != INVALID_HANDLE_VALUE) CloseHandle(serverRead);
            if (childWrite != INVALID_HANDLE_VALUE) CloseHandle(childWrite);
            break;
        }

        SetHandleInformation(serverWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(serverRead, HANDLE_FLAG_INHERIT, 0);

        const std::string clientName = "Client" + std::to_string(i);
        std::wstring command = L"\"" + exePath + L"\" --anon-child " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(childRead)) + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(childWrite)) + L" " +
            widenAscii(clientName);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                            CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &processInfo)) {
            std::cerr << "Could not start child client " << clientName
                      << ". Windows error: " << GetLastError() << "\n";
            CloseHandle(childRead);
            CloseHandle(serverWrite);
            CloseHandle(serverRead);
            CloseHandle(childWrite);
            continue;
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(childRead);
        CloseHandle(childWrite);

        auto peer = std::make_shared<AnonymousPeer>();
        peer->name = clientName;
        peer->fromChild = serverRead;
        peer->toChild = serverWrite;
        peer->process = processInfo.hProcess;
        peers.push_back(peer);

    }

    if (peers.empty()) {
        std::cerr << "No child clients were started.\n";
        return;
    }

    for (const auto& peer : peers) {
        readers.emplace_back([peer, &peers]() {
            std::string message;
            while (peer->connected.load() && readLineFromHandle(peer->fromChild, message)) {
                if (message == "/quit") break;
                const std::string formatted = peer->name + ": " + message;
                std::cout << "\n" << formatted << "\n> " << std::flush;
                broadcastAnonymous(peers, formatted);
            }
            peer->connected.store(false);
            std::cout << "\n" << peer->name << " disconnected.\n> " << std::flush;
        });
    }

    std::cout << "Anonymous-pipe server ready. Send messages to the client windows.\n"
              << "Type /quit to close the demo and its child processes.\n\n";
    std::string line;
    while (true) {
        std::cout << "Server> " << std::flush;
        if (!std::getline(std::cin, line) || line == "/quit") break;
        broadcastAnonymous(peers, "SERVER: " + line);
    }

    broadcastAnonymous(peers, "SERVER: shutting down");
    for (const auto& peer : peers) {
        if (peer->process) TerminateProcess(peer->process, 0);
    }
    for (const auto& peer : peers) {
        if (peer->process) WaitForSingleObject(peer->process, 2000);
    }
    for (auto& reader : readers) {
        if (reader.joinable()) reader.join();
    }
    for (const auto& peer : peers) closeAnonymousPeer(peer);
}

} // namespace lm
