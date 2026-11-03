#include "block_internal.h"
#include "sim_internal.h"
#include "sim_executor.h"

enum { COMPARATOR_DELAY = 2 };

bool comparator_supported(const World *world, BlockPos position)
{
    return block_supports_rigid(world, block_offset(position, DIR_DOWN));
}

static bool valid_facing(Direction facing)
{
    return direction_horizontal(facing);
}

static uint8_t comparator_rear_input(const World *world, BlockPos position,
                                     BlockState state)
{
    Direction facing = (Direction)state.data.comparator.facing;
    if (!valid_facing(facing)) return 0;
    Direction rear = direction_opposite(facing);
    BlockPos source = block_offset(position, rear);
    uint8_t signal = power_signal(world, source, facing, SIGNAL_ALL);
    if (signal < 15) {
        BlockState input = world_get(world, source);
        if (input.id == BLOCK_REDSTONE_WIRE && input.data.wire.power > signal)
            signal = input.data.wire.power;
    }

    if (block_has_analog_output(world, source))
        return block_analog_output(world, source);

    /* Vanilla comparators can read a container through one conducting block,
     * provided the direct rear signal has not already reached 15. */
    if (signal < 15 && block_is_conductor(world, source)) {
        BlockPos second = block_offset(source, rear);
        if (block_has_analog_output(world, second))
            return block_analog_output(world, second);
    }
    return signal;
}

static uint8_t comparator_side_input(const World *world, BlockPos position,
                                     BlockState state)
{
    Direction facing = (Direction)state.data.comparator.facing;
    if (!valid_facing(facing)) return 0;
    const Direction *sides;
    static const Direction east_west_sides[] = {DIR_NORTH, DIR_SOUTH};
    static const Direction north_south_sides[] = {DIR_EAST, DIR_WEST};
    sides = (facing == DIR_EAST || facing == DIR_WEST) ?
            east_west_sides : north_south_sides;

    uint8_t best = 0;
    for (int index = 0; index < 2; ++index) {
        Direction side = sides[index];
        BlockPos source = block_offset(position, side);
        BlockState source_state = world_get(world, source);
        BlockId source_id = (BlockId)source_state.id;
        uint8_t signal = 0;
        if (source_id == BLOCK_REDSTONE_BLOCK) {
            signal = 15;
        } else if (source_id == BLOCK_REDSTONE_WIRE) {
            /* Diode side input reads the dust level, independent of its
             * rendered connection shape. */
            signal = source_state.data.wire.power;
        } else if (block_is_signal_source(world, source)) {
            signal = power_direct(world, source,
                                  direction_opposite(side), SIGNAL_ALL);
        }
        if (signal > best) best = signal;
    }
    return best;
}

uint8_t comparator_desired_output(const World *world, BlockPos position,
                                         BlockState state)
{
    RedstoneEvaluation value;
    if (sim_executor_evaluate(world, position, state, &value) == RS_OK)
        return value.target_power;
    uint8_t rear = comparator_rear_input(world, position, state);
    if (state.data.comparator.subtract) {
        uint8_t side = comparator_side_input(world, position, state);
        return rear > side ? (uint8_t)(rear - side) : 0;
    }
    /* Compare mode stores the rear input even while POWERED is false. */
    return rear;
}

bool comparator_should_turn_on(const World *world, BlockPos position,
                                BlockState state)
{
    RedstoneEvaluation value;
    if (sim_executor_evaluate(world, position, state, &value) == RS_OK)
        return value.target_on;
    uint8_t rear = comparator_rear_input(world, position, state);
    if (!rear) return false;
    uint8_t side = comparator_side_input(world, position, state);
    return rear > side || (rear == side && !state.data.comparator.subtract);
}

static uint8_t comparator_signal(const World *world, BlockPos position,
                                 Direction face, SignalMode mode)
{
    return block_state_signal(world_get(world, position), face, mode, false);
}

static bool comparator_should_prioritize(const World *world,
                                         BlockPos position, BlockState state)
{
    Direction output = (Direction)state.data.comparator.facing;
    BlockState next = world_get(world, block_offset(position, output));
    Direction next_output;
    if (next.id == BLOCK_REDSTONE_REPEATER) {
        next_output = (Direction)next.data.repeater.facing;
    } else if (next.id == BLOCK_REDSTONE_COMPARATOR) {
        next_output = (Direction)next.data.comparator.facing;
    } else {
        return false;
    }
    return next_output != direction_opposite(output);
}

void comparator_check_input(World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    if (state.id != BLOCK_REDSTONE_COMPARATOR) return;
    if (sim_will_tick(world, position)) return;
    if (state.data.comparator.output !=
        comparator_desired_output(world, position, state) ||
        state.data.comparator.powered !=
        comparator_should_turn_on(world, position, state)) {
        TickPriority priority = comparator_should_prioritize(
            world, position, state) ? TICK_HIGH : TICK_NORMAL;
        sim_schedule_priority(world, position, COMPARATOR_DELAY, priority);
    }
}

static void comparator_neighbor(World *world, BlockPos position)
{
    if (!comparator_supported(world, position)) {
        world_remove_internal(world, position);
        sim_update_neighbors_around(world, position);
        return;
    }
    sim_mark_input(world, position);
}

void comparator_refresh_output(World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    if (state.id != BLOCK_REDSTONE_COMPARATOR) return;
    uint8_t previous = state.data.comparator.output;
    uint8_t output = comparator_desired_output(world, position, state);
    state.data.comparator.output = output;
    /* Keep the cache packed with the cell for storage/read locality, but do
     * not run block-state callbacks. This write still invalidates both world
     * read and external-power caches before notifying downstream dust. */
    if (!world_write_visual(world, position, state)) return;
    if (previous != output || !state.data.comparator.subtract) {
        bool powered = comparator_should_turn_on(world, position, state);
        if (state.data.comparator.powered != powered) {
            state.data.comparator.powered = powered;
            if (!world_set_block(world, position, state, UPDATE_CLIENTS)) return;
        }
        /* Java compare mode notifies even when neither POWERED nor the cache
         * changed. A pending tick can therefore wake a BUD with zero output. */
        sim_update_output_neighbors(world, position,
            (Direction)state.data.comparator.facing);
    }
}

static TickAction comparator_tick(World *world, BlockPos position)
{
    comparator_refresh_output(world, position);
    return (TickAction){world_get(world, position), false, 0};
}

const BlockDef rs_comparator_def = {
    .name = "redstone_comparator",
    .signal_source = true,
    .diode = true,
    .push_reaction = PUSH_DESTROY,
    .signal = comparator_signal,
    .direct_signal = comparator_signal,
    .neighbor_changed = comparator_neighbor,
    .input_changed = comparator_check_input,
    .scheduled_tick = comparator_tick
};
