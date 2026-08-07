/**
 * @file lwip_igmp_override.hpp
 * @brief Force LWIP_IGMP=1 for the module gate; unixsim lwipopts.h sets 0.
 * The design doc declares IGMP a target requirement. Include before osp headers.
 */

#ifndef OSP_TEST_LWIP_IGMP_OVERRIDE_HPP_
#define OSP_TEST_LWIP_IGMP_OVERRIDE_HPP_

#include "lwip/opt.h"

#undef LWIP_IGMP
#define LWIP_IGMP 1

#endif  // OSP_TEST_LWIP_IGMP_OVERRIDE_HPP_
