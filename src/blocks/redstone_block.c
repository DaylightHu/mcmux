#include "block_internal.h"

static uint8_t source_signal(const World *world, BlockPos position,
                             Direction face, SignalMode mode)
{
    (void)world;
    (void)position;
    return block_state_signal((BlockState){.id = BLOCK_REDSTONE_BLOCK},
                              face, mode, false);
}

/* Supports dust and torches, but is not a passive redstone conductor. */
const BlockDef rs_redstone_block_def = {
    .name = "redstone_block",
    .full_cube = true,
    .supports_wire = true,
    .signal_source = true,
    .push_reaction = PUSH_NORMAL,
    .signal = source_signal
};
