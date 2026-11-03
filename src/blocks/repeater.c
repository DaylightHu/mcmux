#include "block_internal.h"
#include "sim_internal.h"

enum { REPEATER_DELAY_SCALE = 2 };

bool repeater_supported(const World *world, BlockPos position)
{
    return block_supports_rigid(world, block_offset(position, DIR_DOWN));
}

static bool valid_facing(Direction facing)
{
    return direction_horizontal(facing);
}

static bool repeater_input_powered(const World *world, BlockPos position,
                                   BlockState state)
{
    Direction facing = (Direction)state.data.repeater.facing;
    if (!valid_facing(facing)) return false;
    Direction rear = direction_opposite(facing);
    BlockPos source = block_offset(position, rear);
    if (power_signal(world, source, facing, SIGNAL_ALL) > 0) return true;
    /* DiodeBlock.getInputSignal also reads the rear dust level directly. */
    BlockState input = world_get(world, source);
    return input.id == BLOCK_REDSTONE_WIRE && input.data.wire.power > 0;
}

bool repeater_is_locked(const World *world, BlockPos position,
                                 BlockState state)
{
    Direction facing = (Direction)state.data.repeater.facing;
    if (!valid_facing(facing)) return false;
    const Direction *sides;
    static const Direction east_west_sides[] = {DIR_NORTH, DIR_SOUTH};
    static const Direction north_south_sides[] = {DIR_EAST, DIR_WEST};
    sides = (facing == DIR_EAST || facing == DIR_WEST) ?
            east_west_sides : north_south_sides;

    for (int index = 0; index < 2; ++index) {
        Direction side = sides[index];
        BlockPos source = block_offset(position, side);
        if (block_is_diode(world, source) &&
            power_direct(world, source, direction_opposite(side),
                         SIGNAL_ALL) > 0) {
            return true;
        }
    }
    return false;
}

static bool repeater_should_prioritize(const World *world, BlockPos position,
                                       BlockState state)
{
    Direction output = (Direction)state.data.repeater.facing;
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

static uint8_t repeater_signal(const World *world, BlockPos position,
                               Direction face, SignalMode mode)
{
    return block_state_signal(world_get(world, position), face, mode, false);
}

void repeater_check_input(World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    if (state.id != BLOCK_REDSTONE_REPEATER) return;

    bool locked = repeater_is_locked(world, position, state);
    if (locked || sim_will_tick(world, position)) return;

    bool should_be_powered = repeater_input_powered(world, position, state);
    if (state.data.repeater.powered != should_be_powered) {
        TickPriority priority = TICK_HIGH;
        if (repeater_should_prioritize(world, position, state))
            priority = TICK_EXTREMELY_HIGH;
        else if (state.data.repeater.powered)
            priority = TICK_VERY_HIGH;
        sim_schedule_priority(
            world, position,
            (uint32_t)state.data.repeater.delay*REPEATER_DELAY_SCALE,
            priority);
    }
}

static void repeater_neighbor(World *world, BlockPos position)
{
    if (!repeater_supported(world, position)) {
        world_remove_internal(world, position);
        sim_update_neighbors_around(world, position);
        return;
    }
    sim_mark_input(world, position);
}

static TickAction repeater_tick(World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    if (repeater_is_locked(world, position, state))
        return (TickAction){state, false, 0};
    bool input = repeater_input_powered(world, position, state);
    if (state.data.repeater.powered && !input) {
        state.data.repeater.powered = false;
        (void)world_set_block(world, position, state, 2);
    } else if (!state.data.repeater.powered) {
        state.data.repeater.powered = true;
        if (world_set_block(world, position, state, 2) && !input)
            sim_schedule_priority(world, position,
                (uint32_t)state.data.repeater.delay*REPEATER_DELAY_SCALE,
                TICK_VERY_HIGH);
    }
    return (TickAction){state, false, 0};
}

const BlockDef rs_repeater_def = {
    .name = "redstone_repeater",
    .signal_source = true,
    .diode = true,
    .push_reaction = PUSH_DESTROY,
    .signal = repeater_signal,
    .direct_signal = repeater_signal,
    .neighbor_changed = repeater_neighbor,
    .input_changed = repeater_check_input,
    .scheduled_tick = repeater_tick
};
