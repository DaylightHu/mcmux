#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "world_internal.h"
#include "sim_internal.h"
#include "worldgen.h"
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

static FILE *open_backing_file(void)
{
#ifdef _WIN32
    char directory[MAX_PATH], path[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, directory);
    if (!length || length >= MAX_PATH || !GetTempFileNameA(directory, "mcw", 0, path)) return NULL;
    HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (handle == INVALID_HANDLE_VALUE) { DeleteFileA(path); return NULL; }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); return NULL; }
    FILE *file = _fdopen(fd, "w+b");
    if (!file) _close(fd);
    return file;
#else
    FILE *file = tmpfile();
    if (file) return file;
    /* Some restricted environments have no writable system temp directory. */
    char path[] = ".machinecraft-cache-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    file = fdopen(fd, "w+b");
    if (!file) close(fd);
    return file;
#endif
}

WorldConfig world_config_default(void) { return (WorldConfig){.ground_layers = 1}; }
static void invalidate_reads(World *w, bool wire_properties_only)
{
    if (!++w->read_stamp) {
        memset(w->reads, 0, sizeof(w->reads));
        memset(w->wire_powers, 0, sizeof(w->wire_powers));
        w->read_stamp = 1;
    }
    /* SIGNAL_WITHOUT_WIRE cannot depend on dust power or arm properties.
     * Keep these query results across dust-only writes, never across changes
     * to an emitter, conductor, block type, or newly inserted chunk. */
    if (!wire_properties_only && !++w->external_power_stamp) {
        memset(w->wire_powers, 0, sizeof(w->wire_powers));
        w->external_power_stamp = 1;
    }
}
static BlockPos chunk_key(BlockPos p) { return (BlockPos){floor_div16(p.x), 0, floor_div16(p.z)}; }
bool world_plot_bounds_valid(WorldPlotBounds b)
{
    int64_t max_x = b.max_x, max_z = b.max_z;
    return b.min_x >= INT32_MIN + 16 && b.min_z >= INT32_MIN + 16 &&
           b.max_x <= INT32_MAX - 16 && b.max_z <= INT32_MAX - 16 &&
           b.min_x <= b.max_x && b.min_z <= b.max_z &&
           b.min_x % WORLD_PLOT_SIZE == 0 && b.min_z % WORLD_PLOT_SIZE == 0 &&
           (max_x + 1) % WORLD_PLOT_SIZE == 0 &&
           (max_z + 1) % WORLD_PLOT_SIZE == 0;
}
bool world_chunk_in_bounds(const World *w, BlockPos key)
{
    if (!w || key.y) return false;
    int64_t x = (int64_t)key.x * CHUNK_SIZE;
    int64_t z = (int64_t)key.z * CHUNK_SIZE;
    return x >= w->plot.min_x && x + CHUNK_SIZE - 1 <= w->plot.max_x &&
           z >= w->plot.min_z && z + CHUNK_SIZE - 1 <= w->plot.max_z;
}
Chunk *world_find_chunk(const World *w, BlockPos key)
{
    size_t slot; return posmap_get(&w->chunk_map, key, &slot) ? w->chunks[slot] : NULL;
}
bool world_insert_chunk(World *w, Chunk *c)
{
    if (!world_chunk_in_bounds(w, c->key) || world_find_chunk(w, c->key)) return false;
    if (w->chunk_count == w->chunk_capacity) {
        size_t capacity = w->chunk_capacity ? w->chunk_capacity * 2 : 16;
        if (capacity < w->chunk_capacity || capacity > SIZE_MAX / sizeof(Chunk *)) return false;
        Chunk **next = realloc(w->chunks, capacity * sizeof(Chunk *));
        if (!next) return false;
        w->chunks = next; w->chunk_capacity = capacity;
    }
    if (!posmap_set(&w->chunk_map, c->key, w->chunk_count)) return false;
    w->chunks[w->chunk_count++] = c;
    invalidate_reads(w, false);
    if (c->resident) ++w->loaded_count;
    ++w->topology_revision; return true;
}
static bool restore_chunk(World *w, Chunk *c)
{
    ByteBuffer b; bytes_init(&b);
    if (!w->backing || !c->archive_size || fsetpos(w->backing, &c->archive_position) != 0) return false;
    b.data = malloc(c->archive_size);
    if (!b.data) return false;
    b.size = b.capacity = c->archive_size;
    bool read = fread(b.data, 1, b.size, w->backing) == b.size;
    Chunk *next = read ? chunk_decode(&b) : NULL;
    bool ok = next && b.ok && b.cursor == b.size && pos_equal(next->key, c->key);
    if (ok) {
        memcpy(c->sections, next->sections, sizeof(c->sections));
        memset(next->sections, 0, sizeof(next->sections));
        c->resident = true; ++w->loaded_count; ++w->topology_revision;
    }
    if (next) { chunk_release_sections(next); free(next); }
    bytes_destroy(&b); return ok;
}
Chunk *world_ensure_chunk(World *w, BlockPos key)
{
    Chunk *c = world_find_chunk(w, key);
    if (c) {
        if (!c->resident && !restore_chunk(w, c)) { w->sim->error = RS_INTERNAL_ERROR; return NULL; }
        return c;
    }
    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->key = key;
    if (!world_generate_chunk(w, c) || !world_insert_chunk(w, c)) {
        chunk_release_sections(c); free(c); return NULL;
    }
    return c;
}
bool world_chunk_snapshot(World *w, Chunk *c, ByteBuffer *b)
{
    if (c->resident) return chunk_encode(c, b);
    if (!w->backing || !c->archive_size || fsetpos(w->backing, &c->archive_position) != 0) return false;
    unsigned char block[4096]; size_t remaining = c->archive_size;
    while (remaining) {
        size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
        if (fread(block, 1, count, w->backing) != count) return false;
        bytes_write(b, block, count);
        if (!b->ok) return false;
        remaining -= count;
    }
    return true;
}
bool world_archive_chunk(World *w, Chunk *c)
{
    if (!c->resident) return true;
    if (c->modified && !c->archive_size) {
        ByteBuffer b; bytes_init(&b);
        bool ok = chunk_encode(c, &b);
        if (ok && !w->backing) w->backing = open_backing_file();
        fpos_t position;
        ok = ok && w->backing && fseek(w->backing, 0, SEEK_END) == 0 &&
             fgetpos(w->backing, &position) == 0 && fwrite(b.data, 1, b.size, w->backing) == b.size &&
             fflush(w->backing) == 0;
        if (ok) { c->archive_position = position; c->archive_size = b.size; }
        bytes_destroy(&b);
        if (!ok) return false;
    }
    chunk_release_sections(c); --w->loaded_count; ++w->topology_revision; return true;
}
World *world_create(WorldConfig config)
{
    if (config.ground_layers < 0 || config.ground_layers > 65) return NULL;
    World *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->config = config; w->view_radius = -1;
    w->plot = (WorldPlotBounds){0, 0, WORLD_PLOT_SIZE - 1,
                               WORLD_PLOT_SIZE - 1};
    w->read_stamp = 1;
    w->external_power_stamp = 1;
    if (!sim_init_internal(w)) { world_destroy(w); return NULL; }
    return w;
}
void world_destroy(World *w)
{
    if (!w) return;
    sim_destroy_internal(w);
    for (size_t i = 0; i < w->chunk_count; ++i) { chunk_release_sections(w->chunks[i]); free(w->chunks[i]); }
    free(w->chunks); posmap_destroy(&w->chunk_map);
    if (w->backing) fclose(w->backing);
    free(w);
}
RsResult world_result(const World *w)
{
    if (!w) return RS_INVALID_ARGUMENT;
    if (w->action_error != RS_OK) return w->action_error;
    return w->sim ? w->sim->error : RS_INTERNAL_ERROR;
}
const char *world_error_message(const World *w)
{
    if (!w) return "invalid world";
    if (w->action_error != RS_OK) return w->action_error_message;
    RsResult result = world_result(w);
    return result == RS_OK ? "" : rs_result_name(result);
}
void world_clear_error(World *w)
{
    if (!w || !w->sim || !w->action_error_recoverable ||
        w->sim->handling_tick || w->sim->block_entities.ticking || w->sim->checking ||
        w->sim->error != w->action_error) return;
    w->sim->error = RS_OK;
    w->action_error = RS_OK;
    w->action_error_recoverable = false;
    w->action_error_message[0] = '\0';
}
void world_record_error(World *w, RsResult result, const char *operation,
                         const BlockPos *pos, const char *detail)
{
    if (!w || result == RS_OK || result == RS_UNCHANGED ||
        w->action_error != RS_OK) return;
    w->action_error = result;
    w->action_error_recoverable = w->sim->error == RS_OK &&
        result != RS_INTERNAL_ERROR && result != RS_OUT_OF_MEMORY &&
        !w->sim->handling_tick && !w->sim->block_entities.ticking;
    if (w->sim->error == RS_OK) w->sim->error = result;
    if (pos) {
        snprintf(w->action_error_message, sizeof(w->action_error_message),
                 "%s at (%d, %d, %d), tick %" PRIu64 ": %s",
                 operation, pos->x, pos->y, pos->z, w->tick, detail);
    } else {
        snprintf(w->action_error_message, sizeof(w->action_error_message),
                 "%s at tick %" PRIu64 ": %s", operation, w->tick, detail);
    }
    fprintf(stderr, "%s\n", w->action_error_message);
}
bool world_action_ready(World *w, const char *operation, const BlockPos *pos)
{
    if (!w || w->action_error != RS_OK) return false;
    RsResult result = sim_flush(w);
    if (result == RS_OK) return true;
    world_record_error(w, result, operation, pos, rs_result_name(result));
    return false;
}
void world_expect(World *w, bool condition, const char *format, ...)
{
    if (!world_action_ready(w, "world_expect", NULL) || condition) return;
    char detail[384];
    int length = 0;
    if (format && *format) {
        va_list args;
        va_start(args, format);
        length = vsnprintf(detail, sizeof(detail), format, args);
        va_end(args);
    }
    if (length <= 0) {
        snprintf(detail, sizeof(detail), "condition is false");
    } else if ((size_t)length >= sizeof(detail)) {
        memcpy(detail + sizeof(detail) - 4, "...", 4);
    }
    world_record_error(w, RS_EXPECTATION_FAILED, "world_expect", NULL, detail);
}
bool world_in_bounds(const World *w, BlockPos p)
{
    return w && p.y >= WORLD_MIN_Y && p.y <= WORLD_MAX_Y &&
           p.x >= w->plot.min_x && p.x <= w->plot.max_x &&
           p.z >= w->plot.min_z && p.z <= w->plot.max_z;
}
WorldPlotBounds world_plot_bounds(const World *w)
{
    return w ? w->plot : (WorldPlotBounds){0};
}
RsResult world_expand_plot(World *w, Direction direction, uint32_t count)
{
    if (!w || !count || !direction_horizontal(direction))
        return RS_INVALID_ARGUMENT;
    if (w->sim->handling_tick || w->sim->block_entities.ticking)
        return RS_INVALID_ARGUMENT;
    WorldPlotBounds next = w->plot;
    int64_t delta = (int64_t)count * WORLD_PLOT_SIZE;
    if (direction == DIR_EAST) {
        int64_t value = (int64_t)next.max_x + delta;
        if (value > INT_MAX) return RS_OUT_OF_BOUNDS;
        next.max_x = (int)value;
    } else if (direction == DIR_WEST) {
        int64_t value = (int64_t)next.min_x - delta;
        if (value < INT_MIN) return RS_OUT_OF_BOUNDS;
        next.min_x = (int)value;
    } else if (direction == DIR_SOUTH) {
        int64_t value = (int64_t)next.max_z + delta;
        if (value > INT_MAX) return RS_OUT_OF_BOUNDS;
        next.max_z = (int)value;
    } else {
        int64_t value = (int64_t)next.min_z - delta;
        if (value < INT_MIN) return RS_OUT_OF_BOUNDS;
        next.min_z = (int)value;
    }
    if (!world_plot_bounds_valid(next)) return RS_OUT_OF_BOUNDS;
    RsResult result = sim_flush(w);
    if (result != RS_OK) return result;
    w->plot = next;
    invalidate_reads(w, false);
    ++w->revision;
    ++w->topology_revision;
    return RS_OK;
}
static bool chunk_within(BlockPos key, WorldPlotBounds bounds)
{
    int64_t x = (int64_t)key.x * CHUNK_SIZE, z = (int64_t)key.z * CHUNK_SIZE;
    return x >= bounds.min_x && x + CHUNK_SIZE - 1 <= bounds.max_x &&
           z >= bounds.min_z && z + CHUNK_SIZE - 1 <= bounds.max_z;
}

