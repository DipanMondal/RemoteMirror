#include "net_utils.h"

#include <windows.h>
#include <iphlpapi.h>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace rm {

bool init_winsock() {
    WSADATA data{};
    int result = WSAStartup(MAKEWORD(2, 2), &data);
    return result == 0;
}

void cleanup_winsock() {
    WSACleanup();
}

std::string get_hostname() {
    char name[256]{};
    if (gethostname(name, sizeof(name)) == SOCKET_ERROR) {
        return "Unknown-PC";
    }
    return std::string(name);
}

std::string get_primary_ipv4() {
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname)) == SOCKET_ERROR) {
        return "Unknown";
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) {
        return "Unknown";
    }

    std::string ip = "Unknown";

    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
        char ip_str[INET_ADDRSTRLEN]{};

        if (inet_ntop(AF_INET, &(addr->sin_addr), ip_str, sizeof(ip_str))) {
            std::string candidate(ip_str);

            // Avoid loopback if possible.
            if (candidate != "127.0.0.1") {
                ip = candidate;
                break;
            }

            if (ip == "Unknown") {
                ip = candidate;
            }
        }
    }

    freeaddrinfo(result);
    return ip;
}

bool set_socket_timeout_ms(SOCKET socket_handle, int timeout_ms) {
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    return setsockopt(
        socket_handle,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout),
        sizeof(timeout)
    ) == 0;
}

bool set_broadcast_enabled(SOCKET socket_handle) {
    BOOL enabled = TRUE;
    return setsockopt(
        socket_handle,
        SOL_SOCKET,
        SO_BROADCAST,
        reinterpret_cast<const char*>(&enabled),
        sizeof(enabled)
    ) == 0;
}

}
