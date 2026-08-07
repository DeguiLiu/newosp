/**
 * @file test_lwip_main.cpp
 * @brief Custom Catch2 main that boots lwIP before running backend tests.
 *
 * Built only when OSP_WITH_LWIP is enabled (see tests/CMakeLists.txt).
 */

#define CATCH_CONFIG_RUNNER
#include "lwip_support.hpp"

#include <catch2/catch_all.hpp>

int main(int argc, char* argv[]) {
  osp_lwip::LwipInit();
  return Catch::Session().run(argc, argv);
}
