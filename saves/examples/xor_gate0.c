/*******************************************************************************************
*
*   mcmux example - XOR gate
*
*   Drives the example inputs, derives XOR from the resulting port signals,
*   and verifies that the output changes exactly at game tick 8, not earlier.
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
static const BlockPos PORT_A = {37, 4, 10};
static const BlockPos PORT_B = {35, 4, 10};
static const BlockPos OUTPUT = {36, 6, 14};
static const BlockPos ANCHOR = {37, 4, 9};

enum { PROPAGATION_DELAY = 8 };

typedef struct {
    uint64_t input_tick;
    uint8_t previous_power;
    uint8_t expected_power;
} TransitionCheck;

static bool port_powered(const World *world, BlockPos port)
{
    return redstone_signal(world, port, DIR_UP) > 0;
}

static uint8_t xor_power(bool a, bool b)
{
    return a != b ? 15u : 0u;
}

static uint8_t output_power(const World *world)
{
    return redstone_signal(world, OUTPUT, DIR_SOUTH);
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
    World *world = load_mcs("saves/examples/xor_gate0.mcs", ANCHOR);

    uint64_t input_tick = world_tick(world);
    uint8_t previous_power = output_power(world);

    world_set_lever_powered(world, INPUT_A, true);
    world_set_lever_powered(world, INPUT_B, true);

    TransitionCheck check = {
        .input_tick = input_tick,
        .previous_power = previous_power,
        .expected_power = xor_power(port_powered(world, PORT_A),
                                    port_powered(world, PORT_B)),
    };

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .after_ticks = 0,
        .context = &check,
        .message = "XOR output changed at the wrong game tick",
    });

    sim_advance_ticks(world, PROPAGATION_DELAY);

    writeback_mcs(world, "saves/examples/xor_gate0.mcs",
                  ANCHOR, "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
