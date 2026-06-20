#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

namespace rm {

bool init_winsock();
void cleanup_winsock();

std::string get_hostname();
std::string get_primary_ipv4();

bool set_socket_timeout_ms(SOCKET socket_handle, int timeout_ms);
bool set_broadcast_enabled(SOCKET socket_handle);

// Disables Nagle's algorithm so small control packets and the video
// header+payload are flushed immediately instead of being coalesced.
bool set_tcp_nodelay(SOCKET socket_handle);

}
