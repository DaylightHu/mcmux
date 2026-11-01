/*******************************************************************************************
*
*   mcmux example - XOR gate
*
*   Drives two lever inputs, advances six game ticks, and verifies that the
*   output iron block receives power 15 exactly when one input is powered.
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/xor_gate.mcs
*
*   Change input_a and input_b in main() to select the input case.
*   The simulation result is written back to the example world.
*
*   Example created by DaylightHu (@DaylightHu)
*
********************************************************************************************/
#include "mcmux.h"

static const BlockPos INPUT_A = {37, 4, 9};
static const BlockPos INPUT_B = {35, 4, 9};
static const BlockPos OUTPUT = {36, 6, 14};
static const BlockPos ANCHOR = {37, 4, 9};

static bool valid(const World *world, void *unused)
{
    (void)unused;

    BlockState a = world_get(world, INPUT_A);
    BlockState b = world_get(world, INPUT_B);
    BlockState output = world_get(world, OUTPUT);
    SolidPower output_power = redstone_solid_power(world, OUTPUT);

    bool a_high = a.id == BLOCK_LEVER && a.data.lever.powered;
    bool b_high = b.id == BLOCK_LEVER && b.data.lever.powered;

    uint8_t expected_power = a_high != b_high ? 15u : 0u;

    return a.id == BLOCK_LEVER &&
           b.id == BLOCK_LEVER &&
           output.id == BLOCK_IRON &&
           output_power.received == expected_power;
}

int main(void)
{
    World *world = load_mcs("saves/examples/xor_gate.mcs", ANCHOR);

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .after_ticks = 8,
        .message = "XOR output does not match its lever inputs",
    });

    bool input_a = true;
    bool input_b = false;

    world_set_lever_powered(world, INPUT_A, input_a);
    world_set_lever_powered(world, INPUT_B, input_b);

    sim_advance_ticks(world, 8);

    writeback_mcs(world, "saves/examples/xor_gate.mcs",
                  ANCHOR, "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
