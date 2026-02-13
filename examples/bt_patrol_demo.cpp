/**
 * @file bt_patrol_demo.cpp
 * @brief Patrol robot behavior tree demonstration using osp::BehaviorTree.
 *
 * Demonstrates a patrol robot with emergency handling and battery management:
 * - Patrols waypoints while battery is sufficient
 * - Handles emergency situations with immediate stop
 * - Returns to base when battery is low
 */

#include "osp/bt.hpp"

#include <cstdio>

// ============================================================================
// PatrolContext - Robot state
// ============================================================================

struct PatrolContext {
  int battery_level = 100;
  int current_waypoint = 0;
  int total_waypoints = 5;
  bool emergency = false;
  int patrol_count = 0;
  bool at_base = false;
  bool emergency_triggered = false;  // Track if emergency was already simulated
};

// ============================================================================
// Condition nodes
// ============================================================================

osp::NodeStatus IsEmergency(PatrolContext& ctx) {
  if (ctx.emergency) {
    printf("  [CONDITION] IsEmergency: TRUE\n");
    return osp::NodeStatus::kSuccess;
  }
  return osp::NodeStatus::kFailure;
}

osp::NodeStatus HasBattery(PatrolContext& ctx) {
  if (ctx.battery_level > 20) {
    printf("  [CONDITION] HasBattery: TRUE (battery=%d%%)\n", ctx.battery_level);
    return osp::NodeStatus::kSuccess;
  }
  printf("  [CONDITION] HasBattery: FALSE (battery=%d%%)\n", ctx.battery_level);
  return osp::NodeStatus::kFailure;
}

osp::NodeStatus IsBatteryLow(PatrolContext& ctx) {
  if (ctx.battery_level <= 20) {
    printf("  [CONDITION] IsBatteryLow: TRUE (battery=%d%%)\n", ctx.battery_level);
    return osp::NodeStatus::kSuccess;
  }
  return osp::NodeStatus::kFailure;
}

// ============================================================================
// Action nodes
// ============================================================================

osp::NodeStatus EmergencyStop(PatrolContext& ctx) {
  printf("  [ACTION] EmergencyStop: STOPPING ALL OPERATIONS!\n");
  ctx.emergency = false;  // Clear emergency after handling
  return osp::NodeStatus::kSuccess;
}

osp::NodeStatus MoveToWaypoint(PatrolContext& ctx) {
  printf("  [ACTION] MoveToWaypoint: Moving to waypoint %d/%d\n",
         ctx.current_waypoint + 1, ctx.total_waypoints);
  ctx.current_waypoint = (ctx.current_waypoint + 1) % ctx.total_waypoints;
  return osp::NodeStatus::kSuccess;
}

osp::NodeStatus ScanArea(PatrolContext& ctx) {
  printf("  [ACTION] ScanArea: Scanning surroundings...\n");
  return osp::NodeStatus::kSuccess;
}

osp::NodeStatus ReportClear(PatrolContext& ctx) {
  printf("  [ACTION] ReportClear: Area clear, continuing patrol\n");
  ctx.patrol_count++;
  ctx.battery_level -= 15;  // Consume battery during patrol
  if (ctx.battery_level < 0) {
    ctx.battery_level = 0;
  }
  return osp::NodeStatus::kSuccess;
}

osp::NodeStatus NavigateToBase(PatrolContext& ctx) {
  printf("  [ACTION] NavigateToBase: Returning to base for recharge\n");
  ctx.at_base = true;
  return osp::NodeStatus::kSuccess;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  printf("=== Patrol Robot Behavior Tree Demo ===\n\n");

  // Initialize context
  PatrolContext ctx;

  // Build behavior tree
  osp::BehaviorTree<PatrolContext> tree(ctx, "patrol_robot");

  // Root: Selector (try emergency, patrol, or return to base)
  int32_t root = tree.AddSelector("Root");

  // Branch 1: HandleEmergency (Sequence)
  int32_t handle_emergency = tree.AddSequence("HandleEmergency", root);
  tree.AddCondition("IsEmergency", IsEmergency, handle_emergency);
  tree.AddAction("EmergencyStop", EmergencyStop, handle_emergency);

  // Branch 2: PatrolRoute (Sequence)
  int32_t patrol_route = tree.AddSequence("PatrolRoute", root);
  tree.AddCondition("HasBattery", HasBattery, patrol_route);
  tree.AddAction("MoveToWaypoint", MoveToWaypoint, patrol_route);
  tree.AddAction("ScanArea", ScanArea, patrol_route);
  tree.AddAction("ReportClear", ReportClear, patrol_route);

  // Branch 3: ReturnToBase (Sequence)
  int32_t return_to_base = tree.AddSequence("ReturnToBase", root);
  tree.AddCondition("IsBatteryLow", IsBatteryLow, return_to_base);
  tree.AddAction("NavigateToBase", NavigateToBase, return_to_base);

  tree.SetRoot(root);

  printf("Behavior tree built with %u nodes\n\n", tree.NodeCount());

  // Main loop: tick the tree repeatedly
  int tick_count = 0;
  const int max_ticks = 20;

  while (tick_count < max_ticks && !ctx.at_base) {
    printf("--- Tick %d (Battery: %d%%, Waypoint: %d, Patrols: %d) ---\n",
           tick_count + 1, ctx.battery_level, ctx.current_waypoint,
           ctx.patrol_count);

    // Simulate emergency after completing 2 patrols (only once)
    if (ctx.patrol_count == 2 && !ctx.emergency_triggered) {
      printf("  [SIMULATION] Emergency detected!\n");
      ctx.emergency = true;
      ctx.emergency_triggered = true;
    }

    osp::NodeStatus status = tree.Tick();

    printf("  Result: %s\n\n", osp::NodeStatusToString(status));

    tick_count++;

    // Stop if at base
    if (ctx.at_base) {
      printf("=== Robot reached base. Mission complete. ===\n");
      break;
    }

    // Safety check
    if (ctx.battery_level == 0 && !ctx.at_base) {
      printf("=== Battery depleted! Robot stranded. ===\n");
      break;
    }
  }

  if (tick_count >= max_ticks) {
    printf("=== Max ticks reached. Simulation ended. ===\n");
  }

  printf("\nFinal state:\n");
  printf("  Battery: %d%%\n", ctx.battery_level);
  printf("  Patrols completed: %d\n", ctx.patrol_count);
  printf("  At base: %s\n", ctx.at_base ? "YES" : "NO");

  return 0;
}
