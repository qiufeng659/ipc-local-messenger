#include "transports.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lm {
namespace {

constexpr DWORD kClientWriteTimeoutMs = 3000;
constexpr DWORD kServerWriteTimeoutMs = 1500;
constexpr DWORD kConnectRetryMs = 500;
constexpr DWORD kPipeBufferSize = 8192;

struct NamedPeer {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::string name;
    std::mutex writeMutex;
    std::atomic<bool> connected{true};
    std::thread readerThread;
};

struct NamedServerState {
    std::atomic<bool> running{true};
    HANDLE stopEvent = nullptr;

    std::mutex acceptMutex;
    HANDLE acceptingPipe = INVALID_HANDLE_VALUE;

    std::mutex peersMutex;
    std::vector<std::shared_ptr<NamedPeer>> peers;
};

void cancelAndWait(HANDLE pipe, OVERLAPPED& operation) {
    if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, &operation);
    }
    if (operation.hEvent != nullptr) {
        WaitForSingleObject(operation.hEvent, 5000);   
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &operation, &ignored, FALSE);
    }
}


bool writePipeMessage(
    HANDLE pipe,
    const std::string& message,
    DWORD& error,
    HANDLE stopEvent = nullptr,
    DWORD timeoutMs = kClientWriteTimeoutMs
) {
    error = ERROR_SUCCESS;

    if (pipe == INVALID_HANDLE_VALUE) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    if (message.size() > kMaxMessage) {
        error = ERROR_BUFFER_OVERFLOW;
        return false;
    }
    if (message.empty()) {
        return true;
    }

    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (operation.hEvent == nullptr) {
        error = GetLastError();
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(
        pipe,
        message.data(),
        static_cast<DWORD>(message.size()),
        &written,
        &operation
    );

    if (!ok) {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            CloseHandle(operation.hEvent);
            return false;
        }

        HANDLE waits[2] = { operation.hEvent, stopEvent };
        const DWORD waitCount = stopEvent != nullptr ? 2u : 1u;
        const DWORD waitResult = WaitForMultipleObjects(
            waitCount,
            waits,
            FALSE,
            timeoutMs
        );

        if (waitResult == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(pipe, &operation, &written, FALSE);
            if (!ok) {
                error = GetLastError();
            }
        } else if (stopEvent != nullptr && waitResult == WAIT_OBJECT_0 + 1) {
            cancelAndWait(pipe, operation);
            error = ERROR_OPERATION_ABORTED;
            ok = FALSE;
        } else if (waitResult == WAIT_TIMEOUT) {
            cancelAndWait(pipe, operation);
            error = ERROR_TIMEOUT;
            ok = FALSE;
        } else {
            error = waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
            cancelAndWait(pipe, operation);
            ok = FALSE;
        }
    }

    CloseHandle(operation.hEvent);

    if (!ok) {
        return false;
    }
    if (written != message.size()) {
        error = ERROR_WRITE_FAULT;
        return false;
    }
    return true;
}

bool readPipeMessage(
    HANDLE pipe,
    std::string& message,
    DWORD& error,
    HANDLE stopEvent
) {
    message.clear();
    error = ERROR_SUCCESS;

    if (pipe == INVALID_HANDLE_VALUE) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }

    char buffer[kMaxMessage + 1]{};
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (operation.hEvent == nullptr) {
        error = GetLastError();
        return false;
    }

    DWORD received = 0;
    BOOL ok = ReadFile(
        pipe,
        buffer,
        static_cast<DWORD>(sizeof(buffer)),
        &received,
        &operation
    );

    if (!ok) {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            CloseHandle(operation.hEvent);
            return false;
        }

        HANDLE waits[2] = { operation.hEvent, stopEvent };
        const DWORD waitCount = stopEvent != nullptr ? 2u : 1u;
        const DWORD waitResult = WaitForMultipleObjects(
            waitCount,
            waits,
            FALSE,
            INFINITE
        );

        if (waitResult == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(pipe, &operation, &received, FALSE);
            if (!ok) {
                error = GetLastError();
            }
        } else if (stopEvent != nullptr && waitResult == WAIT_OBJECT_0 + 1) {
            cancelAndWait(pipe, operation);
            error = ERROR_OPERATION_ABORTED;
            ok = FALSE;
        } else {
            error = waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
            cancelAndWait(pipe, operation);
            ok = FALSE;
        }
    }

    CloseHandle(operation.hEvent);

    if (!ok) {
        return false;
    }
    if (received == 0) {
        error = ERROR_BROKEN_PIPE;
        return false;
    }
    if (received > kMaxMessage) {
        error = ERROR_BUFFER_OVERFLOW;
        return false;
    }

    message.assign(buffer, buffer + received);
    return true;
}

void closePeerPipe(const std::shared_ptr<NamedPeer>& peer) {
    std::lock_guard<std::mutex> lock(peer->writeMutex);
    if (peer->pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(peer->pipe, nullptr);
        DisconnectNamedPipe(peer->pipe);
        CloseHandle(peer->pipe);
        peer->pipe = INVALID_HANDLE_VALUE;
    }
}

