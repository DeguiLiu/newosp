// Copyright (c) 2024 liudegui. MIT License.
//
// messages.hpp -- Message definitions for client gateway demo.

#ifndef OSP_EXAMPLES_CLIENT_GATEWAY_MESSAGES_HPP_
#define OSP_EXAMPLES_CLIENT_GATEWAY_MESSAGES_HPP_

#include <cstdint>
#include <variant>

struct ClientConnect {
  uint32_t client_id;
  char ip[16];
  uint16_t port;
};

struct ClientDisconnect {
  uint32_t client_id;
  uint8_t reason;
};

struct ClientData {
  uint32_t client_id;
  uint16_t data_len;
  uint8_t data[512];
};

struct ClientHeartbeat {
  uint32_t client_id;
  uint32_t rtt_us;
};

struct ProcessResult {
  uint32_t client_id;
  uint8_t status;
  uint32_t processed_bytes;
};

using Payload = std::variant<ClientConnect, ClientDisconnect, ClientData,
                             ClientHeartbeat, ProcessResult>;

#endif  // OSP_EXAMPLES_CLIENT_GATEWAY_MESSAGES_HPP_
