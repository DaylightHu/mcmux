#include "mcmux.h"

#include "blocks/block_internal.h"
#include "runtime_clock.h"
#include "world_internal.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MCMUX_BENCHMARK_C
#include "benchmark-models.h"
static const SimCModel *selected_c_model;
static const char *backend_name = "compiled C";
#else
static const char *backend_name = "CPU queries";
#endif

typedef enum {
    CSA_WHOLE, CSA_INDEPENDENT, CSA_SHARED
} CsaLayout;
static CsaLayout csa_layout;

static const char *layout_name(CsaLayout layout)
{
    return layout == CSA_SHARED ? "shared" :
           layout == CSA_INDEPENDENT ? "independent" : "whole";
}

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
    BlockPos cell_size;
    uint32_t cell_count, columns;
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

typedef struct {
    uint64_t writes, digest;
    bool ran;
} VerifyResult;

typedef struct {
    const char *name;
    const char *description;
    Config config;
} Scenario;

typedef enum {
    RUN_SUITE,
    RUN_SCENARIO,
    RUN_CUSTOM
} RunMode;

typedef struct {
    RunMode mode;
    const char *scenario_name;
    const char *output_path;
    const char *emit_c_path;
    double c_build_seconds;
    Config custom;
    uint32_t rounds_override;
    uint64_t seed;
    bool verify;
    bool quick;
    bool verbose;
    bool write_report;
    CsaLayout csa_layout;
} Options;

typedef struct {
    Scenario scenario;
    VerifyResult verification;
    double reference_seconds;
    double cpu_seconds;
    double prepare_seconds;
    double paired_speedup;
    double reference_rounds[31];
    double cpu_rounds[31];
    double prepare_rounds[31];
    double speedup_rounds[31];
    SimStats cpu_stats;
    bool short_runtime;
} BenchmarkResult;

typedef struct {
    FILE *copy;
} Report;

static const Scenario scenarios[] = {
    {
        "balanced-repeater",
        "Balanced repeater matrix",
        {CELL_REPEATER, PATTERN_DENSE, 64, 32, 4096, 4, 7, 1, true}
    },
    {
        "parallel-repeater",
        "Extreme parallel repeater bank",
        {CELL_REPEATER, PATTERN_DENSE, 2048, 1, 4096, 4, 7, 1, true}
    },
    {
        "serial-repeater",
        "Extreme serial repeater chain",
        {CELL_REPEATER, PATTERN_DENSE, 1, 1024, 8192, 4, 7, 1, true}
    },
    {
        "sparse-repeater",
        "Sparse repeater matrix",
        {CELL_REPEATER, PATTERN_SPARSE, 64, 64, 32768, 4, 7, 1, true}
    },
    {
        "csa-random",
        "Parallel CSA bank with random inputs",
        {CELL_CSA, PATTERN_RANDOM, 256, 1, 4096, 8, 7, 1, true}
    },
    {
        "csa-dense",
        "Parallel CSA bank with dense inputs",
        {CELL_CSA, PATTERN_DENSE, 256, 1, 4096, 8, 7, 1, true}
    },
};

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
    printf("Run the complete deterministic benchmark suite and produce one\n");
    printf("Markdown report. Default: full profile, all maintained scenarios,\n");
    printf("seven timed pairs and per-tick equivalence verification.\n\n");
    printf("Suite options:\n");
    printf("  --scenario NAME      Run one maintained scenario\n");
    printf("  --list-scenarios     List maintained scenario names\n");
    printf("  --quick              Short smoke-test version of the selected suite\n");
    printf("  --rounds N           Override timed pairs for every selected scenario\n");
    printf("  --csa-layout MODE    whole (default), independent or shared topology\n");
    printf("  --seed N             Override the deterministic input seed\n");
    printf("  --no-verify          Skip the untimed per-tick equivalence pass\n");
    printf("  --verbose            Include every timed pair in the report\n");
    printf("  --output PATH        Report path (default: build/benchmark-report.md)\n");
    printf("  --no-output          Print only; do not save a report file\n\n");
    printf("Custom workload options (select custom mode automatically):\n");
    printf("  --cell repeater|csa  Circuit cell (default: repeater)\n");
    printf("  --width N            Parallel lanes/cells (default: 64)\n");
    printf("  --depth N            Serial stages per lane (default: 32)\n");
    printf("  --ticks N            Measured game ticks (default: 512)\n");
    printf("  --period N           Game ticks between input batches (default: 4)\n");
    printf("  --pattern NAME       dense, sparse or random (default: dense)\n\n");
    printf("  --help               Show this help\n");
}

