#include "wire_solver.h"
#include "sim_internal.h"
#include "sim_executor.h"

static const Direction horizontal[] = {DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST};
static const Direction values[] = {
    DIR_DOWN, DIR_UP, DIR_NORTH, DIR_SOUTH, DIR_WEST, DIR_EAST
};

static uint8_t dust(const World *w, BlockPos p)
{
    BlockState s = world_get(w, p);
    return s.id == BLOCK_REDSTONE_WIRE ? s.data.wire.power : 0;
}

static uint8_t calculate_strength(const World *w, BlockPos p, uint8_t external)
{
    if (external == 15) return 15;
    uint8_t adjacent = 0;
    bool ceiling = block_is_conductor(w, block_offset(p, DIR_UP));
    for (unsigned i = 0; i < 4; ++i) {
        BlockPos next = block_offset(p, horizontal[i]);
        BlockState neighbor = world_get(w, next);
        uint8_t level = neighbor.id == BLOCK_REDSTONE_WIRE ? neighbor.data.wire.power : 0;
        if (level > adjacent) adjacent = level;
        bool solid = block_definition((BlockId)neighbor.id)->conductor;
        if (solid && !ceiling) level = dust(w, block_offset(next, DIR_UP));
        else if (!solid) level = dust(w, block_offset(next, DIR_DOWN));
        else level = 0;
        if (level > adjacent) adjacent = level;
    }
    if (adjacent) --adjacent;
    return external > adjacent ? external : adjacent;
}

static uint8_t target_strength(World *w, BlockPos p, BlockState state)
{
    RedstoneEvaluation value;
    if (sim_executor_evaluate(w, p, state, &value) == RS_OK)
        return value.target_power;
    uint32_t hash = (uint32_t)p.x*0x9e3779b1u ^ (uint32_t)p.y*0x85ebca6bu ^
                    (uint32_t)p.z*0xc2b2ae35u;
    WirePowerEntry *entry = &w->wire_powers[hash & (WORLD_READ_CACHE_SIZE - 1)];
    /* Repeated notifications still execute in vanilla order. Only the pure
     * strength query is memoized; every stored-state write or chunk insertion
     * invalidates it before any callbacks, using the world read epoch. */
    if (entry->stamp == w->read_stamp && pos_equal(entry->pos, p))
        return entry->power;
    uint8_t external = entry->external_stamp == w->external_power_stamp &&
                       pos_equal(entry->pos, p) ? entry->external_power :
                       wire_external_power(w, p);
    uint8_t power = calculate_strength(w, p, external);
    if (w->sim->error == RS_OK)
        *entry = (WirePowerEntry){p, w->read_stamp, w->external_power_stamp,
                                 power, external};
    return power;
}

static unsigned bucket(BlockPos p)
{
    /* Vec3i.hashCode and Java 8 HashMap's spread, default capacity 16.
     * Seven distinct positions never resize or treeify the HashSet. */
    uint32_t hash = ((uint32_t)p.y + (uint32_t)p.z*31u)*31u + (uint32_t)p.x;
    return (hash ^ (hash >> 16)) & 15u;
}

static void power_neighbors(World *w, BlockPos pos)
{
    BlockPos centers[7] = {pos};
    unsigned char buckets[7];
    for (unsigned i = 0; i < DIR_COUNT; ++i)
        centers[i + 1] = block_offset(pos, values[i]);
    for (unsigned i = 0; i < 7; ++i) buckets[i] = (unsigned char)bucket(centers[i]);
    /* Stable insertion sort retains insertion order inside each hash bucket.
     * This reproduces the vanilla set order without allocating a HashSet. */
    for (unsigned i = 1; i < 7; ++i) {
        BlockPos p = centers[i];
        unsigned j = i;
        unsigned char key = buckets[i];
        while (j && buckets[j - 1] > key) {
            centers[j] = centers[j - 1];
            buckets[j] = buckets[j - 1];
            --j;
        }
        centers[j] = p;
        buckets[j] = key;
    }
    for (unsigned i = 0; i < 7; ++i)
        sim_update_neighbors_at(w, centers[i], DIR_COUNT);
}

void wire_update_power(World *w, BlockPos pos, BlockState previous)
{
    ++w->sim->stats.wires_visited;
    uint8_t power = target_strength(w, pos, previous);
    if (previous.data.wire.power == power) return;
    if (block_states_equal(world_get(w, pos), previous)) {
        BlockState state = previous;
        state.data.wire.power = power;
        if (!world_set_block(w, pos, state, 2)) return;
        ++w->sim->stats.wire_changes;
    }
    power_neighbors(w, pos);
}

static void corner_change(World *w, BlockPos p)
{
    if (world_get(w, p).id != BLOCK_REDSTONE_WIRE) return;
    sim_update_neighbors_at(w, p, DIR_COUNT);
    for (unsigned i = 0; i < DIR_COUNT; ++i)
        sim_update_neighbors_at(w, block_offset(p, values[i]), DIR_COUNT);
}

static void neighboring_wires(World *w, BlockPos p)
{
    for (unsigned i = 0; i < 4; ++i)
        corner_change(w, block_offset(p, horizontal[i]));
    for (unsigned i = 0; i < 4; ++i) {
        BlockPos n = block_offset(p, horizontal[i]);
        corner_change(w, block_offset(n, block_is_conductor(w, n) ? DIR_UP : DIR_DOWN));
    }
}

void wire_on_place(World *w, BlockPos p, BlockState state)
{
    wire_update_power(w, p, state);
    sim_update_neighbors_at(w, block_offset(p, DIR_UP), DIR_COUNT);
    sim_update_neighbors_at(w, block_offset(p, DIR_DOWN), DIR_COUNT);
    neighboring_wires(w, p);
}

void wire_on_remove(World *w, BlockPos p, BlockState previous)
{
    for (unsigned i = 0; i < DIR_COUNT; ++i)
        sim_update_neighbors_at(w, block_offset(p, values[i]), DIR_COUNT);
    wire_update_power(w, p, previous);
    neighboring_wires(w, p);
}

void wire_indirect_shapes(World *w, BlockPos p, BlockState state,
                           unsigned flags, unsigned limit)
{
    for (unsigned i = 0; i < 4; ++i) {
        Direction d = horizontal[i];
        BlockPos n = block_offset(p, d);
        if (!(state.data.wire.connections & (1u << d)) ||
            world_get(w, n).id == BLOCK_REDSTONE_WIRE) continue;
        const Direction vertical[] = {DIR_DOWN, DIR_UP};
        for (unsigned j = 0; j < 2; ++j) {
            BlockPos target = block_offset(n, vertical[j]);
            if (world_get(w, target).id == BLOCK_OBSERVER) continue;
            Direction back = direction_opposite(d);
            sim_update_shape_at(w, target, back, block_offset(target, back), flags, limit);
        }
    }
}
