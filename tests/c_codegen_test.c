#include "mcmux.h"
#include "redstone_c.h"
#include "sim_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MCMUX_TEST_C
#include "test-c-models.h"
#endif

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static const BlockPos MIN = {6,0,4}, MAX = {14,1,18};

static void put(World *world, int x, int y, int z, BlockId kind, Direction facing)
{
    BlockState state = block_default(kind);
    if (kind == BLOCK_REDSTONE_REPEATER) state.data.repeater.facing = facing;
    if (kind == BLOCK_REDSTONE_COMPARATOR) state.data.comparator.facing = facing;
    if (kind == BLOCK_LEVER) {
        state.data.lever.support_dir = DIR_DOWN;
        state.data.lever.facing = DIR_EAST;
    }
    REQUIRE(world_set_block(world, (BlockPos){x,y,z}, state, UPDATE_CLIENTS));
}

static World *fixture(void)
{
    World *world = world_create((WorldConfig){0});
    REQUIRE(world);
    REQUIRE(sim_set_updates_enabled(world, false) == RS_OK);
    for (int x = MIN.x; x <= MAX.x; ++x)
        for (int z = MIN.z; z <= MAX.z; ++z)
            put(world, x, 0, z, BLOCK_IRON, DIR_EAST);
    put(world, 8,1,8, BLOCK_LEVER, DIR_EAST);
    put(world, 9,1,8, BLOCK_REDSTONE_REPEATER, DIR_EAST);
    put(world, 10,1,8, BLOCK_REDSTONE_REPEATER, DIR_EAST);
    put(world, 11,1,8, BLOCK_IRON, DIR_EAST);
    put(world, 9,1,6, BLOCK_LEVER, DIR_EAST);
    put(world, 9,1,7, BLOCK_REDSTONE_REPEATER, DIR_SOUTH);
    put(world, 8,1,16, BLOCK_LEVER, DIR_EAST);
    put(world, 9,1,16, BLOCK_IRON, DIR_EAST);
    put(world, 10,1,16, BLOCK_REDSTONE_COMPARATOR, DIR_EAST);
    put(world, 11,1,16, BLOCK_IRON, DIR_EAST);
    put(world, 10,1,14, BLOCK_LEVER, DIR_EAST);
    put(world, 10,1,15, BLOCK_REDSTONE_REPEATER, DIR_SOUTH);
    REQUIRE(sim_set_updates_enabled(world, true) == RS_OK);
    REQUIRE(world_result(world) == RS_OK);
    return world;
}

/* Synthetic valid query plans exercise every source face, channel, wire
 * suppression, attenuation, conductor relay and explicit unsupported case.
 * They test query semantics, not physical world construction. */
