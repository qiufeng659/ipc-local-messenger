#pragma once
#include "common.hpp"

namespace lm {
void runAnonymousServer(int childCount);
int runAnonymousChild(HANDLE readHandle, HANDLE writeHandle, const std::string& name);
void runNamedPipeServer();
void runNamedPipeClient(const std::string& name);
void runMailslotServer();
void runMailslotClient(const std::string& name);
void runLocalSocketServer();
void runLocalSocketClient(const std::string& name);
}