static RsResult shrink_bounds(World *w, Direction direction, uint32_t count,
                               WorldShrinkPreview *plan)
{
    if (!w || !plan || !count || !direction_horizontal(direction) ||
        w->sim->handling_tick || w->sim->block_entities.ticking)
        return RS_INVALID_ARGUMENT;
    *plan = (WorldShrinkPreview){.before = w->plot, .after = w->plot, .removed = w->plot};
    int64_t delta = (int64_t)count * WORLD_PLOT_SIZE;
    bool x_axis = direction == DIR_EAST || direction == DIR_WEST;
    int64_t extent = x_axis ? (int64_t)w->plot.max_x - w->plot.min_x + 1 :
                              (int64_t)w->plot.max_z - w->plot.min_z + 1;
    if (extent - delta < WORLD_PLOT_SIZE) return RS_OUT_OF_BOUNDS;
    if (direction == DIR_EAST) {
        plan->after.max_x = (int)((int64_t)w->plot.max_x - delta);
        plan->removed.min_x = plan->after.max_x + 1;
    } else if (direction == DIR_WEST) {
        plan->after.min_x = (int)((int64_t)w->plot.min_x + delta);
        plan->removed.max_x = plan->after.min_x - 1;
    } else if (direction == DIR_SOUTH) {
        plan->after.max_z = (int)((int64_t)w->plot.max_z - delta);
        plan->removed.min_z = plan->after.max_z + 1;
    } else {
        plan->after.min_z = (int)((int64_t)w->plot.min_z + delta);
        plan->removed.max_z = plan->after.min_z - 1;
    }
    return sim_flush(w);
}