static RedstonePlan rules(void)
{
    RedstonePlan plan = {0};
    plan.nodes = calloc(2048, sizeof(*plan.nodes));
    plan.inputs = calloc(8192, sizeof(*plan.inputs));
    REQUIRE(plan.nodes && plan.inputs);
    const BlockId sources[] = {BLOCK_REDSTONE_BLOCK, BLOCK_REDSTONE_WIRE,
        BLOCK_REDSTONE_TORCH, BLOCK_REDSTONE_REPEATER, BLOCK_REDSTONE_COMPARATOR,
        BLOCK_OBSERVER, BLOCK_LEVER, BLOCK_IRON, BLOCK_SLIME, BLOCK_BARREL,
        BLOCK_REDSTONE_BLOCK, BLOCK_GLASS};
    for (size_t i = 0; i < sizeof(sources)/sizeof(*sources); ++i) {
        RedstonePlanNode *node = &plan.nodes[plan.node_count++];
        node->initial_state = block_default(sources[i]);
        node->pos = (BlockPos){(int)i,2,0};
        if (sources[i] == BLOCK_REDSTONE_WIRE) {
            node->initial_state.data.wire.connections =
                (1u << DIR_EAST) | (1u << DIR_SOUTH);
            node->wire_output_faces = (1u << DIR_WEST) | (1u << DIR_NORTH);
        }
        if (sources[i] == BLOCK_REDSTONE_TORCH)
            node->initial_state.data.torch.support_dir = DIR_DOWN;
        if (sources[i] == BLOCK_LEVER) {
            node->initial_state.data.lever.support_dir = DIR_DOWN;
            node->initial_state.data.lever.facing = DIR_EAST;
        }
        if (sources[i] == BLOCK_IRON || sources[i] == BLOCK_SLIME)
            node->flags = REDSTONE_NODE_CONDUCTOR;
    }
    plan.nodes[10].flags = REDSTONE_NODE_BOUNDARY;
    plan.nodes[7].first_input = (uint32_t)plan.input_count;
    plan.inputs[plan.input_count++] = (RedstonePlanInput){2,REDSTONE_INPUT_DIRECT,DIR_UP,0};
    plan.inputs[plan.input_count++] = (RedstonePlanInput){6,REDSTONE_INPUT_DIRECT,DIR_DOWN,0};
    plan.nodes[7].input_count = 2;
    plan.nodes[8].first_input = (uint32_t)plan.input_count;
    plan.inputs[plan.input_count++] = (RedstonePlanInput){7,REDSTONE_INPUT_DIRECT,DIR_EAST,0};
    plan.inputs[plan.input_count++] = (RedstonePlanInput){1,REDSTONE_INPUT_DIRECT,DIR_WEST,0};
    plan.nodes[8].input_count = 2;
    const BlockId targets[] = {BLOCK_REDSTONE_WIRE, BLOCK_REDSTONE_TORCH,
        BLOCK_REDSTONE_REPEATER, BLOCK_REDSTONE_COMPARATOR, BLOCK_IRON,
        BLOCK_SLIME, BLOCK_TARGET};
    const uint8_t attenuation[] = {0,1,15};
    for (size_t t = 0; t < sizeof(targets)/sizeof(*targets); ++t) {
        for (size_t s = 0; s < sizeof(sources)/sizeof(*sources); ++s) {
            for (unsigned face = 0; face <= DIR_COUNT; ++face) {
                for (unsigned kind = 0; kind <= REDSTONE_INPUT_DIRECT; ++kind) {
                    size_t id = plan.node_count++;
                    REQUIRE(id < 2048 && plan.input_count + 2 < 8192);
                    RedstonePlanNode *node = &plan.nodes[id];
                    node->pos = (BlockPos){(int)id,2,0};
                    node->initial_state = block_default(targets[t]);
                    node->first_input = (uint32_t)plan.input_count;
                    node->input_count = 2;
                    plan.inputs[plan.input_count++] = (RedstonePlanInput){
                        (uint32_t)s, (uint8_t)kind, (uint8_t)face,
                        attenuation[(face + kind) % 3]};
                    plan.inputs[plan.input_count++] = (RedstonePlanInput){
                        1, kind == REDSTONE_INPUT_SIDE ? REDSTONE_INPUT_DEFAULT : REDSTONE_INPUT_SIDE,
                        DIR_COUNT, 1};
                }
            }
        }
    }
    /* Equal relative query bodies must still read different instance states. */
    for (unsigned pair = 0; pair < 2; ++pair) {
        size_t source = plan.node_count++;
        plan.nodes[source].initial_state = block_default(BLOCK_REDSTONE_WIRE);
        plan.nodes[source].pos = (BlockPos){(int)source,2,0};
        RedstonePlanNode *target = &plan.nodes[plan.node_count++];
        target->initial_state = block_default(BLOCK_REDSTONE_TORCH);
        target->pos = (BlockPos){(int)source + 1,2,0};
        target->first_input = (uint32_t)plan.input_count;
        target->input_count = 1;
        plan.inputs[plan.input_count++] = (RedstonePlanInput){
            (uint32_t)source, REDSTONE_INPUT_DEFAULT, DIR_DOWN, 0};
    }
    for (unsigned flags = 0; flags < 3; ++flags) {
        RedstonePlanNode *node = &plan.nodes[plan.node_count++];
        node->initial_state = block_default(BLOCK_REDSTONE_REPEATER);
        node->first_input = (uint32_t)plan.input_count;
        node->flags = flags == 0 ? REDSTONE_NODE_BOUNDARY :
                      flags == 1 ? REDSTONE_NODE_RUNTIME : REDSTONE_NODE_INPUT_FALLBACK;
    }
    return plan;
}

