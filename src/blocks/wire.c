#include "block_internal.h"
#include "sim_internal.h"

#define FACE_BIT(face) ((uint8_t)(1u << (unsigned)(face)))
#define X_CONNECTIONS ((uint8_t)(FACE_BIT(DIR_EAST) | FACE_BIT(DIR_WEST)))
#define Z_CONNECTIONS ((uint8_t)(FACE_BIT(DIR_SOUTH) | FACE_BIT(DIR_NORTH)))
#define HORIZONTAL_CONNECTIONS ((uint8_t)(X_CONNECTIONS | Z_CONNECTIONS))

static const Direction horizontal_faces[] = {DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST};

static bool is_wire(const World *world, BlockPos position)
{
    return world_get(world, position).id == BLOCK_REDSTONE_WIRE;
}

static bool connects(BlockState state, Direction face)
{
    if (state.id == BLOCK_REDSTONE_REPEATER) {
        Direction facing = (Direction)state.data.repeater.facing;
        return facing == face || facing == direction_opposite(face);
    }
    if (state.id == BLOCK_REDSTONE_COMPARATOR) {
        /* Comparators accept dust from the rear and from either side. */
        return true;
    }
    if (state.id == BLOCK_OBSERVER)
        return state.data.observer.facing == direction_opposite(face);

    return state.id == BLOCK_REDSTONE_WIRE ||
           block_definition((BlockId)state.id)->signal_source;
}

bool wire_supported(const World *world, BlockPos position)
{
    return block_supports_wire(world, block_offset(position, DIR_DOWN));
}

size_t wire_neighbors(const World *world, BlockPos position, BlockPos output[12])
{
    size_t count = 0;
    bool can_ascend = !block_is_conductor(world, block_offset(position, DIR_UP));

    for (size_t cursor = 0; cursor < 4; ++cursor) {
        BlockPos neighbor = block_offset(position, horizontal_faces[cursor]);
        if (is_wire(world, neighbor)) {
            output[count++] = neighbor;
        }
        BlockPos above = block_offset(neighbor, DIR_UP);
        BlockPos below = block_offset(neighbor, DIR_DOWN);
        /* Incoming power samples differ from the rendered arms: an arm can
         * climb glass or a hopper, but its upper dust cannot feed this dust. */
        if (can_ascend && block_is_conductor(world, neighbor) && is_wire(world, above)) {
            output[count++] = above;
        }
        if (!block_is_conductor(world, neighbor) && is_wire(world, below)) {
            output[count++] = below;
        }
    }
    return count;
}

static unsigned connecting_side(const World *w, BlockPos p, Direction d, bool ascend)
{
    BlockPos next = block_offset(p, d);
    BlockState state = world_get(w, next);
    const BlockDef *def = block_definition((BlockId)state.id);
    if (ascend && block_supports_wire(w, next) &&
        is_wire(w, block_offset(next, DIR_UP)))
        return block_face_sturdy(w, next, direction_opposite(d)) ? 3u : 1u;
    return connects(state, d) || (!def->conductor &&
           is_wire(w, block_offset(next, DIR_DOWN))) ? 1u : 0u;
}

BlockState wire_connection_state(const World *w, BlockPos p, BlockState state)
{
    bool dot = state.data.wire.connections == 0;
    bool ascend = !block_is_conductor(w, block_offset(p, DIR_UP));
    uint8_t connections = 0, up = 0;
    for (unsigned i = 0; i < 4; ++i) {
        Direction d = horizontal_faces[i];
        unsigned side = connecting_side(w, p, d, ascend);
        if (side) connections |= FACE_BIT(d);
        if (side == 3) up |= FACE_BIT(d);
    }
    if (!dot || connections) {
        bool no_x = !(connections & X_CONNECTIONS);
        bool no_z = !(connections & Z_CONNECTIONS);
        if (no_z) connections |= X_CONNECTIONS;
        if (no_x) connections |= Z_CONNECTIONS;
    }
    state.data.wire.connections = connections;
    state.data.wire.up_connections = up;
    state.data.wire.prefer_dot = connections == 0;
    return state;
}

BlockState wire_update_shape(const World *w, BlockPos p, BlockState state, Direction d)
{
    if (d == DIR_DOWN) return state;
    if (d == DIR_UP) return wire_connection_state(w, p, state);
    unsigned side = connecting_side(w, p, d,
        !block_is_conductor(w, block_offset(p, DIR_UP)));
    bool connected = (state.data.wire.connections & FACE_BIT(d)) != 0;
    if (connected == (side != 0) && state.data.wire.connections != HORIZONTAL_CONNECTIONS) {
        state.data.wire.connections &= (uint8_t)~FACE_BIT(d);
        state.data.wire.up_connections &= (uint8_t)~FACE_BIT(d);
        if (side) state.data.wire.connections |= FACE_BIT(d);
        if (side == 3) state.data.wire.up_connections |= FACE_BIT(d);
        state.data.wire.prefer_dot = state.data.wire.connections == 0;
        return state;
    }
    state.data.wire.connections = HORIZONTAL_CONNECTIONS;
    return wire_connection_state(w, p, state);
}

void wire_refresh_shape(World *w, BlockPos p)
{
    BlockState state = world_get(w, p);
    if (state.id == BLOCK_REDSTONE_WIRE)
        (void)world_set_block(w, p, wire_connection_state(w, p, state), 2);
}

uint8_t wire_external_power(const World *world, BlockPos position)
{
    /* Suppress wire output throughout solid-block queries to prevent feedback.
     * Wire-to-wire transfer belongs exclusively to the explicit connection graph. */
    return power_received(world, position, SIGNAL_WITHOUT_WIRE);
}

static uint8_t wire_signal(const World *world, BlockPos position,
                           Direction face, SignalMode mode)
{
    if (mode == SIGNAL_WITHOUT_WIRE || face == DIR_UP) {
        return 0;
    }
    BlockState state = world_get(world, position);
    if (!state.data.wire.power) return 0;
    if (face != DIR_DOWN)
        state = wire_connection_state(world, position, state);
    return block_state_signal(state, face, mode, false);
}

static void wire_neighbor(World *world, BlockPos position)
{
    if (!wire_supported(world, position)) {
        world_remove_internal(world, position);
        return;
    }
    wire_update_power(world, position, world_get(world, position));
}

/* Both query forms are suppressed in SIGNAL_WITHOUT_WIRE mode. */
const BlockDef rs_wire_def = {
    .name = "redstone_wire",
    .signal_source = true,
    .push_reaction = PUSH_DESTROY,
    .signal = wire_signal,
    .direct_signal = wire_signal,
    .neighbor_changed = wire_neighbor
};
