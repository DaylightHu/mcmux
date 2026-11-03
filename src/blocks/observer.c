#include "block_internal.h"
#include "sim_internal.h"

enum { OBSERVER_DELAY = 2, OBSERVER_PULSE = 2 };

static uint8_t observer_signal(const World *world, BlockPos position,
                               Direction face, SignalMode mode)
{
    return block_state_signal(world_get(world, position), face, mode, false);
}

static void observer_observed_changed(World *world, BlockPos position,
                                      BlockState state, BlockPos changed)
{
    /* Shape evaluation can run while this cell still holds MOVING_PISTON. */
    if (state.id != BLOCK_OBSERVER || state.data.observer.powered ||
        scheduler_pending(&world->sim->scheduler, position, BLOCK_OBSERVER)) return;

    Direction output = (Direction)state.data.observer.facing;
    if ((unsigned)output >= DIR_COUNT) return;
    BlockPos watched = block_offset(position, direction_opposite(output));
    if (pos_equal(watched, changed))
        sim_schedule_state(world, position, state, OBSERVER_DELAY);
}

static TickAction observer_tick(World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    TickAction action = {state, false, 0};
    if (state.id != BLOCK_OBSERVER) return action;

    action.state.data.observer.powered = !state.data.observer.powered;
    action.changed = true;
    if (!state.data.observer.powered) action.next_delay = OBSERVER_PULSE;
    return action;
}

const BlockDef rs_observer_def = {
    .name = "observer",
    .full_cube = true,
    .supports_wire = true,
    .signal_source = true,
    .push_reaction = PUSH_NORMAL,
    .signal = observer_signal,
    .direct_signal = observer_signal,
    .observed_changed = observer_observed_changed,
    .scheduled_tick = observer_tick
};