static int emit(const char *path)
{
    World *world = fixture();
    FILE *out = fopen(path, "w");
    REQUIRE(out);
    uint64_t tick = world_tick(world), revision = world_revision(world);
    REQUIRE(sim_write_c(world, MIN, MAX, out, "test_live") == RS_OK);
    REQUIRE(world_tick(world) == tick && world_revision(world) == revision);
    REQUIRE(!sim_cpu_active(world));
    REQUIRE(sim_write_c(world, (BlockPos){9,0,4}, MAX, out, "test_boundary") == RS_OK);
    RedstonePlan plan = rules();
    REQUIRE(redstone_c_write(&plan, out, "test_rules") == RS_OK);
    redstone_plan_destroy(&plan);
    World *empty = world_create((WorldConfig){0});
    REQUIRE(empty);
    REQUIRE(sim_write_c(empty, MIN, MAX, out, "test_empty") == RS_OK);
    REQUIRE(fclose(out) == 0);
    world_destroy(empty);
    world_destroy(world);
    return 0;
}

#ifdef MCMUX_TEST_C
static bool evaluation_equal(SimCEvaluation a, SimCEvaluation b)
{
    return a.input == b.input && a.side == b.side && a.target_power == b.target_power &&
           a.target_on == b.target_on && a.locked == b.locked;
}

static void query_tests(void)
{
    RedstonePlan plan = rules();
    REQUIRE(redstone_c_matches(&plan, &test_rules));
    RedstoneState state = {0};
    REQUIRE(redstone_state_build(&plan, &state) == RS_OK);
    uint32_t random = 1234567;
    size_t supported = 0, unsupported = 0;
    for (unsigned round = 0; round < 128; ++round) {
        for (size_t i = 0; i < state.node_count; ++i) {
            random = random*1664525u + 1013904223u;
            BlockState block = state.blocks[i];
            switch ((BlockId)block.id) {
            case BLOCK_REDSTONE_WIRE: block.data.wire.power = (uint8_t)(random % 16); break;
            case BLOCK_REDSTONE_TORCH: block.data.torch.lit = (random >> 16) & 1u; break;
            case BLOCK_REDSTONE_REPEATER:
                block.data.repeater.powered = (random >> 16) & 1u;
                block.data.repeater.delay = (uint8_t)(1 + random % 4);
                block.data.repeater.locked = (random >> 17) & 1u;
                break;
            case BLOCK_REDSTONE_COMPARATOR:
                block.data.comparator.output = (uint8_t)(random % 16);
                block.data.comparator.powered = (random >> 16) & 1u;
                block.data.comparator.subtract = (random >> 17) & 1u;
                break;
            case BLOCK_OBSERVER: block.data.observer.powered = (random >> 16) & 1u; break;
            case BLOCK_LEVER: block.data.lever.powered = (random >> 16) & 1u; break;
            default: break;
            }
            RsResult result = redstone_state_set(&state, (RedstoneNodeId)i, block);
            REQUIRE(result == RS_OK || result == RS_UNCHANGED);
        }
        for (size_t i = 0; i < state.node_count; ++i) {
            SimCEvaluation sentinel = {3,4,5,true,true}, expected = sentinel, actual = sentinel;
            RsResult a = redstone_evaluate(&state, (RedstoneNodeId)i, &expected);
            RsResult b = test_rules.evaluate(state.blocks, (uint32_t)i, &actual);
            REQUIRE(a == b && evaluation_equal(expected, actual));
            if (a == RS_OK) ++supported;
            else { REQUIRE(a == RS_UNSUPPORTED); ++unsupported; }
        }
    }
    REQUIRE(supported && unsupported);
    REQUIRE(test_rules.evaluate(state.blocks, UINT32_MAX, NULL) == RS_INVALID_ARGUMENT);
    printf("query equivalence: %zu supported, %zu explicit unsupported results\n",
           supported, unsupported);
    redstone_state_destroy(&state);
    redstone_plan_destroy(&plan);
}

