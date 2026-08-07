/**
 * @file test_lwip_main.cpp
 * @brief Custom Catch2 main booting lwIP before backend tests.
 * Built only when OSP_WITH_LWIP is enabled.
 */

#define CATCH_CONFIG_RUNNER
#include "lwip_support.hpp"

#include <catch2/catch_all.hpp>

int main(int argc, char* argv[]) {
  osp_lwip::LwipInit();
  return Catch::Session().run(argc, argv);
}
