#include "mcmux.h"

#include "blocks/block_internal.h"
#include "runtime_clock.h"
#include "world_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    CELL_REPEATER,
    CELL_CSA
} CellKind;

typedef enum {
    PATTERN_DENSE,
    PATTERN_SPARSE,
    PATTERN_RANDOM
} InputPattern;

typedef struct {
    CellKind cell;
    InputPattern pattern;
    uint32_t width, depth, ticks, period, rounds;
    uint64_t seed;
    bool verify;
} Config;

typedef struct {
    World *world;
    BlockPos *inputs, *outputs;
    size_t input_count, output_count;
    BlockPos min, max;
} Fixture;

typedef struct {
    double seconds, prepare_seconds;
    uint64_t digest;
    SimStats stats;
} RunResult;

typedef struct {
    uint64_t hash, count;
    bool valid;
} WriteTrace;

static const char *cell_name(CellKind cell)
{
    return cell == CELL_REPEATER ? "repeater" : "csa";
}

static const char *pattern_name(InputPattern pattern)
{
    switch (pattern) {
    case PATTERN_DENSE: return "dense";
    case PATTERN_SPARSE: return "sparse";
    case PATTERN_RANDOM: return "random";
    }
    return "invalid";
}

static const char *platform_name(void)
{
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n\n", program);
    printf("Build a deterministic vanilla-redstone workload, then compare the\n");
    printf("reference and optional CPU query backends for the same game ticks.\n\n");
    printf("Options:\n");
    printf("  --cell repeater|csa  Circuit cell (default: repeater)\n");
    printf("  --width N            Parallel lanes/cells (default: 64)\n");
    printf("  --depth N            Serial stages per lane (default: 32)\n");
    printf("  --ticks N            Measured game ticks (default: 512)\n");
    printf("  --period N           Game ticks between input batches (default: 4)\n");
    printf("  --rounds N           Timed reference/CPU pairs (default: 7)\n");
    printf("  --seed N             Deterministic input seed (default: 1)\n");
    printf("  --pattern NAME       dense, sparse or random (default: dense)\n");
    printf("  --no-verify          Skip the untimed per-tick equivalence pass\n");
    printf("  --help               Show this help\n\n");
    printf("CSA cells are independent saved structures and currently require depth=1.\n");
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (!text || !*text || *text == '-') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno || !end || *end || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    if (!text || !*text || *text == '-') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno || !end || *end) return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool option_value(int argc, char **argv, int *index, const char **value)
{
    if (*index + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", argv[*index]);
        return false;
    }
    *value = argv[++*index];
    return true;
}

static bool parse_config(int argc, char **argv, Config *config)
{
    *config = (Config){
        .cell = CELL_REPEATER,
        .pattern = PATTERN_DENSE,
        .width = 64,
        .depth = 32,
        .ticks = 512,
        .period = 4,
        .rounds = 7,
        .seed = 1,
        .verify = true,
    };
    for (int i = 1; i < argc; ++i) {
        const char *value = NULL;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            config->verify = false;
        } else if (strcmp(argv[i], "--cell") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            if (strcmp(value, "repeater") == 0) config->cell = CELL_REPEATER;
            else if (strcmp(value, "csa") == 0) config->cell = CELL_CSA;
            else { fprintf(stderr, "unknown cell: %s\n", value); return false; }
        } else if (strcmp(argv[i], "--pattern") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            if (strcmp(value, "dense") == 0) config->pattern = PATTERN_DENSE;
            else if (strcmp(value, "sparse") == 0) config->pattern = PATTERN_SPARSE;
            else if (strcmp(value, "random") == 0) config->pattern = PATTERN_RANDOM;
            else { fprintf(stderr, "unknown pattern: %s\n", value); return false; }
        } else if (strcmp(argv[i], "--width") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->width)) return false;
        } else if (strcmp(argv[i], "--depth") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->depth)) return false;
        } else if (strcmp(argv[i], "--ticks") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->ticks)) return false;
        } else if (strcmp(argv[i], "--period") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->period)) return false;
        } else if (strcmp(argv[i], "--rounds") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->rounds)) return false;
        } else if (strcmp(argv[i], "--seed") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u64(value, &config->seed)) return false;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return false;
        }
    }
    if (!config->width || !config->depth || !config->ticks ||
        !config->period || !config->rounds || config->rounds > 31) {
        fprintf(stderr, "width, depth, ticks and period must be positive; rounds must be 1..31\n");
        return false;
    }
    if (config->cell == CELL_CSA && config->depth != 1) {
        fprintf(stderr, "CSA cells currently support only --depth 1\n");
        return false;
    }
    uint64_t modules = (uint64_t)config->width * config->depth;
    uint64_t limit = config->cell == CELL_REPEATER ? 1000000 : 16384;
    if (modules > limit) {
        fprintf(stderr, "workload exceeds the %llu-module safety limit for %s cells\n",
                (unsigned long long)limit, cell_name(config->cell));
        return false;
    }
    return true;
}