typedef struct {
    BlockPos pos;
    StateId previous, current;
} Write;
typedef struct { Write *items; size_t count, capacity; } Trace;
static void observe(void *context, BlockPos pos, BlockState previous, BlockState current)
{
    Trace *trace = context;
    if (trace->count == trace->capacity) {
        trace->capacity = trace->capacity ? trace->capacity*2 : 64;
        trace->items = realloc(trace->items, trace->capacity*sizeof(*trace->items));
        REQUIRE(trace->items);
    }
    Write *write = &trace->items[trace->count++];
    write->pos = pos;
    REQUIRE(state_encode(previous, &write->previous));
    REQUIRE(state_encode(current, &write->current));
}

static void compare(World *a, World *b, Trace *ta, Trace *tb)
{
    REQUIRE(world_result(a) == RS_OK && world_result(b) == RS_OK);
    REQUIRE(world_tick(a) == world_tick(b) && world_revision(a) == world_revision(b));
    REQUIRE(ta->count == tb->count);
    for (size_t i = 0; i < ta->count; ++i) {
        Write x = ta->items[i], y = tb->items[i];
        REQUIRE(x.pos.x == y.pos.x && x.pos.y == y.pos.y && x.pos.z == y.pos.z &&
                x.previous == y.previous && x.current == y.current);
    }
    ta->count = tb->count = 0;
    for (int x = MIN.x; x <= MAX.x; ++x)
        for (int z = MIN.z; z <= MAX.z; ++z)
            for (int y = MIN.y; y <= MAX.y; ++y) {
                StateId first, second;
                REQUIRE(state_encode(world_get(a, (BlockPos){x,y,z}), &first));
                REQUIRE(state_encode(world_get(b, (BlockPos){x,y,z}), &second));
                REQUIRE(first == second);
            }
    /* Equal visible states alone do not ensure equal future behavior. */
    Scheduler *sa = &a->sim->scheduler, *sb = &b->sim->scheduler;
    REQUIRE(sa->count == sb->count && sa->next_sequence == sb->next_sequence);
    for (size_t i = 0; i < sa->count; ++i) {
        ScheduledEvent x = sa->heap[i], y = sb->heap[i];
        REQUIRE(x.due == y.due && x.sequence == y.sequence && x.block == y.block &&
            x.priority == y.priority && x.pos.x == y.pos.x && x.pos.y == y.pos.y &&
            x.pos.z == y.pos.z);
    }
}

enum { SECOND_OFFSET = 40 };

static World *pair_fixture(void)
{
    World *world = fixture();
    REQUIRE(sim_set_updates_enabled(world, false) == RS_OK);
    for (int x = MIN.x; x <= MAX.x; ++x)
        for (int y = MIN.y; y <= MAX.y; ++y)
            for (int z = MIN.z; z <= MAX.z; ++z) {
                BlockPos source = {x,y,z};
                BlockState state = world_get(world, source);
                if (state.id != BLOCK_AIR)
                    REQUIRE(world_set_block(world,
                        (BlockPos){x + SECOND_OFFSET,y,z}, state, UPDATE_CLIENTS));
            }
    REQUIRE(sim_set_updates_enabled(world, true) == RS_OK);
    return world;
}

