#include "sim_executor.h"
#include "redstone_queue.h"
#include "redstone_c.h"

#include <stdlib.h>

struct SimExecutorHost {
    World *world;
};

static RsResult reference_flush(SimExecutorHost *host, void *context)
{
    (void)context;
    return host->world->sim->error;
}

static RsResult reference_scheduled_tick(SimExecutorHost *host, void *context,
                                         ScheduledEvent event)
{
    (void)context;
    World *world = host->world;
    Simulator *sim = world->sim;
    BlockState state = world_get(world, event.pos);
    if (scheduler_block_type(state) != event.block) return sim->error;
    const BlockDef *definition = block_definition((BlockId)state.id);
    if (!definition->scheduled_tick) return sim->error;

    TickAction action = definition->scheduled_tick(world, event.pos);
    ++sim->stats.events_processed;
    if (action.changed)
        (void)world_set_block(world, event.pos, action.state,
            state.id == BLOCK_REDSTONE_TORCH ? 3 : 2);
    if (action.next_delay)
        sim_schedule(world, event.pos, action.next_delay);
    if (state.id == BLOCK_OBSERVER)
        sim_update_output_neighbors(world, event.pos,
                                    (Direction)state.data.observer.facing);
    return sim->error;
}

static void reference_state_changed(SimExecutorHost *host, void *context,
                                    StateChange change)
{
    (void)context;
    sim_reference_state_changed(host->world, change);
}

static void reference_edit_state_changed(SimExecutorHost *host, void *context,
                                         StateChange change)
{
    (void)context;
    sim_reference_edit_state_changed(host->world, change);
}

static const SimExecutorOps reference_ops = {
    .flush = reference_flush,
    .scheduled_tick = reference_scheduled_tick,
    .state_changed = reference_state_changed,
    .edit_state_changed = reference_edit_state_changed
};

void sim_executor_init_reference(World *world)
{
    world->sim->executor = (SimExecutor){&reference_ops, NULL};
}

void sim_executor_destroy(World *world)
{
    if (!world || !world->sim) return;
    SimExecutor *executor = &world->sim->executor;
    if (executor->ops && executor->ops->destroy) {
        SimExecutorHost host = {world};
        executor->ops->destroy(&host, executor->context);
    }
    *executor = (SimExecutor){0};
}

RsResult sim_executor_flush(World *world)
{
    if (!world) return RS_INVALID_ARGUMENT;
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->flush)
        return world->sim->error = RS_INTERNAL_ERROR;
    SimExecutorHost host = {world};
    return executor->ops->flush(&host, executor->context);
}

RsResult sim_executor_scheduled_tick(World *world, ScheduledEvent event)
{
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->scheduled_tick)
        return world->sim->error = RS_INTERNAL_ERROR;
    SimExecutorHost host = {world};
    return executor->ops->scheduled_tick(&host, executor->context, event);
}

void sim_executor_state_changed(World *world, StateChange change)
{
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->state_changed) {
        world->sim->error = RS_INTERNAL_ERROR;
        return;
    }
    SimExecutorHost host = {world};
    executor->ops->state_changed(&host, executor->context, change);
}

void sim_executor_edit_state_changed(World *world, StateChange change)
{
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->edit_state_changed) {
        world->sim->error = RS_INTERNAL_ERROR;
        return;
    }
    SimExecutorHost host = {world};
    executor->ops->edit_state_changed(&host, executor->context, change);
}

void sim_executor_stored_active(World *world, BlockPos pos,
                                BlockState previous, BlockState current)
{
    SimExecutor *executor = &world->sim->executor;
    SimExecutorHost host = {world};
    executor->ops->stored(&host, executor->context, pos, previous, current);
}

RsResult sim_executor_evaluate_active(const World *world, BlockPos pos,
                                      BlockState expected,
                                      RedstoneEvaluation *output)
{
    SimExecutor *executor = &world->sim->executor;
    SimExecutorHost host = {(World *)world};
    RsResult result = executor->ops->evaluate(&host, executor->context,
                                             pos, expected, output);
    if (result != RS_OK && result != RS_UNSUPPORTED)
        world->sim->error = result;
    return result;
}

enum { CPU_CACHE_EMPTY, CPU_CACHE_VALUE, CPU_CACHE_UNSUPPORTED };
typedef struct {
    RedstoneEvaluation value;
    uint32_t visited;
    StateId block_state;
    uint8_t cached;
} CpuNodeCache;