static bool result_ok(RsResult result)
{
    return result == RS_OK || result == RS_UNCHANGED;
}

static bool world_ok(const World *world, const char *operation)
{
    RsResult result = world_result(world);
    if (result == RS_OK) return true;
    fprintf(stderr, "%s failed: %s", operation, rs_result_name(result));
    const char *detail = world_error_message(world);
    if (detail && *detail) fprintf(stderr, " (%s)", detail);
    fputc('\n', stderr);
    return false;
}

static bool expand_for(World *world, int max_x, int max_z)
{
    WorldPlotBounds bounds = world_plot_bounds(world);
    if (max_x > bounds.max_x) {
        uint64_t distance = (uint64_t)((int64_t)max_x - bounds.max_x);
        uint64_t plots = (distance + WORLD_PLOT_SIZE - 1) / WORLD_PLOT_SIZE;
        if (plots > UINT32_MAX ||
            world_expand_plot(world, DIR_EAST, (uint32_t)plots) != RS_OK)
            return false;
    }
    bounds = world_plot_bounds(world);
    if (max_z > bounds.max_z) {
        uint64_t distance = (uint64_t)((int64_t)max_z - bounds.max_z);
        uint64_t plots = (distance + WORLD_PLOT_SIZE - 1) / WORLD_PLOT_SIZE;
        if (plots > UINT32_MAX ||
            world_expand_plot(world, DIR_SOUTH, (uint32_t)plots) != RS_OK)
            return false;
    }
    return true;
}

static bool fixture_allocate(Fixture *fixture, size_t inputs, size_t outputs)
{
    if (inputs > SIZE_MAX / sizeof(*fixture->inputs) ||
        outputs > SIZE_MAX / sizeof(*fixture->outputs)) return false;
    fixture->inputs = inputs ? malloc(inputs * sizeof(*fixture->inputs)) : NULL;
    fixture->outputs = outputs ? malloc(outputs * sizeof(*fixture->outputs)) : NULL;
    if ((inputs && !fixture->inputs) || (outputs && !fixture->outputs)) return false;
    fixture->input_count = inputs;
    fixture->output_count = outputs;
    return true;
}

static bool set_silent(World *world, BlockPos pos, BlockState state)
{
    return world_set_block(world, pos, state, UPDATE_CLIENTS);
}

