cmake_minimum_required(VERSION 3.20)
project(LocalMessengerIPC LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project uses native Windows IPC APIs and is intended to build on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ipc_messenger
    src/main.cpp
    src/common.cpp
    src/anonymous_pipe.cpp
    src/named_pipe.cpp
    src/mailslot_queue.cpp
    src/local_socket.cpp
)

target_include_directories(ipc_messenger PRIVATE include)
target_compile_definitions(ipc_messenger PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)
target_link_libraries(ipc_messenger PRIVATE ws2_32)

if(MSVC)
    target_compile_options(ipc_messenger PRIVATE /W4 /permissive-)
else()
    target_compile_options(ipc_messenger PRIVATE -Wall -Wextra)
endif()