void namedBroadcast(
    const std::shared_ptr<NamedServerState>& state,
    const std::string& message
) {
    std::vector<std::shared_ptr<NamedPeer>> peers;
    {
        std::lock_guard<std::mutex> lock(state->peersMutex);
        peers = state->peers;
    }

    for (const auto& peer : peers) {
        if (!state->running.load() || !peer->connected.load()) {
            continue;
        }

        std::lock_guard<std::mutex> writeLock(peer->writeMutex);
        if (!state->running.load() || !peer->connected.load() ||
            peer->pipe == INVALID_HANDLE_VALUE) {
            continue;
        }

        DWORD error = ERROR_SUCCESS;
        if (!writePipeMessage(
                peer->pipe,
                message,
                error,
                state->stopEvent,
                kServerWriteTimeoutMs)) {
            std::cerr << "[named pipe] Send to client failed (error "
                      << error << "); disconnecting this client.\n";
            peer->connected.store(false);
            CancelIoEx(peer->pipe, nullptr);
        }
    }
}

void namedPipeReader(
    const std::shared_ptr<NamedServerState>& state,
    const std::shared_ptr<NamedPeer>& peer
) {
    while (state->running.load() && peer->connected.load()) {
        std::string message;
        DWORD error = ERROR_SUCCESS;

        if (!readPipeMessage(peer->pipe, message, error, state->stopEvent)) {
            if (state->running.load() && peer->connected.load() &&
                error != ERROR_OPERATION_ABORTED) {
                std::cerr << "[named pipe] Read from client failed (error "
                          << error << ").\n";
            }
            break;
        }

        if (message.empty()) {
            continue;
        }

        if (message.compare(0, 6, "HELLO\t") == 0) {
            peer->name = sanitizeName(message.substr(6));
            std::cout << "[named pipe] " << peer->name << " connected.\n";
            namedBroadcast(state, "SERVER: " + peer->name + " joined the chat.");
            continue;
        }

        if (message == "/quit") {
            break;
        }

        if (peer->name.empty()) {
            peer->name = "Client";
        }

        std::string formatted = peer->name + ": " + message;
        if (formatted.size() > kMaxMessage) {
            formatted.resize(kMaxMessage);
        }

        std::cout << formatted << '\n' << std::flush;
        namedBroadcast(state, formatted);
    }

    peer->connected.store(false);
    closePeerPipe(peer);
    if (!peer->name.empty()) {
        std::cout << "[named pipe] " << peer->name << " disconnected.\n";
    }
}

bool connectNamedPipeInstance(
    HANDLE pipe,
    const std::shared_ptr<NamedServerState>& state,
    DWORD& error
) {
    error = ERROR_SUCCESS;
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (operation.hEvent == nullptr) {
        error = GetLastError();
        return false;
    }

    BOOL ok = ConnectNamedPipe(pipe, &operation);
    if (ok) {
        CloseHandle(operation.hEvent);
        return true;
    }

    error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        SetEvent(operation.hEvent);
        CloseHandle(operation.hEvent);
        return true;
    }
    if (error != ERROR_IO_PENDING) {
        CloseHandle(operation.hEvent);
        return false;
    }

    HANDLE waits[2] = { operation.hEvent, state->stopEvent };
    const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0) {
        DWORD ignored = 0;
        ok = GetOverlappedResult(pipe, &operation, &ignored, FALSE);
        if (!ok) {
            error = GetLastError();
        }
    } else if (waitResult == WAIT_OBJECT_0 + 1) {
        cancelAndWait(pipe, operation);
        error = ERROR_OPERATION_ABORTED;
        ok = FALSE;
    } else {
        error = waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        cancelAndWait(pipe, operation);
        ok = FALSE;
    }

    CloseHandle(operation.hEvent);
    return ok != FALSE;
}

void namedPipeAcceptLoop(const std::shared_ptr<NamedServerState>& state) {
    while (state->running.load()) {
        HANDLE pipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\LocalMessengerIPC",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            kPipeBufferSize,
            kPipeBufferSize,
            0,
            nullptr
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            if (state->running.load()) {
                std::cerr << "CreateNamedPipe failed: " << GetLastError() << '\n';
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->acceptMutex);
            if (!state->running.load()) {
                CloseHandle(pipe);
                break;
            }
            state->acceptingPipe = pipe;
        }

        DWORD error = ERROR_SUCCESS;
        const bool connected = connectNamedPipeInstance(pipe, state, error);

        {
            std::lock_guard<std::mutex> lock(state->acceptMutex);
            if (state->acceptingPipe == pipe) {
                state->acceptingPipe = INVALID_HANDLE_VALUE;
            }
        }

        if (!connected) {
            CloseHandle(pipe);
            if (!state->running.load() || error == ERROR_OPERATION_ABORTED) {
                break;
            }
            std::cerr << "ConnectNamedPipe failed: " << error << '\n';
            continue;
        }

        if (!state->running.load()) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        auto peer = std::make_shared<NamedPeer>();
        peer->pipe = pipe;
        {
            std::lock_guard<std::mutex> lock(state->peersMutex);
            state->peers.push_back(peer);
        }

        std::cout << "[named pipe] Client connected.\n" << std::flush;
        peer->readerThread = std::thread(namedPipeReader, state, peer);
    }
}

} // namespace

