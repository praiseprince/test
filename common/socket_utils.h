#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
using SockLenType = int;
#define INVALID_SOCK INVALID_SOCKET
#define SOCK_ERR SOCKET_ERROR
inline void initSockets() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}
inline void cleanupSockets() { WSACleanup(); }
inline void closeSocket(SocketHandle s) {
    if (s != INVALID_SOCK) {
        closesocket(s);
    }
}
inline void shutdownSocket(SocketHandle s) {
    if (s != INVALID_SOCK) {
        shutdown(s, SD_BOTH);
    }
}
inline int getLastSocketError() { return WSAGetLastError(); }
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
using SocketHandle = int;
using SockLenType = socklen_t;
#define INVALID_SOCK -1
#define SOCK_ERR -1
inline void initSockets() {}
inline void cleanupSockets() {}
inline void closeSocket(SocketHandle s) {
    if (s != INVALID_SOCK) {
        close(s);
    }
}
inline void shutdownSocket(SocketHandle s) {
    if (s != INVALID_SOCK) {
        shutdown(s, SHUT_RDWR);
    }
}
inline int getLastSocketError() { return errno; }
#endif

inline bool setReuseAddress(SocketHandle socketHandle) {
    int opt = 1;
    return setsockopt(
               socketHandle,
               SOL_SOCKET,
               SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt),
               static_cast<SockLenType>(sizeof(opt))) != SOCK_ERR;
}

inline bool sendAll(SocketHandle socketHandle, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t totalSent = 0;
    while (totalSent < size) {
        const auto remaining = size - totalSent;
        const int sent = send(socketHandle, bytes + totalSent, static_cast<int>(remaining), 0);
        if (sent == SOCK_ERR || sent == 0) {
            return false;
        }
        totalSent += static_cast<std::size_t>(sent);
    }
    return true;
}

inline bool recvAll(SocketHandle socketHandle, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    std::size_t totalRead = 0;
    while (totalRead < size) {
        const auto remaining = size - totalRead;
        const int received = recv(socketHandle, bytes + totalRead, static_cast<int>(remaining), 0);
        if (received <= 0) {
            return false;
        }
        totalRead += static_cast<std::size_t>(received);
    }
    return true;
}

inline bool waitForReadable(SocketHandle socketHandle, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketHandle, &readSet);

    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef _WIN32
    const int result = select(0, &readSet, nullptr, nullptr, &timeout);
#else
    const int result = select(socketHandle + 1, &readSet, nullptr, nullptr, &timeout);
#endif
    return result > 0 && FD_ISSET(socketHandle, &readSet);
}

inline std::string socketErrorString(const std::string& prefix) {
    return prefix + " (socket error " + std::to_string(getLastSocketError()) + ")";
}
