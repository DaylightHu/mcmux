#include "sim_internal.h"
#include "sim_executor.h"

static const Direction neighbors[] = {
    DIR_WEST, DIR_EAST, DIR_DOWN, DIR_UP, DIR_NORTH, DIR_SOUTH
};
static const Direction shapes[] = {
    DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH, DIR_DOWN, DIR_UP
};
static const Direction values[] = {
    DIR_DOWN, DIR_UP, DIR_NORTH, DIR_SOUTH, DIR_WEST, DIR_EAST
};

bool block_states_equal(BlockState a, BlockState b)
{
    if (a.id != b.id) return false;
    /* Compare the hot dust path without encoding state IDs. prefer_dot and
     * the piston's input indicator are not vanilla block-state properties. */
    if (a.id == BLOCK_REDSTONE_WIRE)
        return a.data.wire.power == b.data.wire.power &&
               a.data.wire.connections == b.data.wire.connections &&
               a.data.wire.up_connections == b.data.wire.up_connections;
    if (a.id == BLOCK_REDSTONE_COMPARATOR)
        return a.data.comparator.facing == b.data.comparator.facing &&
               a.data.comparator.subtract == b.data.comparator.subtract &&
               a.data.comparator.powered == b.data.comparator.powered;
    if (a.id == BLOCK_NOTE_BLOCK)
        return a.data.note_block.note == b.data.note_block.note &&
               a.data.note_block.instrument == b.data.note_block.instrument &&
               a.data.note_block.powered == b.data.note_block.powered;
    if (a.id == BLOCK_BARREL)
        return a.data.barrel.facing == b.data.barrel.facing &&
               a.data.barrel.open == b.data.barrel.open;
    if (block_is_rail((BlockId)a.id))
        return a.data.rail.powered == b.data.rail.powered &&
               a.data.rail.ascending == b.data.rail.ascending &&
               (a.data.rail.ascending ?
                    a.data.rail.facing == b.data.rail.facing :
                    a.data.rail.facing/2 == b.data.rail.facing/2);
    if (block_is_piston_base((BlockId)a.id) || block_is_piston_head((BlockId)a.id))
        return a.data.piston.facing == b.data.piston.facing &&
               a.data.piston.extended == b.data.piston.extended;
    StateId x, y;
    return state_encode(a, &x) && state_encode(b, &y) && x == y;
}

void sim_neighbor_changed(World *w, BlockPos pos)
{
    if (w->sim->error != RS_OK || w->sim->updates_suppressed ||
        !world_in_bounds(w, pos)) return;
    const BlockDef *d = block_definition((BlockId)world_get(w, pos).id);
    if (d->neighbor_changed) {
        ++w->sim->stats.neighbor_checks;
        d->neighbor_changed(w, pos);
    }
}

void sim_update_neighbors_at(World *w, BlockPos pos, Direction except)
{
    uint8_t mask = 0;
    bool known = sim_executor_neighbor_mask(w, pos, &mask);
    if (!known) {
        for (unsigned i = 0; i < DIR_COUNT; ++i)
            if (neighbors[i] != except)
                sim_neighbor_changed(w, block_offset(pos, neighbors[i]));
        return;
    }
    uint64_t revision = w->revision;
    for (unsigned i = 0; i < DIR_COUNT; ++i) {
        if (known && revision != w->revision) {
            known = sim_executor_neighbor_mask(w, pos, &mask);
            revision = w->revision;
        }
        if (neighbors[i] != except && (!known || (mask & (1u << neighbors[i]))))
            sim_neighbor_changed(w, block_offset(pos, neighbors[i]));
    }
}

void sim_update_output_neighbors(World *w, BlockPos pos, Direction output)
{
    BlockPos front = block_offset(pos, output);
    sim_neighbor_changed(w, front);
    sim_update_neighbors_at(w, front, direction_opposite(output));
}

void sim_update_neighbors_around(World *w, BlockPos pos)
{
    for (unsigned i = 0; i < DIR_COUNT; ++i)
        sim_update_neighbors_at(w, block_offset(pos, values[i]), DIR_COUNT);
}

