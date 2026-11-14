/*
 * mcmux example - 16-bit carry-cancel array driver
 *
 * Drives A, B and Cin through the structure, checks its iron-block outputs
 * each game tick, then writes the result back into the example world.
 * The seventeenth output is Cout.
 */

#include "mcmux.h"

#include <errno.h>

// mcmux-model: saves/examples/carry_cancel_array.mcs build/carry_cancel_array_model.c carry_cancel_array_model

#define REFERENCE_BACKEND 1
#define CPU_BACKEND 2
#define GENERATED_C_BACKEND 3
#ifndef ADDER_BACKEND
#define ADDER_BACKEND REFERENCE_BACKEND
#endif

#if ADDER_BACKEND == GENERATED_C_BACKEND
#include "../../build/carry_cancel_array_model.c"
#endif

enum { INPUT_BITS = 16, CELL_STEP = 2, MAX_SETTLE_TICKS = 64, NOMINAL_TPS = 20 };

static const char structure_path[] = "saves/examples/carry_cancel_array.mcs";
static const char world_path[] = "saves/examples/examples.mcw";
static const BlockPos ANCHOR = {2, 34, 10};
static const BlockPos INPUT_A = {2, 4, 9};
static const BlockPos INPUT_B = {-1, 4, 9};
static const BlockPos CARRY_IN = {0, 3, 15};
static const BlockPos OUTPUT = {3, 3, 16};

typedef struct {
    uint64_t input_tick, last_output_change;
    uint32_t output, expected;
    bool check_default_trace;
} TransitionCheck;

static BlockPos bit_position(BlockPos first, unsigned bit)
{
    first.y += CELL_STEP * (int)bit;
    return first;
}

static void expect_port(World *world, BlockPos pos, BlockId id)
{
    BlockState block = world_get(world, pos);
    world_expect(world, block.id == id, "Port (%d, %d, %d): expected %s, found %s",
                 pos.x, pos.y, pos.z, block_name(id), block_name((BlockId)block.id));
}

static void drive_input(World *world, BlockPos first, uint32_t value)
{
    for (unsigned bit = 0; bit < INPUT_BITS; ++bit) {
        world_set_lever_powered(world, bit_position(first, bit),
                                ((value >> bit) & 1u) != 0);
    }
}

static uint32_t read_output(const World *world)
{
    uint32_t word = 0;
    for (unsigned bit = 0; bit <= INPUT_BITS; ++bit) {
        if (redstone_solid_power(world, bit_position(OUTPUT, bit)).received)
            word |= UINT32_C(1) << bit;
    }
    return word;
}

static bool check_transition(const World *world, void *context)
{
    TransitionCheck *check = context;
    uint64_t elapsed = world_tick(world) - check->input_tick;
    uint32_t output = read_output(world);
    if (output != check->output) {
        check->output = output;
        check->last_output_change = elapsed;
    }
    if (check->check_default_trace) {
        /* From zero, 255 + 1 crosses the first continuation at 12 gt. */
        uint32_t expected = elapsed < 6 ? 0u : elapsed < 12 ? 128u : 256u;
        if (output != expected) {
            fprintf(stderr, "At %" PRIu64 " gt: expected %" PRIu32 ", got %" PRIu32 "\n",
                    elapsed, expected, output);
            return false;
        }
    }
    return true;
}

static void settle(World *world)
{
    for (unsigned tick = 0; tick < MAX_SETTLE_TICKS &&
         world_result(world) == RS_OK && sim_stats(world).pending_events; ++tick) {
        sim_advance_ticks(world, 1);
    }
    world_expect(world, sim_stats(world).pending_events == 0,
                 "Circuit did not settle within %d game ticks", MAX_SETTLE_TICKS);
}

static bool read_number(const char *text, uint32_t max, uint32_t *value)
{
    if (*text < '0' || *text > '9') return false;
    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (errno || *end || number > max) return false;
    *value = (uint32_t)number;
    return true;
}