typedef struct {
    RedstonePlan plan;
    RedstoneState state;
    RedstoneDirtyQueue dirty;
    CpuNodeCache *cache;
    uint64_t revision;
    uint32_t generation;
    bool valid;
    const SimCModel *model;
} CpuExecutor;

static void cpu_destroy(SimExecutorHost *host, void *context)
{
    (void)host;
    CpuExecutor *cpu = context;
    if (!cpu) return;
    free(cpu->cache);
    redstone_dirty_queue_destroy(&cpu->dirty);
    redstone_state_destroy(&cpu->state);
    redstone_plan_destroy(&cpu->plan);
    free(cpu);
}

static bool cpu_valid(const World *world, const CpuExecutor *cpu)
{
    return cpu->valid && cpu->revision == world_revision(world) &&
           !world->sim->piston_motions.active;
}

static RsResult cpu_invalidate(CpuExecutor *cpu, RedstoneNodeId source)
{
    if (++cpu->generation == 0) {
        for (size_t i = 0; i < cpu->plan.node_count; ++i)
            cpu->cache[i].visited = 0;
        cpu->generation = 1;
    }
    RsResult result = redstone_dirty_queue_push(&cpu->dirty, source);
    if (result != RS_OK) return result;
    RedstoneNodeId node;
    while (redstone_dirty_queue_pop(&cpu->dirty, &node)) {
        CpuNodeCache *cache = &cpu->cache[node];
        if (cache->visited == cpu->generation) continue;
        cache->visited = cpu->generation;
        cache->cached = CPU_CACHE_EMPTY;
        /* Conductors relay their direct sources without a stored-state
         * change. Other nodes propagate only when their output is written.
         * This queue invalidates pure reads, never runs block notifications. */
        if (node == source ||
            (cpu->plan.nodes[node].flags & REDSTONE_NODE_CONDUCTOR)) {
            result = redstone_dirty_queue_push_consumers(&cpu->dirty,
                                                         &cpu->plan, node);
            if (result != RS_OK) return result;
        }
    }
    return RS_OK;
}

static void cpu_stored(SimExecutorHost *host, void *context, BlockPos pos,
                        BlockState previous, BlockState current)
{
    CpuExecutor *cpu = context;
    if (!cpu->valid) return;
    World *world = host->world;
    /* A revision not observed by this hook (inventory edits, plot changes,
     * etc.) must not be hidden by the next ordinary state write. */
    if (world_revision(world) != cpu->revision + 1 ||
        !redstone_state_compatible(previous, current)) {
        cpu->valid = false;
        return;
    }
    cpu->revision = world_revision(world);
    RedstoneNodeId node;
    if (!redstone_plan_find(&cpu->plan, pos, &node)) return;
    RsResult result = redstone_state_set(&cpu->state, node, current);
    if (result == RS_UNSUPPORTED) { cpu->valid = false; return; }
    if (result == RS_OK || result == RS_UNCHANGED) {
        if (!state_encode(current, &cpu->cache[node].block_state))
            result = RS_INTERNAL_ERROR;
        else
            result = cpu_invalidate(cpu, node);
    }
    if (result != RS_OK) {
        cpu->valid = false;
        world->sim->error = result;
    }
}

static RsResult cpu_evaluate(SimExecutorHost *host, void *context, BlockPos pos,
                              BlockState expected, RedstoneEvaluation *output)
{
    CpuExecutor *cpu = context;
    World *world = host->world;
    if (!cpu_valid(world, cpu)) {
        cpu->valid = false;
        ++world->sim->stats.cpu_fallbacks;
        return RS_UNSUPPORTED;
    }
    RedstoneNodeId node;
    StateId encoded;
    if (!redstone_plan_find(&cpu->plan, pos, &node) ||
        !state_encode(expected, &encoded) ||
        encoded != cpu->cache[node].block_state) {
        ++world->sim->stats.cpu_fallbacks;
        return RS_UNSUPPORTED;
    }
    CpuNodeCache *cache = &cpu->cache[node];
    if (cache->cached == CPU_CACHE_VALUE) {
        ++world->sim->stats.cpu_cache_hits;
        *output = cache->value;
        return RS_OK;
    }
    if (cache->cached == CPU_CACHE_UNSUPPORTED) {
        ++world->sim->stats.cpu_fallbacks;
        return RS_UNSUPPORTED;
    }
    RedstoneEvaluation value;
    RsResult result = cpu->model ?
        cpu->model->evaluate(cpu->state.blocks, node, &value) :
        redstone_evaluate(&cpu->state, node, &value);
    if (result == RS_OK) {
        ++world->sim->stats.cpu_evaluations;
        if (cpu->model) ++world->sim->stats.c_evaluations;
        cache->value = value;
        cache->cached = CPU_CACHE_VALUE;
        *output = value;
    } else if (result == RS_UNSUPPORTED) {
        ++world->sim->stats.cpu_fallbacks;
        cache->cached = CPU_CACHE_UNSUPPORTED;
    }
    return result;
}

