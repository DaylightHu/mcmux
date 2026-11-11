/*******************************************************************************************
*
*   mcmux example - Carry-save array driver
*
*   Drives three unsigned 16-bit operands through vertically stacked cells.
*   Cell 1 is the least significant bit. Every next cell is two blocks higher.
*   Reads both iron-block output buses and checks their 6 gt transition.
*   The carry bus is unshifted: A + B + C = SUM + (CARRY << 1).
*
*   World:     saves/examples/examples.mcw
*   Structure: saves/examples/carry_save_array.mcs
*
*   Circuit created by DaylightHu (@DaylightHu)
*
********************************************************************************************/

#include "mcmux.h"

// mcmux-model: saves/examples/carry_save_array.mcs build/carry_save_array_model.c carry_save_array_model

#define REFERENCE_BACKEND 1
#define CPU_BACKEND 2
#define GENERATED_C_BACKEND 3
#define ADDER_BACKEND GENERATED_C_BACKEND

#if ADDER_BACKEND == GENERATED_C_BACKEND
#include "../../build/carry_save_array_model.c"
#endif

enum { INPUT_BITS = 16, CELL_STEP = 2, PROPAGATION_DELAY = 6,
       NOMINAL_TPS = 20, SUM_HIGH_POWER = 14, CARRY_HIGH_POWER = 15 };

static const char structure_path[] = "saves/examples/carry_save_array.mcs";
static const BlockPos ANCHOR = {13, 34, 10};
static const BlockPos INPUT_A = {13, 4, 9};
static const BlockPos INPUT_B = {12, 3, 9};
static const BlockPos INPUT_C = {9, 4, 9};
static const BlockPos SUM = {12, 3, 17};
static const BlockPos CARRY = {9, 2, 17};

typedef struct {
    uint8_t sum[INPUT_BITS], carry[INPUT_BITS];
} ArrayOutput;

typedef struct {
    uint64_t input_tick;
    ArrayOutput previous, expected;
} TransitionCheck;

static BlockPos bit_position(BlockPos first, unsigned bit)
{
    first.y += CELL_STEP*(int)bit;
    return first;
}

static void drive_input(World *world, BlockPos first, char operand, uint32_t value)
{
    for (unsigned bit = 0; bit < INPUT_BITS; ++bit) {
        BlockPos pos = bit_position(first, bit);
        world_expect(world, world_get(world, pos).id == BLOCK_LEVER,
                     "%c%u at (%d, %d, %d) must be a lever",
                     operand, bit + 1, pos.x, pos.y, pos.z);
        world_set_lever_powered(world, pos, ((value >> bit) & 1u) != 0);
    }
}

static ArrayOutput read_output(const World *world)
{
    ArrayOutput output;
    for (unsigned bit = 0; bit < INPUT_BITS; ++bit) {
        output.sum[bit] = redstone_solid_power(world, bit_position(SUM, bit)).received;
        output.carry[bit] = redstone_solid_power(world, bit_position(CARRY, bit)).received;
    }
    return output;
}

static uint32_t output_word(const uint8_t power[INPUT_BITS])
{
    uint32_t word = 0;
    for (unsigned bit = 0; bit < INPUT_BITS; ++bit)
        if (power[bit]) word |= UINT32_C(1) << bit;
    return word;
}

static bool valid(const World *world, void *context)
{
    const TransitionCheck *check = context;
    uint64_t elapsed = world_tick(world) - check->input_tick;
    const ArrayOutput *expected = elapsed < PROPAGATION_DELAY ?
        &check->previous : &check->expected;
    ArrayOutput actual = read_output(world);
    for (unsigned bit = 0; bit < INPUT_BITS; ++bit)
        if (actual.sum[bit] != expected->sum[bit] ||
            actual.carry[bit] != expected->carry[bit]) return false;
    return true;
}