int main(int argc, char **argv)
{
    uint32_t a = 255, b = 1, cin = 0;
    if (argc != 1 &&
        ((argc != 3 && argc != 4) ||
         !read_number(argv[1], UINT16_MAX, &a) ||
         !read_number(argv[2], UINT16_MAX, &b) ||
         (argc == 4 && !read_number(argv[3], 1, &cin)))) {
        fprintf(stderr, "Usage: %s [A B [Cin]] (A and B: 0..65535, Cin: 0 or 1)\n", argv[0]);
        return EXIT_FAILURE;
    }

#if ADDER_BACKEND != REFERENCE_BACKEND
    StructureResult load_status;
    Structure *structure = structure_read(structure_path, &load_status);
    if (!structure) {
        fprintf(stderr, "Cannot read '%s': %s\n", structure_path,
                structure_result_name(load_status));
        return EXIT_FAILURE;
    }
    StructureInfo info = structure_info(structure);
    structure_destroy(structure);
#endif

    World *world = load_mcs(structure_path, ANCHOR);
    if (!world) return EXIT_FAILURE;

#if ADDER_BACKEND != REFERENCE_BACKEND
    const BlockPos min = {
        (int)((int64_t)info.origin.x + ANCHOR.x - info.anchor.x),
        (int)((int64_t)info.origin.y + ANCHOR.y - info.anchor.y),
        (int)((int64_t)info.origin.z + ANCHOR.z - info.anchor.z),
    };
    const BlockPos max = {
        (int)((int64_t)min.x + info.width - 1),
        (int)((int64_t)min.y + info.height - 1),
        (int)((int64_t)min.z + info.length - 1),
    };
#endif
#if ADDER_BACKEND == REFERENCE_BACKEND
    const char *backend = "reference";
    RsResult attach = RS_OK;
#elif ADDER_BACKEND == CPU_BACKEND
    const char *backend = "cpu";
    RsResult attach = sim_use_cpu(world, min, max);
#elif ADDER_BACKEND == GENERATED_C_BACKEND
    const char *backend = "generated-c";
    RsResult attach = sim_use_c(world, min, max, &carry_cancel_array_model);
#else
#error Invalid ADDER_BACKEND
#endif
    if (attach != RS_OK) {
        fprintf(stderr, "Cannot select %s backend (%s)\n", backend, rs_result_name(attach));
        world_destroy(world);
        return EXIT_FAILURE;
    }

    for (unsigned bit = 0; bit < INPUT_BITS; ++bit) {
        expect_port(world, bit_position(INPUT_A, bit), BLOCK_LEVER);
        expect_port(world, bit_position(INPUT_B, bit), BLOCK_LEVER);
        expect_port(world, bit_position(OUTPUT, bit), BLOCK_IRON);
    }
    expect_port(world, CARRY_IN, BLOCK_LEVER);
    expect_port(world, bit_position(OUTPUT, INPUT_BITS), BLOCK_IRON);

    drive_input(world, INPUT_A, 0);
    drive_input(world, INPUT_B, 0);
    world_set_lever_powered(world, CARRY_IN, false);
    settle(world);
    world_expect(world, read_output(world) == 0, "Zero inputs must produce zero");

    TransitionCheck check = {
        .input_tick = world_tick(world),
        .expected = a + b + cin,
        .check_default_trace = a == 255 && b == 1 && cin == 0,
    };
    double start = mcmux_monotonic_seconds();
    drive_input(world, INPUT_A, a);
    drive_input(world, INPUT_B, b);
    world_set_lever_powered(world, CARRY_IN, cin != 0);
    if (world_result(world) == RS_OK) {
        world_expect(world, check_transition(world, &check),
                     "Output disagrees with the expected input-tick state");
    }
    sim_set_check(world, (SimCheckConfig){
        .check = check_transition,
        .context = &check,
        .message = "Carry-cancel output disagrees with the expected transition",
    });
    settle(world);
    sim_set_check(world, (SimCheckConfig){0});

    uint32_t result = read_output(world);
    world_expect(world, result == check.expected,
                 "Expected %" PRIu32 ", circuit returned %" PRIu32, check.expected, result);
    double end = mcmux_monotonic_seconds();
    uint64_t elapsed = world_tick(world) - check.input_tick;

    if (world_result(world) == RS_OK) {
        writeback_mcs(world, structure_path, ANCHOR, world_path);
        if (world_result(world) == RS_OK) {
            printf("%" PRIu32 " + %" PRIu32 " + %" PRIu32 " = %" PRIu32
                   " (SUM=%" PRIu32 ", Cout=%" PRIu32 ")\n",
                   a, b, cin, result, result & UINT16_MAX, result >> INPUT_BITS);
            printf("backend=%s last_output_change_gt=%" PRIu64 " elapsed_gt=%" PRIu64
                   " nominal_ms=%.3f wall_ms=",
                   backend, check.last_output_change, elapsed, 1000.0 * elapsed / NOMINAL_TPS);
            if (start > 0.0 && end >= start) printf("%.3f\n", (end - start)*1000.0);
            else puts("unavailable");
        }
    }

    RsResult status = world_result(world);
    world_destroy(world);
    return status == RS_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
