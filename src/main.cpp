#include "transports.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
std::uint64_t parseHandle(const char* value) {
    return static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10));
}

void printUsage() {
    lm::printTransportHelp();
}
}

int main(int argc, char* argv[]) {
    lm::configureConsole();

    if (argc == 1 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        printUsage();
        return 0;
    }

    if (std::string(argv[1]) == "--anon-child") {
        if (argc < 5) {
            std::cerr << "Invalid anonymous-pipe child arguments.\n";
            return 2;
        }
        HANDLE readHandle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(parseHandle(argv[2])));
        HANDLE writeHandle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(parseHandle(argv[3])));
        return lm::runAnonymousChild(readHandle, writeHandle, argv[4]);
    }

    if (argc >= 3 && std::string(argv[1]) == "--server") {
        const std::string transport = argv[2];
        if (transport == "anonymous") {
            int clients = 2;
            if (argc >= 5 && std::string(argv[3]) == "--clients") {
                clients = std::atoi(argv[4]);
            }
            lm::runAnonymousServer(clients);
            return 0;
        }
        if (transport == "named-pipe") {
            lm::runNamedPipeServer();
            return 0;
        }
        if (transport == "mailslot") {
            lm::runMailslotServer();
            return 0;
        }
        if (transport == "socket") {
            lm::runLocalSocketServer();
            return 0;
        }
        std::cerr << "Unknown transport: " << transport << "\n";
        printUsage();
        return 2;
    }

    if (argc >= 4 && std::string(argv[1]) == "--client") {
        const std::string transport = argv[2];
        const std::string name = lm::sanitizeName(argv[3]);
        if (transport == "anonymous") {
            std::cerr << "Anonymous-pipe clients are started automatically by the server.\n";
            return 2;
        }
        if (transport == "named-pipe") {
            lm::runNamedPipeClient(name);
            return 0;
        }
        if (transport == "mailslot") {
            lm::runMailslotClient(name);
            return 0;
        }
        if (transport == "socket") {
            lm::runLocalSocketClient(name);
            return 0;
        }
        std::cerr << "Unknown transport: " << transport << "\n";
        printUsage();
        return 2;
    }

    printUsage();
    return 2;
}
