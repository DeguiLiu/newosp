// Copyright 2024 OSP-CPP Authors. All rights reserved.
//
// messages.hpp -- Protocol message definitions for streaming demo.

#ifndef OSP_EXAMPLES_STREAMING_PROTOCOL_MESSAGES_HPP_
#define OSP_EXAMPLES_STREAMING_PROTOCOL_MESSAGES_HPP_

#include <cstdint>
#include <variant>

struct RegisterRequest {
  char device_id[32];
  char ip[16];
  uint16_t port;
};

struct RegisterResponse {
  char device_id[32];
  uint8_t result;       // 0 = success, 1 = rejected
  uint32_t session_id;
};

struct HeartbeatMsg {
  uint32_t session_id;
  uint64_t timestamp_us;
};

struct StreamCommand {
  uint32_t session_id;
  uint8_t action;       // 0 = stop, 1 = start
  uint8_t media_type;   // 0 = video, 1 = audio, 2 = both
};

struct StreamData {
  uint32_t session_id;
  uint32_t seq;
  uint16_t payload_size;
  uint8_t data[256];
};

using Payload = std::variant<RegisterRequest, RegisterResponse,
                             HeartbeatMsg, StreamCommand, StreamData>;

#endif  // OSP_EXAMPLES_STREAMING_PROTOCOL_MESSAGES_HPP_
