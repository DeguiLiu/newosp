/**
 * @file unix_socket_benchmark.cpp
 * @brief Unix Domain Socket vs TCP loopback throughput benchmark.
 *
 * Measures single-threaded ping-pong throughput for both Unix domain socket
 * and TCP 127.0.0.1, printing messages/sec and MB/s for comparison.
 *
 * Build:
 *   cmake --build build --target unix_socket_benchmark
 * Run:
 *   ./build/examples/unix_socket_benchmark [msg_size] [iterations]
 *   Default: msg_size=4096, iterations=100000
 */

#include "osp/socket.hpp"
#include "osp/timer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static constexpr uint32_t kDefaultMsgSize = 4096;
static constexpr uint32_t kDefaultIterations = 100000;

// ============================================================================
// Helpers
// ============================================================================

/// Send exactly `len` bytes (handles partial writes).
static bool SendAll(int32_t fd, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  size_t sent = 0;
  while (sent < len) {
    auto n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

/// Recv exactly `len` bytes.
static bool RecvAll(int32_t fd, void* buf, size_t len) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < len) {
    auto n = ::recv(fd, p + got, len - got, 0);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

struct BenchResult {
  double elapsed_ms;
  double msg_per_sec;
  double mb_per_sec;
};

// ============================================================================
// Unix Domain Socket Benchmark
// ============================================================================

static BenchResult BenchUnix(uint32_t msg_size, uint32_t iterations) {
  const char* path = "/tmp/osp_bench_unix.sock";
  ::unlink(path);

  auto addr_r = osp::UnixAddress::FromPath(path);
  auto listener_r = osp::UnixListener::Create();
  auto& listener = listener_r.value();
  listener.Bind(addr_r.value());
  listener.Listen();

  // Echo server thread
  std::thread server([&]() {
    auto conn_r = listener.Accept();
    auto& conn = conn_r.value();
    auto* buf = static_cast<uint8_t*>(std::malloc(msg_size));
    for (uint32_t i = 0; i < iterations; ++i) {
      RecvAll(conn.Fd(), buf, msg_size);
      SendAll(conn.Fd(), buf, msg_size);
    }
    std::free(buf);
  });

  // Client: connect and ping-pong
  auto client_r = osp::UnixSocket::Create();
  auto& client = client_r.value();
  client.Connect(addr_r.value());

  auto* send_buf = static_cast<uint8_t*>(std::malloc(msg_size));
  auto* recv_buf = static_cast<uint8_t*>(std::malloc(msg_size));
  std::memset(send_buf, 0xAB, msg_size);

  uint64_t start = osp::SteadyNowUs();

  for (uint32_t i = 0; i < iterations; ++i) {
    SendAll(client.Fd(), send_buf, msg_size);
    RecvAll(client.Fd(), recv_buf, msg_size);
  }

  uint64_t end = osp::SteadyNowUs();
  double elapsed_ms = static_cast<double>(end - start) / 1000.0;

  server.join();
  std::free(send_buf);
  std::free(recv_buf);
  ::unlink(path);

  double msg_per_sec = static_cast<double>(iterations) / (elapsed_ms / 1000.0);
  // Each iteration = 1 send + 1 recv = 2 * msg_size bytes transferred
  double total_bytes = static_cast<double>(iterations) * 2.0 *
                       static_cast<double>(msg_size);
  double mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);

  return {elapsed_ms, msg_per_sec, mb_per_sec};
}

// ============================================================================
// TCP Loopback Benchmark
// ============================================================================

static BenchResult BenchTcp(uint32_t msg_size, uint32_t iterations) {
  auto addr_r = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  auto listener_r = osp::TcpListener::Create();
  auto& listener = listener_r.value();

  // Bind to port 0 (OS picks a free port)
  osp::SocketAddress bind_addr;
  auto ba = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  listener.Bind(ba.value());
  listener.Listen();

  // Get the actual port
  struct sockaddr_in sin {};
  socklen_t slen = sizeof(sin);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  ::getsockname(listener.Fd(), reinterpret_cast<sockaddr*>(&sin), &slen);
  uint16_t port = ntohs(sin.sin_port);

  auto connect_addr = osp::SocketAddress::FromIpv4("127.0.0.1", port);

  // Echo server thread
  std::thread server([&]() {
    auto conn_r = listener.Accept();
    auto& conn = conn_r.value();
    auto* buf = static_cast<uint8_t*>(std::malloc(msg_size));
    for (uint32_t i = 0; i < iterations; ++i) {
      RecvAll(conn.Fd(), buf, msg_size);
      SendAll(conn.Fd(), buf, msg_size);
    }
    std::free(buf);
  });

  // Client
  auto client_r = osp::TcpSocket::Create();
  auto& client = client_r.value();
  client.SetNoDelay(true);
  client.Connect(connect_addr.value());

  auto* send_buf = static_cast<uint8_t*>(std::malloc(msg_size));
  auto* recv_buf = static_cast<uint8_t*>(std::malloc(msg_size));
  std::memset(send_buf, 0xCD, msg_size);

  uint64_t start = osp::SteadyNowUs();

  for (uint32_t i = 0; i < iterations; ++i) {
    SendAll(client.Fd(), send_buf, msg_size);
    RecvAll(client.Fd(), recv_buf, msg_size);
  }

  uint64_t end = osp::SteadyNowUs();
  double elapsed_ms = static_cast<double>(end - start) / 1000.0;

  server.join();
  std::free(send_buf);
  std::free(recv_buf);

  double msg_per_sec = static_cast<double>(iterations) / (elapsed_ms / 1000.0);
  double total_bytes = static_cast<double>(iterations) * 2.0 *
                       static_cast<double>(msg_size);
  double mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);

  return {elapsed_ms, msg_per_sec, mb_per_sec};
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  uint32_t msg_size = kDefaultMsgSize;
  uint32_t iterations = kDefaultIterations;

  if (argc > 1) msg_size = static_cast<uint32_t>(std::atoi(argv[1]));
  if (argc > 2) iterations = static_cast<uint32_t>(std::atoi(argv[2]));

  std::printf("=== Unix Domain Socket vs TCP Loopback Benchmark ===\n");
  std::printf("  msg_size:   %u bytes\n", msg_size);
  std::printf("  iterations: %u round-trips\n\n", iterations);

  // Warmup
  std::printf("Warming up...\n");
  BenchUnix(msg_size, 1000);
  BenchTcp(msg_size, 1000);

  // Unix benchmark
  std::printf("Running Unix Domain Socket benchmark...\n");
  auto unix_r = BenchUnix(msg_size, iterations);
  std::printf("  [Unix]  %.1f ms  |  %.0f msg/s  |  %.1f MB/s\n\n",
              unix_r.elapsed_ms, unix_r.msg_per_sec, unix_r.mb_per_sec);

  // TCP benchmark
  std::printf("Running TCP loopback benchmark...\n");
  auto tcp_r = BenchTcp(msg_size, iterations);
  std::printf("  [TCP]   %.1f ms  |  %.0f msg/s  |  %.1f MB/s\n\n",
              tcp_r.elapsed_ms, tcp_r.msg_per_sec, tcp_r.mb_per_sec);

  // Summary
  std::printf("--- Summary ---\n");
  double speedup = tcp_r.elapsed_ms / unix_r.elapsed_ms;
  std::printf("  Unix is %.2fx %s than TCP loopback\n",
              speedup > 1.0 ? speedup : 1.0 / speedup,
              speedup > 1.0 ? "faster" : "slower");

  // Multi-size sweep
  std::printf("\n=== Message Size Sweep (10000 round-trips each) ===\n");
  std::printf("  %-12s  %-18s  %-18s  %s\n",
              "msg_size", "Unix (MB/s)", "TCP (MB/s)", "Speedup");
  std::printf("  %-12s  %-18s  %-18s  %s\n",
              "--------", "-----------", "----------", "-------");

  static const uint32_t sizes[] = {64, 256, 1024, 4096, 16384, 65536};
  for (auto sz : sizes) {
    auto u = BenchUnix(sz, 10000);
    auto t = BenchTcp(sz, 10000);
    double sp = t.elapsed_ms / u.elapsed_ms;
    std::printf("  %-12u  %-18.1f  %-18.1f  %.2fx\n",
                sz, u.mb_per_sec, t.mb_per_sec, sp);
  }

  return 0;
}
