#include "sim_executor.h"

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

typedef struct {
    RedstoneShadow shadow;
    unsigned callback_depth;
} ShadowExecutor;

static RsResult shadow_flush(SimExecutorHost *host, void *context)
{
    ShadowExecutor *executor = context;
    if (host->world->sim->error != RS_OK) return host->world->sim->error;
    if (executor->callback_depth) return RS_OK;
    RsResult result = redstone_shadow_validate(&executor->shadow, host->world);
    if (result != RS_OK && host->world->sim->error == RS_OK)
        host->world->sim->error = result;
    return host->world->sim->error;
}

static RsResult shadow_scheduled_tick(SimExecutorHost *host, void *context,
                                      ScheduledEvent event)
{
    ShadowExecutor *executor = context;
    ++executor->callback_depth;
    RsResult result = reference_scheduled_tick(host, NULL, event);
    --executor->callback_depth;
    return result;
}

static void shadow_state_changed(SimExecutorHost *host, void *context,
                                 StateChange change)
{
    ShadowExecutor *executor = context;
    redstone_shadow_state_changed(&executor->shadow, change);
    ++executor->callback_depth;
    sim_reference_state_changed(host->world, change);
    --executor->callback_depth;
}

static void shadow_edit_state_changed(SimExecutorHost *host, void *context,
                                      StateChange change)
{
    ShadowExecutor *executor = context;
    redstone_shadow_state_changed(&executor->shadow, change);
    ++executor->callback_depth;
    sim_reference_edit_state_changed(host->world, change);
    --executor->callback_depth;
}

static void shadow_destroy(SimExecutorHost *host, void *context)
{
    (void)host;
    ShadowExecutor *executor = context;
    redstone_shadow_destroy(&executor->shadow);
    free(executor);
}

static const SimExecutorOps shadow_ops = {
    .flush = shadow_flush,
    .scheduled_tick = shadow_scheduled_tick,
    .state_changed = shadow_state_changed,
    .edit_state_changed = shadow_edit_state_changed,
    .destroy = shadow_destroy
};

void sim_executor_init_reference(World *world)
{
    if (world->sim->executor.ops) sim_executor_destroy(world);
    world->sim->executor = (SimExecutor){&reference_ops, NULL};
}

RsResult sim_executor_init_shadow(World *world, RedstonePlanBounds bounds)
{
    if (!world || !world->sim || world->sim->handling_tick ||
        world->sim->block_entities.ticking || world->sim->checking)
        return RS_INVALID_ARGUMENT;
    if (world_result(world) != RS_OK) return world_result(world);

    ShadowExecutor *executor = calloc(1, sizeof(*executor));
    if (!executor) return RS_OUT_OF_MEMORY;
    redstone_shadow_init(&executor->shadow);
    RsResult result = redstone_shadow_build(&executor->shadow, world, bounds);
    if (result != RS_OK) {
        redstone_shadow_destroy(&executor->shadow);
        free(executor);
        return result;
    }
    result = redstone_shadow_validate(&executor->shadow, world);
    if (result != RS_OK) {
        redstone_shadow_destroy(&executor->shadow);
        free(executor);
        return result;
    }
    sim_executor_destroy(world);
    world->sim->executor = (SimExecutor){&shadow_ops, executor};
    return RS_OK;
}

bool sim_executor_shadow_stats(const World *world, RedstoneShadowStats *stats)
{
    if (!world || !stats || world->sim->executor.ops != &shadow_ops) return false;
    const ShadowExecutor *executor = world->sim->executor.context;
    *stats = executor->shadow.stats;
    return true;
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