static bool chunk_has_edits(const World *w, const Chunk *c)
{
    for (unsigned s = 0; s < SECTION_COUNT; ++s) {
        ChunkSection *section = c->sections[s];
        int low_y = WORLD_MIN_Y + (int)s*CHUNK_SIZE;
        if (!section && (low_y > 0 || low_y + CHUNK_SIZE <= 1 - w->config.ground_layers))
            continue;
        for (size_t i = 0; i < SECTION_VOLUME; ++i) {
            int y = low_y + (int)(i / (CHUNK_SIZE*CHUNK_SIZE));
            StateId expected = y <= 0 && y > -w->config.ground_layers ? BLOCK_IRON : BLOCK_AIR;
            if ((section ? section_get(section, i) : BLOCK_AIR) != expected) return true;
        }
    }
    return false;
}

RsResult world_preview_shrink(World *w, Direction direction, uint32_t count,
                               WorldShrinkPreview *plan)
{
    RsResult result = shrink_bounds(w, direction, count, plan);
    if (result != RS_OK) return result;
    for (size_t i = 0; i < w->chunk_count; ++i) {
        Chunk *c = w->chunks[i], *snapshot = NULL;
        if (!c->modified || chunk_within(c->key, plan->after)) continue;
        if (!c->resident) {
            ByteBuffer bytes; bytes_init(&bytes);
            bool ok = world_chunk_snapshot(w, c, &bytes);
            if (ok) snapshot = chunk_decode(&bytes);
            ok = ok && snapshot && bytes.ok && bytes.cursor == bytes.size &&
                 pos_equal(snapshot->key, c->key);
            bytes_destroy(&bytes);
            if (!ok) {
                if (snapshot) { chunk_release_sections(snapshot); free(snapshot); }
                return RS_INTERNAL_ERROR;
            }
        }
        bool edited = chunk_has_edits(w, snapshot ? snapshot : c);
        if (snapshot) { chunk_release_sections(snapshot); free(snapshot); }
        if (edited) { plan->has_edits = true; break; }
    }
    /* Default terrain can support an edited block just inside the new edge.
     * Warn before its removal would drop that block as a shape update. */
    if (!plan->has_edits && !w->sim->updates_suppressed) {
        bool x_axis = direction == DIR_EAST || direction == DIR_WEST;
        int edge = direction == DIR_EAST ? plan->after.max_x : direction == DIR_WEST ? plan->after.min_x :
                   direction == DIR_SOUTH ? plan->after.max_z : plan->after.min_z;
        for (size_t i = 0; i < w->chunk_count; ++i) {
            Chunk *c = w->chunks[i];
            if (!c->modified || !chunk_within(c->key, plan->after) ||
                (x_axis ? c->key.x : c->key.z) != floor_div16(edge)) continue;
            int start = (x_axis ? c->key.z : c->key.x)*CHUNK_SIZE;
            for (int t = start; t < start + CHUNK_SIZE; ++t)
                for (int y = WORLD_MIN_Y; y <= WORLD_MAX_Y; ++y) {
                    BlockPos pos = x_axis ? (BlockPos){edge,y,t} : (BlockPos){t,y,edge};
                    BlockState s = world_get(w, pos);
                    bool supported_outside =
                        (s.id == BLOCK_REDSTONE_TORCH && s.data.torch.support_dir == direction) ||
                        (block_is_rail((BlockId)s.id) && s.data.rail.ascending && s.data.rail.facing == direction);
                    if (supported_outside && world_get(w, block_offset(pos,direction)).id != BLOCK_AIR) {
                        plan->has_edits = true;
                        return sim_flush(w);
                    }
                    if (w->sim->error != RS_OK) return w->sim->error;
                }
        }
    }
    return RS_OK;
}

