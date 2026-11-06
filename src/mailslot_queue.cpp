#include "transports.hpp"
#include <algorithm>
#include <memory>

namespace lm {
namespace {
struct MailslotClientInfo {
    std::string name;
    std::string slotPath;
};
struct MailslotServerState {
    std::atomic<bool> running{true};
    std::mutex clientsMutex;
    std::vector<MailslotClientInfo> clients;
    HANDLE slot = INVALID_HANDLE_VALUE;
};

bool sendMailslot(const std::string& slotPath, const std::string& message) {
    if (message.size() > 8192) return false;
    HANDLE writer = CreateFileA(slotPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(writer, message.data(), static_cast<DWORD>(message.size()),
                              &written, nullptr);
    CloseHandle(writer);
    return ok && written == message.size();
}

void mailslotBroadcast(const std::shared_ptr<MailslotServerState>& state,
                       const std::string& message) {
    std::vector<MailslotClientInfo> clients;
    {
        std::lock_guard<std::mutex> lock(state->clientsMutex);
        clients = state->clients;
    }
    for (const auto& client : clients) {
        if (!sendMailslot(client.slotPath, message)) {
            std::cout << "[mailslot] Could not deliver to " << client.name
                      << " (client may have disconnected).\n";
        }
    }
}

void processMailslotMessage(const std::shared_ptr<MailslotServerState>& state,
                            const std::string& message) {
    std::string command, second, rest;
    if (!splitFirstTwoTabs(message, command, second, rest)) return;

    if (command == "REG") {
        const std::string name = sanitizeName(second);
        {
            std::lock_guard<std::mutex> lock(state->clientsMutex);
            const auto it = std::find_if(state->clients.begin(), state->clients.end(),
                [&](const MailslotClientInfo& c) { return c.slotPath == rest; });
            if (it == state->clients.end()) state->clients.push_back({name, rest});
        }
        std::cout << "[mailslot] " << name << " registered.\n";
        mailslotBroadcast(state, "FROM\tSERVER\t" + name + " joined the chat.");
    } else if (command == "MSG") {
        const std::string name = sanitizeName(second);
        std::cout << name << ": " << rest << "\n";
        mailslotBroadcast(state, "FROM\t" + name + "\t" + rest);
    } else if (command == "BYE") {
        std::lock_guard<std::mutex> lock(state->clientsMutex);
        state->clients.erase(std::remove_if(state->clients.begin(), state->clients.end(),
            [&](const MailslotClientInfo& c) { return c.slotPath == rest; }), state->clients.end());
        std::cout << "[mailslot] " << sanitizeName(second) << " disconnected.\n";
    }
}

void mailslotReceiveLoop(std::shared_ptr<MailslotServerState> state) {
    char buffer[8192];
    while (state->running.load()) {
        DWORD received = 0;
        const BOOL ok = ReadFile(state->slot, buffer, sizeof(buffer), &received, nullptr);
        if (!ok) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) continue;
            if (!state->running.load()) break;
            std::cerr << "Mailslot ReadFile failed: " << error << "\n";
            break;
        }
        if (received > 0) processMailslotMessage(state, std::string(buffer, buffer + received));
    }
}

std::string buildClientSlotPath(const std::string& name) {
    return R"(\\.\mailslot\LocalMessenger_)" + name + "_" +
           std::to_string(GetCurrentProcessId());
}
} // namespace

void runMailslotServer() {
    configureConsole();
    auto state = std::make_shared<MailslotServerState>();
    state->slot = CreateMailslotA(kMailslotServerPath, 8192, 500, nullptr);
    if (state->slot == INVALID_HANDLE_VALUE) {
        std::cerr << "Cannot create server mailslot. Error: " << GetLastError() << "\n";
        return;
    }
    std::cout << "Mailslot message-queue server is ready.\n"
              << "Start clients with: ipc_messenger.exe --client mailslot Alice\n"
              << "Type /quit to stop the server.\n\n";
    std::thread receiver(mailslotReceiveLoop, state);

    std::string line;
    while (true) {
        std::cout << "Server> " << std::flush;
        if (!std::getline(std::cin, line) || line == "/quit") break;
        mailslotBroadcast(state, "FROM\tSERVER\t" + line);
    }

    mailslotBroadcast(state, "FROM\tSERVER\tserver shutting down");
    state->running.store(false);
    if (receiver.joinable()) receiver.join();
    CloseHandle(state->slot);
    state->slot = INVALID_HANDLE_VALUE;
}

void runMailslotClient(const std::string& rawName) {
    configureConsole();
    const std::string name = sanitizeName(rawName);
    const std::string ownPath = buildClientSlotPath(name);
    HANDLE ownSlot = CreateMailslotA(ownPath.c_str(), 8192, 500, nullptr);
    if (ownSlot == INVALID_HANDLE_VALUE) {
        std::cerr << "Cannot create client mailslot. Error: " << GetLastError() << "\n";
        return;
    }

    if (!sendMailslot(kMailslotServerPath, "REG\t" + name + "\t" + ownPath)) {
        std::cerr << "Cannot reach the server mailslot. Start the server first.\n";
        CloseHandle(ownSlot);
        return;
    }

    std::atomic<bool> receiving{true};
    std::thread receiver([&]() {
        char buffer[8192];
        while (receiving.load()) {
            DWORD received = 0;
            const BOOL ok = ReadFile(ownSlot, buffer, sizeof(buffer), &received, nullptr);
            if (!ok) {
                const DWORD error = GetLastError();
                if (error == ERROR_SEM_TIMEOUT) continue;
                break;
            }
            if (received == 0) continue;
            std::string command, sender, text;
            if (splitFirstTwoTabs(std::string(buffer, buffer + received), command, sender, text)
                && command == "FROM") {
                std::cout << "\n[" << sender << "] " << text << "\n> " << std::flush;
            }
        }
    });

    std::cout << "Connected to mailslot chat as " << name << ". Type /quit to exit.\n";
    std::string line;
    while (receiving.load()) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line == "/quit") {
            sendMailslot(kMailslotServerPath, "BYE\t" + name + "\t" + ownPath);
            break;
        }
        if (line.size() > kMaxMessage) line.resize(kMaxMessage);
        if (!sendMailslot(kMailslotServerPath, "MSG\t" + name + "\t" + line)) {
            std::cerr << "Message could not be sent; the server may be stopped.\n";
        }
    }
    receiving.store(false);
    if (receiver.joinable()) receiver.join();
    CloseHandle(ownSlot);
}

} // namespace lm
