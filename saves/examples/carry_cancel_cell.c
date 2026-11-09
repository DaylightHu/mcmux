/*******************************************************************************************
*
*   mcmux example - carry-cancel cell(I made it a two-piece design to make stacking easier.)
*
*   Adds two unsigned two-bit operands using four levers. Checks the three
*   output iron blocks after each game tick, then writes back the result.
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/carry_cancel_cell.mcs
*
*   Example created by DaylightHu (@DaylightHu)
*
********************************************************************************************/

#include "mcmux.h"

// mcmux-model: saves/examples/carry_cancel_cell.mcs build/carry_cancel_model.c carry_cancel_model

#define REFERENCE_BACKEND 1
#define CPU_BACKEND 2
#define GENERATED_C_BACKEND 3
#define ADDER_BACKEND GENERATED_C_BACKEND

#if ADDER_BACKEND == GENERATED_C_BACKEND
/* Export the model as build/carry_cancel_model.c with symbol carry_cancel_model. */
#include "../../build/carry_cancel_model.c"
#endif

enum { INPUT_BITS = 2, OUTPUT_BITS = 3, PROPAGATION_DELAY = 6, NOMINAL_TPS = 20 };

static const BlockPos INPUT_A[INPUT_BITS] = {{23, 4, 22}, {23, 6, 22}};
static const BlockPos INPUT_B[INPUT_BITS] = {{20, 4, 22}, {20, 6, 22}};
static const BlockPos OUTPUT[OUTPUT_BITS] = {{19, 3, 30}, {19, 5, 30}, {19, 7, 30}};

static const BlockPos ANCHOR = {23, 4, 23};

typedef struct {
    uint64_t input_tick;
    unsigned previous_sum;
    unsigned expected_sum;
} TransitionCheck;

static void drive_input(World *world, const BlockPos input[INPUT_BITS], char operand,
                        unsigned value)
{
    for (unsigned bit = 0; bit < INPUT_BITS; ++bit) {
        world_expect(world, world_get(world, input[bit]).id == BLOCK_LEVER,
                     "%c%u must be a lever. ",
                     operand, bit + 1);
        world_set_lever_powered(world, input[bit], ((value >> bit) & 1u) != 0);
    }
}

static unsigned read_output(const World *world)
{
    unsigned value = 0;
    for (unsigned bit = 0; bit < OUTPUT_BITS; ++bit)
        if (redstone_solid_power(world, OUTPUT[bit]).received > 0)
            value |= 1u << bit;
    return value;
}

static bool valid(const World *world, void *context)
{
    const TransitionCheck *check = context;
    uint64_t elapsed = world_tick(world) - check->input_tick;
    unsigned expected = elapsed < PROPAGATION_DELAY
                            ? check->previous_sum : check->expected_sum;
    return read_output(world) == expected;
}

int main(void)
{
    World *world = load_mcs("saves/examples/carry_cancel_cell.mcs", ANCHOR);
    if (!world) return EXIT_FAILURE;

#if ADDER_BACKEND != REFERENCE_BACKEND
    const BlockPos min = {18, 2, 22};
    const BlockPos max = {23, 7, 30};
#endif
#if ADDER_BACKEND == REFERENCE_BACKEND
    const char *backend = "reference";
    RsResult attach = RS_OK;
#elif ADDER_BACKEND == CPU_BACKEND
    const char *backend = "cpu";
    RsResult attach = sim_use_cpu(world, min, max);
#elif ADDER_BACKEND == GENERATED_C_BACKEND
    const char *backend = "generated-c";
    RsResult attach = sim_use_c(world, min, max, &carry_cancel_model);
#else
#error Invalid ADDER_BACKEND
#endif
    if (attach != RS_OK) {
        fprintf(stderr, "Cannot select %s backend (%s)\n", backend, rs_result_name(attach));
        world_destroy(world);
        return attach;
    }

    for (unsigned bit = 0; bit < OUTPUT_BITS; ++bit)
        world_expect(world, world_get(world, OUTPUT[bit]).id == BLOCK_IRON,
                     "output %u must be an iron block", bit + 1);

    const unsigned a = 3, b = 1;
    TransitionCheck check = {
        .input_tick = world_tick(world),
        .previous_sum = read_output(world),
        .expected_sum = a + b,
    };

    double start = mcmux_monotonic_seconds();
    drive_input(world, INPUT_A, 'A', a);
    drive_input(world, INPUT_B, 'B', b);
    world_expect(world, valid(world, &check), "adder output changed on the input tick");

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .context = &check,
        .message = "adder output disagrees with the expected 6 gt transition",
    });
    sim_advance_ticks(world, PROPAGATION_DELAY);

    unsigned sum = world_result(world) == RS_OK ? read_output(world) : 0;
    double end = mcmux_monotonic_seconds();
    uint64_t elapsed_ticks = world_tick(world) - check.input_tick;

    if (world_result(world) == RS_OK) {
        writeback_mcs(world, "saves/examples/carry_cancel_cell.mcs", ANCHOR, "saves/examples/examples.mcw");
        if (world_result(world) == RS_OK)
            printf("%u + %u = %u (O3 O2 O1 = %u%u%u) at %" PRIu64 " gt\n",
                   a, b, sum, (sum >> 2) & 1u, (sum >> 1) & 1u, sum & 1u,
                   elapsed_ticks);
    }
    printf("backend=%s expected_ticks=%d elapsed_ticks=%" PRIu64 " nominal_ms=%.3f wall_ms=",
           backend, PROPAGATION_DELAY, elapsed_ticks, (double)elapsed_ticks * 1000.0 / NOMINAL_TPS);
    if (start > 0.0 && end >= start) printf("%.3f\n", (end - start) * 1000.0);
    else puts("unavailable");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
