#include "sim_executor.h"

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