static bool build_repeater_fixture(const Config *config, Fixture *fixture)
{
    const int base_x = 8, base_z = 8, support_y = 0, signal_y = 1;
    const int lane_spacing = 2;
    int64_t max_x64 = (int64_t)base_x + config->depth + 1;
    int64_t max_z64 = (int64_t)base_z + (int64_t)(config->width - 1) * lane_spacing;
    if (max_x64 > INT_MAX || max_z64 > INT_MAX) return false;

    World *world = world_create((WorldConfig){.ground_layers = 0});
    if (!world) return false;
    fixture->world = world;
    if (!expand_for(world, (int)max_x64, (int)max_z64) ||
        !fixture_allocate(fixture, config->width, config->width) ||
        !result_ok(sim_set_updates_enabled(world, false))) return false;

    BlockState iron = block_default(BLOCK_IRON);
    BlockState lever = block_default(BLOCK_LEVER);
    lever.data.lever.support_dir = DIR_DOWN;
    lever.data.lever.facing = DIR_EAST;
    BlockState repeater = block_default(BLOCK_REDSTONE_REPEATER);
    repeater.data.repeater.facing = DIR_EAST;
    repeater.data.repeater.delay = 1;

    for (uint32_t lane = 0; lane < config->width; ++lane) {
        int z = base_z + (int)lane * lane_spacing;
        for (uint32_t stage = 0; stage <= config->depth + 1; ++stage)
            if (!set_silent(world,
                    (BlockPos){base_x + (int)stage, support_y, z}, iron))
                return false;
        fixture->inputs[lane] = (BlockPos){base_x, signal_y, z};
        fixture->outputs[lane] =
            (BlockPos){base_x + (int)config->depth + 1, signal_y, z};
        if (!set_silent(world, fixture->inputs[lane], lever)) return false;
        for (uint32_t stage = 1; stage <= config->depth; ++stage)
            if (!set_silent(world,
                    (BlockPos){base_x + (int)stage, signal_y, z}, repeater))
                return false;
        if (!set_silent(world, fixture->outputs[lane], iron)) return false;
    }
    if (!result_ok(sim_set_updates_enabled(world, true)) ||
        !world_ok(world, "build repeater fixture")) return false;
    fixture->min = (BlockPos){base_x, support_y, base_z};
    fixture->max = (BlockPos){(int)max_x64, signal_y, (int)max_z64};
    return true;
}

static uint32_t square_columns(uint32_t count)
{
    uint32_t columns = 1;
    while (columns < count / columns || columns * columns < count) ++columns;
    return columns;
}

static bool local_is(const Structure *structure, BlockPos local, BlockId id)
{
    BlockState state;
    return structure_get(structure, local, &state) && state.id == id;
}

static bool build_csa_fixture(const Config *config, Fixture *fixture)
{
    static const BlockPos local_inputs[] = {{5,2,0}, {4,1,0}, {1,2,0}};
    static const BlockPos local_outputs[] = {{4,1,8}, {1,0,8}};
    StructureResult structure_result;
    Structure *structure = structure_read("saves/examples/carry_save_cell.mcs",
                                          &structure_result);
    if (!structure) {
        fprintf(stderr, "cannot read CSA structure: %s\n",
                structure_result_name(structure_result));
        return false;
    }
    bool ports_valid = true;
    for (size_t i = 0; i < sizeof(local_inputs)/sizeof(local_inputs[0]); ++i)
        ports_valid &= local_is(structure, local_inputs[i], BLOCK_LEVER);
    for (size_t i = 0; i < sizeof(local_outputs)/sizeof(local_outputs[0]); ++i)
        ports_valid &= local_is(structure, local_outputs[i], BLOCK_IRON);
    if (!ports_valid) {
        fprintf(stderr, "carry_save_cell.mcs no longer matches its benchmark port contract\n");
        structure_destroy(structure);
        return false;
    }

    StructureInfo info = structure_info(structure);
    uint32_t columns = square_columns(config->width);
    uint32_t rows = (config->width + columns - 1) / columns;
    int64_t stride_x = (int64_t)info.width + 4;
    int64_t stride_z = (int64_t)info.length + 4;
    const int base_x = 8, base_y = 2, base_z = 8;
    int64_t max_x64 = base_x + (int64_t)(columns - 1) * stride_x + info.width - 1;
    int64_t max_z64 = base_z + (int64_t)(rows - 1) * stride_z + info.length - 1;
    int64_t max_y64 = base_y + info.height - 1;
    if (max_x64 > INT_MAX || max_z64 > INT_MAX || max_y64 > WORLD_MAX_Y) {
        structure_destroy(structure);
        return false;
    }

    World *world = world_create((WorldConfig){.ground_layers = 0});
    if (!world) { structure_destroy(structure); return false; }
    fixture->world = world;
    if (!expand_for(world, (int)max_x64, (int)max_z64) ||
        !fixture_allocate(fixture, (size_t)config->width * 3,
                          (size_t)config->width * 2) ||
        !result_ok(sim_set_updates_enabled(world, false))) {
        structure_destroy(structure);
        return false;
    }

    for (uint32_t cell = 0; cell < config->width; ++cell) {
        uint32_t column = cell % columns, row = cell / columns;
        BlockPos origin = {
            base_x + (int)((int64_t)column * stride_x),
            base_y,
            base_z + (int)((int64_t)row * stride_z)
        };
        for (unsigned y = 0; y < info.height; ++y)
            for (unsigned z = 0; z < info.length; ++z)
                for (unsigned x = 0; x < info.width; ++x) {
                    BlockState state;
                    if (!structure_get(structure,
                            (BlockPos){(int)x,(int)y,(int)z}, &state)) {
                        structure_destroy(structure);
                        return false;
                    }
                    if (state.id != BLOCK_AIR && !set_silent(world,
                            (BlockPos){origin.x + (int)x, origin.y + (int)y,
                                       origin.z + (int)z}, state)) {
                        structure_destroy(structure);
                        return false;
                    }
                }
        for (size_t input = 0; input < 3; ++input)
            fixture->inputs[(size_t)cell * 3 + input] = (BlockPos){
                origin.x + local_inputs[input].x,
                origin.y + local_inputs[input].y,
                origin.z + local_inputs[input].z
            };
        for (size_t output = 0; output < 2; ++output)
            fixture->outputs[(size_t)cell * 2 + output] = (BlockPos){
                origin.x + local_outputs[output].x,
                origin.y + local_outputs[output].y,
                origin.z + local_outputs[output].z
            };
    }
    structure_destroy(structure);
    if (!result_ok(sim_set_updates_enabled(world, true)) ||
        !world_ok(world, "build CSA fixture")) return false;
    fixture->min = (BlockPos){base_x, base_y, base_z};
    fixture->max = (BlockPos){(int)max_x64, (int)max_y64, (int)max_z64};
    return true;
}

