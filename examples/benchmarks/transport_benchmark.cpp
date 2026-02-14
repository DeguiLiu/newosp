// transport_benchmark.cpp -- TCP and SHM transport throughput benchmark
//
// Measures throughput for different payload sizes.
// TCP: loopback, v0 frame (14B header), max frame 2048B
// SHM: ShmRingBuffer SPSC, SlotSize=8192, SlotCount=1024

#include "osp/transport.hpp"
#include "osp/shm_transport.hpp"
#include "osp/platform.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(OSP_PLATFORM_LINUX)
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

using Clock = std::chrono::high_resolution_clock;

// -- SHM Benchmark -----------------------------------------------------------

struct ShmBenchResult {
  uint32_t payload_size;
  uint32_t count;
  uint32_t pushed;
  uint32_t popped;
  double elapsed_ms;
  double throughput_mops;  // million ops/s
  double throughput_mbps;  // megabytes/s (payload only)
};

template <uint32_t SlotSize, uint32_t SlotCount>
static ShmBenchResult RunShmBench(uint32_t payload_size, uint32_t count) {
  ShmBenchResult result{};
  result.payload_size = payload_size;
  result.count = count;

  using Ring = osp::ShmRingBuffer<SlotSize, SlotCount>;
  std::vector<uint8_t> mem(sizeof(Ring), 0);
  auto* ring = Ring::InitAt(mem.data());

  std::vector<uint8_t> payload(payload_size, 0xAB);
  std::vector<uint8_t> recv_buf(SlotSize, 0);

  uint32_t push_ok = 0;
  uint32_t pop_ok = 0;

  auto t0 = Clock::now();

  for (uint32_t i = 0; i < count; ++i) {
    if (ring->TryPush(payload.data(), payload_size)) {
      ++push_ok;
    }
    uint32_t recv_size = 0;
    if (ring->TryPop(recv_buf.data(), recv_size)) {
      ++pop_ok;
    }
  }

  auto t1 = Clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  result.pushed = push_ok;
  result.popped = pop_ok;
  result.elapsed_ms = elapsed_ms;
  result.throughput_mops =
      (elapsed_ms > 0) ? (pop_ok / (elapsed_ms / 1000.0) / 1e6) : 0;
  result.throughput_mbps =
      (elapsed_ms > 0)
          ? (static_cast<double>(pop_ok) * payload_size / 1048576.0 /
             (elapsed_ms / 1000.0))
          : 0;

  return result;
}

// -- TCP Benchmark -----------------------------------------------------------

#if defined(OSP_PLATFORM_LINUX)

struct TcpBenchResult {
  uint32_t payload_size;
  uint32_t count;
  uint32_t sent;
  uint32_t received;
  double elapsed_ms;
  double throughput_fps;   // frames per second
  double throughput_mbps;  // megabytes/s (payload only)
};

