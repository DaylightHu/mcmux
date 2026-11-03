#include "block_internal.h"

uint8_t block_state_signal(BlockState state, Direction face, SignalMode mode,
                           bool direct)
{
    if ((unsigned)face >= DIR_COUNT) return 0;
    switch ((BlockId)state.id) {
    case BLOCK_REDSTONE_BLOCK:
        return direct ? 0 : 15;
    case BLOCK_REDSTONE_WIRE:
        if (mode == SIGNAL_WITHOUT_WIRE || face == DIR_UP) return 0;
        return face == DIR_DOWN ||
               (state.data.wire.connections & (1u << (unsigned)face)) ?
               state.data.wire.power : 0;
    case BLOCK_REDSTONE_TORCH:
        return state.data.torch.lit &&
               face != (Direction)state.data.torch.support_dir &&
               (!direct || face == DIR_UP) ? 15 : 0;
    case BLOCK_REDSTONE_REPEATER:
        return state.data.repeater.powered &&
               face == (Direction)state.data.repeater.facing ? 15 : 0;
    case BLOCK_REDSTONE_COMPARATOR:
        return state.data.comparator.powered &&
               face == (Direction)state.data.comparator.facing ?
               state.data.comparator.output : 0;
    case BLOCK_OBSERVER:
        return state.data.observer.powered &&
               face == (Direction)state.data.observer.facing ? 15 : 0;
    case BLOCK_LEVER:
        return state.data.lever.powered &&
               (!direct || face == (Direction)state.data.lever.support_dir) ?
               15 : 0;
    default:
        return 0;
    }
}