typedef struct { BlockPos pos; BlockState before; } BoundaryRemoval;
static int compare_removals(const void *a, const void *b)
{
    return pos_compare(((const BoundaryRemoval *)a)->pos, ((const BoundaryRemoval *)b)->pos);
}

static RsResult collect_boundary_removals(World *w, Direction direction,
                                         WorldPlotBounds after,
                                         BoundaryRemoval **items, size_t *count)
{
    bool x_axis = direction == DIR_EAST || direction == DIR_WEST;
    int edge = direction == DIR_EAST ? after.max_x : direction == DIR_WEST ? after.min_x :
               direction == DIR_SOUTH ? after.max_z : after.min_z;
    int outside = edge + (direction == DIR_EAST || direction == DIR_SOUTH ? 1 : -1);
    PosMap seen = {0}; size_t capacity = 0;
    RsResult result = RS_OK;
    for (size_t c = 0; c < w->chunk_count && result == RS_OK; ++c) {
        Chunk *chunk = w->chunks[c];
        if (!chunk->modified || !chunk_within(chunk->key, after) ||
            (x_axis ? chunk->key.x : chunk->key.z) != floor_div16(edge)) continue;
        if (!chunk->resident && !restore_chunk(w, chunk)) { result = RS_INTERNAL_ERROR; break; }
        int start = (x_axis ? chunk->key.z : chunk->key.x)*CHUNK_SIZE;
        for (int t = start - 1; t <= start + CHUNK_SIZE && result == RS_OK; ++t)
            for (int y = WORLD_MIN_Y; y <= WORLD_MAX_Y; ++y) {
                BlockPos pos = x_axis ? (BlockPos){outside,y,t} : (BlockPos){t,y,outside};
                if (!world_in_bounds(w, pos) || posmap_get(&seen, pos, NULL)) continue;
                BlockState before = world_get(w, pos);
                if (w->sim->error != RS_OK) { result = w->sim->error; break; }
                if (before.id == BLOCK_AIR) continue;
                if (*count == capacity) {
                    size_t next = capacity ? capacity*2 : 32;
                    if (next < capacity || next > SIZE_MAX/sizeof(**items)) { result = RS_OUT_OF_MEMORY; break; }
                    BoundaryRemoval *p = realloc(*items, next*sizeof(**items));
                    if (!p) { result = RS_OUT_OF_MEMORY; break; }
                    *items = p; capacity = next;
                }
                if (!posmap_set(&seen, pos, *count)) { result = RS_OUT_OF_MEMORY; break; }
                (*items)[(*count)++] = (BoundaryRemoval){pos, before};
            }
    }
    posmap_destroy(&seen);
    if (result == RS_OK && *count > 1) qsort(*items, *count, sizeof(**items), compare_removals);
    return result;
}

