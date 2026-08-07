/**
 * @file lwip_support.hpp
 * @brief One-time lwIP (unixsim) bootstrap: tcpip_thread + loopback netif.
 * Lets osp network tests run over the real lwIP stack without root. Built only
 * when OSP_WITH_LWIP is enabled.
 */

#ifndef OSP_TEST_LWIP_SUPPORT_HPP_
#define OSP_TEST_LWIP_SUPPORT_HPP_

extern "C" {
#include "lwip/err.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
}

#include <atomic>
#include <thread>

namespace osp_lwip {

inline struct netif g_loop_netif;
inline ip4_addr_t g_loop_ipaddr;
inline ip4_addr_t g_loop_netmask;
inline ip4_addr_t g_loop_gw;
inline std::atomic<bool> g_netif_ready{false};

// Minimal loopback netif driver (lwIP 2.2.0 no longer ships loopif.c).
inline err_t LoopIfOutput(struct netif* netif, struct pbuf* p, const ip4_addr_t* ipaddr) {
  (void)ipaddr;
  if (netif->input == nullptr) {
    return ERR_IF;
  }
  pbuf_ref(p);
  err_t err = netif->input(p, netif);
  if (err != ERR_OK) {
    pbuf_free(p);
  }
  return ERR_OK;
}

inline err_t LoopIfInit(struct netif* netif) {
  netif->name[0] = 'l';
  netif->name[1] = 'o';
  netif->output = LoopIfOutput;
  netif->mtu = 1500;
  netif->hwaddr_len = 0;
  netif->flags = NETIF_FLAG_LINK_UP;
  return ERR_OK;
}

inline void NetifSetup(void* arg) {
  (void)arg;
  IP4_ADDR(&g_loop_ipaddr, 127, 0, 0, 1);
  IP4_ADDR(&g_loop_netmask, 255, 0, 0, 0);
  IP4_ADDR(&g_loop_gw, 127, 0, 0, 1);
  netif_add(&g_loop_netif, &g_loop_ipaddr, &g_loop_netmask, &g_loop_gw, nullptr, LoopIfInit, tcpip_input);
  netif_set_default(&g_loop_netif);
  netif_set_up(&g_loop_netif);
  g_netif_ready.store(true, std::memory_order_release);
}

/** @brief One-time lwIP initialization; call before running any test. */
inline void LwipInit() {
  static bool inited = false;
  if (inited) {
    return;
  }
  tcpip_init(nullptr, nullptr);
  // netif_add must run on the tcpip_thread (core lock is held there).
  tcpip_callback(NetifSetup, nullptr);
  while (!g_netif_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  inited = true;
}

}  // namespace osp_lwip

#endif  // OSP_TEST_LWIP_SUPPORT_HPP_
