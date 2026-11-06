#ifndef RS_SIM_EXECUTOR_H
#define RS_SIM_EXECUTOR_H

#include "sim_internal.h"
#include "redstone_eval.h"

/*
 * A simulator executor implements redstone state propagation, not the game
 * tick timeline. sim.c remains responsible for advancing time, freezing and
 * consuming the scheduled-tick batch, piston block events, block entities and
 * user checks, in that order.
 *
 * Each scheduled_tick call handles exactly the event supplied by sim.c and
 * must settle its zero-delay consequences before returning. State-change
 * callbacks observe a world cell after the new state has been stored and must
 * complete that change's synchronous propagation before returning. An
 * executor may also use them to maintain or invalidate a compiled plan.
 * flush must synchronize executor-owned state back to World without advancing
 * time or executing a timed phase.
 */
/* Borrowed only for the duration of an executor callback. Its definition is
 * private so a compiled executor receives capabilities explicitly instead of
 * unrestricted access to World and Simulator internals. */
typedef struct SimExecutorHost SimExecutorHost;

struct SimExecutorOps {
    RsResult (*flush)(SimExecutorHost *host, void *context);
    RsResult (*scheduled_tick)(SimExecutorHost *host, void *context,
                               ScheduledEvent event);
    void (*state_changed)(SimExecutorHost *host, void *context,
                          StateChange change);
    void (*edit_state_changed)(SimExecutorHost *host, void *context,
                               StateChange change);
    void (*destroy)(SimExecutorHost *host, void *context);
    /* Stored-state observation runs before user observers and block callbacks,
     * including auxiliary writes which deliberately skip state_changed. */
    void (*stored)(SimExecutorHost *host, void *context, BlockPos pos,
                    BlockState previous, BlockState current);
    RsResult (*evaluate)(SimExecutorHost *host, void *context, BlockPos pos,
                          BlockState expected, RedstoneEvaluation *output);
    /* Optional known callback targets, with one bit per Direction. */
    bool (*neighbor_mask)(SimExecutorHost *host, void *context, BlockPos center,
                           uint8_t *mask);
};

void sim_executor_init_reference(World *world);
void sim_executor_destroy(World *world);
RsResult sim_executor_flush(World *world);
RsResult sim_executor_scheduled_tick(World *world, ScheduledEvent event);
void sim_executor_state_changed(World *world, StateChange change);
void sim_executor_edit_state_changed(World *world, StateChange change);

/* A mask can skip only positions without neighbor_changed callbacks. Callers
 * keep the original order and refresh it after any stored-state write, since
 * a callback or user observer may edit a later neighbor in the same walk. */
bool sim_executor_neighbor_mask_active(const World *world, BlockPos center,
                                       uint8_t *mask);
static inline bool sim_executor_neighbor_mask(const World *world, BlockPos center,
                                               uint8_t *mask)
{
    return world->sim->executor.ops->neighbor_mask &&
           sim_executor_neighbor_mask_active(world, center, mask);
}

void sim_executor_stored_active(World *world, BlockPos pos,
                                BlockState previous, BlockState current);
RsResult sim_executor_evaluate_active(const World *world, BlockPos pos,
                                      BlockState expected,
                                      RedstoneEvaluation *output);

/* Keep default reference writes/queries on a single optional-hook check. */
static inline void sim_executor_stored(World *world, BlockPos pos,
                                       BlockState previous, BlockState current)
{
    if (world->sim->executor.ops->stored)
        sim_executor_stored_active(world, pos, previous, current);
}

static inline RsResult sim_executor_evaluate(const World *world, BlockPos pos,
                                             BlockState expected,
                                             RedstoneEvaluation *output)
{
    return world->sim->executor.ops->evaluate ?
        sim_executor_evaluate_active(world, pos, expected, output) : RS_UNSUPPORTED;
}

#endif
