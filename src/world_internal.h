#ifndef RS_WORLD_INTERNAL_H
#define RS_WORLD_INTERNAL_H
#include "rs/world.h"
#include "chunk.h"
#include "posmap.h"
typedef struct Simulator Simulator;
enum { WORLD_READ_CACHE_SIZE = 64 };
typedef struct {
    BlockPos pos;
    BlockState state;
    uint64_t stamp;
} WorldReadEntry;
typedef struct {
    BlockPos pos;
    uint64_t stamp, external_stamp;
    uint8_t power, external_power;
} WirePowerEntry;
struct World {
    WorldConfig config;
    PosMap chunk_map;
    Chunk **chunks;
    size_t chunk_count, chunk_capacity, loaded_count;
    FILE *backing;
    BlockPos view_center;
    int view_radius;
    WorldPlotBounds plot;
    uint64_t tick, revision, topology_revision;
    Simulator *sim;
    WorldChangeObserver observer;
    void *observer_context;
    uint64_t read_stamp, read_cache_stamp, external_power_stamp;
    WorldReadEntry reads[WORLD_READ_CACHE_SIZE];
    WirePowerEntry wire_powers[WORLD_READ_CACHE_SIZE];
    RsResult action_error;
    bool action_error_recoverable;
    char action_error_message[512];
};
bool world_action_ready(World *world, const char *operation, const BlockPos *pos);
void world_record_error(World *world, RsResult result, const char *operation,
                         const BlockPos *pos, const char *detail);
bool world_write_derived(World *world, BlockPos pos, BlockState state);
enum { UPDATE_NEIGHBORS = 1, UPDATE_CLIENTS = 2, UPDATE_NO_RERENDER = 4,
       UPDATE_KNOWN_SHAPE = 16,
       UPDATE_MOVING = 64, UPDATE_SHAPE_LIMIT = 512 };
bool world_set_block(World *, BlockPos, BlockState, unsigned flags);
bool world_set_block_depth(World *, BlockPos, BlockState, unsigned flags,
                           unsigned remaining_shapes);
/* Writes auxiliary cell data (input indicators, inventory fullness, comparator
 * output cache) without vanilla block-state callbacks. Read/power caches and
 * the storage observer are still updated. */
bool world_write_visual(World *world, BlockPos pos, BlockState state);
void world_remove_internal(World *world, BlockPos pos);
Chunk *world_find_chunk(const World *world, BlockPos key);
Chunk *world_ensure_chunk(World *world, BlockPos key);
bool world_insert_chunk(World *world, Chunk *chunk);
bool world_plot_bounds_valid(WorldPlotBounds bounds);
bool world_chunk_in_bounds(const World *world, BlockPos key);
bool world_chunk_snapshot(World *world, Chunk *chunk, ByteBuffer *bytes);
bool world_archive_chunk(World *world, Chunk *chunk);
#endif