static void removed(World *w, StateChange c, bool changed_type)
{
    bool moving = (c.flags & UPDATE_MOVING) != 0;
    switch ((BlockId)c.previous.id) {
    case BLOCK_REDSTONE_WIRE:
        if (changed_type && !moving) wire_on_remove(w, c.pos, c.previous);
        break;
    case BLOCK_REDSTONE_TORCH:
        if (!moving) sim_update_neighbors_around(w, c.pos);
        break;
    case BLOCK_LEVER:
        if (changed_type && !moving && c.previous.data.lever.powered)
            lever_update_neighbors(w, c.pos, c.previous);
        break;
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
        if (changed_type && !moving)
            sim_update_output_neighbors(w, c.pos,
                (Direction)(c.previous.id == BLOCK_REDSTONE_REPEATER ?
                    c.previous.data.repeater.facing : c.previous.data.comparator.facing));
        break;
    case BLOCK_OBSERVER:
        if (changed_type && c.previous.data.observer.powered &&
            scheduler_pending(&w->sim->scheduler, c.pos,
                               scheduler_block_type(c.previous)))
            sim_update_output_neighbors(w, c.pos,
                (Direction)c.previous.data.observer.facing);
        break;
    case BLOCK_POWERED_RAIL:
    case BLOCK_ACTIVATOR_RAIL:
        if (!moving) {
            if (c.previous.data.rail.ascending)
                sim_update_neighbors_at(w, block_offset(c.pos, DIR_UP), DIR_COUNT);
            sim_update_neighbors_at(w, c.pos, DIR_COUNT);
            sim_update_neighbors_at(w, block_offset(c.pos, DIR_DOWN), DIR_COUNT);
        }
        break;
    case BLOCK_PISTON_HEAD:
    case BLOCK_STICKY_PISTON_HEAD:
        if (changed_type) {
            Direction facing = (Direction)c.previous.data.piston.facing;
            BlockPos behind = block_offset(c.pos, direction_opposite(facing));
            BlockState base = world_get(w, behind);
            if (block_piston_pair((BlockId)base.id, (BlockId)c.previous.id) &&
                base.data.piston.extended && base.data.piston.facing == facing)
                world_remove_internal(w, behind);
        }
        break;
    case BLOCK_MOVING_PISTON:
        if (changed_type) (void)sim_finish_piston_motion(w, c.pos, true);
        break;
    default: break;
    }
    if (changed_type && container_is_block((BlockId)c.previous.id))
        sim_notify_analog(w, c.pos);
}

static void placed(World *w, StateChange c, bool changed_type)
{
    switch ((BlockId)c.current.id) {
    case BLOCK_REDSTONE_WIRE:
        if (changed_type) wire_on_place(w, c.pos, c.current);
        break;
    case BLOCK_REDSTONE_TORCH:
        sim_update_neighbors_around(w, c.pos);
        break;
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
        sim_update_output_neighbors(w, c.pos,
            (Direction)(c.current.id == BLOCK_REDSTONE_REPEATER ?
                c.current.data.repeater.facing : c.current.data.comparator.facing));
        break;
    case BLOCK_PISTON:
    case BLOCK_STICKY_PISTON:
        if (changed_type) piston_check_input(w, c.pos);
        break;
    case BLOCK_HOPPER:
        if (changed_type) sim_mark_input(w, c.pos);
        break;
    case BLOCK_POWERED_RAIL:
    case BLOCK_ACTIVATOR_RAIL:
        if (changed_type)
            rail_on_place(w, c.pos, c.current,
                          (c.flags & UPDATE_MOVING) != 0);
        break;
    case BLOCK_OBSERVER:
        if (changed_type && c.current.data.observer.powered &&
            !sim_has_scheduled_tick(w, c.pos)) {
            BlockState state = c.current;
            state.data.observer.powered = false;
            if (world_set_block(w, c.pos, state, 18))
                sim_update_output_neighbors(w, c.pos,
                    (Direction)state.data.observer.facing);
        }
        break;
    case BLOCK_LEVER:
        if (changed_type && c.current.data.lever.powered)
            lever_update_neighbors(w, c.pos, c.current);
        break;
    default: break;
    }
}

/* Direction and support properties here are part of the compiled region's
 * shape. Keep this predicate in step with update_shape below. */
