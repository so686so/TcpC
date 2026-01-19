cmake_minimum_required(VERSION 3.10)
project(TcpContextProject C)

set(CMAKE_C_STANDARD 99)

# 헤더 경로 포함
include_directories(include examples)

# 라이브러리 소스 파일
set(LIB_SOURCES
    src/PacketUtils.c
    src/SafeQueue.c
    src/TcpClient.c
    src/TcpServer.c
)

# Pthread 라이브러리 연결
find_package(Threads REQUIRED)

# 1. 서버 실행 파일 빌드
add_executable(server_app examples/MainServer.c ${LIB_SOURCES})
target_link_libraries(server_app Threads::Threads)

# 2. 클라이언트 실행 파일 빌드
add_executable(client_app examples/MainClient.c ${LIB_SOURCES})
target_link_libraries(client_app Threads::Threads)