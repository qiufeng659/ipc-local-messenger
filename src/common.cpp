#include "common.hpp"
#include <algorithm>
#include <cctype>
#include <limits>

namespace lm {

void configureConsole() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::ios::sync_with_stdio(true);
}

std::string sanitizeName(std::string name) {
    if (name.empty()) name = "User";
    for (char& ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_' || ch == '-')) ch = '_';
    }
    if (name.size() > 24) name.resize(24);
    return name;
}

bool readLineFromHandle(HANDLE handle, std::string& line) {
    line.clear();
    char ch = 0;
    DWORD readCount = 0;
    while (line.size() < kMaxMessage) {
        if (!ReadFile(handle, &ch, 1, &readCount, nullptr) || readCount == 0) {
            return false;
        }
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(ch);
    }
    while (true) {
        if (!ReadFile(handle, &ch, 1, &readCount, nullptr) || readCount == 0) return false;
        if (ch == '\n') break;
    }
    line = "[message rejected: over 4096 bytes]";
    return true;
}

bool writeBytesToHandle(HANDLE handle, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD toWrite = static_cast<DWORD>(std::min<std::size_t>(
            data.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        if (!WriteFile(handle, data.data() + offset, toWrite, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool writeLineToHandle(HANDLE handle, const std::string& line) {
    std::string framed = line;
    if (framed.size() > kMaxMessage) framed.resize(kMaxMessage);
    framed.push_back('\n');
    return writeBytesToHandle(handle, framed);
}

bool splitFirstTab(const std::string& input, std::string& first, std::string& rest) {
    const auto pos = input.find('\t');
    if (pos == std::string::npos) return false;
    first = input.substr(0, pos);
    rest = input.substr(pos + 1);
    return true;
}

bool splitFirstTwoTabs(const std::string& input, std::string& first,
                       std::string& second, std::string& rest) {
    std::string tail;
    if (!splitFirstTab(input, first, tail)) return false;
    return splitFirstTab(tail, second, rest);
}

std::wstring getExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return L"";
    path.resize(size);
    return path;
}

std::wstring widenAscii(const std::string& input) {
    return std::wstring(input.begin(), input.end());
}

void printTransportHelp() {
    std::cout << R"(Local Messenger - Windows IPC demo

Run the server first, then start clients in other VS Code terminals.

1) Anonymous pipes (server automatically starts independent child client windows):
   ipc_messenger.exe --server anonymous --clients 2

2) Named pipes:
   ipc_messenger.exe --server named-pipe
   ipc_messenger.exe --client named-pipe Alice
   ipc_messenger.exe --client named-pipe Bob

3) Message queue using Windows Mailslots:
   ipc_messenger.exe --server mailslot
   ipc_messenger.exe --client mailslot Alice
   ipc_messenger.exe --client mailslot Bob

4) Local socket (Windows AF_UNIX, Windows 10 version 1803 or later):
   ipc_messenger.exe --server socket
   ipc_messenger.exe --client socket Alice
   ipc_messenger.exe --client socket Bob

Type /quit to leave a client or stop the server. Messages are local to this machine.
For named-pipe, mailslot and socket modes, the server must be started before clients.
)";
}

} // namespace lm