static bool fixture_build(const Config *config, Fixture *fixture)
{
    *fixture = (Fixture){0};
    bool ok = config->cell == CELL_REPEATER ?
              build_repeater_fixture(config, fixture) :
              build_csa_fixture(config, fixture);
    if (!ok) {
        if (fixture->world) world_ok(fixture->world, "fixture construction");
        world_destroy(fixture->world);
        free(fixture->inputs);
        free(fixture->outputs);
        *fixture = (Fixture){0};
    }
    return ok;
}

static void fixture_destroy(Fixture *fixture)
{
    if (!fixture) return;
    world_destroy(fixture->world);
    free(fixture->inputs);
    free(fixture->outputs);
    *fixture = (Fixture){0};
}

static uint64_t rng_next(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static void generate_toggles(const Config *config, uint8_t *toggles,
                             size_t count, uint64_t *rng)
{
    memset(toggles, 0, count);
    if (config->pattern == PATTERN_DENSE) {
        memset(toggles, 1, count);
    } else if (config->pattern == PATTERN_SPARSE) {
        toggles[rng_next(rng) % count] = 1;
    } else {
        size_t changed = 0;
        for (size_t i = 0; i < count; ++i) {
            toggles[i] = (rng_next(rng) & 7u) == 0;
            changed += toggles[i] != 0;
        }
        if (!changed) toggles[rng_next(rng) % count] = 1;
    }
}

static bool apply_toggles(Fixture *fixture, uint8_t *states,
                          const uint8_t *toggles)
{
    for (size_t i = 0; i < fixture->input_count; ++i) {
        if (!toggles[i]) continue;
        states[i] = !states[i];
        world_set_lever_powered(fixture->world, fixture->inputs[i], states[i]);
    }
    if (world_result(fixture->world) == RS_OK) return true;
    return world_ok(fixture->world, "set benchmark input");
}

static uint64_t hash_mix(uint64_t hash, uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    hash ^= value;
    return (hash << 27 | hash >> 37) * 5 + UINT64_C(0x52dce729);
}

static uint64_t output_digest(const Fixture *fixture)
{
    uint64_t hash = UINT64_C(0x6a09e667f3bcc909);
    for (size_t i = 0; i < fixture->output_count; ++i)
        hash = hash_mix(hash,
            ((uint64_t)i << 8) |
            redstone_solid_power(fixture->world, fixture->outputs[i]).received);
    hash = hash_mix(hash, world_tick(fixture->world));
    hash = hash_mix(hash, world_revision(fixture->world));
    return hash;
}

static void trace_write(void *context, BlockPos pos,
                        BlockState previous, BlockState current)
{
    WriteTrace *trace = context;
    StateId before, after;
    if (!state_encode(previous, &before) || !state_encode(current, &after)) {
        trace->valid = false;
        return;
    }
    uint64_t position = (uint32_t)pos.x;
    position = position * UINT64_C(0x9e3779b1) ^ (uint32_t)pos.y;
    position = position * UINT64_C(0x85ebca6b) ^ (uint32_t)pos.z;
    trace->hash = hash_mix(trace->hash, position);
    trace->hash = hash_mix(trace->hash, ((uint64_t)before << 32) | after);
    ++trace->count;
}

static bool verify_equivalence(const Config *config)
{
    Fixture reference = {0}, cpu = {0};
    uint8_t *states = NULL, *toggles = NULL;
    bool ok = false;
    if (!fixture_build(config, &reference) || !fixture_build(config, &cpu))
        goto done;
    if (reference.input_count != cpu.input_count ||
        reference.output_count != cpu.output_count) goto done;
    RsResult backend = sim_use_cpu(cpu.world, cpu.min, cpu.max);
    if (backend != RS_OK) {
        fprintf(stderr, "CPU setup failed during verification: %s\n",
                rs_result_name(backend));
        goto done;
    }
    states = calloc(reference.input_count, 1);
    toggles = malloc(reference.input_count);
    if (!states || !toggles) goto done;
    WriteTrace reference_trace = {UINT64_C(0x243f6a8885a308d3), 0, true};
    WriteTrace cpu_trace = {UINT64_C(0x243f6a8885a308d3), 0, true};
    world_set_change_observer(reference.world, trace_write, &reference_trace);
    world_set_change_observer(cpu.world, trace_write, &cpu_trace);
    uint64_t rng = config->seed ? config->seed : UINT64_C(0x9e3779b97f4a7c15);

    for (uint32_t tick = 0; tick < config->ticks; ++tick) {
        if (tick % config->period == 0) {
            generate_toggles(config, toggles, reference.input_count, &rng);
            if (!apply_toggles(&reference, states, toggles)) goto done;
            for (size_t i = 0; i < reference.input_count; ++i)
                if (toggles[i])
                    world_set_lever_powered(cpu.world, cpu.inputs[i], states[i]);
            if (!world_ok(cpu.world, "set CPU verification input")) goto done;
        }
        if (!reference_trace.valid || !cpu_trace.valid ||
            reference_trace.count != cpu_trace.count ||
            reference_trace.hash != cpu_trace.hash) {
            fprintf(stderr, "state-write trace differs before game tick %u\n", tick + 1);
            goto done;
        }
        sim_advance_ticks(reference.world, 1);
        sim_advance_ticks(cpu.world, 1);
        if (!world_ok(reference.world, "advance reference verification") ||
            !world_ok(cpu.world, "advance CPU verification")) goto done;
        if (!reference_trace.valid || !cpu_trace.valid ||
            reference_trace.count != cpu_trace.count ||
            reference_trace.hash != cpu_trace.hash) {
            fprintf(stderr, "state-write trace differs at game tick %u\n", tick + 1);
            goto done;
        }
        for (size_t output = 0; output < reference.output_count; ++output) {
            uint8_t expected = redstone_solid_power(
                reference.world, reference.outputs[output]).received;
            uint8_t actual = redstone_solid_power(
                cpu.world, cpu.outputs[output]).received;
            if (expected != actual) {
                fprintf(stderr,
                    "output %zu differs at game tick %u: reference=%u cpu=%u\n",
                    output, tick + 1, expected, actual);
                goto done;
            }
        }
    }
    if (!sim_cpu_active(cpu.world)) {
        fprintf(stderr, "CPU plan became inactive during verification\n");
        goto done;
    }
    if (sim_stats(cpu.world).cpu_fallbacks) {
        fprintf(stderr, "CPU verification used %llu fallback queries\n",
            (unsigned long long)sim_stats(cpu.world).cpu_fallbacks);
        goto done;
    }
    printf("VERIFY status=ok ticks=%u writes=%llu digest=%016llx\n",
        config->ticks,
        (unsigned long long)reference_trace.count,
        (unsigned long long)output_digest(&reference));
    ok = true;

done:
    free(states);
    free(toggles);
    fixture_destroy(&reference);
    fixture_destroy(&cpu);
    return ok;
}

static bool timed_run(const Config *config, bool use_cpu, RunResult *result)
{
    Fixture fixture = {0};
    uint8_t *states = NULL, *toggles = NULL;
    bool ok = false;
    *result = (RunResult){0};
    if (!fixture_build(config, &fixture)) goto done;
    if (use_cpu) {
        double start = runtime_clock_now();
        RsResult backend = sim_use_cpu(fixture.world, fixture.min, fixture.max);
        result->prepare_seconds = runtime_clock_now() - start;
        if (backend != RS_OK) {
            fprintf(stderr, "CPU setup failed: %s\n", rs_result_name(backend));
            goto done;
        }
    }
    states = calloc(fixture.input_count, 1);
    toggles = malloc(fixture.input_count);
    if (!states || !toggles) goto done;
    uint64_t rng = config->seed ? config->seed : UINT64_C(0x9e3779b97f4a7c15);
    sim_reset_stats(fixture.world);
    double start = runtime_clock_now();
    for (uint32_t tick = 0; tick < config->ticks;) {
        generate_toggles(config, toggles, fixture.input_count, &rng);
        if (!apply_toggles(&fixture, states, toggles)) goto done;
        uint32_t remaining = config->ticks - tick;
        uint32_t batch = config->period < remaining ? config->period : remaining;
        sim_advance_ticks(fixture.world, batch);
        tick += batch;
    }
    result->seconds = runtime_clock_now() - start;
    if (!world_ok(fixture.world, "run timed workload")) goto done;
    result->stats = sim_stats(fixture.world);
    result->digest = output_digest(&fixture);
    if (world_tick(fixture.world) != config->ticks) {
        fprintf(stderr, "workload advanced an unexpected number of game ticks\n");
        goto done;
    }
    if (use_cpu && !sim_cpu_active(fixture.world)) {
        fprintf(stderr, "CPU plan became inactive during the timed workload\n");
        goto done;
    }
    if (use_cpu && result->stats.cpu_fallbacks) {
        fprintf(stderr, "CPU workload used %llu fallback queries; refusing the score\n",
            (unsigned long long)result->stats.cpu_fallbacks);
        goto done;
    }
    ok = true;

done:
    free(states);
    free(toggles);
    fixture_destroy(&fixture);
    return ok;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}

static double median(double *values, size_t count)
{
    qsort(values, count, sizeof(*values), compare_double);
    if (count & 1u) return values[count / 2];
    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

static bool work_stats_equal(SimStats a, SimStats b)
{
    return a.neighbor_checks == b.neighbor_checks &&
           a.components_solved == b.components_solved &&
           a.wires_visited == b.wires_visited &&
           a.wire_changes == b.wire_changes &&
           a.rail_components_solved == b.rail_components_solved &&
           a.rails_visited == b.rails_visited &&
           a.rail_changes == b.rail_changes &&
           a.events_processed == b.events_processed &&
           a.pending_events == b.pending_events;
}

int main(int argc, char **argv)
{
    Config config;
    if (!parse_config(argc, argv, &config)) {
        usage(argv[0]);
        return 2;
    }
    printf("CONFIG cell=%s width=%u depth=%u modules=%llu ticks=%u period=%u "
           "pattern=%s seed=%llu rounds=%u verify=%s platform=%s compiler=%s\n",
        cell_name(config.cell), config.width, config.depth,
        (unsigned long long)((uint64_t)config.width * config.depth),
        config.ticks, config.period, pattern_name(config.pattern),
        (unsigned long long)config.seed, config.rounds,
        config.verify ? "yes" : "no", platform_name(),
#ifdef __VERSION__
        __VERSION__
#else
        "unknown"
#endif
    );
    if (config.verify && !verify_equivalence(&config)) return 1;

    double *reference_times = calloc(config.rounds, sizeof(*reference_times));
    double *cpu_times = calloc(config.rounds, sizeof(*cpu_times));
    double *prepare_times = calloc(config.rounds, sizeof(*prepare_times));
    double *speedups = calloc(config.rounds, sizeof(*speedups));
    if (!reference_times || !cpu_times || !prepare_times || !speedups) {
        free(reference_times); free(cpu_times); free(prepare_times); free(speedups);
        return 1;
    }
    SimStats cpu_stats = {0};
    for (uint32_t round = 0; round < config.rounds; ++round) {
        RunResult reference, cpu;
        bool ok;
        if ((round & 1u) == 0) {
            ok = timed_run(&config, false, &reference) &&
                 timed_run(&config, true, &cpu);
        } else {
            ok = timed_run(&config, true, &cpu) &&
                 timed_run(&config, false, &reference);
        }
        if (!ok) {
            free(reference_times); free(cpu_times); free(prepare_times); free(speedups);
            return 1;
        }
        if (reference.digest != cpu.digest) {
            fprintf(stderr,
                "timed final state differs in round %u: reference=%016llx cpu=%016llx\n",
                round + 1,
                (unsigned long long)reference.digest,
                (unsigned long long)cpu.digest);
            free(reference_times); free(cpu_times); free(prepare_times); free(speedups);
            return 1;
        }
        if (!work_stats_equal(reference.stats, cpu.stats)) {
            fprintf(stderr, "reference and CPU work counters differ in round %u\n",
                    round + 1);
            free(reference_times); free(cpu_times); free(prepare_times); free(speedups);
            return 1;
        }
        reference_times[round] = reference.seconds;
        cpu_times[round] = cpu.seconds;
        prepare_times[round] = cpu.prepare_seconds;
        speedups[round] = reference.seconds / cpu.seconds;
        cpu_stats = cpu.stats;
        printf("ROUND index=%u reference_seconds=%.9f cpu_seconds=%.9f "
               "speedup=%.6f cpu_prepare_seconds=%.9f digest=%016llx\n",
            round + 1, reference.seconds, cpu.seconds,
            reference.seconds / cpu.seconds, cpu.prepare_seconds,
            (unsigned long long)reference.digest);
    }

    double reference = median(reference_times, config.rounds);
    double cpu = median(cpu_times, config.rounds);
    double prepare = median(prepare_times, config.rounds);
    double speedup = median(speedups, config.rounds);
    double module_ticks = (double)config.width * config.depth * config.ticks;
    if (reference < 0.25 || cpu < 0.25)
        printf("WARNING median runtime is below 0.25 s; "
               "increase --ticks for a stable record\n");
    printf("RESULT cell=%s width=%u depth=%u modules=%llu ticks=%u period=%u "
           "pattern=%s seed=%llu rounds=%u verify=%s platform=%s "
           "reference_seconds=%.9f cpu_seconds=%.9f speedup=%.6f "
           "reference_gt_per_second=%.3f cpu_gt_per_second=%.3f "
           "reference_module_gt_per_second=%.3f cpu_module_gt_per_second=%.3f "
           "cpu_prepare_seconds=%.9f cpu_evaluations=%llu cpu_cache_hits=%llu "
           "cpu_fallbacks=%llu neighbor_checks=%llu wires_visited=%llu "
           "wire_changes=%llu events_processed=%llu pending_events=%zu\n",
        cell_name(config.cell), config.width, config.depth,
        (unsigned long long)((uint64_t)config.width * config.depth),
        config.ticks, config.period, pattern_name(config.pattern),
        (unsigned long long)config.seed, config.rounds,
        config.verify ? "yes" : "no", platform_name(),
        reference, cpu, speedup,
        config.ticks / reference, config.ticks / cpu,
        module_ticks / reference, module_ticks / cpu, prepare,
        (unsigned long long)cpu_stats.cpu_evaluations,
        (unsigned long long)cpu_stats.cpu_cache_hits,
        (unsigned long long)cpu_stats.cpu_fallbacks,
        (unsigned long long)cpu_stats.neighbor_checks,
        (unsigned long long)cpu_stats.wires_visited,
        (unsigned long long)cpu_stats.wire_changes,
        (unsigned long long)cpu_stats.events_processed,
        cpu_stats.pending_events);

    free(reference_times);
    free(cpu_times);
    free(prepare_times);
    free(speedups);
    return 0;
}
