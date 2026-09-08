/**
 * @file test_serial_ota_parser.cpp
 * @brief Tests for examples/serial_ota/parser.hpp (TableHsm frame parser).
 *
 * Regression tests for the static-table migration: hand-built frames verify
 * the happy path, no-payload frames, CRC mismatch, bad tail and resync.
 */

#include "serial_ota/parser.hpp"

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace {

struct FrameLog {
  std::vector<ota::Frame> frames;
  void OnFrame(const ota::Frame& f, void* /*user*/) { frames.push_back(f); }
};

std::vector<uint8_t> BuildFrame(uint8_t cmd_class, uint8_t cmd, const uint8_t* data, uint16_t data_len,
                                bool good_crc = true, uint8_t tail = ota::kFrameTail) {
  const uint16_t len = static_cast<uint16_t>(data_len + 2U);
  uint8_t buf[ota::kMaxPayloadLen + 2U];
  buf[0] = cmd_class;
  buf[1] = cmd;
  for (uint16_t i = 0; i < data_len; ++i) {
    buf[2U + i] = data[i];
  }
  const uint16_t crc = ota::CalcCrc16(buf, len);
  const uint16_t wire_crc = good_crc ? crc : static_cast<uint16_t>(crc ^ 0xFFFFU);

  std::vector<uint8_t> frame;
  frame.push_back(ota::kFrameHeader);
  frame.push_back(static_cast<uint8_t>(len & 0xFFU));
  frame.push_back(static_cast<uint8_t>(len >> 8U));
  frame.push_back(cmd_class);
  frame.push_back(cmd);
  for (uint16_t i = 0; i < data_len; ++i) {
    frame.push_back(data[i]);
  }
  frame.push_back(static_cast<uint8_t>(wire_crc & 0xFFU));
  frame.push_back(static_cast<uint8_t>(wire_crc >> 8U));
  frame.push_back(tail);
  return frame;
}

}  // namespace

TEST_CASE("parser - happy path delivers frame and returns to Idle", "[ota_parser]") {
  FrameLog log;
  ota::FrameParser parser;
  parser.SetCallback([](const ota::Frame& f, void* user) { static_cast<FrameLog*>(user)->OnFrame(f, nullptr); }, &log);
  parser.Start();

  const uint8_t data[4] = {0x11, 0x22, 0x33, 0x44};
  const auto frame = BuildFrame(0x04, 0x02, data, 4);
  parser.PutData(frame.data(), static_cast<uint32_t>(frame.size()));

  REQUIRE(log.frames.size() == 1);
  REQUIRE(log.frames[0].cmd_class == 0x04);
  REQUIRE(log.frames[0].cmd == 0x02);
  REQUIRE(log.frames[0].data_len == 4);
  REQUIRE(std::strcmp(parser.CurrentStateName(), "Idle") == 0);

  const auto& s = parser.GetStats();
  REQUIRE(s.frames_received == 1);
  REQUIRE(s.bytes_received == frame.size());
  REQUIRE(s.sync_errors == 0);
  REQUIRE(s.crc_errors == 0);
  REQUIRE(s.length_errors == 0);
  REQUIRE(s.tail_errors == 0);
}

TEST_CASE("parser - zero-payload frame skips Data state", "[ota_parser]") {
  FrameLog log;
  ota::FrameParser parser;
  parser.SetCallback([](const ota::Frame& f, void* user) { static_cast<FrameLog*>(user)->OnFrame(f, nullptr); }, &log);
  parser.Start();

  const auto frame = BuildFrame(0x04, 0x01, nullptr, 0);
  parser.PutData(frame.data(), static_cast<uint32_t>(frame.size()));

  REQUIRE(log.frames.size() == 1);
  REQUIRE(log.frames[0].data_len == 0);
  REQUIRE(parser.GetStats().frames_received == 1);
}

TEST_CASE("parser - CRC mismatch is counted and frame dropped", "[ota_parser]") {
  FrameLog log;
  ota::FrameParser parser;
  parser.SetCallback([](const ota::Frame& f, void* user) { static_cast<FrameLog*>(user)->OnFrame(f, nullptr); }, &log);
  parser.Start();

  const uint8_t data[2] = {0xAA, 0x55};
  const auto frame = BuildFrame(0x04, 0x02, data, 2, /*good_crc=*/false);
  parser.PutData(frame.data(), static_cast<uint32_t>(frame.size()));

  REQUIRE(log.frames.empty());
  REQUIRE(parser.GetStats().crc_errors == 1);
  REQUIRE(parser.GetStats().frames_received == 0);
  REQUIRE(std::strcmp(parser.CurrentStateName(), "Idle") == 0);
}

TEST_CASE("parser - bad tail byte rejects frame", "[ota_parser]") {
  FrameLog log;
  ota::FrameParser parser;
  parser.SetCallback([](const ota::Frame& f, void* user) { static_cast<FrameLog*>(user)->OnFrame(f, nullptr); }, &log);
  parser.Start();

  const uint8_t data[2] = {0x01, 0x02};
  const auto frame = BuildFrame(0x04, 0x02, data, 2, /*good_crc=*/true, /*tail=*/0x00);
  parser.PutData(frame.data(), static_cast<uint32_t>(frame.size()));

  REQUIRE(log.frames.empty());
  REQUIRE(parser.GetStats().tail_errors == 1);
}

TEST_CASE("parser - non-header noise in Idle counts sync errors then resyncs", "[ota_parser]") {
  FrameLog log;
  ota::FrameParser parser;
  parser.SetCallback([](const ota::Frame& f, void* user) { static_cast<FrameLog*>(user)->OnFrame(f, nullptr); }, &log);
  parser.Start();

  // Two noise bytes before the real frame.
  const uint8_t noise[2] = {0x00, 0xFF};
  parser.PutData(noise, 2);

  const uint8_t data[4] = {0x11, 0x22, 0x33, 0x44};
  const auto frame = BuildFrame(0x04, 0x02, data, 4);
  parser.PutData(frame.data(), static_cast<uint32_t>(frame.size()));

  REQUIRE(parser.GetStats().sync_errors == 2);
  REQUIRE(parser.GetStats().frames_received == 1);
  REQUIRE(log.frames.size() == 1);
}

TEST_CASE("parser - invalid length rejects frame", "[ota_parser]") {
  FrameLog log;
  ota::FrameParser parser;
  parser.SetCallback([](const ota::Frame& f, void* user) { static_cast<FrameLog*>(user)->OnFrame(f, nullptr); }, &log);
  parser.Start();

  // Hand-build len=1 (< 2 overhead): header, LEN_LO=1, LEN_HI=0, ...
  const uint8_t bad[] = {ota::kFrameHeader, 0x01, 0x00, 0x04, 0x02, 0x55};
  parser.PutData(bad, sizeof(bad));

  REQUIRE(parser.GetStats().length_errors >= 1);
  REQUIRE(log.frames.empty());
}