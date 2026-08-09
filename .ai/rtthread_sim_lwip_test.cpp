// newosp lwIP backend behavioral test running on a REAL RT-Thread kernel
// (bsp/simulator host process) with lwIP 2.1.2 initialized by the kernel's
// lwip_system_init. Exercises the loopback netif through newosp's socket_api
// (OSP_NET_BACKEND=1). Linked against the simulator kernel objects; main()
// runs in the RT-Thread main-thread context.
//
// NOTE: lwIP core must be initialized (lwip_system_init, INIT_PREV_EXPORT)
// before this runs. The loopback netif is brought up on the tcpip thread via
// tcpip_callback so the netif lock is respected.

// Ensure RT-Thread core headers fully define rt_uint8_t & friends BEFORE any
// lwIP or newosp network header, to avoid the glibc-errno/rttypes ordering clash
// on host.

#include <rtthread.h>

#include <cstdlib>
#include <cstring>
#include <csignal>

#include "osp/socket.hpp"

// Diagnostic: dump scheduler lock state + every RT-Thread thread's status and
// name on SIGUSR2, so an external timer can inspect a hung simulator process
// (gdb attach is blocked by yama/ptrace on this host).
static void DumpState(int) {
  extern rt_uint16_t rt_critical_level(void);
  rt_kprintf("\n[dump] scheduler lock nest = %u\n", (unsigned)rt_critical_level());
  rt_kprintf("[dump] current thread = %.*s\n", RT_NAME_MAX, rt_thread_self()->parent.name);
  std::exit(3);
}

extern "C" {
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
}

static int g_failures = 0;

static void Check(bool cond, const char* what) {
  if (!cond) {
    rt_kprintf("  FAIL: %s\n", what);
    ++g_failures;
  }
}

// Bring the loopback netif up. Must run on the tcpip thread (core lock held).
// The kernel's lwip_system_init already tcpip_init'd and netif_add'd "lo"; we
// only mark it up here (netif_set_up) so socket traffic can flow.
static void BringUpLoopback(void* /*arg*/) {
  struct netif* n = netif_find("lo");
  if (n != nullptr) {
    netif_set_default(n);
    netif_set_up(n);
    rt_kprintf("  [osp-net] loopback '%s' up\n", n->name);
  } else {
    rt_kprintf("  [osp-net] WARN: no loopback netif\n");
  }
}

int main(void) {
  struct sigaction act;
  std::memset(&act, 0, sizeof(act));
  act.sa_handler = DumpState;
  sigemptyset(&act.sa_mask);
  sigaction(SIGUSR2, &act, RT_NULL);

  rt_kprintf("\n=== newosp lwIP backend test on RT-Thread %d.%d.%d ===\n",
             RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH);

  // Kernel already ran lwip_system_init (INIT_PREV_EXPORT); wait briefly for
  // the tcpip thread to spin up, then ask it to mark loopback up.
  rt_thread_mdelay(100);
  tcpip_callback(BringUpLoopback, nullptr);
  rt_thread_mdelay(50);

  {
    // TCP loopback: connect a client to a local listening socket, pipe a byte.
    int lfd = osp::socket_api::Socket(AF_INET, SOCK_STREAM, 0);
    int cfd = osp::socket_api::Socket(AF_INET, SOCK_STREAM, 0);
    Check(lfd >= 0 && cfd >= 0, "create sockets");

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = osp::socket_api::Htons(18080);
    Check(osp::socket_api::ParseIpv4("127.0.0.1", &addr.sin_addr.s_addr) == 1, "parse loopback ip");

    Check(osp::socket_api::Bind(lfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0,
          "bind listener");
    Check(osp::socket_api::Listen(lfd, 1) == 0, "listen");

    // connect + accept are local/blocking; loopback completes quickly.
    struct sockaddr_in caddr = addr;
    int cstat = osp::socket_api::Connect(cfd, reinterpret_cast<const sockaddr*>(&caddr), sizeof(caddr));
    Check(cstat == 0, "connect to loopback");

    int afd = osp::socket_api::Accept(lfd, nullptr, nullptr);
    Check(afd >= 0, "accept peer");

    if (afd >= 0 && cstat == 0) {
      const char kMsg[] = "PING";
      Check(osp::socket_api::Send(cfd, kMsg, 4, 0) == 4, "send PING");
      char buf[4];
      memset(buf, 0, sizeof(buf));
      Check(osp::socket_api::Recv(afd, buf, 4, 0) == 4, "recv PING");
      Check(memcmp(buf, kMsg, 4) == 0, "payload matches");
    }
    if (afd >= 0) {
      osp::socket_api::Close(afd);
    }
    osp::socket_api::Close(cfd);
    osp::socket_api::Close(lfd);
  }

  if (g_failures == 0) {
    rt_kprintf("=== newosp lwIP backend test: PASS ===\n");
    std::exit(0);
  }
  rt_kprintf("=== newosp lwIP backend test: FAIL (%d) ===\n", g_failures);
  std::exit(1);
}