static const SimExecutorOps cpu_ops = {
    .flush = reference_flush,
    .scheduled_tick = reference_scheduled_tick,
    .state_changed = reference_state_changed,
    .edit_state_changed = reference_edit_state_changed,
    .destroy = cpu_destroy,
    .stored = cpu_stored,
    .evaluate = cpu_evaluate
};

static RsResult backend_change_ready(const World *world)
{
    if (!world) return RS_INVALID_ARGUMENT;
    const Simulator *sim = world->sim;
    if (sim->handling_tick || sim->block_entities.ticking || sim->checking ||
        sim->observing)
        return RS_INVALID_ARGUMENT;
    return world_result(world);
}

static RsResult use_cpu(World *world, BlockPos min, BlockPos max,
                         const SimCModel *model)
{
    RsResult result = backend_change_ready(world);
    if (result != RS_OK) return result;
    if (world->sim->piston_motions.active) return RS_UNSUPPORTED;
    CpuExecutor *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) return RS_OUT_OF_MEMORY;
    result = redstone_plan_build(world, (RedstonePlanBounds){min, max}, &cpu->plan);
    if (result != RS_OK) goto fail;
    if (model && !redstone_c_matches(&cpu->plan, model)) {
        result = RS_UNSUPPORTED;
        goto fail;
    }
    cpu->model = model;
    result = redstone_state_build(&cpu->plan, &cpu->state);
    if (result != RS_OK) goto fail;
    result = redstone_dirty_queue_prepare(&cpu->dirty, cpu->plan.node_count);
    if (result != RS_OK) goto fail;
    if (cpu->plan.node_count) {
        if (cpu->plan.node_count > SIZE_MAX/sizeof(*cpu->cache)) {
            result = RS_OUT_OF_MEMORY;
            goto fail;
        }
        cpu->cache = calloc(cpu->plan.node_count, sizeof(*cpu->cache));
        if (!cpu->cache) { result = RS_OUT_OF_MEMORY; goto fail; }
        for (size_t i = 0; i < cpu->plan.node_count; ++i)
            if (!state_encode(cpu->state.blocks[i], &cpu->cache[i].block_state)) {
                result = RS_INTERNAL_ERROR;
                goto fail;
            }
    }
    cpu->revision = world_revision(world);
    cpu->valid = true;
    sim_executor_destroy(world);
    world->sim->executor = (SimExecutor){&cpu_ops, cpu};
    return RS_OK;
fail:
    cpu_destroy(NULL, cpu);
    return result;
}

RsResult sim_use_cpu(World *world, BlockPos min, BlockPos max)
{
    return use_cpu(world, min, max, NULL);
}

RsResult sim_use_c(World *world, BlockPos min, BlockPos max,
                   const SimCModel *model)
{
    if (!model || model->abi_version != SIM_C_ABI_VERSION ||
        !model->topology || !model->evaluate)
        return RS_INVALID_ARGUMENT;
    return use_cpu(world, min, max, model);
}

RsResult sim_write_c(const World *world, BlockPos min, BlockPos max,
                     FILE *output, const char *symbol)
{
    if (!output || !symbol) return RS_INVALID_ARGUMENT;
    RsResult result = backend_change_ready(world);
    if (result != RS_OK) return result;
    if (world->sim->piston_motions.active) return RS_UNSUPPORTED;
    RedstonePlan plan = {0};
    result = redstone_plan_build(world, (RedstonePlanBounds){min, max}, &plan);
    if (result == RS_OK) result = redstone_c_write(&plan, output, symbol);
    redstone_plan_destroy(&plan);
    return result;
}

RsResult sim_use_reference(World *world)
{
    RsResult result = backend_change_ready(world);
    if (result != RS_OK) return result;
    if (world->sim->executor.ops == &reference_ops) return RS_UNCHANGED;
    sim_executor_destroy(world);
    sim_executor_init_reference(world);
    return RS_OK;
}

bool sim_cpu_active(const World *world)
{
    return world && world->sim->executor.ops == &cpu_ops &&
           cpu_valid(world, world->sim->executor.context);
}

bool sim_c_active(const World *world)
{
    if (!sim_cpu_active(world)) return false;
    const CpuExecutor *cpu = world->sim->executor.context;
    return cpu->model != NULL;
}