int main(void)
{
#if ADDER_BACKEND != REFERENCE_BACKEND
    StructureResult status;
    Structure *structure = structure_read(structure_path, &status);
    if (!structure) {
        fprintf(stderr, "Cannot read '%s': %s\n",
                structure_path, structure_result_name(status));
        return EXIT_FAILURE;
    }
    StructureInfo info = structure_info(structure);
    structure_destroy(structure);
#endif

    /* Place the structure's saved anchor at the circuit's workspace anchor. */
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
    RsResult attach = sim_use_c(world, min, max, &carry_save_array_model);
#else
#error Invalid ADDER_BACKEND
#endif
    if (attach != RS_OK) {
        fprintf(stderr, "Cannot select %s backend (%s)\n", backend, rs_result_name(attach));
        world_destroy(world);
        return attach;
    }

    for (unsigned bit = 0; bit < INPUT_BITS; ++bit) {
        BlockPos sum_pos = bit_position(SUM, bit);
        BlockPos carry_pos = bit_position(CARRY, bit);
        BlockState sum_block = world_get(world, sum_pos);
        BlockState carry_block = world_get(world, carry_pos);
        world_expect(world, sum_block.id == BLOCK_IRON,
                     "SUM%u at (%d, %d, %d) must be an iron block, found %s",
                     bit + 1, sum_pos.x, sum_pos.y, sum_pos.z,
                     block_name((BlockId)sum_block.id));
        world_expect(world, carry_block.id == BLOCK_IRON,
                     "CARRY%u at (%d, %d, %d) must be an iron block, found %s",
                     bit + 1, carry_pos.x, carry_pos.y, carry_pos.z,
                     block_name((BlockId)carry_block.id));
    }

    /* Edit these operands to drive another input pattern. */
    const uint32_t a = 0x1234u, b = 0x5678u, c = 0x9abcu;
    world_expect(world, (a | b | c) <= UINT16_MAX, "inputs must fit in 16 bits");
    uint32_t expected_sum = a ^ b ^ c;
    uint32_t expected_carry = (a & b) | (a & c) | (b & c);
    TransitionCheck check = {
        .input_tick = world_tick(world),
        .previous = read_output(world),
    };
    for (unsigned bit = 0; bit < INPUT_BITS; ++bit) {
        check.expected.sum[bit] = ((expected_sum >> bit) & 1u) ? SUM_HIGH_POWER : 0;
        check.expected.carry[bit] = ((expected_carry >> bit) & 1u) ? CARRY_HIGH_POWER : 0;
    }

    double start = mcmux_monotonic_seconds();
    drive_input(world, INPUT_A, 'A', a);
    drive_input(world, INPUT_B, 'B', b);
    drive_input(world, INPUT_C, 'C', c);
    world_expect(world, valid(world, &check), "carry-save array output changed on the input tick");
    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .context = &check,
        .message = "carry-save array output disagrees with the expected 6 gt transition",
    });
    sim_advance_ticks(world, PROPAGATION_DELAY);

    ArrayOutput output = read_output(world);
    uint32_t sum = output_word(output.sum), carry = output_word(output.carry);
    uint32_t total = sum + (carry << 1);
    world_expect(world, total == a + b + c, "SUM + (CARRY << 1) must equal A + B + C");
    double end = mcmux_monotonic_seconds();
    uint64_t elapsed_ticks = world_tick(world) - check.input_tick;

    if (world_result(world) == RS_OK) {
        writeback_mcs(world, structure_path, ANCHOR, "saves/examples/examples.mcw");
        if (world_result(world) == RS_OK)
            printf("%" PRIu32 " + %" PRIu32 " + %" PRIu32 " = %" PRIu32
                   " (SUM=%" PRIu32 " CARRY=%" PRIu32 ") at %" PRIu64 " gt\n",
                   a, b, c, total, sum, carry, elapsed_ticks);
    }
    printf("backend=%s expected_ticks=%d elapsed_ticks=%" PRIu64 " nominal_ms=%.3f wall_ms=",
           backend, PROPAGATION_DELAY, elapsed_ticks, (double)elapsed_ticks*1000.0/NOMINAL_TPS);
    if (start > 0.0 && end >= start) printf("%.3f\n", (end - start)*1000.0);
    else puts("unavailable");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}
