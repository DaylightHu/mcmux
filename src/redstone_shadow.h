#ifndef RS_REDSTONE_SHADOW_H
#define RS_REDSTONE_SHADOW_H

#include "redstone_plan.h"
#include "sim_internal.h"

/* Diagnostic CPU evaluator. It reads the compact plan, while the reference
 * executor remains the sole writer and source of game-tick ordering. */
typedef struct {
    uint64_t validations, links_checked, nodes_checked, nodes_skipped;
    uint64_t rebuilds;
} RedstoneShadowStats;

typedef struct {
    RedstonePlanBounds bounds;
    RedstonePlan plan;
    BlockState *states;
    size_t state_capacity;
    RedstoneShadowStats stats;
    bool dirty;
} RedstoneShadow;

void redstone_shadow_init(RedstoneShadow *shadow);
void redstone_shadow_destroy(RedstoneShadow *shadow);
RsResult redstone_shadow_build(RedstoneShadow *shadow, const World *world,
                               RedstonePlanBounds bounds);
void redstone_shadow_state_changed(RedstoneShadow *shadow, StateChange change);
RsResult redstone_shadow_validate(RedstoneShadow *shadow, World *world);

#endif
