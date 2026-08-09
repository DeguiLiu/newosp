// newosp POSIX-socket backend behavioral test running on the REAL RT-Thread
// 5.2.1 bsp/simulator kernel (a host process). OSP_NET_BACKEND=0 routes
// socket_api through the host Linux socket stack (::socket/bind/connect/...),
// single-threaded blocking semantics -- no lwIP multithread contention. This
// exercises a TCP loopback through newosp's socket layer entirely inside the
// RT-Thread main-thread context.
//
// Linked against the simulator kernel objects; main() runs in the RT-Thread
// main-thread context.
#include <rtthread.h>

#include <cstdlib>
#include <cstring>

#include "osp/socket.hpp"

extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
}

static int g_failures = 0;

static void Check(bool cond, const char* what) {
  if (!cond) {
    rt_kprintf("  FAIL: %s\n", what);
    ++g_failures;
  }
}

int main(void) {
  rt_kprintf("\n=== newosp POSIX-socket test on RT-Thread %d.%d.%d ===\n",
             RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH);

  {
    // TCP loopback via the host kernel (127.0.0.1): connect a client to a
    // locally listening socket, pipe one message.
    int lfd = osp::socket_api::Socket(AF_INET, SOCK_STREAM, 0);
    int cfd = osp::socket_api::Socket(AF_INET, SOCK_STREAM, 0);
    Check(lfd >= 0 && cfd >= 0, "create sockets");

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = osp::socket_api::Htons(18080);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(osp::socket_api::ParseIpv4("127.0.0.1", &addr.sin_addr.s_addr) == 1,
          "parse loopback ip");

    int opt = 1;
    osp::socket_api::SetSockOpt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt,
                                sizeof(opt));
    Check(osp::socket_api::Bind(lfd, reinterpret_cast<const sockaddr*>(&addr),
                                sizeof(addr)) == 0, "bind listener");
    Check(osp::socket_api::Listen(lfd, 1) == 0, "listen");

    struct sockaddr_in caddr = addr;
    int cstat = osp::socket_api::Connect(
        cfd, reinterpret_cast<const sockaddr*>(&caddr), sizeof(caddr));
    Check(cstat == 0, "connect to loopback");

    int afd = osp::socket_api::Accept(lfd, nullptr, nullptr);
    Check(afd >= 0, "accept peer");

    if (afd >= 0 && cstat == 0) {
      const char kMsg[] = "PING";
      Check(osp::socket_api::Send(cfd, kMsg, 4, 0) == 4, "send PING");
      char buf[4];
      std::memset(buf, 0, sizeof(buf));
      Check(osp::socket_api::Recv(afd, buf, 4, 0) == 4, "recv PING");
      Check(std::memcmp(buf, kMsg, 4) == 0, "payload matches");
    }
    if (afd >= 0) {
      osp::socket_api::Close(afd);
    }
    osp::socket_api::Close(cfd);
    osp::socket_api::Close(lfd);
  }

  if (g_failures == 0) {
    rt_kprintf("=== newosp POSIX-socket test: PASS ===\n");
    std::exit(0);
  }
  rt_kprintf("=== newosp POSIX-socket test: FAIL (%d) ===\n", g_failures);
  std::exit(1);
}
