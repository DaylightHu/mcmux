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

// mcmux-model: saves/examples/carry_save_cell.mcs build/carry_save_model.c carry_save_model

#define REFERENCE_BACKEND 1
#define CPU_BACKEND 2
#define GENERATED_C_BACKEND 3
#define ADDER_BACKEND REFERENCE_BACKEND

#if ADDER_BACKEND == GENERATED_C_BACKEND
/* Export the model as build/carry_save_model.c with symbol carry_save_model. */
#include "../../build/carry_save_model.c"
#endif

static const BlockPos INPUT_A = {23, 4, 9};
static const BlockPos INPUT_B = {22, 3, 9};
static const BlockPos INPUT_C = {19, 4, 9};

static const BlockPos ANCHOR = {23, 4, 10};

static const BlockPos SUM = {22, 3, 17};
static const BlockPos CARRY = {19, 2, 17};

enum {
    PROPAGATION_DELAY = 6,
    NOMINAL_TPS = 20,
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
    if (!world) return EXIT_FAILURE;

#if ADDER_BACKEND != REFERENCE_BACKEND
    const BlockPos min = {18, 2, 9};
    const BlockPos max = {23, 4, 18};
#endif
#if ADDER_BACKEND == REFERENCE_BACKEND
    const char *backend = "reference";
    RsResult attach = RS_OK;
#elif ADDER_BACKEND == CPU_BACKEND
    const char *backend = "cpu";
    RsResult attach = sim_use_cpu(world, min, max);
#elif ADDER_BACKEND == GENERATED_C_BACKEND
    const char *backend = "generated-c";
    RsResult attach = sim_use_c(world, min, max, &carry_save_model);
#else
#error Invalid ADDER_BACKEND
#endif
    if (attach != RS_OK) {
        fprintf(stderr, "Cannot select %s backend (%s)\n", backend, rs_result_name(attach));
        world_destroy(world);
        return attach;
    }

    world_expect(world, world_get(world, SUM).id == BLOCK_IRON,
                 "sum output must be an iron block");
    world_expect(world, world_get(world, CARRY).id == BLOCK_IRON,
                 "carry output must be an iron block");

    uint64_t input_tick = world_tick(world);
    uint8_t previous_sum = output_power(world, SUM);
    uint8_t previous_carry = output_power(world, CARRY);

    bool input_a = true;
    bool input_b = true;
    bool input_c = true;

    double start = mcmux_monotonic_seconds();
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

    world_expect(world, valid(world, &check), "carry-save output changed on the input tick");

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .context = &check,
        .message = "carry-save output changed at the wrong game tick",
    });

    sim_advance_ticks(world, PROPAGATION_DELAY);

    unsigned sum = world_result(world) == RS_OK ? output_power(world, SUM) > 0 : 0;
    unsigned carry = world_result(world) == RS_OK ? output_power(world, CARRY) > 0 : 0;
    double end = mcmux_monotonic_seconds();
    uint64_t elapsed_ticks = world_tick(world) - check.input_tick;

    if (world_result(world) == RS_OK) {
        writeback_mcs(world, "saves/examples/carry_save_cell.mcs", ANCHOR, "saves/examples/examples.mcw");
        if (world_result(world) == RS_OK)
            printf("%u + %u + %u = %u (CARRY SUM = %u%u) at %" PRIu64 " gt\n",
                   (unsigned)actual_a, (unsigned)actual_b, (unsigned)actual_c,
                   sum + 2u * carry, carry, sum, elapsed_ticks);
    }
    printf("backend=%s expected_ticks=%d elapsed_ticks=%" PRIu64 " nominal_ms=%.3f wall_ms=",
           backend, PROPAGATION_DELAY, elapsed_ticks, (double)elapsed_ticks * 1000.0 / NOMINAL_TPS);
    if (start > 0.0 && end >= start) printf("%.3f\n", (end - start) * 1000.0);
    else puts("unavailable");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
