#ifndef RS_SIM_EXECUTOR_H
#define RS_SIM_EXECUTOR_H

#include "sim_internal.h"

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
};

void sim_executor_init_reference(World *world);
void sim_executor_destroy(World *world);
RsResult sim_executor_flush(World *world);
RsResult sim_executor_scheduled_tick(World *world, ScheduledEvent event);
void sim_executor_state_changed(World *world, StateChange change);
void sim_executor_edit_state_changed(World *world, StateChange change);

#endif