static void list_scenarios(void)
{
    printf("Maintained benchmark scenarios:\n\n");
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        const Config *config = &scenarios[i].config;
        printf("  %-20s %s (%s, %ux%u, %u gt, %s)\n",
            scenarios[i].name, scenarios[i].description,
            cell_name(config->cell), config->width, config->depth,
            config->ticks, pattern_name(config->pattern));
    }
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

static bool validate_config(const Config *config)
{
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

static bool parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){
        .mode = RUN_SUITE,
        .output_path = "build/benchmark-report.md",
        .custom = {
            .cell = CELL_REPEATER,
            .pattern = PATTERN_DENSE,
            .width = 64,
            .depth = 32,
            .ticks = 512,
            .period = 4,
            .rounds = 7,
            .seed = 1,
            .verify = true,
        },
        .seed = 1,
        .verify = true,
        .write_report = true,
    };
    bool custom = false;
    for (int i = 1; i < argc; ++i) {
        const char *value = NULL;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--list-scenarios") == 0) {
            list_scenarios();
            exit(0);
        } else if (strcmp(argv[i], "--quick") == 0) {
            options->quick = true;
        } else if (strcmp(argv[i], "--csa-layout") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            if (!strcmp(value, "whole")) options->csa_layout = CSA_WHOLE;
            else if (!strcmp(value, "independent")) options->csa_layout = CSA_INDEPENDENT;
            else if (!strcmp(value, "shared")) options->csa_layout = CSA_SHARED;
            else { fprintf(stderr, "unknown CSA layout: %s\n", value); return false; }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            options->verbose = true;
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            options->verify = false;
            options->custom.verify = false;
        } else if (strcmp(argv[i], "--no-output") == 0) {
            options->write_report = false;
        } else if (strcmp(argv[i], "--output") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            options->output_path = value;
            options->write_report = true;
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            options->emit_c_path = value;
        } else if (strcmp(argv[i], "--c-build-seconds") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            char *end;
            errno = 0;
            options->c_build_seconds = strtod(value, &end);
            if (errno || end == value || *end || !isfinite(options->c_build_seconds) ||
                options->c_build_seconds < 0) return false;
        } else if (strcmp(argv[i], "--scenario") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            options->mode = RUN_SCENARIO;
            options->scenario_name = value;
        } else if (strcmp(argv[i], "--cell") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            if (strcmp(value, "repeater") == 0) options->custom.cell = CELL_REPEATER;
            else if (strcmp(value, "csa") == 0) options->custom.cell = CELL_CSA;
            else { fprintf(stderr, "unknown cell: %s\n", value); return false; }
            custom = true;
        } else if (strcmp(argv[i], "--pattern") == 0) {
            if (!option_value(argc, argv, &i, &value)) return false;
            if (strcmp(value, "dense") == 0) options->custom.pattern = PATTERN_DENSE;
            else if (strcmp(value, "sparse") == 0) options->custom.pattern = PATTERN_SPARSE;
            else if (strcmp(value, "random") == 0) options->custom.pattern = PATTERN_RANDOM;
            else { fprintf(stderr, "unknown pattern: %s\n", value); return false; }
            custom = true;
        } else if (strcmp(argv[i], "--width") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &options->custom.width)) return false;
            custom = true;
        } else if (strcmp(argv[i], "--depth") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &options->custom.depth)) return false;
            custom = true;
        } else if (strcmp(argv[i], "--ticks") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &options->custom.ticks)) return false;
            custom = true;
        } else if (strcmp(argv[i], "--period") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &options->custom.period)) return false;
            custom = true;
        } else if (strcmp(argv[i], "--rounds") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u32(value, &options->rounds_override)) return false;
            if (!options->rounds_override) {
                fprintf(stderr, "rounds must be 1..31\n");
                return false;
            }
        } else if (strcmp(argv[i], "--seed") == 0) {
            if (!option_value(argc, argv, &i, &value) ||
                !parse_u64(value, &options->seed)) return false;
            options->custom.seed = options->seed;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return false;
        }
    }
    if (custom && options->mode == RUN_SCENARIO) {
        fprintf(stderr, "--scenario cannot be combined with custom workload options\n");
        return false;
    }
    if (custom && options->quick) {
        fprintf(stderr, "--quick applies to maintained scenarios, not custom workloads\n");
        return false;
    }
    if (options->rounds_override > 31) {
        fprintf(stderr, "rounds must be 1..31\n");
        return false;
    }
    if (custom) {
        options->mode = RUN_CUSTOM;
        if (options->rounds_override)
            options->custom.rounds = options->rounds_override;
        return validate_config(&options->custom);
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
    fixture->cell_size = (BlockPos){(int)info.width,(int)info.height,(int)info.length};
    fixture->cell_count = config->width;
    fixture->columns = columns;
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

static void format_count(char *buffer, size_t size, uint64_t value)
{
    char reverse[32];
    size_t count = 0, digits = 0;
    do {
        if (digits && digits % 3 == 0) reverse[count++] = ',';
        reverse[count++] = (char)('0' + value % 10);
        value /= 10;
        ++digits;
    } while (value && count < sizeof(reverse));
    size_t output = count < size - 1 ? count : size - 1;
    for (size_t i = 0; i < output; ++i) buffer[i] = reverse[count - i - 1];
    buffer[output] = '\0';
}

static void format_time_change(char *buffer, size_t size,
                               double reference, double cpu)
{
    double ratio = cpu / reference;
    if (ratio > 1.0005)
        snprintf(buffer, size, "%.2f%% slower", (ratio - 1.0) * 100.0);
    else if (ratio < 0.9995)
        snprintf(buffer, size, "%.2f%% faster", (1.0 - ratio) * 100.0);
    else
        snprintf(buffer, size, "effectively equal");
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

static RsResult select_region(World *world, BlockPos min, BlockPos max, bool append)
{
#ifdef MCMUX_BENCHMARK_C
    return append ? sim_add_c(world, min, max, selected_c_model) :
                    sim_use_c(world, min, max, selected_c_model);
#else
    return append ? sim_add_cpu(world, min, max) : sim_use_cpu(world, min, max);
#endif
}

static RsResult select_backend(Fixture *fixture)
{
    if (!fixture->cell_count || csa_layout == CSA_WHOLE)
        return select_region(fixture->world, fixture->min, fixture->max, false);
    for (uint32_t cell = 0; cell < fixture->cell_count; ++cell) {
        BlockPos min = {
            fixture->min.x + (int)(cell % fixture->columns) * (fixture->cell_size.x + 4),
            fixture->min.y,
            fixture->min.z + (int)(cell / fixture->columns) * (fixture->cell_size.z + 4)
        };
        BlockPos max = {min.x + fixture->cell_size.x - 1,
                        min.y + fixture->cell_size.y - 1,
                        min.z + fixture->cell_size.z - 1};
        RsResult result = cell && csa_layout == CSA_SHARED ?
            sim_add_cpu_instance(fixture->world, fixture->min, min) :
            select_region(fixture->world, min, max, cell != 0);
        if (result != RS_OK) return result;
    }
    return RS_OK;
}

static bool generated_queries_ran(const World *world)
{
#ifdef MCMUX_BENCHMARK_C
    return sim_c_active(world) && sim_stats(world).c_evaluations > 0;
#else
    (void)world;
    return true;
#endif
}

static bool verify_equivalence(const Config *config, VerifyResult *verification)
{
    Fixture reference = {0}, cpu = {0};
    uint8_t *states = NULL, *toggles = NULL;
    bool ok = false;
    *verification = (VerifyResult){.ran = true};
    if (!fixture_build(config, &reference) || !fixture_build(config, &cpu))
        goto done;
    if (reference.input_count != cpu.input_count ||
        reference.output_count != cpu.output_count) goto done;
    RsResult backend = select_backend(&cpu);
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
    if (!generated_queries_ran(cpu.world)) {
        fprintf(stderr, "generated C queries did not execute during verification\n");
        goto done;
    }
    verification->writes = reference_trace.count;
    verification->digest = output_digest(&reference);
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
        RsResult backend = select_backend(&fixture);
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
    if (use_cpu && !generated_queries_ran(fixture.world)) {
        fprintf(stderr, "generated C queries did not execute during timing\n");
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

static double median_copy(const double *values, size_t count)
{
    double copy[31];
    memcpy(copy, values, count * sizeof(*copy));
    return median(copy, count);
}

static const Scenario *find_scenario(const char *name)
{
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
        if (strcmp(scenarios[i].name, name) == 0) return &scenarios[i];
    return NULL;
}

static void apply_options(Scenario *scenario, const Options *options)
{
    scenario->config.seed = options->seed;
    scenario->config.verify = options->verify;
    if (options->rounds_override)
        scenario->config.rounds = options->rounds_override;
    else if (options->quick)
        scenario->config.rounds = 3;
    if (options->quick) {
        scenario->config.ticks /= 8;
        if (scenario->config.ticks < 512) scenario->config.ticks = 512;
    }
}

static bool run_scenario(const Scenario *scenario, size_t index, size_t count,
                         BenchmarkResult *result)
{
#ifdef MCMUX_BENCHMARK_C
    if (index >= sizeof(mcmux_benchmark_models)/sizeof(*mcmux_benchmark_models))
        return false;
    selected_c_model = mcmux_benchmark_models[index];
#endif
    const Config *config = &scenario->config;
    *result = (BenchmarkResult){.scenario = *scenario};
    if (config->verify) {
        fprintf(stderr, "[%zu/%zu] %-20s verifying", index + 1, count,
                scenario->name);
        fflush(stderr);
        if (!verify_equivalence(config, &result->verification)) {
            fputc('\n', stderr);
            return false;
        }
    }
    for (uint32_t round = 0; round < config->rounds; ++round) {
        fprintf(stderr, "\r[%zu/%zu] %-20s pair %u/%u", index + 1, count,
                scenario->name, round + 1, config->rounds);
        fflush(stderr);
        RunResult reference, cpu;
        bool ok;
        if ((round & 1u) == 0) {
            ok = timed_run(config, false, &reference) &&
                 timed_run(config, true, &cpu);
        } else {
            ok = timed_run(config, true, &cpu) &&
                 timed_run(config, false, &reference);
        }
        if (!ok) {
            fputc('\n', stderr);
            return false;
        }
        if (reference.digest != cpu.digest) {
            fprintf(stderr,
                "\ntimed final state differs in round %u: reference=%016llx cpu=%016llx\n",
                round + 1,
                (unsigned long long)reference.digest,
                (unsigned long long)cpu.digest);
            return false;
        }
        if (!work_stats_equal(reference.stats, cpu.stats)) {
            fprintf(stderr, "\nreference and CPU work counters differ in round %u\n",
                    round + 1);
            return false;
        }
        result->reference_rounds[round] = reference.seconds;
        result->cpu_rounds[round] = cpu.seconds;
        result->prepare_rounds[round] = cpu.prepare_seconds;
        result->speedup_rounds[round] = reference.seconds / cpu.seconds;
        result->cpu_stats = cpu.stats;
    }
    result->reference_seconds = median_copy(result->reference_rounds,
                                             config->rounds);
    result->cpu_seconds = median_copy(result->cpu_rounds, config->rounds);
    result->prepare_seconds = median_copy(result->prepare_rounds,
                                           config->rounds);
    result->paired_speedup = median_copy(result->speedup_rounds,
                                          config->rounds);
    result->short_runtime = result->reference_seconds < 0.25 ||
                            result->cpu_seconds < 0.25;
    fprintf(stderr, "\r[%zu/%zu] %-20s done: %.4fx\n", index + 1, count,
            scenario->name, result->paired_speedup);
    return true;
}

static void report_printf(Report *report, const char *format, ...)
{
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    vprintf(format, args);
    if (report->copy) vfprintf(report->copy, format, copy);
    va_end(copy);
    va_end(args);
}

static void format_duration(char *buffer, size_t size, double seconds)
{
    if (seconds >= 1.0) snprintf(buffer, size, "%.3f s", seconds);
    else snprintf(buffer, size, "%.3f ms", seconds * 1000.0);
}

static double cache_hit_rate(const BenchmarkResult *result)
{
    double queries = (double)result->cpu_stats.cpu_evaluations +
                     result->cpu_stats.cpu_cache_hits;
    return queries ? 100.0 * result->cpu_stats.cpu_cache_hits / queries : 0.0;
}

static void write_report(Report *report, const BenchmarkResult *results,
                         size_t count, const Options *options)
{
    const char *profile = options->mode == RUN_CUSTOM ? "custom" :
                          options->quick ? "quick" :
                          options->mode == RUN_SCENARIO ? "single" : "full";
    report_printf(report, "# mcmux redstone benchmark\n\n");
    report_printf(report, "One command compares the reference and CPU "
                  "backends across every selected workload.\n\n");
    report_printf(report, "| Environment | Value |\n|:--|:--|\n");
    report_printf(report, "| Profile | `%s` |\n", profile);
    report_printf(report, "| Scenarios | %zu |\n", count);
    report_printf(report, "| CPU implementation | %s |\n", backend_name);
    report_printf(report, "| CSA topology layout | `%s` |\n", layout_name(csa_layout));
#ifdef MCMUX_BENCHMARK_C
    report_printf(report, "| AOT generation | %.6f s |\n", mcmux_c_generation_seconds);
    report_printf(report, "| AOT compile + link | %.6f s |\n", options->c_build_seconds);
#endif
    report_printf(report, "| Platform | `%s` |\n", platform_name());
    report_printf(report, "| Compiler | `%s` |\n",
#ifdef __VERSION__
                  __VERSION__
#else
                  "unknown"
#endif
    );
    report_printf(report, "| Correctness | **%s** |\n",
                  options->verify ? "PASS" : "SKIPPED");

    report_printf(report, "\n## Performance summary\n\n");
    report_printf(report, "| Scenario | Reference | CPU | CPU outcome | Paired speedup | Cache hit rate |\n");
    report_printf(report, "|:--|--:|--:|:--|--:|--:|\n");
    for (size_t i = 0; i < count; ++i) {
        char reference[32], cpu[32], change[48];
        format_duration(reference, sizeof(reference), results[i].reference_seconds);
        format_duration(cpu, sizeof(cpu), results[i].cpu_seconds);
        format_time_change(change, sizeof(change), results[i].reference_seconds,
                           results[i].cpu_seconds);
        report_printf(report, "| `%s` | %s | %s | **%s** | **%.4fx** | %.2f%% |\n",
            results[i].scenario.name, reference, cpu, change,
            results[i].paired_speedup, cache_hit_rate(&results[i]));
    }

    report_printf(report, "\n## Workloads\n\n");
    report_printf(report, "| Scenario | Circuit | Layout | Pattern | Game ticks | Period | Pairs |\n");
    report_printf(report, "|:--|:--|--:|:--|--:|--:|--:|\n");
    for (size_t i = 0; i < count; ++i) {
        const Config *config = &results[i].scenario.config;
        report_printf(report, "| `%s` | `%s` | %u x %u | `%s` | %u | %u gt | %u |\n",
            results[i].scenario.name, cell_name(config->cell), config->width,
            config->depth, pattern_name(config->pattern), config->ticks,
            config->period, config->rounds);
    }

    report_printf(report, "\n## Correctness\n\n");
    report_printf(report, "| Scenario | Status | Ordered state writes | Digest |\n");
    report_printf(report, "|:--|:--:|--:|:--|\n");
    for (size_t i = 0; i < count; ++i) {
        char writes[32];
        format_count(writes, sizeof(writes), results[i].verification.writes);
        if (results[i].verification.ran)
            report_printf(report, "| `%s` | **PASS** | %s | `%016llx` |\n",
                results[i].scenario.name, writes,
                (unsigned long long)results[i].verification.digest);
        else
            report_printf(report, "| `%s` | SKIPPED | - | - |\n",
                results[i].scenario.name);
    }

    report_printf(report, "\n## CPU backend\n\n");
#ifdef MCMUX_BENCHMARK_C
    report_printf(report, "Generated C uses the same cache and runtime as CPU queries. "
        "Preparation below is model attachment only; break-even excludes AOT generation "
        "and compilation (reported above).\n\n");
    report_printf(report, "| Scenario | Generated query evaluations |\n|:--|--:|\n");
    for (size_t i = 0; i < count; ++i)
        report_printf(report, "| `%s` | %llu |\n", results[i].scenario.name,
            (unsigned long long)results[i].cpu_stats.c_evaluations);
    report_printf(report, "\n");
#endif
    report_printf(report, "| Scenario | Preparation | Computed | Cache hits | Fallbacks | Break-even |\n");
    report_printf(report, "|:--|--:|--:|--:|--:|--:|\n");
    for (size_t i = 0; i < count; ++i) {
        char computed[32], hits[32], fallbacks[32], break_even[32];
        format_count(computed, sizeof(computed), results[i].cpu_stats.cpu_evaluations);
        format_count(hits, sizeof(hits), results[i].cpu_stats.cpu_cache_hits);
        format_count(fallbacks, sizeof(fallbacks), results[i].cpu_stats.cpu_fallbacks);
        if (results[i].cpu_seconds < results[i].reference_seconds)
            snprintf(break_even, sizeof(break_even), "%.2f windows",
                results[i].prepare_seconds /
                (results[i].reference_seconds - results[i].cpu_seconds));
        else
            snprintf(break_even, sizeof(break_even), "not reached");
        report_printf(report, "| `%s` | %.3f ms | %s | %s | **%s** | %s |\n",
            results[i].scenario.name, results[i].prepare_seconds * 1000.0,
            computed, hits, fallbacks, break_even);
    }

    report_printf(report, "\n## Simulation work\n\n");
    report_printf(report, "| Scenario | Neighbor checks | Wires visited | Wire changes | Events | Pending |\n");
    report_printf(report, "|:--|--:|--:|--:|--:|--:|\n");
    for (size_t i = 0; i < count; ++i) {
        char neighbors[32], wires[32], changes[32], events[32], pending[32];
        format_count(neighbors, sizeof(neighbors), results[i].cpu_stats.neighbor_checks);
        format_count(wires, sizeof(wires), results[i].cpu_stats.wires_visited);
        format_count(changes, sizeof(changes), results[i].cpu_stats.wire_changes);
        format_count(events, sizeof(events), results[i].cpu_stats.events_processed);
        format_count(pending, sizeof(pending), results[i].cpu_stats.pending_events);
        report_printf(report, "| `%s` | %s | %s | %s | %s | %s |\n",
            results[i].scenario.name, neighbors, wires, changes, events, pending);
    }

    bool warned = false;
    for (size_t i = 0; i < count; ++i) warned |= results[i].short_runtime;
    if (warned)
        report_printf(report, "\n> **Timing note:** one or more median runtimes are below "
                      "0.25 seconds; use the full profile for stable records.\n");

    if (options->verbose) {
        report_printf(report, "\n## Timed pairs\n");
        for (size_t i = 0; i < count; ++i) {
            const Config *config = &results[i].scenario.config;
            report_printf(report, "\n### `%s`\n\n", results[i].scenario.name);
            report_printf(report, "| Pair | Reference | CPU | Speedup | Preparation |\n");
            report_printf(report, "|--:|--:|--:|--:|--:|\n");
            for (uint32_t round = 0; round < config->rounds; ++round)
                report_printf(report, "| %u | %.3f ms | %.3f ms | %.4fx | %.3f ms |\n",
                    round + 1, results[i].reference_rounds[round] * 1000.0,
                    results[i].cpu_rounds[round] * 1000.0,
                    results[i].speedup_rounds[round],
                    results[i].prepare_rounds[round] * 1000.0);
        }
    }

    report_printf(report, "\n## Machine-readable results\n\n```text\n");
    for (size_t i = 0; i < count; ++i) {
        const Config *config = &results[i].scenario.config;
        double module_ticks = (double)config->width * config->depth * config->ticks;
        report_printf(report,
            "RESULT scenario=%s cell=%s width=%u depth=%u modules=%llu ticks=%u "
            "period=%u pattern=%s seed=%llu rounds=%u verify=%s platform=%s "
            "cpu_layout=%s reference_seconds=%.9f cpu_seconds=%.9f speedup=%.6f "
            "reference_gt_per_second=%.3f cpu_gt_per_second=%.3f "
            "reference_module_gt_per_second=%.3f cpu_module_gt_per_second=%.3f "
            "cpu_prepare_seconds=%.9f cpu_evaluations=%llu cpu_cache_hits=%llu "
            "cpu_fallbacks=%llu neighbor_checks=%llu wires_visited=%llu "
            "wire_changes=%llu events_processed=%llu pending_events=%zu "
            "c_evaluations=%llu\n",
            results[i].scenario.name, cell_name(config->cell), config->width,
            config->depth,
            (unsigned long long)((uint64_t)config->width * config->depth),
            config->ticks, config->period, pattern_name(config->pattern),
            (unsigned long long)config->seed, config->rounds,
            config->verify ? "yes" : "no", platform_name(),
            config->cell == CELL_CSA ? layout_name(csa_layout) : "whole",
            results[i].reference_seconds, results[i].cpu_seconds,
            results[i].paired_speedup,
            config->ticks / results[i].reference_seconds,
            config->ticks / results[i].cpu_seconds,
            module_ticks / results[i].reference_seconds,
            module_ticks / results[i].cpu_seconds,
            results[i].prepare_seconds,
            (unsigned long long)results[i].cpu_stats.cpu_evaluations,
            (unsigned long long)results[i].cpu_stats.cpu_cache_hits,
            (unsigned long long)results[i].cpu_stats.cpu_fallbacks,
            (unsigned long long)results[i].cpu_stats.neighbor_checks,
            (unsigned long long)results[i].cpu_stats.wires_visited,
            (unsigned long long)results[i].cpu_stats.wire_changes,
            (unsigned long long)results[i].cpu_stats.events_processed,
            results[i].cpu_stats.pending_events,
            (unsigned long long)results[i].cpu_stats.c_evaluations);
    }
    report_printf(report, "SUITE_RESULT profile=%s scenarios=%zu verify=%s\n```\n",
                  profile, count, options->verify ? "yes" : "no");
}

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }

    csa_layout = options.csa_layout;
    Scenario selected[sizeof(scenarios) / sizeof(scenarios[0])];
    size_t selected_count = 0;
    if (options.mode == RUN_SUITE) {
        selected_count = sizeof(scenarios) / sizeof(scenarios[0]);
        memcpy(selected, scenarios, sizeof(scenarios));
    } else if (options.mode == RUN_SCENARIO) {
        const Scenario *scenario = find_scenario(options.scenario_name);
        if (!scenario) {
            fprintf(stderr, "unknown scenario: %s\n", options.scenario_name);
            list_scenarios();
            return 2;
        }
        selected[selected_count++] = *scenario;
    } else {
        selected[selected_count++] = (Scenario){
            "custom", "Custom workload", options.custom
        };
    }

    for (size_t i = 0; i < selected_count; ++i) {
        apply_options(&selected[i], &options);
        if (!validate_config(&selected[i].config)) return 2;
    }

    if (options.emit_c_path) {
        FILE *out = fopen(options.emit_c_path, "w");
        if (!out) return 1;
        bool ok = true;
        double start = runtime_clock_now();
        for (size_t i = 0; i < selected_count && ok; ++i) {
            Fixture fixture = {0};
            Config prototype = selected[i].config;
            if (prototype.cell == CELL_CSA && csa_layout != CSA_WHOLE)
                prototype.width = 1;
            ok = fixture_build(&prototype, &fixture);
            if (ok) {
                char symbol[64];
                snprintf(symbol, sizeof(symbol), "mcmux_bench_%zu", i);
                RsResult result = sim_write_c(fixture.world, fixture.min,
                                               fixture.max, out, symbol);
                ok = result == RS_OK;
                if (!ok) fprintf(stderr, "C export failed: %s\n", rs_result_name(result));
            }
            fixture_destroy(&fixture);
        }
        double seconds = runtime_clock_now() - start;
        fputs("\nstatic const SimCModel *const mcmux_benchmark_models[] = {\n", out);
        for (size_t i = 0; i < selected_count; ++i)
            fprintf(out, "    &mcmux_bench_%zu,\n", i);
        fprintf(out, "};\nstatic const double mcmux_c_generation_seconds = %.9f;\n", seconds);
        if (ferror(out)) ok = false;
        if (fclose(out) != 0) ok = false;
        if (!ok) (void)remove(options.emit_c_path);
        return ok ? 0 : 1;
    }

    BenchmarkResult results[sizeof(scenarios) / sizeof(scenarios[0])];
    for (size_t i = 0; i < selected_count; ++i)
        if (!run_scenario(&selected[i], i, selected_count, &results[i])) return 1;

    FILE *report_file = NULL;
    if (options.write_report) {
        report_file = fopen(options.output_path, "w");
        if (!report_file) {
            fprintf(stderr, "cannot write benchmark report: %s\n", options.output_path);
            return 1;
        }
    }
    Report report = {.copy = report_file};
    write_report(&report, results, selected_count, &options);
    fflush(stdout);
    if (report_file && fclose(report_file) != 0) {
        fprintf(stderr, "cannot finish benchmark report: %s\n", options.output_path);
        return 1;
    }
    if (options.write_report)
        fprintf(stderr, "Benchmark report written to %s\n", options.output_path);
    return 0;
}
