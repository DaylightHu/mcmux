/*******************************************************************************************
*
*   mcmux example - NOT gate
*
*   Drives the example input and verifies that the output iron block remains
*   at power 15 for 1 game tick, then changes to 0 exactly at game tick 2.
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/not_gate.mcs
*
*   Example created by DaylightHu (@DaylightHu)
*
********************************************************************************************/
#include "mcmux.h"

static const BlockPos INPUT = {41, 4, 9};
static const BlockPos OUTPUT = {41, 6, 12};
static const BlockPos ANCHOR = {41, 4, 9};

enum { PROPAGATION_DELAY = 2 };

typedef struct {
    uint64_t input_tick;
} TransitionCheck;

static bool valid(const World *world, void *context)
{
    const TransitionCheck *check = context;

    uint64_t elapsed = world_tick(world) - check->input_tick;
    uint8_t expected_power = elapsed < PROPAGATION_DELAY ? 15u : 0u;

    return redstone_solid_power(world, OUTPUT).received == expected_power;
}

int main(void)
{
    World *world = load_mcs("saves/examples/not_gate.mcs", ANCHOR);

    TransitionCheck check = {
        .input_tick = world_tick(world),
    };

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .after_ticks = 0,
        .context = &check,
        .message = "NOT output changed at the wrong game tick",
    });

    world_set_lever_powered(world, INPUT, true);
    sim_advance_ticks(world, PROPAGATION_DELAY);

    writeback_mcs(world,
                  "saves/examples/not_gate.mcs",
                  ANCHOR,
                  "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
