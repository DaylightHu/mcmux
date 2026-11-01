#include "block_internal.h"
#include "../sim_internal.h"

bool lever_supported(const World *world, BlockPos position, Direction support)
{
    return (unsigned)support < DIR_COUNT &&
           block_face_sturdy(world, block_offset(position, support),
                             direction_opposite(support));
}

static uint8_t lever_signal(const World *world, BlockPos position,
                            Direction face, SignalMode mode)
{
    (void)face;
    (void)mode;
    return world_get(world, position).data.lever.powered ? 15 : 0;
}

static uint8_t lever_direct(const World *world, BlockPos position,
                            Direction face, SignalMode mode)
{
    (void)mode;
    BlockState state = world_get(world, position);
    return state.data.lever.powered &&
           face == (Direction)state.data.lever.support_dir ? 15 : 0;
}

void lever_update_neighbors(World *world, BlockPos position, BlockState state)
{
    sim_update_neighbors_at(world, position, DIR_COUNT);
    Direction support = (Direction)state.data.lever.support_dir;
    if ((unsigned)support < DIR_COUNT)
        sim_update_neighbors_at(world, block_offset(position, support),
                                DIR_COUNT);
}

const BlockDef rs_lever_def = {
    .name = "lever",
    .signal_source = true,
    .push_reaction = PUSH_DESTROY,
    .signal = lever_signal,
    .direct_signal = lever_direct
};