static void pair_tests(bool shared)
{
    static const BlockPos INPUTS[] = {{8,1,8}, {9,1,6}, {8,1,16}, {10,1,14}};
    BlockPos min = {MIN.x + SECOND_OFFSET, MIN.y, MIN.z};
    BlockPos max = {MAX.x + SECOND_OFFSET, MAX.y, MAX.z};
    World *reference = pair_fixture(), *compiled = pair_fixture();
    REQUIRE(sim_use_c(compiled, MIN, MAX, &test_live) == RS_OK);
    REQUIRE((shared ? sim_add_cpu_instance(compiled, MIN, min) :
             sim_add_c(compiled, min, max, &test_live)) == RS_OK);
    REQUIRE(sim_add_c(compiled, MIN, MAX, &test_live) == RS_INVALID_ARGUMENT);
    Trace a = {0}, b = {0};
    world_set_change_observer(reference, observe, &a);
    world_set_change_observer(compiled, observe, &b);
    for (unsigned tick = 0; tick < 128; ++tick) {
        for (unsigned instance = 0; instance < 2; ++instance) {
            unsigned i = (tick + instance) % 4;
            BlockPos pos = INPUTS[i];
            pos.x += instance * SECOND_OFFSET;
            bool value = ((tick / (i + 1)) >> instance) & 1u;
            world_set_lever_powered(reference, pos, value);
            world_set_lever_powered(compiled, pos, value);
            compare(reference, compiled, &a, &b);
        }
        if (tick % 13 == 0) {
            BlockPos pos = {10 + (tick / 13 % 2) * SECOND_OFFSET, 1, 16};
            REQUIRE(world_toggle_comparator_mode(reference, pos) ==
                    world_toggle_comparator_mode(compiled, pos));
        }
        sim_advance_ticks(reference, 1);
        sim_advance_ticks(compiled, 1);
        compare(reference, compiled, &a, &b);
        for (int x = min.x; x <= max.x; ++x)
            for (int z = min.z; z <= max.z; ++z)
                for (int y = min.y; y <= max.y; ++y) {
                    StateId first, second;
                    REQUIRE(state_encode(world_get(reference, (BlockPos){x,y,z}), &first));
                    REQUIRE(state_encode(world_get(compiled, (BlockPos){x,y,z}), &second));
                    REQUIRE(first == second);
                }
    }
    REQUIRE(sim_stats(compiled).c_evaluations > 0);
    BlockPos removed = {10,1,8};
    world_remove(reference, removed);
    world_remove(compiled, removed);
    compare(reference, compiled, &a, &b);
    REQUIRE(sim_c_active(compiled)); /* Second instance is still attached. */
    sim_reset_stats(compiled);
    BlockPos second_input = {8 + SECOND_OFFSET,1,8};
    world_set_lever_powered(reference, second_input, false);
    world_set_lever_powered(compiled, second_input, false);
    compare(reference, compiled, &a, &b);
    world_set_lever_powered(reference, second_input, true);
    world_set_lever_powered(compiled, second_input, true);
    sim_advance_ticks(reference, 4);
    sim_advance_ticks(compiled, 4);
    compare(reference, compiled, &a, &b);
    REQUIRE(sim_stats(compiled).c_evaluations > 0);
    free(a.items); free(b.items);
    world_destroy(reference); world_destroy(compiled);

    /* Regions may also choose different query backends. */
    reference = pair_fixture(); compiled = pair_fixture();
    REQUIRE(sim_use_c(compiled, MIN, MAX, &test_live) == RS_OK);
    REQUIRE(sim_add_cpu(compiled, min, max) == RS_OK);
    a = (Trace){0}; b = (Trace){0};
    world_set_change_observer(reference, observe, &a);
    world_set_change_observer(compiled, observe, &b);
    world_set_lever_powered(reference, (BlockPos){8 + SECOND_OFFSET,1,8}, true);
    world_set_lever_powered(compiled, (BlockPos){8 + SECOND_OFFSET,1,8}, true);
    sim_advance_ticks(reference, 4);
    sim_advance_ticks(compiled, 4);
    compare(reference, compiled, &a, &b);
    REQUIRE(sim_cpu_active(compiled) && sim_c_active(compiled));
    REQUIRE(sim_stats(compiled).cpu_evaluations > 0);
    free(a.items); free(b.items);
    world_destroy(reference); world_destroy(compiled);
}

