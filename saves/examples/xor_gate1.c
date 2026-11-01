/*******************************************************************************************
*
*   mcmux example - Compact XOR gate
*
*   Drives the example inputs and verifies that the output iron block remains
*   at power 0 for 1 game tick, then receives power 2 exactly at game tick 2.
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/xor_gate1.mcs
*
*   Example created by DaylightHu (@DaylightHu)
*
********************************************************************************************/
#include "mcmux.h"

static const BlockPos INPUT_A = {31, 4, 9};
static const BlockPos INPUT_B = {28, 4, 9};
static const BlockPos OUTPUT = {30, 3, 13};
static const BlockPos ANCHOR = {31, 4, 10};

enum { PROPAGATION_DELAY = 2, OUTPUT_POWER = 2 };

typedef struct {
    uint64_t input_tick;
} TransitionCheck;

static bool valid(const World *world, void *context)
{
    const TransitionCheck *check = context;

    uint64_t elapsed = world_tick(world) - check->input_tick;
    uint8_t expected_power = elapsed < PROPAGATION_DELAY ? 0u : OUTPUT_POWER;

    return redstone_solid_power(world, OUTPUT).received == expected_power;
}

int main(void)
{
    World *world = load_mcs("saves/examples/xor_gate1.mcs", ANCHOR);

    TransitionCheck check = {
        .input_tick = world_tick(world),
    };

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .after_ticks = 0,
        .context = &check,
        .message = "XOR output changed at the wrong game tick",
    });

    world_set_lever_powered(world, INPUT_A, true);
    world_set_lever_powered(world, INPUT_B, false);

    sim_advance_ticks(world, PROPAGATION_DELAY);

    writeback_mcs(world,
                  "saves/examples/xor_gate1.mcs",
                  ANCHOR,
                  "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
