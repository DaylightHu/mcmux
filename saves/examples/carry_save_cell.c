/*******************************************************************************************
*
*   mcmux example - Carry-save cell
*
*   Drives three lever inputs, derives the carry-save result from the actual
*   input-port signals, and verifies both output iron blocks change exactly at
*   game tick 6.
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/carry_save_cell.mcs
*
*   Example created by DaylightHu (@DaylightHu)
*
********************************************************************************************/

#include "mcmux.h"
static const BlockPos INPUT_A = {23, 4, 9};
static const BlockPos INPUT_B = {22, 3, 9};
static const BlockPos INPUT_C = {19, 4, 9};

static const BlockPos ANCHOR = {23, 4, 10};

static const BlockPos SUM = {22, 3, 17};
static const BlockPos CARRY = {19, 2, 17};

enum {
    PROPAGATION_DELAY = 6,
    SUM_HIGH_POWER = 14,
    CARRY_HIGH_POWER = 15,
};

typedef struct {
    uint64_t input_tick;
    uint8_t previous_sum;
    uint8_t previous_carry;
    uint8_t expected_sum;
    uint8_t expected_carry;
} TransitionCheck;

static bool input_powered(const World *world, BlockPos input)
{
    return redstone_signal(world, input, DIR_SOUTH) > 0;
}

static uint8_t csa_sum_power(bool a, bool b, bool c)
{
    return ((a != b) != c) ? SUM_HIGH_POWER : 0u;
}

static uint8_t csa_carry_power(bool a, bool b, bool c)
{
    bool carry = (a && b) || (a && c) || (b && c);
    return carry ? CARRY_HIGH_POWER : 0u;
}

static uint8_t output_power(const World *world, BlockPos output)
{
    return redstone_solid_power(world, output).received;
}

static bool valid(const World *world, void *context)
{
    const TransitionCheck *check = context;
    uint64_t elapsed = world_tick(world) - check->input_tick;

    uint8_t expected_sum = elapsed < PROPAGATION_DELAY
                               ? check->previous_sum
                               : check->expected_sum;
    uint8_t expected_carry = elapsed < PROPAGATION_DELAY
                                 ? check->previous_carry
                                 : check->expected_carry;

    return output_power(world, SUM) == expected_sum &&
           output_power(world, CARRY) == expected_carry;
}

int main(void)
{
    World *world = load_mcs("saves/examples/carry_save_cell.mcs", ANCHOR);

    uint64_t input_tick = world_tick(world);
    uint8_t previous_sum = output_power(world, SUM);
    uint8_t previous_carry = output_power(world, CARRY);

    bool input_a = true;
    bool input_b = false;
    bool input_c = true;

    world_set_lever_powered(world, INPUT_A, input_a);
    world_set_lever_powered(world, INPUT_B, input_b);
    world_set_lever_powered(world, INPUT_C, input_c);

    bool actual_a = input_powered(world, INPUT_A);
    bool actual_b = input_powered(world, INPUT_B);
    bool actual_c = input_powered(world, INPUT_C);

    TransitionCheck check = {
        .input_tick = input_tick,
        .previous_sum = previous_sum,
        .previous_carry = previous_carry,
        .expected_sum = csa_sum_power(actual_a, actual_b, actual_c),
        .expected_carry = csa_carry_power(actual_a, actual_b, actual_c),
    };

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .after_ticks = 0,
        .context = &check,
        .message = "carry-save output changed at the wrong game tick",
    });

    sim_advance_ticks(world, PROPAGATION_DELAY);

    writeback_mcs(world,
                  "saves/examples/carry_save_cell.mcs",
                  ANCHOR,
                  "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
