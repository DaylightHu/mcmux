#include "block_internal.h"

#include <string.h>
#include <limits.h>

static const BlockDef air = {
    .name = "air",
    .push_reaction = PUSH_IGNORE
};

const BlockDef rs_moving_piston_def = {
    .name = "moving_piston",
    /* MOVING_PISTON is an inert, unbreakable placeholder in vanilla. The
     * carried state lives in the simulator's sparse piston-motion table. */
    .push_reaction = PUSH_BLOCK
};

const BlockDef *block_definition(BlockId id)
{
    static const BlockDef *const definitions[BLOCK_COUNT] = {
        [BLOCK_AIR] = &air,
        [BLOCK_IRON] = &rs_iron_def,
        [BLOCK_REDSTONE_BLOCK] = &rs_redstone_block_def,
        [BLOCK_REDSTONE_WIRE] = &rs_wire_def,
        [BLOCK_REDSTONE_TORCH] = &rs_torch_def,
        [BLOCK_GLASS] = &rs_glass_def,
        [BLOCK_REDSTONE_REPEATER] = &rs_repeater_def,
        [BLOCK_REDSTONE_COMPARATOR] = &rs_comparator_def,
        [BLOCK_POWERED_RAIL] = &rs_powered_rail_def,
        [BLOCK_ACTIVATOR_RAIL] = &rs_activator_rail_def,
        [BLOCK_OBSERVER] = &rs_observer_def,
        [BLOCK_PISTON] = &rs_piston_def,
        [BLOCK_STICKY_PISTON] = &rs_sticky_piston_def,
        [BLOCK_BARREL] = &rs_barrel_def,
        [BLOCK_SLIME] = &rs_slime_def,
        [BLOCK_NOTE_BLOCK] = &rs_note_block_def,
        [BLOCK_HONEY] = &rs_honey_def,
        [BLOCK_TARGET] = &rs_target_def,
        [BLOCK_HOPPER] = &rs_hopper_def,
        [BLOCK_DROPPER] = &rs_dropper_def,
        [BLOCK_REDSTONE_LAMP] = &rs_lamp_def,
        [BLOCK_LEVER] = &rs_lever_def,
        [BLOCK_PISTON_HEAD] = &rs_piston_head_def,
        [BLOCK_STICKY_PISTON_HEAD] = &rs_sticky_piston_head_def,
        [BLOCK_MOVING_PISTON] = &rs_moving_piston_def
    };
    return (unsigned)id < BLOCK_COUNT ? definitions[id] : &air;
}

const char *block_name(BlockId id)
{
    return block_definition(id)->name;
}

BlockState block_default(BlockId id)
{
    BlockState state;
    memset(&state, 0, sizeof(state));
    state.id = (uint8_t)id;
    if (id == BLOCK_REDSTONE_TORCH) {
        state.data.torch.support_dir = DIR_DOWN;
        state.data.torch.lit = true;
    } else if (id == BLOCK_REDSTONE_REPEATER) {
        state.data.repeater.facing = DIR_NORTH;
        state.data.repeater.delay = 1;
    } else if (id == BLOCK_REDSTONE_COMPARATOR) {
        state.data.comparator.facing = DIR_NORTH;
    } else if (block_is_rail(id)) {
        state.data.rail.facing = DIR_NORTH;
    } else if (id == BLOCK_OBSERVER) {
        state.data.observer.facing = DIR_NORTH;
    } else if (block_is_piston_base(id) || block_is_piston_head(id)) {
        state.data.piston.facing = DIR_NORTH;
    } else if (id == BLOCK_MOVING_PISTON) {
        state.data.moving_piston.facing = DIR_NORTH;
    } else if (id == BLOCK_BARREL) {
        state.data.barrel.facing = DIR_NORTH;
    } else if (id == BLOCK_HOPPER) {
        state.data.hopper.facing = DIR_DOWN;
        state.data.hopper.enabled = true;
    } else if (id == BLOCK_DROPPER) {
        state.data.dropper.facing = DIR_NORTH;
    } else if (id == BLOCK_LEVER) {
        state.data.lever.support_dir = DIR_DOWN;
        state.data.lever.facing = DIR_NORTH;
    }
    return state;
}

bool block_is_piston_base(BlockId id)
{
    return id == BLOCK_PISTON || id == BLOCK_STICKY_PISTON;
}

bool block_is_piston_head(BlockId id)
{
    return id == BLOCK_PISTON_HEAD || id == BLOCK_STICKY_PISTON_HEAD;
}

bool block_piston_pair(BlockId base, BlockId head)
{
    return (base == BLOCK_PISTON && head == BLOCK_PISTON_HEAD) ||
           (base == BLOCK_STICKY_PISTON &&
            head == BLOCK_STICKY_PISTON_HEAD);
}

bool direction_horizontal(Direction direction)
{
    return direction == DIR_EAST || direction == DIR_WEST ||
           direction == DIR_SOUTH || direction == DIR_NORTH;
}

Direction direction_opposite(Direction direction)
{
    static const Direction opposite[DIR_COUNT] = {
        DIR_WEST, DIR_EAST, DIR_DOWN, DIR_UP, DIR_NORTH, DIR_SOUTH
    };
    return (unsigned)direction < DIR_COUNT ? opposite[direction] : DIR_COUNT;
}

BlockPos block_offset(BlockPos position, Direction direction)
{
    static const BlockPos offsets[DIR_COUNT] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };
    if ((unsigned)direction < DIR_COUNT) {
        int64_t x = (int64_t)position.x + offsets[direction].x;
        int64_t y = (int64_t)position.y + offsets[direction].y;
        int64_t z = (int64_t)position.z + offsets[direction].z;
        if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX || z < INT_MIN || z > INT_MAX)
            return (BlockPos){INT_MIN, INT_MIN, INT_MIN};
        position = (BlockPos){(int)x, (int)y, (int)z};
    }
    return position;
}