static void instance_attachment_tests(void)
{
    const BlockPos min = {MIN.x + SECOND_OFFSET, MIN.y, MIN.z};
    World *world = pair_fixture();
    REQUIRE(sim_use_c(world, MIN, MAX, &test_live) == RS_OK);
    uint64_t tick = world_tick(world), revision = world_revision(world);
    size_t events = sim_stats(world).pending_events;
    REQUIRE(sim_add_cpu_instance(world, MIN, MIN) == RS_INVALID_ARGUMENT);
    REQUIRE(sim_add_cpu_instance(world, MIN, (BlockPos){INT_MAX,0,0}) == RS_OUT_OF_BOUNDS);
    REQUIRE(sim_add_cpu_instance(world, (BlockPos){0,0,0}, min) == RS_UNSUPPORTED);
    REQUIRE(world_tick(world) == tick && world_revision(world) == revision &&
            sim_stats(world).pending_events == events && sim_c_active(world));

    /* An extra block in template air, and an external dependency two cells
     * away, must both reject an instance without disabling its prototype. */
    BlockPos extra = {MIN.x + SECOND_OFFSET,1,MIN.z};
    REQUIRE(world_set_block(world, extra, block_default(BLOCK_IRON), UPDATE_CLIENTS));
    REQUIRE(sim_add_cpu_instance(world, MIN, min) == RS_UNSUPPORTED);
    REQUIRE(sim_c_active(world));
    world_remove(world, extra);
    BlockPos halo = {MAX.x + SECOND_OFFSET + 2,1,MAX.z};
    REQUIRE(world_set_block(world, halo, block_default(BLOCK_IRON), UPDATE_CLIENTS));
    REQUIRE(sim_add_cpu_instance(world, MIN, min) == RS_UNSUPPORTED);
    world_remove(world, halo);
    REQUIRE(sim_add_cpu_instance(world, MIN, min) == RS_OK);

    /* Reusing a shared instance as the next prototype retains the same graph.
     * Build the next translated fixture before attempting its attachment. */
    REQUIRE(sim_set_updates_enabled(world, false) == RS_OK);
    for (int x = MIN.x; x <= MAX.x; ++x)
        for (int y = MIN.y; y <= MAX.y; ++y)
            for (int z = MIN.z; z <= MAX.z; ++z) {
                BlockState state = world_get(world, (BlockPos){x,y,z});
                if (state.id != BLOCK_AIR)
                    REQUIRE(world_set_block(world,
                        (BlockPos){x + 2 * SECOND_OFFSET,y,z}, state, UPDATE_CLIENTS));
            }
    REQUIRE(sim_set_updates_enabled(world, true) == RS_OK);
    REQUIRE(sim_add_cpu_instance(world, min,
        (BlockPos){MIN.x + 2 * SECOND_OFFSET,MIN.y,MIN.z}) == RS_OK);
    BlockPos source_halo = {MAX.x + 2,1,MAX.z};
    REQUIRE(world_set_block(world, source_halo, block_default(BLOCK_IRON), UPDATE_CLIENTS));
    REQUIRE(sim_add_cpu_instance(world, MIN, (BlockPos){MIN.x + 120,0,MIN.z}) == RS_UNSUPPORTED);
    REQUIRE(sim_c_active(world));
    REQUIRE(sim_use_reference(world) == RS_OK);
    REQUIRE(!sim_cpu_active(world));
    world_destroy(world);

    /* Empty shared graphs still have a lifetime and an independently checked
     * footprint. No zero-sized allocations may be treated as a failure. */
    world = world_create((WorldConfig){0});
    REQUIRE(world && sim_use_cpu(world, MIN, MAX) == RS_OK);
    REQUIRE(sim_add_cpu_instance(world, MIN, min) == RS_OK);
    REQUIRE(sim_cpu_active(world));
    world_destroy(world);

    /* Translation across zero and to a different height uses the same node
     * indices, while loading the destination's own dynamic block states. */
    world = fixture();
    REQUIRE(world_expand_plot(world, DIR_WEST, 1) == RS_OK);
    REQUIRE(sim_set_updates_enabled(world, false) == RS_OK);
    const BlockPos shift = {-40,-16,32};
    for (int x = MIN.x; x <= MAX.x; ++x)
        for (int y = MIN.y; y <= MAX.y; ++y)
            for (int z = MIN.z; z <= MAX.z; ++z) {
                BlockState state = world_get(world, (BlockPos){x,y,z});
                if (state.id != BLOCK_AIR)
                    REQUIRE(world_set_block(world,
                        (BlockPos){x+shift.x,y+shift.y,z+shift.z}, state, UPDATE_CLIENTS));
            }
    REQUIRE(sim_set_updates_enabled(world, true) == RS_OK);
    REQUIRE(sim_use_c(world, MIN, MAX, &test_live) == RS_OK);
    REQUIRE(sim_add_cpu_instance(world, MIN,
        (BlockPos){MIN.x+shift.x,MIN.y+shift.y,MIN.z+shift.z}) == RS_OK);
    world_set_lever_powered(world, (BlockPos){8+shift.x,1+shift.y,8+shift.z}, true);
    sim_advance_ticks(world, 8);
    REQUIRE(world_result(world) == RS_OK && sim_stats(world).c_evaluations > 0);
    REQUIRE(world_get(world, (BlockPos){9+shift.x,1+shift.y,8+shift.z}).data.repeater.powered);
    REQUIRE(!world_get(world, (BlockPos){9,1,8}).data.repeater.powered);
    world_destroy(world);

    world = pair_fixture();
    world_set_lever_powered(world, (BlockPos){8+SECOND_OFFSET,1,8}, true);
    events = sim_stats(world).pending_events;
    REQUIRE(events > 0 && sim_use_c(world, MIN, MAX, &test_live) == RS_OK);
    REQUIRE(sim_add_cpu_instance(world, MIN, min) == RS_OK);
    REQUIRE(sim_stats(world).pending_events == events);
    sim_advance_ticks(world, 8);
    REQUIRE(world_result(world) == RS_OK && sim_stats(world).c_evaluations > 0);
    REQUIRE(world_get(world, (BlockPos){9+SECOND_OFFSET,1,8}).data.repeater.powered);
    REQUIRE(!world_get(world, (BlockPos){9,1,8}).data.repeater.powered);
    world_destroy(world);

    /* A large sparse region keeps the position-map path instead of allocating
     * a dense table larger than that map. The same state rules still apply. */
    world = fixture();
    REQUIRE(sim_set_updates_enabled(world, false) == RS_OK);
    for (int x = MIN.x; x <= MAX.x; ++x)
        for (int y = MIN.y; y <= MAX.y; ++y)
            for (int z = MIN.z; z <= MAX.z; ++z) {
                BlockState state = world_get(world, (BlockPos){x,y,z});
                if (state.id != BLOCK_AIR)
                    REQUIRE(world_set_block(world, (BlockPos){x+200,y,z}, state, UPDATE_CLIENTS));
            }
    REQUIRE(sim_set_updates_enabled(world, true) == RS_OK);
    REQUIRE(sim_use_cpu(world, (BlockPos){0,0,0}, (BlockPos){100,1,100}) == RS_OK);
    REQUIRE(sim_add_cpu_instance(world, (BlockPos){0,0,0}, (BlockPos){200,0,0}) == RS_OK);
    world_set_lever_powered(world, (BlockPos){208,1,8}, true);
    sim_advance_ticks(world, 8);
    REQUIRE(world_result(world) == RS_OK && sim_cpu_active(world));
    REQUIRE(sim_stats(world).cpu_evaluations > 0 && sim_stats(world).cpu_fallbacks == 0);
    REQUIRE(world_get(world, (BlockPos){209,1,8}).data.repeater.powered);
    REQUIRE(!world_get(world, (BlockPos){9,1,8}).data.repeater.powered);
    world_destroy(world);
}

