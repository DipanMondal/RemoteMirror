#pragma once

#include <cstdint>

namespace rm {

constexpr const char* APP_NAME = "RemoteMirror";
constexpr const char* DISCOVER_REQUEST = "RM_DISCOVER_REQUEST_V1";
constexpr const char* DISCOVER_REPLY_PREFIX = "RM_DISCOVER_REPLY_V1";

constexpr uint16_t DISCOVERY_PORT = 50500;
constexpr uint16_t CONTROL_PORT   = 50510;
constexpr uint16_t VIDEO_PORT     = 50511;

}
