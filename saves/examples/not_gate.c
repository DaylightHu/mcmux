/*******************************************************************************************
*
*   mcmux example - NOT gate
*
*   Drives the example input, derives NOT from the resulting port signal, and
*   verifies that the output changes exactly at game tick 2, not earlier.
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/not_gate.mcs
*
*   Example created by DaylightHu (@DaylightHu)
*
********************************************************************************************/
#include "mcmux.h"

static const BlockPos INPUT = {41, 4, 9};
static const BlockPos PORT = {41, 4, 10};
static const BlockPos OUTPUT = {41, 6, 12};
static const BlockPos ANCHOR = {41, 4, 9};

enum { PROPAGATION_DELAY = 2 };

typedef struct {
    uint64_t input_tick;
    uint8_t previous_power;
    uint8_t expected_power;
} TransitionCheck;

static bool port_powered(const World *world)
{
    return redstone_signal(world, PORT, DIR_UP) > 0;
}

static uint8_t not_power(bool input)
{
    return input ? 0u : 15u;
}

static uint8_t output_power(const World *world)
{
    return redstone_solid_power(world, OUTPUT).received;
}

static bool valid(const World *world, void *context)
{
    const TransitionCheck *check = context;

    uint64_t elapsed = world_tick(world) - check->input_tick;
    uint8_t expected_power = elapsed < PROPAGATION_DELAY
                                 ? check->previous_power
                                 : check->expected_power;

    return output_power(world) == expected_power;
}

int main(void)
{
    World *world = load_mcs("saves/examples/not_gate.mcs", ANCHOR);

    uint64_t input_tick = world_tick(world);
    uint8_t previous_power = output_power(world);

    world_set_lever_powered(world, INPUT, false);

    TransitionCheck check = {
        .input_tick = input_tick,
        .previous_power = previous_power,
        .expected_power = not_power(port_powered(world)),
    };

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .after_ticks = 0,
        .context = &check,
        .message = "NOT output changed at the wrong game tick",
    });

    sim_advance_ticks(world, PROPAGATION_DELAY);

    writeback_mcs(world,
                  "saves/examples/not_gate.mcs",
                  ANCHOR,
                  "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