bool sim_shape_update_needed(BlockState state, Direction toward)
{
    if (block_definition((BlockId)state.id)->observed_changed) return true;
    switch ((BlockId)state.id) {
    case BLOCK_REDSTONE_WIRE: return toward != DIR_DOWN;
    case BLOCK_REDSTONE_REPEATER: return toward/2 != state.data.repeater.facing/2;
    case BLOCK_NOTE_BLOCK: return toward == DIR_DOWN;
    case BLOCK_REDSTONE_TORCH: return toward == (Direction)state.data.torch.support_dir;
    case BLOCK_LEVER: return toward == (Direction)state.data.lever.support_dir;
    case BLOCK_PISTON_HEAD:
    case BLOCK_STICKY_PISTON_HEAD:
        return toward == direction_opposite((Direction)state.data.piston.facing);
    default: return false;
    }
}

static BlockState update_shape(World *w, BlockPos pos, BlockState state,
                                Direction toward, BlockPos source)
{
    if (state.id == BLOCK_REDSTONE_WIRE)
        state = wire_update_shape(w, pos, state, toward);
    else if (state.id == BLOCK_REDSTONE_REPEATER) {
        if (toward/2 != state.data.repeater.facing/2)
            state.data.repeater.locked = repeater_is_locked(w, pos, state);
    } else if (state.id == BLOCK_NOTE_BLOCK) {
        if (toward == DIR_DOWN)
            state.data.note_block.instrument = (uint8_t)note_instrument(world_get(w, source));
    } else if (state.id == BLOCK_REDSTONE_TORCH) {
        Direction support = (Direction)state.data.torch.support_dir;
        if (toward == support && !torch_supported(w, pos, support))
            state = block_default(BLOCK_AIR);
    } else if (state.id == BLOCK_LEVER) {
        Direction support = (Direction)state.data.lever.support_dir;
        if (toward == support && !lever_supported(w, pos, support))
            state = block_default(BLOCK_AIR);
    } else if (block_is_piston_head((BlockId)state.id)) {
        Direction facing = (Direction)state.data.piston.facing;
        if (toward == direction_opposite(facing)) {
            BlockState base = world_get(w, source);
            bool fitted = block_piston_pair((BlockId)base.id, (BlockId)state.id) &&
                          base.data.piston.extended && base.data.piston.facing == facing;
            bool moving = base.id == BLOCK_MOVING_PISTON &&
                          base.data.moving_piston.facing == facing;
            if (!fitted && !moving)
                state = block_default(BLOCK_AIR);
        }
    }
    const BlockDef *d = block_definition((BlockId)state.id);
    if (d->observed_changed) d->observed_changed(w, pos, state, source);
    return state;
}

BlockState sim_update_from_neighbor_shapes(World *w, BlockPos pos, BlockState state)
{
    if (w->sim->updates_suppressed) return state;
    /* Block.updateFromNeighbourShapes evaluates a carried state while its
     * world cell still contains MOVING_PISTON, in W/E/N/S/D/U order. */
    for (unsigned i = 0; i < DIR_COUNT && w->sim->error == RS_OK; ++i)
        state = update_shape(w, pos, state, shapes[i], block_offset(pos, shapes[i]));
    return state;
}

void sim_update_shape_at(World *w, BlockPos pos, Direction toward,
                          BlockPos source, unsigned flags, unsigned limit)
{
    if (w->sim->error != RS_OK || w->sim->updates_suppressed ||
        !world_in_bounds(w, pos)) return;
    BlockState previous = world_get(w, pos);
    BlockState state = update_shape(w, pos, previous, toward, source);
    if (!block_states_equal(previous, state))
        (void)world_set_block_depth(w, pos, state,
            state.id == BLOCK_AIR ? 3 : flags & ~32u, limit);
}

void sim_update_neighbor_shapes(World *w, BlockPos pos, unsigned flags, unsigned limit)
{
    uint8_t mask;
    bool known = sim_executor_shape_mask(w, pos, &mask);
    uint64_t revision = w->revision, topology = w->topology_revision;
    for (unsigned i = 0; i < DIR_COUNT; ++i) {
        if (known && (revision != w->revision || topology != w->topology_revision)) {
            known = sim_executor_shape_mask(w, pos, &mask);
            revision = w->revision;
            topology = w->topology_revision;
        }
        if (known && !(mask & (1u << shapes[i]))) continue;
        sim_update_shape_at(w, block_offset(pos, shapes[i]),
            direction_opposite(shapes[i]), pos, flags, limit);
    }
}

