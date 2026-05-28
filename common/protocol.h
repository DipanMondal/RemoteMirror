#pragma once

#include <cstdint>

namespace rm {

constexpr const char* APP_NAME = "RemoteMirror";

constexpr const char* DISCOVER_REQUEST = "RM_DISCOVER_REQUEST_V1";
constexpr const char* DISCOVER_REPLY_PREFIX = "RM_DISCOVER_REPLY_V1";

constexpr const char* CONTROL_HELLO = "RM_CONTROL_HELLO_V1";
constexpr const char* CONTROL_OK = "RM_CONTROL_OK_V1";
constexpr const char* CONTROL_BUSY = "RM_CONTROL_BUSY_V1";
constexpr const char* CONTROL_DISCONNECT = "RM_CONTROL_DISCONNECT_V1";

constexpr const char* CONTROL_ACCESS_STATE = "RM_ACCESS_STATE";
constexpr const char* CONTROL_KEY_EVENT = "RM_KEY_EVENT";
constexpr const char* CONTROL_MOUSE_MOVE = "RM_MOUSE_MOVE";
constexpr const char* CONTROL_MOUSE_BUTTON = "RM_MOUSE_BUTTON";
constexpr const char* CONTROL_MOUSE_WHEEL = "RM_MOUSE_WHEEL";

constexpr uint16_t DISCOVERY_PORT = 50500;
constexpr uint16_t CONTROL_PORT   = 50510;
constexpr uint16_t VIDEO_PORT     = 50511;

constexpr uint32_t FRAME_MAGIC = 0x524D4652;
constexpr uint32_t FRAME_FORMAT_BGRA = 1;

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t format;
    uint32_t payload_size;
    uint64_t frame_id;
};
#pragma pack(pop)

}