void runNamedPipeServer() {
    configureConsole();

    auto state = std::make_shared<NamedServerState>();
    state->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state->stopEvent == nullptr) {
        std::cerr << "Cannot create server stop event: " << GetLastError() << '\n';
        return;
    }

    std::cout
        << "Named-pipe server listening at \\\\.\\pipe\\LocalMessengerIPC.\n"
        << "Start clients with: ipc_messenger.exe --client named-pipe Alice\n"
        << "Type /quit to stop the server.\n\n";

    std::thread acceptThread(namedPipeAcceptLoop, state);

    std::string line;
    while (state->running.load()) {
        std::cout << "Server> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line == "/quit") {
            break;
        }
        if (!line.empty()) {
            namedBroadcast(state, "SERVER: " + line);
        }
    }

    namedBroadcast(state, "SERVER: shutting down");
    state->running.store(false);
    SetEvent(state->stopEvent);

    {
        std::lock_guard<std::mutex> lock(state->acceptMutex);
        if (state->acceptingPipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(state->acceptingPipe, nullptr);
        }
    }

    if (acceptThread.joinable()) {
        acceptThread.join();
    }

    std::vector<std::shared_ptr<NamedPeer>> peers;
    {
        std::lock_guard<std::mutex> lock(state->peersMutex);
        peers = state->peers;
    }

    for (const auto& peer : peers) {
        if (peer->readerThread.joinable()) {
            peer->readerThread.join();
        }
    }

    CloseHandle(state->stopEvent);
    state->stopEvent = nullptr;
    std::cout << "Named-pipe server stopped.\n";
}

void runNamedPipeClient(const std::string& rawName) {
    configureConsole();
    const std::string name = sanitizeName(rawName);

    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (pipe == INVALID_HANDLE_VALUE) {
        pipe = CreateFileW(
            L"\\\\.\\pipe\\LocalMessengerIPC",
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
            std::cerr << "Cannot connect to named pipe. Error: " << error << '\n';
            return;
        }

        if (!WaitNamedPipeW(L"\\\\.\\pipe\\LocalMessengerIPC", 1000)) {
            std::cout << "Waiting for named-pipe server...\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kConnectRetryMs));
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        std::cerr << "Cannot set named-pipe message mode. Error: " << GetLastError() << '\n';
        CloseHandle(pipe);
        return;
    }

    DWORD error = ERROR_SUCCESS;
    if (!writePipeMessage(pipe, "HELLO\t" + name, error, nullptr,
                          kClientWriteTimeoutMs)) {
        std::cerr << "Failed to send initial HELLO message. Error: " << error << '\n';
        CloseHandle(pipe);
        return;
    }

    HANDLE receiverStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (receiverStopEvent == nullptr) {
        std::cerr << "Cannot create receiver stop event: " << GetLastError() << '\n';
        CloseHandle(pipe);
        return;
    }

    std::atomic<bool> receiving{true};
    std::thread receiver([&]() {
        while (receiving.load()) {
            std::string message;
            DWORD readError = ERROR_SUCCESS;
            if (!readPipeMessage(pipe, message, readError, receiverStopEvent)) {
                if (receiving.load() && readError != ERROR_OPERATION_ABORTED) {
                    std::cerr << "\nConnection closed or read failed (error "
                              << readError << ").\n";
                }
                receiving.store(false);
                break;
            }

            if (message == "SERVER: shutting down") {
                std::cout << "\n[RECEIVED] " << message << '\n';
                receiving.store(false);
                break;
            }

            std::cout << "\n[RECEIVED] " << message << "\n> " << std::flush;
        }
    });

    std::cout << "Connected to named-pipe chat as " << name
              << ". Type /quit to exit.\n";

    std::string line;
    while (receiving.load()) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (!receiving.load()) {
            break;
        }
        if (line == "/quit") {
            DWORD writeError = ERROR_SUCCESS;
            writePipeMessage(pipe, "/quit", writeError, receiverStopEvent,
                             kClientWriteTimeoutMs);
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() > kMaxMessage - name.size() - 2) {
            line.resize(kMaxMessage - name.size() - 2);
        }

        DWORD writeError = ERROR_SUCCESS;
        if (!writePipeMessage(pipe, line, writeError, receiverStopEvent,
                              kClientWriteTimeoutMs)) {
            std::cerr << "Sending failed (error " << writeError
                      << "). The server may have stopped.\n";
            break;
        }
    }

    receiving.store(false);
    SetEvent(receiverStopEvent);
    CancelIoEx(pipe, nullptr);

    if (receiver.joinable()) {
        receiver.join();
    }

    CloseHandle(receiverStopEvent);
    CloseHandle(pipe);
    std::cout << "Named-pipe client exited.\n";
}

} // namespace lm
