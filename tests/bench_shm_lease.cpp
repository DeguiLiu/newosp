// Lease vs copy path benchmark for the SPMC byte channel.
// Not a CTest target; run manually: ./bench_shm_lease
#include "osp/data_dispatcher.hpp"
#include "osp/shm_transport.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

#include <unistd.h>

namespace {

constexpr uint32_t kPayload = 1024U * 1024U;
constexpr int kIterations = 2000;

using LeasePool = osp::ShmDispatcher<kPayload, 8>;

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

int main() {
  std::vector<uint8_t> payload(kPayload, 0x5AU);
  std::vector<uint8_t> sink(kPayload);

  // Copy path: one write + two 1 MiB reads through the ring.
  auto ring_seg = osp::SharedMemorySegment::ForceReplace("bench_shm_lease_ring",
                                                         osp::ShmSpmcByteRing::RequiredSize(2U * kPayload));
  if (!ring_seg.has_value()) {
    std::printf("copy path segment failed\n");
    return 1;
  }
  auto ring = osp::ShmSpmcByteRing::InitAt(ring_seg.value().Data(), ring_seg.value().Size(), 2U);
  auto c0 = ring.RegisterConsumer(osp::ShmChannelConfig{});
  auto c1 = ring.RegisterConsumer(osp::ShmChannelConfig{});
  if (!c0.has_value() || !c1.has_value()) {
    std::printf("copy path registration failed\n");
    return 1;
  }
  const uint64_t copy_start = NowNs();
  for (int i = 0; i < kIterations; ++i) {
    if (!ring.Write(payload.data(), kPayload)) {
      std::printf("copy path write failed at %d\n", i);
      return 1;
    }
    (void)ring.ReadChecked(c0.value(), sink.data(), kPayload);
    (void)ring.ReadChecked(c1.value(), sink.data(), kPayload);
  }
  const uint64_t copy_ns = NowNs() - copy_start;

  // Lease path: one 1 MiB copy into the pool, two generation-checked leases.
  auto seg = osp::SharedMemorySegment::ForceReplace("bench_shm_lease_pool",
                                                    osp::detail::ShmStore<kPayload, 8>::RequiredShmSize());
  if (!seg.has_value()) {
    std::printf("lease pool segment failed\n");
    return 1;
  }
  static LeasePool pool;
  pool.Init(LeasePool::Config{}, seg.value().Data(), seg.value().Size());
  const int32_t s0 = pool.RegisterConsumer(static_cast<uint32_t>(::getpid()));
  const int32_t s1 = pool.RegisterConsumer(static_cast<uint32_t>(::getpid()));
  if (s0 < 0 || s1 < 0) {
    std::printf("lease pool registration failed\n");
    return 1;
  }
  const int32_t slots[2] = {s0, s1};

  auto wr = osp::ShmSpmcByteChannel::ForceReplaceWriter("bench_shm_lease_channel", 64U * 1024U, 2U);
  auto rd0 = osp::ShmSpmcByteChannel::OpenReader("bench_shm_lease_channel");
  auto rd1 = osp::ShmSpmcByteChannel::OpenReader("bench_shm_lease_channel");
  if (!wr.has_value() || !rd0.has_value() || !rd1.has_value()) {
    std::printf("lease channel setup failed\n");
    return 1;
  }
  auto writer = static_cast<osp::ShmSpmcByteChannel&&>(wr.value());
  auto reader0 = static_cast<osp::ShmSpmcByteChannel&&>(rd0.value());
  auto reader1 = static_cast<osp::ShmSpmcByteChannel&&>(rd1.value());
  osp::ShmLeaseProducer<LeasePool> producer(writer, pool, 0U);
  osp::ShmLeaseConsumer<LeasePool> consumer0(reader0, pool, s0);
  osp::ShmLeaseConsumer<LeasePool> consumer1(reader1, pool, s1);

  const uint64_t lease_start = NowNs();
  for (int i = 0; i < kIterations; ++i) {
    if (!producer.Publish(payload.data(), kPayload, slots, 2U).has_value()) {
      std::printf("lease publish failed at %d\n", i);
      return 1;
    }
    auto lease0 = consumer0.ReadLease(100U);
    auto lease1 = consumer1.ReadLease(100U);
    if (!lease0.has_value() || !lease1.has_value()) {
      std::printf("lease read failed at %d\n", i);
      return 1;
    }
    if (lease0.value().Data()[0] != 0x5AU || lease1.value().Data()[kPayload - 1U] != 0x5AU) {
      std::printf("lease data mismatch at %d\n", i);
      return 1;
    }
  }
  const uint64_t lease_ns = NowNs() - lease_start;

  const double copy_us = static_cast<double>(copy_ns) / (1000.0 * kIterations);
  const double lease_us = static_cast<double>(lease_ns) / (1000.0 * kIterations);
  std::printf("payload=%u bytes, iterations=%d, consumers=2\n", kPayload, kIterations);
  std::printf("copy  path: %.2f us/iter (%.1f MiB/s per consumer)\n", copy_us,
              2.0 * kPayload / (copy_us * 1.048576));
  std::printf("lease path: %.2f us/iter (%.1f MiB/s per consumer)\n", lease_us,
              2.0 * kPayload / (lease_us * 1.048576));
  std::printf("ratio lease/copy: %.2f\n", lease_us / copy_us);

  writer.Unlink();
  seg.value().Unlink();
  ring_seg.value().Unlink();
  return 0;
}