static void live_tests(const SimCModel *model, BlockPos min, bool fallback)
{
    static const BlockPos INPUTS[] = {{8,1,8}, {9,1,6}, {8,1,16}, {10,1,14}};
    World *reference = fixture(), *compiled = fixture();
    REQUIRE(sim_use_c(compiled, min, MAX, model) == RS_OK);
    Trace a = {0}, b = {0};
    world_set_change_observer(reference, observe, &a);
    world_set_change_observer(compiled, observe, &b);
    for (unsigned tick = 0; tick < 192; ++tick) {
        for (size_t i = 0; i < sizeof(INPUTS)/sizeof(*INPUTS); ++i) {
            bool value = ((tick / (i + 1)) >> i) & 1u;
            world_set_lever_powered(reference, INPUTS[i], value);
            world_set_lever_powered(compiled, INPUTS[i], value);
            compare(reference, compiled, &a, &b);
        }
        if (tick % 11 == 0) {
            (void)world_toggle_comparator_mode(reference, (BlockPos){10,1,16});
            (void)world_toggle_comparator_mode(compiled, (BlockPos){10,1,16});
            (void)world_cycle_repeater_delay(reference, (BlockPos){10,1,8});
            (void)world_cycle_repeater_delay(compiled, (BlockPos){10,1,8});
        }
        if (tick % 13 == 0) {
            REQUIRE(sim_use_reference(compiled) == RS_OK);
            REQUIRE(sim_use_c(compiled, min, MAX, model) == RS_OK);
        }
        sim_advance_ticks(reference, 1);
        sim_advance_ticks(compiled, 1);
        compare(reference, compiled, &a, &b);
        REQUIRE(sim_c_active(compiled));
    }
    SimStats stats = sim_stats(compiled);
    REQUIRE(stats.c_evaluations > 0);
    REQUIRE(fallback ? stats.cpu_fallbacks > 0 : stats.cpu_fallbacks == 0);
    world_remove(compiled, (BlockPos){10,1,8});
    REQUIRE(!sim_c_active(compiled));
    sim_advance_ticks(compiled, 4);
    REQUIRE(world_result(compiled) == RS_OK);
    free(a.items); free(b.items);
    world_destroy(reference); world_destroy(compiled);
}