static TcpBenchResult RunTcpBench(uint32_t payload_size, uint32_t count) {
  TcpBenchResult result{};
  result.payload_size = payload_size;
  result.count = count;

  // Create listener
  auto listener_r = osp::TcpListener::Create();
  if (!listener_r.has_value()) {
    printf("  [ERROR] Failed to create listener\n");
    return result;
  }
  auto listener = static_cast<osp::TcpListener&&>(listener_r.value());

  auto addr_r = osp::SocketAddress::FromIpv4("127.0.0.1", 0);
  if (!addr_r.has_value()) {
    printf("  [ERROR] Failed to create address\n");
    return result;
  }

  if (!listener.Bind(addr_r.value()).has_value()) {
    printf("  [ERROR] Failed to bind\n");
    return result;
  }

  if (!listener.Listen(1).has_value()) {
    printf("  [ERROR] Failed to listen\n");
    return result;
  }

  // Get actual port via getsockname
  struct sockaddr_in sin{};
  socklen_t sin_len = sizeof(sin);
  ::getsockname(listener.Fd(), reinterpret_cast<struct sockaddr*>(&sin), &sin_len);
  uint16_t port = ntohs(sin.sin_port);

  // Receiver thread
  std::atomic<uint32_t> rx_count{0};
  std::atomic<bool> rx_ready{false};

  std::thread rx_thread([&]() {
    auto accept_r = listener.Accept();
    if (!accept_r.has_value()) return;

    osp::TcpTransport rx_transport;
    rx_transport.AcceptFrom(static_cast<osp::TcpSocket&&>(accept_r.value()));
    rx_ready.store(true, std::memory_order_release);

    uint8_t payload_buf[OSP_TRANSPORT_MAX_FRAME_SIZE];
    osp::FrameHeaderV1 hdr{};

    while (rx_count.load(std::memory_order_relaxed) < count) {
      auto r = rx_transport.RecvFrameAuto(hdr, payload_buf, sizeof(payload_buf));
      if (r.has_value()) {
        rx_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        break;
      }
    }
  });

  // Connect sender
  osp::TcpTransport tx_transport;
  osp::Endpoint ep;
  ep.host.assign(osp::TruncateToCapacity, "127.0.0.1");
  ep.port = port;

  auto conn_r = tx_transport.Connect(ep);
  if (!conn_r.has_value()) {
    printf("  [ERROR] Failed to connect\n");
    rx_thread.join();
    return result;
  }

  // Wait for receiver ready
  while (!rx_ready.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  std::vector<uint8_t> payload(payload_size, 0xAB);

  auto t0 = Clock::now();
  uint32_t tx_ok = 0;

  for (uint32_t i = 0; i < count; ++i) {
    auto r = tx_transport.SendFrame(1, 100, payload.data(), payload_size);
    if (r.has_value()) {
      ++tx_ok;
    }
  }

  // Wait for receiver
  for (int drain = 0; drain < 5000; ++drain) {
    if (rx_count.load(std::memory_order_relaxed) >= tx_ok) break;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  auto t1 = Clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  uint32_t rx = rx_count.load();

  result.sent = tx_ok;
  result.received = rx;
  result.elapsed_ms = elapsed_ms;
  result.throughput_fps =
      (elapsed_ms > 0) ? (rx / (elapsed_ms / 1000.0)) : 0;
  result.throughput_mbps =
      (elapsed_ms > 0)
          ? (static_cast<double>(rx) * payload_size / 1048576.0 /
             (elapsed_ms / 1000.0))
          : 0;

  rx_thread.join();
  return result;
}

#endif  // OSP_PLATFORM_LINUX

// -- Main --------------------------------------------------------------------

int main() {
  printf("newosp Transport Benchmark\n");
  printf("==========================\n\n");

  // SHM: SlotSize=8192 supports up to 8K payload
  static constexpr uint32_t kShmPayloads[] = {64, 256, 512, 1024, 4096};
  static constexpr uint32_t kShmCount = 1000000;

  printf("=== ShmRingBuffer SPSC (%u ops, SlotSize=8192, SlotCount=1024) ===\n",
         kShmCount);
  printf("  %-10s  %10s  %10s  %10s  %10s  %10s\n",
         "Payload", "Pushed", "Popped", "Time(ms)", "M ops/s", "MB/s");
  printf("  -------------------------------------------------------------------\n");

  for (uint32_t sz : kShmPayloads) {
    auto r = RunShmBench<8192, 1024>(sz, kShmCount);
    printf("  %-10u  %10u  %10u  %10.1f  %10.2f  %10.1f\n",
           r.payload_size, r.pushed, r.popped, r.elapsed_ms,
           r.throughput_mops, r.throughput_mbps);
  }

#if defined(OSP_PLATFORM_LINUX)
  // TCP: max frame 2048, so payload up to ~2034 (2048 - 14B header)
  static constexpr uint32_t kTcpPayloads[] = {64, 256, 512, 1024};
  static constexpr uint32_t kTcpCount = 100000;

  printf("\n=== TCP Loopback (%u frames, v0 header %uB, max frame %uB) ===\n",
         kTcpCount, osp::FrameCodec::kHeaderSize, OSP_TRANSPORT_MAX_FRAME_SIZE);
  printf("  %-10s  %10s  %10s  %10s  %10s  %10s\n",
         "Payload", "Sent", "Recv", "Time(ms)", "FPS", "MB/s");
  printf("  -------------------------------------------------------------------\n");

  for (uint32_t sz : kTcpPayloads) {
    auto r = RunTcpBench(sz, kTcpCount);
    printf("  %-10u  %10u  %10u  %10.1f  %10.0f  %10.1f\n",
           r.payload_size, r.sent, r.received, r.elapsed_ms,
           r.throughput_fps, r.throughput_mbps);
  }
#endif

  printf("\nDone.\n");
  return 0;
}
