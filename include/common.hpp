#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lm {
constexpr std::size_t kMaxMessage = 4096;
constexpr const char* kNamedPipePath = R"(\\.\pipe\LocalMessengerIPC)";
constexpr const char* kMailslotServerPath = R"(\\.\mailslot\LocalMessengerServer)";
constexpr const char* kSocketPath = R"(.\local_messenger.sock)";

void configureConsole();
std::string sanitizeName(std::string name);
bool readLineFromHandle(HANDLE handle, std::string& line);
bool writeLineToHandle(HANDLE handle, const std::string& line);
bool writeBytesToHandle(HANDLE handle, const std::string& data);
bool splitFirstTab(const std::string& input, std::string& first, std::string& rest);
bool splitFirstTwoTabs(const std::string& input, std::string& first,
                       std::string& second, std::string& rest);
std::wstring getExecutablePath();
std::wstring widenAscii(const std::string& input);
void printTransportHelp();

} // namespace lm