static void attachment_tests(void)
{
    World *world = fixture();
    REQUIRE(sim_use_c(world, MIN, MAX, &test_live) == RS_OK);
    uint64_t revision = world_revision(world);
    SimCModel bad = test_live;
    bad.node_count++;
    REQUIRE(sim_use_c(world, MIN, MAX, &bad) == RS_UNSUPPORTED);
    REQUIRE(sim_c_active(world) && world_revision(world) == revision);
    uint32_t *words = malloc(test_live.topology_word_count*sizeof(*words));
    REQUIRE(words);
    memcpy(words, test_live.topology, test_live.topology_word_count*sizeof(*words));
    words[test_live.topology_word_count - 1] ^= 1;
    bad = test_live; bad.topology = words;
    REQUIRE(sim_use_c(world, MIN, MAX, &bad) == RS_UNSUPPORTED);
    REQUIRE(sim_c_active(world));
    free(words);
    bad = test_live; bad.abi_version++;
    REQUIRE(sim_use_c(world, MIN, MAX, &bad) == RS_INVALID_ARGUMENT);
    REQUIRE(sim_use_c(world, MIN, MAX, NULL) == RS_INVALID_ARGUMENT);
    const char *invalid_path = "build/c-codegen-invalid.c";
    FILE *out = fopen(invalid_path, "w+");
    REQUIRE(out);
    REQUIRE(sim_write_c(world, MIN, MAX, out, "bad;symbol") == RS_INVALID_ARGUMENT);
    REQUIRE(sim_write_c(world, MIN, MAX, out, "static") == RS_INVALID_ARGUMENT);
    REQUIRE(ftell(out) == 0 && world_revision(world) == revision && sim_c_active(world));
    REQUIRE(fclose(out) == 0);
    REQUIRE(remove(invalid_path) == 0);
    REQUIRE(sim_use_reference(world) == RS_OK);
    REQUIRE(sim_set_updates_enabled(world, false) == RS_OK);
    put(world, 9,1,8, BLOCK_REDSTONE_REPEATER, DIR_NORTH);
    REQUIRE(sim_set_updates_enabled(world, true) == RS_OK);
    REQUIRE(sim_use_cpu(world, MIN, MAX) == RS_OK);
    REQUIRE(sim_use_c(world, MIN, MAX, &test_live) == RS_UNSUPPORTED);
    REQUIRE(sim_cpu_active(world) && !sim_c_active(world));
    world_destroy(world);
    World *empty = world_create((WorldConfig){0});
    REQUIRE(empty && sim_use_c(empty, MIN, MAX, &test_empty) == RS_OK);
    REQUIRE(sim_c_active(empty));
    sim_advance_ticks(empty, 2);
    REQUIRE(world_result(empty) == RS_OK);
    world_destroy(empty);
}
#endif

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit-c") == 0) return emit(argv[2]);
#ifdef MCMUX_TEST_C
    REQUIRE(argc == 1);
    query_tests();
    attachment_tests();
    live_tests(&test_live, MIN, false);
    live_tests(&test_boundary, (BlockPos){9,0,4}, true);
    pair_tests(false);
    pair_tests(true);
    instance_attachment_tests();
    puts("generated C tests: PASS (queries, ordered writes, pending events, switching, fallback, invalidation)");
    return 0;
#else
    fprintf(stderr, "run ./nob test-c\n");
    return 2;
#endif
}
