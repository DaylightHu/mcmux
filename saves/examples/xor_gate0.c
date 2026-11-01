/*******************************************************************************************
*
*   mcmux example - XOR gate
*
*   Drives the example inputs and verifies that the output iron block emits 0
*   toward the lamp for 7 game ticks, then emits 15 exactly at game tick 8.
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/xor_gate0.mcs
*
*   Example created by DaylightHu (@DaylightHu)
*
********************************************************************************************/
#include "mcmux.h"

static const BlockPos INPUT_A = {37, 4, 9};
static const BlockPos INPUT_B = {35, 4, 9};
static const BlockPos OUTPUT = {36, 6, 14};
static const BlockPos ANCHOR = {37, 4, 9};

enum { PROPAGATION_DELAY = 8 };

typedef struct {
    uint64_t input_tick;
} TransitionCheck;

static bool valid(const World *world, void *context)
{
    const TransitionCheck *check = context;

    uint64_t elapsed = world_tick(world) - check->input_tick;
    uint8_t expected_power = elapsed < PROPAGATION_DELAY ? 0u : 15u;

    return redstone_signal(world, OUTPUT, DIR_SOUTH) == expected_power;
}

int main(void)
{
    World *world = load_mcs("saves/examples/xor_gate0.mcs", ANCHOR);

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

    writeback_mcs(world, "saves/examples/xor_gate0.mcs",
                  ANCHOR, "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