RsResult world_shrink_plot(World *w, Direction direction, uint32_t count)
{
    WorldShrinkPreview plan;
    RsResult result = shrink_bounds(w, direction, count, &plan);
    if (result != RS_OK) return result;
    BoundaryRemoval *removals = NULL; size_t removed_count = 0;
    if (!w->sim->updates_suppressed)
        result = collect_boundary_removals(w, direction, plan.after, &removals, &removed_count);
    if (result != RS_OK) { free(removals); return result; }
    w->plot = plan.after;
    invalidate_reads(w, false);
    ++w->revision; ++w->topology_revision;
    for (size_t i = 0; i < w->chunk_count;) {
        Chunk *c = w->chunks[i];
        if (world_chunk_in_bounds(w, c->key)) { ++i; continue; }
        posmap_remove(&w->chunk_map, c->key);
        if (c->resident) --w->loaded_count;
        chunk_release_sections(c); free(c);
        if (i < --w->chunk_count) {
            w->chunks[i] = w->chunks[w->chunk_count];
            (void)posmap_set(&w->chunk_map, w->chunks[i]->key, i);
        }
    }
    for (size_t i = 0; i < removed_count && w->sim->error == RS_OK; ++i) {
        BoundaryRemoval r = removals[i];
        sim_state_changed(w, (StateChange){r.pos,r.before,block_default(BLOCK_AIR),3,UPDATE_SHAPE_LIMIT});
        if (r.before.id == BLOCK_MOVING_PISTON) {
            BlockPos behind = block_offset(r.pos, direction_opposite((Direction)r.before.data.moving_piston.facing));
            BlockState base = world_get(w, behind);
            if (block_is_piston_base((BlockId)base.id) && base.data.piston.extended)
                world_remove_internal(w, behind);
        }
    }
    free(removals);
    sim_forget_outside_plot(w);
    return sim_flush(w);
}