static void notify_state_change(World *w, StateChange c)
{
    if (!block_states_equal(world_get(w, c.pos), c.current) ||
        w->sim->error != RS_OK) return;
    if (c.flags & UPDATE_NEIGHBORS) {
        sim_update_neighbors_at(w, c.pos, DIR_COUNT);
        if (block_definition((BlockId)c.current.id)->analog_output)
            sim_notify_analog(w, c.pos);
    }
    if (!(c.flags & UPDATE_KNOWN_SHAPE) && c.remaining_shapes) {
        unsigned flags = c.flags & ~33u, limit = c.remaining_shapes - 1;
        if (c.previous.id == BLOCK_REDSTONE_WIRE)
            wire_indirect_shapes(w, c.pos, c.previous, flags, limit);
        sim_update_neighbor_shapes(w, c.pos, flags, limit);
        if (c.current.id == BLOCK_REDSTONE_WIRE)
            wire_indirect_shapes(w, c.pos, c.current, flags, limit);
    }
}

void sim_reference_state_changed(World *w, StateChange c)
{
    bool changed_type = scheduler_block_type(c.previous) != scheduler_block_type(c.current);
    if (w->sim->updates_suppressed) {
        /* Silent edits still release and attach per-cell runtime resources. */
        if (changed_type) {
            sim_forget_cell(w, c.pos);
            if (c.current.id == BLOCK_REDSTONE_TORCH && !sim_attach_torch(w, c.pos))
                w->sim->error = RS_OUT_OF_MEMORY;
            if (container_is_block((BlockId)c.current.id) &&
                !container_attach(w, c.pos, c.current))
                w->sim->error = RS_OUT_OF_MEMORY;
        }
        return;
    }
    removed(w, c, changed_type);
    /* A plot crop reports virtual removals outside the new bounds. Its
     * discarded runtimes are released together, preserving linear cleanup. */
    if (changed_type && world_in_bounds(w, c.pos)) sim_forget_cell(w, c.pos);
    if (world_get(w, c.pos).id != c.current.id || w->sim->error != RS_OK) return;
    placed(w, c, changed_type);
    if (changed_type && w->sim->error == RS_OK &&
        world_get(w, c.pos).id == c.current.id) {
        BlockState state = world_get(w, c.pos);
        if (state.id == BLOCK_REDSTONE_TORCH && !sim_attach_torch(w, c.pos))
            w->sim->error = RS_OUT_OF_MEMORY;
        if (container_is_block((BlockId)state.id) &&
            !container_attach(w, c.pos, state))
            w->sim->error = RS_OUT_OF_MEMORY;
    }
    notify_state_change(w, c);
}

void sim_reference_edit_state_changed(World *w, StateChange c)
{
    if (w->sim->updates_suppressed || w->sim->error != RS_OK) return;
    bool changed_type = scheduler_block_type(c.previous) != scheduler_block_type(c.current);
    removed(w, c, changed_type);
    if (world_get(w, c.pos).id != c.current.id || w->sim->error != RS_OK) return;
    placed(w, c, true);
    sim_neighbor_changed(w, c.pos);
    BlockState state = world_get(w, c.pos);
    if (state.id == BLOCK_NOTE_BLOCK || block_is_piston_head((BlockId)state.id)) {
        Direction support = state.id == BLOCK_NOTE_BLOCK ? DIR_DOWN :
            direction_opposite((Direction)state.data.piston.facing);
        sim_update_shape_at(w, c.pos, support, block_offset(c.pos, support),
                            c.flags, c.remaining_shapes);
    }
    c.current = world_get(w, c.pos);
    if (block_states_equal(c.previous, c.current)) {
        if (block_definition((BlockId)c.current.id)->analog_output)
            sim_notify_analog(w, c.pos);
        return;
    }
    notify_state_change(w, c);
}
