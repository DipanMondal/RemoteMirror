#include "net_utils.h"

#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cctype>
#include <string>
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

static std::string to_lower_copy(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return text;
}

static std::string wide_to_utf8(const wchar_t* wide_text) {
    if (!wide_text) {
        return "";
    }

    int needed = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_text,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (needed <= 0) {
        return "";
    }

    std::string result;
    result.resize(static_cast<size_t>(needed - 1));

    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide_text,
        -1,
        result.data(),
        needed,
        nullptr,
        nullptr
    );

    return result;
}

static bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

static bool is_169_254_address(const std::string& ip) {
    return starts_with(ip, "169.254.");
}

static bool is_loopback_address(const std::string& ip) {
    return starts_with(ip, "127.");
}

static bool is_private_ipv4(const std::string& ip) {
    if (starts_with(ip, "10.")) {
        return true;
    }

    if (starts_with(ip, "192.168.")) {
        return true;
    }

    if (starts_with(ip, "172.")) {
        size_t first_dot = ip.find('.');
        size_t second_dot = ip.find('.', first_dot + 1);

        if (first_dot != std::string::npos && second_dot != std::string::npos) {
            std::string second_part = ip.substr(first_dot + 1, second_dot - first_dot - 1);
            int second_octet = std::atoi(second_part.c_str());

            return second_octet >= 16 && second_octet <= 31;
        }
    }

    return false;
}

static bool is_bad_adapter_name(const std::string& adapter_name) {
    std::string name = to_lower_copy(adapter_name);

    return name.find("virtual") != std::string::npos ||
           name.find("vmware") != std::string::npos ||
           name.find("virtualbox") != std::string::npos ||
           name.find("hyper-v") != std::string::npos ||
           name.find("bluetooth") != std::string::npos ||
           name.find("loopback") != std::string::npos ||
           name.find("tailscale") != std::string::npos ||
           name.find("zerotier") != std::string::npos ||
           name.find("wsl") != std::string::npos ||
           name.find("tunnel") != std::string::npos;
}

static int score_adapter_ip(
    const std::string& ip,
    const std::string& adapter_name,
    ULONG if_type
) {
    int score = 0;

    if (is_private_ipv4(ip)) {
        score += 100;
    }

    if (starts_with(ip, "192.168.")) {
        score += 40;
    }

    // Windows hotspot / ICS commonly uses 192.168.137.x
    if (starts_with(ip, "192.168.137.")) {
        score += 50;
    }

    if (!is_bad_adapter_name(adapter_name)) {
        score += 50;
    } else {
        score -= 80;
    }

    if (if_type == IF_TYPE_IEEE80211) {
        score += 40;
    }

    if (if_type == IF_TYPE_ETHERNET_CSMACD) {
        score += 30;
    }

    return score;
}

std::string get_primary_ipv4() {
    ULONG buffer_size = 15000;

    std::vector<unsigned char> buffer(buffer_size);

    IP_ADAPTER_ADDRESSES* adapters =
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    DWORD result = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_ANYCAST |
        GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER,
        nullptr,
        adapters,
        &buffer_size
    );

    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

        result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            adapters,
            &buffer_size
        );
    }

    if (result != NO_ERROR) {
        return "Unknown";
    }

    std::string best_ip = "Unknown";
    int best_score = -100000;

    for (IP_ADAPTER_ADDRESSES* adapter = adapters;
         adapter != nullptr;
         adapter = adapter->Next) {

        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }

        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            adapter->IfType == IF_TYPE_TUNNEL) {
            continue;
        }

        std::string adapter_name = wide_to_utf8(adapter->FriendlyName);

        for (IP_ADAPTER_UNICAST_ADDRESS* address = adapter->FirstUnicastAddress;
             address != nullptr;
             address = address->Next) {

            if (!address->Address.lpSockaddr) {
                continue;
            }

            if (address->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }

            sockaddr_in* ipv4 =
                reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);

            char ip_text[INET_ADDRSTRLEN]{};

            if (!inet_ntop(AF_INET, &(ipv4->sin_addr), ip_text, sizeof(ip_text))) {
                continue;
            }

            std::string ip(ip_text);

            if (ip == "0.0.0.0" ||
                is_loopback_address(ip) ||
                is_169_254_address(ip)) {
                continue;
            }

            int score = score_adapter_ip(ip, adapter_name, adapter->IfType);

            if (score > best_score) {
                best_score = score;
                best_ip = ip;
            }
        }
    }

    return best_ip;
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