BlockState world_get(const World *w, BlockPos p)
{
    if (!world_in_bounds(w, p)) return block_default(BLOCK_AIR);
    uint32_t hash = (uint32_t)p.x*0x9e3779b1u ^ (uint32_t)p.y*0x85ebca6bu ^
                    (uint32_t)p.z*0xc2b2ae35u;
    /* A per-world value cache, invalidated before notification callbacks on
     * every stored-state write. It never retains chunk/section pointers. */
    WorldReadEntry *entry = &((World *)w)->reads[hash & (WORLD_READ_CACHE_SIZE - 1)];
    if (entry->stamp == w->read_stamp && pos_equal(entry->pos, p)) return entry->state;
    Chunk *c = world_find_chunk(w, chunk_key(p));
    if (!c) {
        BlockState state = world_generated_state(w, p);
        *entry = (WorldReadEntry){p, state, w->read_stamp};
        return state;
    }
    /* Restoring storage is logically const: blocks and simulation time do not change. */
    if (!c->resident && !restore_chunk((World *)w, c)) {
        w->sim->error = RS_INTERNAL_ERROR; return block_default(BLOCK_AIR);
    }
    StateId id = section_get(c->sections[(p.y - WORLD_MIN_Y) / 16], section_index(p));
    BlockState state;
    if (!state_decode(id, &state)) { w->sim->error = RS_INTERNAL_ERROR; return block_default(BLOCK_AIR); }
    *entry = (WorldReadEntry){p, state, w->read_stamp};
    return state;
}
bool world_get_piston_motion(const World *w, BlockPos p,
                             WorldPistonMotion *out)
{
    if (!w || !out) return false;
    const PistonMotion *motion = sim_piston_motion(w, p);
    if (!motion || world_get(w, p).id != BLOCK_MOVING_PISTON) return false;
    *out = (WorldPistonMotion){
        .moved_state = motion->moved_state,
        .last_ticked = motion->last_ticked,
        .direction = (Direction)motion->direction,
        .progress = motion->progress,
        .previous_progress = motion->previous_progress,
        .extending = motion->extending,
        .source = motion->source
    };
    return true;
}
static bool world_write_state(World *w, BlockPos p, BlockState state,
                               BlockState *before, bool *changed)
{
    if (!w || w->sim->error != RS_OK) return false;
    StateId id;
    if (!world_in_bounds(w, p) || !state_encode(state, &id)) { w->sim->error = RS_INTERNAL_ERROR; return false; }
    BlockState previous = world_get(w, p); StateId old;
    if (!state_encode(previous, &old)) { w->sim->error = RS_INTERNAL_ERROR; return false; }
    if (before) *before = previous;
    if (changed) *changed = old != id;
    if (old == id) return true;
    Chunk *c = world_ensure_chunk(w, chunk_key(p));
    if (!c) { w->sim->error = RS_OUT_OF_MEMORY; return false; }
    unsigned section = (unsigned)(p.y - WORLD_MIN_Y) / 16;
    if (!c->sections[section]) {
        c->sections[section] = section_create();
        if (!c->sections[section]) { w->sim->error = RS_OUT_OF_MEMORY; return false; }
        ++w->topology_revision;
    }
    if (!section_set(c->sections[section], section_index(p), id)) { w->sim->error = RS_OUT_OF_MEMORY; return false; }
    if (!c->sections[section]->block_count) {
        section_destroy(c->sections[section]); c->sections[section] = NULL; ++w->topology_revision;
    }
    c->modified = true; c->archive_size = 0; ++w->revision;
    invalidate_reads(w, previous.id == BLOCK_REDSTONE_WIRE &&
                        state.id == BLOCK_REDSTONE_WIRE);
    if (w->observer) w->observer(w->observer_context, p, previous, state);
    return true;
}
bool world_write_derived(World *w, BlockPos p, BlockState state)
{
    return world_set_block(w, p, state, UPDATE_CLIENTS);
}
bool world_write_visual(World *w, BlockPos p, BlockState state)
{
    return world_write_state(w, p, state, NULL, NULL);
}
bool world_set_block_depth(World *w, BlockPos p, BlockState state,
                           unsigned flags, unsigned remaining_shapes)
{
    if (state.id == BLOCK_HOPPER || state.id == BLOCK_DROPPER ||
        state.id == BLOCK_BARREL) {
        ContainerRuntime *container = container_runtime(w, p);
        if (container && container->id == state.id) {
            uint8_t level = container_level(container);
            if (state.id == BLOCK_HOPPER) state.data.hopper.level = level;
            else if (state.id == BLOCK_DROPPER) state.data.dropper.level = level;
            else state.data.barrel.level = level;
        }
    }
    BlockState previous;
    bool changed;
    if (!world_write_state(w, p, state, &previous, &changed)) return false;
    if (!changed) return true;
    if (block_states_equal(previous, state)) return true;
    StateChange change = {p, previous, state, flags, remaining_shapes};
    sim_state_changed(w, change);
    return w->sim->error == RS_OK;
}
bool world_set_block(World *w, BlockPos p, BlockState state, unsigned flags)
{
    return world_set_block_depth(w, p, state, flags, UPDATE_SHAPE_LIMIT);
}
void world_remove_internal(World *w, BlockPos p)
{
    (void)world_set_block(w, p, block_default(BLOCK_AIR), 3);
}
static RsResult place_block(World *w, BlockPos p, BlockId id, Direction support)
{
    if (!w) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, p)) return RS_OUT_OF_BOUNDS;
    if (id <= BLOCK_AIR || id >= BLOCK_INTERNAL_FIRST)
        return RS_INVALID_ARGUMENT;
    if (block_is_rail(id) && direction_horizontal(support))
        support = support/2 == DIR_EAST/2 ? DIR_EAST : DIR_NORTH;
    if (id == BLOCK_HOPPER) {
        if ((unsigned)support >= DIR_COUNT) return RS_INVALID_ARGUMENT;
        if (support == DIR_UP) support = DIR_DOWN;
    }
    if (id == BLOCK_REDSTONE_WIRE && !wire_supported(w, p)) return RS_UNSUPPORTED;
    if (id == BLOCK_REDSTONE_TORCH) {
        if (support != DIR_DOWN && !direction_horizontal(support)) return RS_INVALID_ARGUMENT;
        if (!torch_supported(w, p, support)) return RS_UNSUPPORTED;
    } else if (id == BLOCK_REDSTONE_REPEATER ||
               id == BLOCK_REDSTONE_COMPARATOR || block_is_rail(id) ||
               id == BLOCK_OBSERVER || block_is_piston_base(id) ||
               id == BLOCK_BARREL || id == BLOCK_DROPPER) {
        if (id != BLOCK_OBSERVER && id != BLOCK_BARREL && id != BLOCK_DROPPER &&
            !block_is_piston_base(id) &&
            !direction_horizontal(support))
            return RS_INVALID_ARGUMENT;
        if ((id == BLOCK_OBSERVER || block_is_piston_base(id) ||
             id == BLOCK_BARREL || id == BLOCK_DROPPER) &&
            (unsigned)support >= DIR_COUNT)
            return RS_INVALID_ARGUMENT;
        if (id == BLOCK_REDSTONE_REPEATER && !repeater_supported(w, p))
            return RS_UNSUPPORTED;
        if (id == BLOCK_REDSTONE_COMPARATOR && !comparator_supported(w, p))
            return RS_UNSUPPORTED;
        if (block_is_rail(id) && !rail_supported(w, p))
            return RS_UNSUPPORTED;
    }
    BlockState previous = world_get(w, p);
    if (previous.id == id &&
        (id != BLOCK_REDSTONE_TORCH || previous.data.torch.support_dir == support) &&
        (id != BLOCK_REDSTONE_REPEATER || previous.data.repeater.facing == support) &&
        (id != BLOCK_REDSTONE_COMPARATOR ||
         previous.data.comparator.facing == support) &&
        (!block_is_rail(id) || previous.data.rail.facing/2 == support/2) &&
        (id != BLOCK_OBSERVER ||
         previous.data.observer.facing == support) &&
        (!block_is_piston_base(id) ||
         previous.data.piston.facing == support) &&
        (id != BLOCK_BARREL || previous.data.barrel.facing == support) &&
        (id != BLOCK_HOPPER || previous.data.hopper.facing == support) &&
        (id != BLOCK_DROPPER || previous.data.dropper.facing == support))
        return RS_UNCHANGED;
    if (id == BLOCK_REDSTONE_TORCH && sim_prepare_torch(w) != RS_OK) return RS_OUT_OF_MEMORY;
    BlockState state = block_default(id);
    if (id == BLOCK_REDSTONE_TORCH) state.data.torch.support_dir = (uint8_t)support;
    if (id == BLOCK_REDSTONE_REPEATER) state.data.repeater.facing = (uint8_t)support;
    if (id == BLOCK_REDSTONE_COMPARATOR)
        state.data.comparator.facing = (uint8_t)support;
    if (block_is_rail(id)) state.data.rail.facing = (uint8_t)support;
    if (id == BLOCK_OBSERVER)
        state.data.observer.facing = (uint8_t)support;
    if (block_is_piston_base(id))
        state.data.piston.facing = (uint8_t)support;
    if (id == BLOCK_BARREL)
        state.data.barrel.facing = (uint8_t)support;
    if (id == BLOCK_HOPPER)
        state.data.hopper.facing = (uint8_t)support;
    if (id == BLOCK_DROPPER)
        state.data.dropper.facing = (uint8_t)support;
    if (id == BLOCK_REDSTONE_LAMP)
        state.data.lamp.lit = power_received(w, p, SIGNAL_ALL) > 0;
    if (id == BLOCK_NOTE_BLOCK)
        state.data.note_block.instrument = (uint8_t)note_instrument(world_get(w, block_offset(p, DIR_DOWN)));
    if (id == BLOCK_REDSTONE_WIRE) {
        state.data.wire.connections = 0x33;
        state = wire_connection_state(w, p, state);
    }
    if (id == BLOCK_REDSTONE_REPEATER)
        state.data.repeater.locked = repeater_is_locked(w, p, state);
    if (!world_set_block(w, p, state, 3)) return w->sim->error;
    if (!w->sim->updates_suppressed &&
        (id == BLOCK_REDSTONE_REPEATER || id == BLOCK_REDSTONE_COMPARATOR)) {
        Direction rear = direction_opposite((Direction)(id == BLOCK_REDSTONE_REPEATER ?
            state.data.repeater.facing : state.data.comparator.facing));
        BlockPos input = block_offset(p, rear);
        BlockState source = world_get(w, input);
        bool powered = id == BLOCK_REDSTONE_COMPARATOR ?
            comparator_should_turn_on(w, p, state) :
            power_signal(w, input, direction_opposite(rear), SIGNAL_ALL) > 0 ||
            (source.id == BLOCK_REDSTONE_WIRE && source.data.wire.power > 0);
        if (powered) sim_schedule(w, p, 1);
    }
    return w->sim->error;
}
void world_place(World *w, BlockPos p, BlockId id, Direction support)
{
    if (!world_action_ready(w, "world_place", &p)) return;
    RsResult result = place_block(w, p, id, support);
    world_record_error(w, result, "world_place", &p, rs_result_name(result));
}
static RsResult remove_block(World *w, BlockPos p)
{
    if (!w) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, p)) return RS_OUT_OF_BOUNDS;
    BlockState removed = world_get(w, p);
    if (removed.id == BLOCK_AIR) return RS_UNCHANGED;
    if (!w->sim->updates_suppressed && block_is_piston_head((BlockId)removed.id)) {
        Direction facing = (Direction)removed.data.piston.facing;
        BlockPos behind = block_offset(p, direction_opposite(facing));
        BlockState base = world_get(w, behind);
        if (block_piston_pair((BlockId)base.id, (BlockId)removed.id) &&
            base.data.piston.extended && base.data.piston.facing == facing)
            world_remove_internal(w, behind);
        if (w->sim->error != RS_OK) return w->sim->error;
    }
    world_remove_internal(w, p);
    if (!w->sim->updates_suppressed && removed.id == BLOCK_MOVING_PISTON &&
        w->sim->error == RS_OK) {
        Direction facing = (Direction)removed.data.moving_piston.facing;
        BlockPos behind = block_offset(p, direction_opposite(facing));
        BlockState base = world_get(w, behind);
        if (block_is_piston_base((BlockId)base.id) && base.data.piston.extended)
            (void)world_set_block(w, behind, block_default(BLOCK_AIR), 3);
    }
    return w->sim->error;
}
void world_remove(World *w, BlockPos p)
{
    if (!world_action_ready(w, "world_remove", &p)) return;
    RsResult result = remove_block(w, p);
    world_record_error(w, result, "world_remove", &p, rs_result_name(result));
}
RsResult world_toggle_wire_dot(World *w, BlockPos p)
{
    if (!w) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, p)) return RS_OUT_OF_BOUNDS;
    RsResult r = sim_flush(w); if (r != RS_OK) return r;
    BlockState state = world_get(w, p);
    if (state.id != BLOCK_REDSTONE_WIRE) return RS_INVALID_ARGUMENT;
    if (state.data.wire.connections != 0 && state.data.wire.connections != 0x33)
        return RS_UNCHANGED;
    BlockState previous = state;
    state.data.wire.connections = state.data.wire.connections ? 0 : 0x33;
    state = wire_connection_state(w, p, state);
    if (block_states_equal(previous, state)) return RS_UNCHANGED;
    if (!world_set_block(w, p, state, 3)) return w->sim->error;
    static const Direction horizontal[] = {DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST};
    for (unsigned i = 0; i < 4; ++i) {
        Direction d = horizontal[i];
        BlockPos next = block_offset(p, d);
        if (((previous.data.wire.connections ^ state.data.wire.connections) & (1u << d)) &&
            block_is_conductor(w, next))
            sim_update_neighbors_at(w, next, direction_opposite(d));
    }
    return w->sim->error;
}
RsResult world_cycle_repeater_delay(World *w, BlockPos p)
{
    if (!w) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, p)) return RS_OUT_OF_BOUNDS;
    RsResult result = sim_flush(w); if (result != RS_OK) return result;
    BlockState state = world_get(w, p);
    if (state.id != BLOCK_REDSTONE_REPEATER) return RS_INVALID_ARGUMENT;
    state.data.repeater.delay = (uint8_t)(state.data.repeater.delay % 4 + 1);
    if (!world_set_block(w, p, state, 3)) return w->sim->error;
    return w->sim->error;
}
RsResult world_toggle_comparator_mode(World *w, BlockPos p)
{
    if (!w) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, p)) return RS_OUT_OF_BOUNDS;
    RsResult result = sim_flush(w); if (result != RS_OK) return result;
    BlockState state = world_get(w, p);
    if (state.id != BLOCK_REDSTONE_COMPARATOR) return RS_INVALID_ARGUMENT;
    state.data.comparator.subtract = !state.data.comparator.subtract;
    if (!world_write_derived(w, p, state)) return w->sim->error;
    comparator_refresh_output(w, p); return w->sim->error;
}
RsResult world_set_barrel_level(World *w, BlockPos p, uint8_t level)
{
    if (!w || level > 15) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, p)) return RS_OUT_OF_BOUNDS;
    RsResult result = sim_flush(w); if (result != RS_OK) return result;
    BlockState state = world_get(w, p);
    if (state.id != BLOCK_BARREL) return RS_INVALID_ARGUMENT;
    return container_set_barrel_level(w, p, level);
}
RsResult world_cycle_note(World *w, BlockPos p)
{
    if (!w) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, p)) return RS_OUT_OF_BOUNDS;
    RsResult result = sim_flush(w); if (result != RS_OK) return result;
    BlockState state = world_get(w, p);
    if (state.id != BLOCK_NOTE_BLOCK) return RS_INVALID_ARGUMENT;
    state.data.note_block.note = (uint8_t)((state.data.note_block.note + 1u) % NOTE_COUNT);
    (void)world_set_block(w, p, state, 3);
    return w->sim->error;
}
static bool in_view(const World *w, BlockPos key)
{
    if (w->view_radius < 0) return true;
    int64_t dx = (int64_t)key.x - w->view_center.x, dz = (int64_t)key.z - w->view_center.z;
    return dx >= -w->view_radius && dx <= w->view_radius && dz >= -w->view_radius && dz <= w->view_radius;
}
RsResult world_stream(World *w, BlockPos focus, int radius)
{
    if (!w || radius < 0 || radius > 32) return RS_INVALID_ARGUMENT;
    if (!world_in_bounds(w, (BlockPos){focus.x, 0, focus.z})) return RS_OUT_OF_BOUNDS;
    RsResult result = sim_flush(w); if (result != RS_OK) return result;
    BlockPos center = chunk_key(focus);
    for (int dz = -radius; dz <= radius; ++dz) for (int dx = -radius; dx <= radius; ++dx) {
        BlockPos key = {center.x + dx, 0, center.z + dz};
        if (!world_chunk_in_bounds(w, key)) continue;
        if (!world_ensure_chunk(w, key)) return RS_OUT_OF_MEMORY;
    }
    if (!pos_equal(w->view_center, center) || w->view_radius != radius) ++w->topology_revision;
    w->view_center = center; w->view_radius = radius;
    for (size_t i = 0; i < w->chunk_count;) {
        Chunk *c = w->chunks[i];
        if (c->resident && !in_view(w, c->key) && !sim_chunk_pinned(w, c->key)) {
            if (!world_archive_chunk(w, c)) return RS_INTERNAL_ERROR;
            if (!c->modified) {
                posmap_remove(&w->chunk_map, c->key); free(c);
                --w->chunk_count;
                if (i < w->chunk_count) {
                    w->chunks[i] = w->chunks[w->chunk_count];
                    (void)posmap_set(&w->chunk_map, w->chunks[i]->key, i);
                }
                continue;
            }
        }
        ++i;
    }
    return w->sim->error;
}
void world_visit_sections(const World *w, WorldSectionVisitor visitor, void *context)
{
    if (!w || !visitor) return;
    for (size_t i = 0; i < w->chunk_count; ++i) {
        Chunk *c = w->chunks[i];
        if (!c->resident || !in_view(w, c->key)) continue;
        for (int s = 0; s < SECTION_COUNT; ++s) if (c->sections[s] && c->sections[s]->block_count)
            visitor(context, (BlockPos){c->key.x * 16, WORLD_MIN_Y + s * 16, c->key.z * 16});
    }
}
uint64_t world_tick(const World *w) { return w ? w->tick : 0; }
uint64_t world_revision(const World *w) { return w ? w->revision : 0; }
uint64_t world_topology_revision(const World *w) { return w ? w->topology_revision : 0; }
size_t world_volume(const World *w) { return w ? w->loaded_count * 16 * 384 * 16 : 0; }
size_t world_loaded_chunks(const World *w) { return w ? w->loaded_count : 0; }
size_t world_stored_chunks(const World *w) { return w ? w->chunk_count : 0; }
WorldConfig world_config(const World *w) { return w ? w->config : (WorldConfig){0}; }
void world_set_change_observer(World *w, WorldChangeObserver observer, void *context)
{
    if (!w) return;
    w->observer = observer; w->observer_context = observer ? context : NULL;
}
const char *rs_result_name(RsResult result)
{
    static const char *const names[] = {"ok", "unchanged", "out of bounds", "unsupported", "invalid argument", "out of memory", "internal error", "I/O error", "corrupt data", "expectation failed"};
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ? names[result] : "unknown";
}
