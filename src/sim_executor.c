#include "sim_executor.h"
#include "redstone_queue.h"
#include "redstone_c.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

struct SimExecutorHost {
    World *world;
};

static RsResult reference_flush(SimExecutorHost *host, void *context)
{
    (void)context;
    return host->world->sim->error;
}

static RsResult reference_scheduled_tick(SimExecutorHost *host, void *context,
                                         ScheduledEvent event)
{
    (void)context;
    World *world = host->world;
    Simulator *sim = world->sim;
    BlockState state = world_get(world, event.pos);
    if (scheduler_block_type(state) != event.block) return sim->error;
    const BlockDef *definition = block_definition((BlockId)state.id);
    if (!definition->scheduled_tick) return sim->error;

    TickAction action = definition->scheduled_tick(world, event.pos);
    ++sim->stats.events_processed;
    if (action.changed)
        (void)world_set_block(world, event.pos, action.state,
            state.id == BLOCK_REDSTONE_TORCH ? 3 : 2);
    if (action.next_delay)
        sim_schedule(world, event.pos, action.next_delay);
    if (state.id == BLOCK_OBSERVER)
        sim_update_output_neighbors(world, event.pos,
                                    (Direction)state.data.observer.facing);
    return sim->error;
}

static void reference_state_changed(SimExecutorHost *host, void *context,
                                    StateChange change)
{
    (void)context;
    sim_reference_state_changed(host->world, change);
}

static void reference_edit_state_changed(SimExecutorHost *host, void *context,
                                         StateChange change)
{
    (void)context;
    sim_reference_edit_state_changed(host->world, change);
}

static const SimExecutorOps reference_ops = {
    .flush = reference_flush,
    .scheduled_tick = reference_scheduled_tick,
    .state_changed = reference_state_changed,
    .edit_state_changed = reference_edit_state_changed
};

void sim_executor_init_reference(World *world)
{
    world->sim->executor = (SimExecutor){&reference_ops, NULL};
}

void sim_executor_destroy(World *world)
{
    if (!world || !world->sim) return;
    SimExecutor *executor = &world->sim->executor;
    if (executor->ops && executor->ops->destroy) {
        SimExecutorHost host = {world};
        executor->ops->destroy(&host, executor->context);
    }
    *executor = (SimExecutor){0};
}

RsResult sim_executor_flush(World *world)
{
    if (!world) return RS_INVALID_ARGUMENT;
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->flush)
        return world->sim->error = RS_INTERNAL_ERROR;
    SimExecutorHost host = {world};
    return executor->ops->flush(&host, executor->context);
}

RsResult sim_executor_scheduled_tick(World *world, ScheduledEvent event)
{
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->scheduled_tick)
        return world->sim->error = RS_INTERNAL_ERROR;
    SimExecutorHost host = {world};
    return executor->ops->scheduled_tick(&host, executor->context, event);
}

void sim_executor_state_changed(World *world, StateChange change)
{
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->state_changed) {
        world->sim->error = RS_INTERNAL_ERROR;
        return;
    }
    SimExecutorHost host = {world};
    executor->ops->state_changed(&host, executor->context, change);
}

void sim_executor_edit_state_changed(World *world, StateChange change)
{
    SimExecutor *executor = &world->sim->executor;
    if (!executor->ops || !executor->ops->edit_state_changed) {
        world->sim->error = RS_INTERNAL_ERROR;
        return;
    }
    SimExecutorHost host = {world};
    executor->ops->edit_state_changed(&host, executor->context, change);
}

void sim_executor_stored_active(World *world, BlockPos pos,
                                BlockState previous, BlockState current)
{
    SimExecutor *executor = &world->sim->executor;
    SimExecutorHost host = {world};
    executor->ops->stored(&host, executor->context, pos, previous, current);
}

RsResult sim_executor_evaluate_active(const World *world, BlockPos pos,
                                      BlockState expected,
                                      RedstoneEvaluation *output)
{
    SimExecutor *executor = &world->sim->executor;
    SimExecutorHost host = {(World *)world};
    RsResult result = executor->ops->evaluate(&host, executor->context,
                                             pos, expected, output);
    if (result != RS_OK && result != RS_UNSUPPORTED)
        world->sim->error = result;
    return result;
}

enum { CPU_CACHE_EMPTY, CPU_CACHE_VALUE, CPU_CACHE_UNSUPPORTED };
typedef struct {
    RedstoneEvaluation value;
    uint32_t visited;
    StateId block_state;
    uint8_t cached;
} CpuNodeCache;

typedef struct {
    RedstonePlan plan;
    BlockState *shape; /* Lazily captured region plus its two-cell read halo. */
    RedstoneNodeId *cells;
    size_t size_x, size_y, size_z;
    size_t references;
    uint8_t *neighbor_masks; /* Shared callback masks include one-cell centers. */
    size_t mask_nx, mask_ny, mask_nz;
} CpuTopology;

typedef struct CpuRegionSet CpuRegionSet;

typedef struct CpuExecutor {
    CpuTopology *topology;
    RedstonePlanBounds bounds;
    RedstoneState state;
    RedstoneDirtyQueue dirty;
    CpuNodeCache *cache;
    uint64_t revision;
    uint32_t generation;
    bool valid, packed;
    const SimCModel *model;
    struct CpuExecutor *next;
    CpuRegionSet *set; /* Only the list head owns multi-region metadata. */
} CpuExecutor;

typedef struct {
    CpuExecutor *cpu;
    size_t next;
} CpuRegionLink;

struct CpuRegionSet {
    CpuExecutor *recent;
    PosMap cells;
    CpuRegionLink *links;
    bool attempted;
};

static void cpu_regions_clear(CpuExecutor *head)
{
    CpuRegionSet *set = head->set;
    if (!set) return;
    posmap_destroy(&set->cells);
    free(set->links);
    set->links = NULL;
    set->attempted = false;
}

static void cpu_regions_changed(CpuExecutor *head)
{
    if (!head->set) head->set = calloc(1, sizeof(*head->set));
    cpu_regions_clear(head);
}

/* Chunks are only routing buckets; membership still uses exact region bounds. */
static int cpu_region_axis(int coordinate)
{
    int key = coordinate / CHUNK_SIZE;
    if (coordinate % CHUNK_SIZE < 0) --key;
    return key;
}

static BlockPos cpu_region_key(BlockPos pos)
{
    return (BlockPos){cpu_region_axis(pos.x), cpu_region_axis(pos.y),
                      cpu_region_axis(pos.z)};
}

static void cpu_regions_build(CpuExecutor *head)
{
    CpuRegionSet *set = head->set;
    set->attempted = true;
    size_t count = 0, references = 0;
    for (CpuExecutor *cpu = head; cpu; cpu = cpu->next) {
        if (count == SIZE_MAX / 16) return;
        ++count;
    }
    /* An optional index with at most 16 bucket links per instance on average.
     * Huge regions and allocation failures keep the original list lookup. */
    size_t budget = count * 16;
    for (CpuExecutor *cpu = head; cpu; cpu = cpu->next) {
        BlockPos min = cpu_region_key(cpu->bounds.min);
        BlockPos max = cpu_region_key(cpu->bounds.max);
        size_t cells = 1;
        const uint64_t dimensions[] = {
            (uint64_t)((int64_t)max.x - min.x + 1),
            (uint64_t)((int64_t)max.y - min.y + 1),
            (uint64_t)((int64_t)max.z - min.z + 1)
        };
        for (size_t axis = 0; axis < 3; ++axis) {
            if (dimensions[axis] > (budget - references) / cells) return;
            cells *= (size_t)dimensions[axis];
        }
        references += cells;
    }
    if (count > SIZE_MAX / sizeof(CpuExecutor *) ||
        references > SIZE_MAX / sizeof(CpuRegionLink)) return;
    PosMap cells = {0};
    CpuExecutor **ordered = malloc(count * sizeof(*ordered));
    CpuRegionLink *links = malloc(references * sizeof(*links));
    if (!ordered || !links || !posmap_reserve(&cells, references)) goto fail;
    size_t i = 0;
    for (CpuExecutor *cpu = head; cpu; cpu = cpu->next) ordered[i++] = cpu;
    size_t next = 0;
    /* Prepend in reverse attachment order so collisions retain list order. */
    while (i) {
        CpuExecutor *cpu = ordered[--i];
        BlockPos min = cpu_region_key(cpu->bounds.min);
        BlockPos max = cpu_region_key(cpu->bounds.max);
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z)
                for (int x = min.x; x <= max.x; ++x) {
                    BlockPos key = {x,y,z};
                    size_t previous = SIZE_MAX;
                    (void)posmap_get(&cells, key, &previous);
                    links[next] = (CpuRegionLink){cpu, previous};
                    if (!posmap_set(&cells, key, next++)) goto fail;
                }
    }
    free(ordered);
    set->cells = cells;
    set->links = links;
    return;
fail:
    posmap_destroy(&cells);
    free(links);
    free(ordered);
}

static void cpu_destroy(SimExecutorHost *host, void *context)
{
    (void)host;
    CpuExecutor *cpu = context;
    if (cpu) { cpu_regions_clear(cpu); free(cpu->set); }
    while (cpu) {
        CpuExecutor *next = cpu->next;
        if (cpu->packed) free(cpu->state.blocks);
        else {
            free(cpu->cache);
            redstone_dirty_queue_destroy(&cpu->dirty);
            redstone_state_destroy(&cpu->state);
        }
        if (cpu->topology && !--cpu->topology->references) {
            redstone_plan_destroy(&cpu->topology->plan);
            free(cpu->topology->shape);
            free(cpu->topology->cells);
            free(cpu->topology->neighbor_masks);
            free(cpu->topology);
        }
        free(cpu);
        cpu = next;
    }
}

static bool inside(RedstonePlanBounds bounds, BlockPos pos, int margin)
{
    return (int64_t)pos.x >= (int64_t)bounds.min.x - margin &&
           (int64_t)pos.x <= (int64_t)bounds.max.x + margin &&
           (int64_t)pos.y >= (int64_t)bounds.min.y - margin &&
           (int64_t)pos.y <= (int64_t)bounds.max.y + margin &&
           (int64_t)pos.z >= (int64_t)bounds.min.z - margin &&
           (int64_t)pos.z <= (int64_t)bounds.max.z + margin;
}

static bool overlaps(RedstonePlanBounds a, RedstonePlanBounds b)
{
    return a.min.x <= b.max.x && b.min.x <= a.max.x &&
           a.min.y <= b.max.y && b.min.y <= a.max.y &&
           a.min.z <= b.max.z && b.min.z <= a.max.z;
}

static bool cpu_valid(const World *world, const CpuExecutor *cpu)
{
    return cpu->valid && !world->sim->piston_motions.active;
}

static RsResult cpu_invalidate(CpuExecutor *cpu, RedstoneNodeId source)
{
    if (++cpu->generation == 0) {
        for (size_t i = 0; i < cpu->topology->plan.node_count; ++i)
            cpu->cache[i].visited = 0;
        cpu->generation = 1;
    }
    RsResult result = redstone_dirty_queue_push(&cpu->dirty, source);
    if (result != RS_OK) return result;
    RedstoneNodeId node;
    while (redstone_dirty_queue_pop(&cpu->dirty, &node)) {
        CpuNodeCache *cache = &cpu->cache[node];
        if (cache->visited == cpu->generation) continue;
        cache->visited = cpu->generation;
        cache->cached = CPU_CACHE_EMPTY;
        /* Conductors relay their direct sources without a stored-state
         * change. Other nodes propagate only when their output is written.
         * This queue invalidates pure reads, never runs block notifications. */
        if (node == source ||
            (cpu->topology->plan.nodes[node].flags & REDSTONE_NODE_CONDUCTOR)) {
            result = redstone_dirty_queue_push_consumers(&cpu->dirty,
                                                         &cpu->topology->plan, node);
            if (result != RS_OK) return result;
        }
    }
    return RS_OK;
}

/* A set observes one world revision stream. Routing a write to its region
 * must not require visiting and updating every other instance. */
static void cpu_invalidate_all(CpuExecutor *head)
{
    for (CpuExecutor *cpu = head; cpu; cpu = cpu->next) cpu->valid = false;
}

/* Keep construction and full scans out of the common query's stack frame. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static CpuExecutor *cpu_region_miss(CpuExecutor *head, BlockPos pos)
{
    CpuRegionSet *set = head->set;
    /* Build once on the first miss, after a set of attachments has settled. */
    if (set && !set->attempted) cpu_regions_build(head);
    if (set && set->links) {
        size_t link;
        if (!posmap_get(&set->cells, cpu_region_key(pos), &link)) return NULL;
        while (link != SIZE_MAX) {
            CpuRegionLink item = set->links[link];
            if (inside(item.cpu->bounds, pos, 0)) return set->recent = item.cpu;
            link = item.next;
        }
        return NULL;
    }
    for (CpuExecutor *cpu = head; cpu; cpu = cpu->next)
        if (inside(cpu->bounds, pos, 0)) {
            if (set) set->recent = cpu;
            return cpu;
        }
    return NULL;
}

static CpuExecutor *cpu_region(CpuExecutor *head, BlockPos pos)
{
    CpuRegionSet *set = head->set;
    if (set && set->recent && inside(set->recent->bounds, pos, 0))
        return set->recent;
    return cpu_region_miss(head, pos);
}

static bool cpu_find(const CpuExecutor *cpu, BlockPos pos, RedstoneNodeId *node)
{
    const CpuTopology *topology = cpu->topology;
    if (topology->cells) {
        uint64_t x = (uint64_t)((int64_t)pos.x - cpu->bounds.min.x);
        uint64_t y = (uint64_t)((int64_t)pos.y - cpu->bounds.min.y);
        uint64_t z = (uint64_t)((int64_t)pos.z - cpu->bounds.min.z);
        if (x >= topology->size_x || y >= topology->size_y ||
            z >= topology->size_z) return false;
        RedstoneNodeId id = topology->cells[
            ((size_t)y * topology->size_z + (size_t)z) * topology->size_x + (size_t)x];
        if (id == REDSTONE_NODE_NONE) return false;
        *node = id;
        return true;
    }
    const RedstonePlan *plan = &cpu->topology->plan;
    int64_t x = (int64_t)pos.x - cpu->bounds.min.x + plan->bounds.min.x;
    int64_t y = (int64_t)pos.y - cpu->bounds.min.y + plan->bounds.min.y;
    int64_t z = (int64_t)pos.z - cpu->bounds.min.z + plan->bounds.min.z;
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX ||
        z < INT_MIN || z > INT_MAX) return false;
    return redstone_plan_find(plan, (BlockPos){(int)x,(int)y,(int)z}, node);
}

static void cpu_stored(SimExecutorHost *host, void *context, BlockPos pos,
                        BlockState previous, BlockState current)
{
    World *world = host->world;
    CpuExecutor *head = context;
    if (world_revision(world) != head->revision + 1) cpu_invalidate_all(head);
    head->revision = world_revision(world);
    if (!redstone_state_compatible(previous, current)) {
        /* Comparator overrides and quasi-connectivity can sample two cells
         * away. Shape edits in that halo invalidate the corresponding plan. */
        for (CpuExecutor *cpu = head; cpu; cpu = cpu->next)
            if (inside(cpu->bounds, pos, 2)) cpu->valid = false;
        return;
    }
    CpuExecutor *cpu = head->next ? cpu_region(head, pos) : head;
    if (!cpu || !cpu->valid) return;
    RedstoneNodeId node;
    if (!cpu_find(cpu, pos, &node)) return;
    RsResult result = redstone_state_set(&cpu->state, node, current);
    if (result == RS_UNSUPPORTED) { cpu->valid = false; return; }
    if (result == RS_OK || result == RS_UNCHANGED) {
        if (!state_encode(current, &cpu->cache[node].block_state))
            result = RS_INTERNAL_ERROR;
        else
            result = cpu_invalidate(cpu, node);
    }
    if (result != RS_OK) {
        cpu->valid = false;
        world->sim->error = result;
    }
}

static RsResult cpu_evaluate(SimExecutorHost *host, void *context, BlockPos pos,
                              BlockState expected, RedstoneEvaluation *output)
{
    World *world = host->world;
    CpuExecutor *head = context;
    if (head->revision != world_revision(world)) cpu_invalidate_all(head);
    CpuExecutor *cpu = head->next ? cpu_region(head, pos) : head;
    if (!cpu) {
        ++world->sim->stats.cpu_fallbacks;
        return RS_UNSUPPORTED;
    }
    if (!cpu_valid(world, cpu)) {
        cpu->valid = false;
        ++world->sim->stats.cpu_fallbacks;
        return RS_UNSUPPORTED;
    }
    RedstoneNodeId node;
    StateId encoded;
    if (!cpu_find(cpu, pos, &node) ||
        !state_encode(expected, &encoded) ||
        encoded != cpu->cache[node].block_state) {
        ++world->sim->stats.cpu_fallbacks;
        return RS_UNSUPPORTED;
    }
    CpuNodeCache *cache = &cpu->cache[node];
    if (cache->cached == CPU_CACHE_VALUE) {
        ++world->sim->stats.cpu_cache_hits;
        *output = cache->value;
        return RS_OK;
    }
    if (cache->cached == CPU_CACHE_UNSUPPORTED) {
        ++world->sim->stats.cpu_fallbacks;
        return RS_UNSUPPORTED;
    }
    RedstoneEvaluation value;
    RsResult result = cpu->model ?
        cpu->model->evaluate(cpu->state.blocks, node, &value) :
        redstone_evaluate(&cpu->state, node, &value);
    if (result == RS_OK) {
        ++world->sim->stats.cpu_evaluations;
        if (cpu->model) ++world->sim->stats.c_evaluations;
        cache->value = value;
        cache->cached = CPU_CACHE_VALUE;
        *output = value;
    } else if (result == RS_UNSUPPORTED) {
        ++world->sim->stats.cpu_fallbacks;
        cache->cached = CPU_CACHE_UNSUPPORTED;
    }
    return result;
}

static bool cpu_neighbor_mask(SimExecutorHost *host, void *context,
                               BlockPos pos, uint8_t *mask)
{
    const World *world = host->world;
    CpuExecutor *head = context, *cpu = head;
    if (head->revision != world_revision(world)) return false;
    if (head->next) {
        CpuExecutor *recent = head->set ? head->set->recent : NULL;
        if (recent && inside(recent->bounds, pos, 1)) cpu = recent;
        else {
            for (cpu = head; cpu; cpu = cpu->next)
                if (inside(cpu->bounds, pos, 1)) break;
        }
    }
    if (!cpu || !cpu_valid(world, cpu) || !cpu->topology->neighbor_masks) return false;
    const CpuTopology *topology = cpu->topology;
    uint64_t x = (uint64_t)((int64_t)pos.x - cpu->bounds.min.x + 1);
    uint64_t y = (uint64_t)((int64_t)pos.y - cpu->bounds.min.y + 1);
    uint64_t z = (uint64_t)((int64_t)pos.z - cpu->bounds.min.z + 1);
    if (x >= topology->mask_nx || y >= topology->mask_ny || z >= topology->mask_nz)
        return false;
    *mask = topology->neighbor_masks[
        ((size_t)y * topology->mask_nz + (size_t)z) * topology->mask_nx + (size_t)x];
    return true;
}

bool sim_executor_neighbor_mask_active(const World *world, BlockPos center,
                                       uint8_t *mask)
{
    SimExecutor *executor = &world->sim->executor;
    SimExecutorHost host = {(World *)world};
    return executor->ops->neighbor_mask(&host, executor->context, center, mask);
}

static const SimExecutorOps cpu_ops = {
    .flush = reference_flush,
    .scheduled_tick = reference_scheduled_tick,
    .state_changed = reference_state_changed,
    .edit_state_changed = reference_edit_state_changed,
    .destroy = cpu_destroy,
    .stored = cpu_stored,
    .evaluate = cpu_evaluate,
    .neighbor_mask = cpu_neighbor_mask
};

static RsResult backend_change_ready(const World *world)
{
    if (!world) return RS_INVALID_ARGUMENT;
    const Simulator *sim = world->sim;
    if (sim->handling_tick || sim->block_entities.ticking || sim->checking ||
        sim->observing)
        return RS_INVALID_ARGUMENT;
    return world_result(world);
}

/* Read translated template coordinates without signed overflow, including
 * the halo outside the world's supported coordinates (which is air). */
static BlockState cpu_sample(const World *world, BlockPos origin,
                              int64_t x, int64_t y, int64_t z)
{
    x += origin.x; y += origin.y; z += origin.z;
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX ||
        z < INT_MIN || z > INT_MAX) return block_default(BLOCK_AIR);
    return world_get(world, (BlockPos){(int)x,(int)y,(int)z});
}

static void cpu_build_neighbor_masks(World *world, CpuExecutor *cpu)
{
    CpuTopology *topology = cpu->topology;
    size_t nodes = topology->plan.node_count;
    if (!nodes) return;
    const uint64_t dimensions[] = {
        (uint64_t)((int64_t)cpu->bounds.max.x - cpu->bounds.min.x + 3),
        (uint64_t)((int64_t)cpu->bounds.max.y - cpu->bounds.min.y + 3),
        (uint64_t)((int64_t)cpu->bounds.max.z - cpu->bounds.min.z + 3)
    };
    size_t count = 1;
    for (size_t i = 0; i < 3; ++i) {
        if (dimensions[i] > SIZE_MAX / count) return;
        count *= (size_t)dimensions[i];
    }
    /* Optional and bounded: sparse regions and allocation failures keep the
     * ordinary walk. Centers extend one cell; their targets stay within the
     * same two-cell halo checked by shape matching and live invalidation. */
    if (nodes <= SIZE_MAX / 16 && count > nodes * 16) return;
    uint8_t *masks = malloc(count);
    if (!masks) return;
    static const BlockPos offsets[DIR_COUNT] = {
        [DIR_EAST] = {1,0,0}, [DIR_WEST] = {-1,0,0},
        [DIR_UP] = {0,1,0}, [DIR_DOWN] = {0,-1,0},
        [DIR_SOUTH] = {0,0,1}, [DIR_NORTH] = {0,0,-1}
    };
    _Static_assert(DIR_COUNT <= 8, "neighbor mask fits one byte");
    size_t index = 0;
    for (size_t y = 0; y < dimensions[1]; ++y)
        for (size_t z = 0; z < dimensions[2]; ++z)
            for (size_t x = 0; x < dimensions[0]; ++x) {
                uint8_t mask = 0;
                for (unsigned direction = 0; direction < DIR_COUNT; ++direction) {
                    BlockPos offset = offsets[direction];
                    BlockState block = cpu_sample(world, cpu->bounds.min,
                        (int64_t)x - 1 + offset.x, (int64_t)y - 1 + offset.y,
                        (int64_t)z - 1 + offset.z);
                    if (block_definition((BlockId)block.id)->neighbor_changed)
                        mask |= 1u << direction;
                }
                masks[index++] = mask;
            }
    topology->neighbor_masks = masks;
    topology->mask_nx = (size_t)dimensions[0];
    topology->mask_ny = (size_t)dimensions[1];
    topology->mask_nz = (size_t)dimensions[2];
}

static void cpu_build_lookup(CpuTopology *topology)
{
    const RedstonePlan *plan = &topology->plan;
    if (!plan->node_count) return;
    const uint64_t dimensions[] = {
        (uint64_t)((int64_t)plan->bounds.max.x - plan->bounds.min.x + 1),
        (uint64_t)((int64_t)plan->bounds.max.y - plan->bounds.min.y + 1),
        (uint64_t)((int64_t)plan->bounds.max.z - plan->bounds.min.z + 1)
    };
    size_t count = 1;
    for (size_t i = 0; i < 3; ++i) {
        if (dimensions[i] > SIZE_MAX / count) return;
        count *= (size_t)dimensions[i];
    }
    /* Dense local indexing is optional. Never retain a larger lookup table
     * than the position map it replaces; sparse regions keep their hash map. */
    size_t budget = plan->positions.capacity * sizeof(*plan->positions.entries);
    if (count > budget / sizeof(*topology->cells)) return;
    RedstoneNodeId *cells = malloc(count * sizeof(*cells));
    if (!cells) return;
    for (size_t i = 0; i < count; ++i) cells[i] = REDSTONE_NODE_NONE;
    size_t nx = (size_t)dimensions[0], nz = (size_t)dimensions[2];
    for (size_t i = 0; i < plan->node_count; ++i) {
        BlockPos pos = plan->nodes[i].pos;
        size_t x = (size_t)((int64_t)pos.x - plan->bounds.min.x);
        size_t y = (size_t)((int64_t)pos.y - plan->bounds.min.y);
        size_t z = (size_t)((int64_t)pos.z - plan->bounds.min.z);
        cells[(y * nz + z) * nx + x] = (RedstoneNodeId)i;
    }
    topology->cells = cells;
    topology->size_x = nx;
    topology->size_y = (size_t)dimensions[1];
    topology->size_z = nz;
    posmap_destroy(&topology->plan.positions);
}

static RsResult cpu_shape(World *world, CpuExecutor *source,
                           RedstonePlanBounds destination)
{
    CpuTopology *topology = source->topology;
    int64_t nx = (int64_t)source->bounds.max.x - source->bounds.min.x + 5;
    int64_t ny = (int64_t)source->bounds.max.y - source->bounds.min.y + 5;
    int64_t nz = (int64_t)source->bounds.max.z - source->bounds.min.z + 5;
    size_t count = 1;
    const uint64_t dimensions[] = {(uint64_t)nx,(uint64_t)ny,(uint64_t)nz};
    for (size_t i = 0; i < 3; ++i) {
        if (dimensions[i] > SIZE_MAX / count) return RS_OUT_OF_MEMORY;
        count *= (size_t)dimensions[i];
    }
    if (count > SIZE_MAX / sizeof(*topology->shape)) return RS_OUT_OF_MEMORY;
    if (!topology->shape) {
        BlockState *shape = malloc(count * sizeof(*shape));
        if (!shape) return RS_OUT_OF_MEMORY;
        size_t i = 0;
        for (int64_t y = -2; y < ny - 2; ++y)
            for (int64_t z = -2; z < nz - 2; ++z)
                for (int64_t x = -2; x < nx - 2; ++x)
                    shape[i++] = cpu_sample(world, source->bounds.min, x,y,z);
        RsResult result = world_result(world);
        if (result != RS_OK) { free(shape); return result; }
        topology->shape = shape;
    }
    size_t i = 0;
    for (int64_t y = -2; y < ny - 2; ++y)
        for (int64_t z = -2; z < nz - 2; ++z)
            for (int64_t x = -2; x < nx - 2; ++x) {
                BlockState block = cpu_sample(world, destination.min, x,y,z);
                BlockState expected = topology->shape[i++];
                if (block.id == BLOCK_AIR && expected.id == BLOCK_AIR) continue;
                if (!redstone_state_compatible(expected, block))
                    return world_result(world) == RS_OK ? RS_UNSUPPORTED :
                           world_result(world);
            }
    return world_result(world);
}

/* Lay out owned arrays with malloc-compatible alignment and checked sizes. */
static bool cpu_state_array(size_t *bytes, size_t count, size_t stride,
                              size_t *offset)
{
    size_t alignment = _Alignof(max_align_t);
    size_t remainder = *bytes % alignment;
    size_t padding = remainder ? alignment - remainder : 0;
    if (padding > SIZE_MAX - *bytes) return false;
    *offset = *bytes + padding;
    if (count > (SIZE_MAX - *offset) / stride) return false;
    *bytes = *offset + count * stride;
    return true;
}

static RsResult cpu_instance_storage(World *world, CpuExecutor *cpu)
{
    const RedstonePlan *plan = &cpu->topology->plan;
    size_t count = plan->node_count;
    size_t bytes = 0, blocks, cache, items, queued;
    if (!cpu_state_array(&bytes, count, sizeof(BlockState), &blocks) ||
        !cpu_state_array(&bytes, count, sizeof(CpuNodeCache), &cache) ||
        !cpu_state_array(&bytes, count, sizeof(RedstoneNodeId), &items) ||
        !cpu_state_array(&bytes, count, sizeof(uint8_t), &queued))
        return RS_OUT_OF_MEMORY;
    unsigned char *storage = bytes ? calloc(1, bytes) : NULL;
    if (bytes && !storage) return RS_OUT_OF_MEMORY;
    cpu->packed = true;
    cpu->state = (RedstoneState){plan, (BlockState *)storage, count};
    if (storage) {
        cpu->cache = (CpuNodeCache *)(storage + cache);
        cpu->dirty = (RedstoneDirtyQueue){
            .items = (RedstoneNodeId *)(storage + items),
            .queued = storage + queued, .capacity = count
        };
    }
    /* The graph was validated by its first state build. Values still come
     * from this instance, and no state is shared between allocations. */
    for (size_t i = 0; i < count; ++i) {
        BlockPos pos = plan->nodes[i].pos;
        cpu->state.blocks[i] = cpu_sample(world, cpu->bounds.min,
            (int64_t)pos.x - plan->bounds.min.x,
            (int64_t)pos.y - plan->bounds.min.y,
            (int64_t)pos.z - plan->bounds.min.z);
    }
    return world_result(world);
}

static RsResult cpu_prepare(World *world, CpuExecutor *cpu, bool instance)
{
    const RedstonePlan *plan = &cpu->topology->plan;
    RsResult result;
    if (instance) {
        result = cpu_instance_storage(world, cpu);
        if (result != RS_OK) return result;
    } else {
        result = redstone_state_build(plan, &cpu->state);
        if (result != RS_OK) return result;
        result = redstone_dirty_queue_prepare(&cpu->dirty, plan->node_count);
        if (result != RS_OK) return result;
        if (plan->node_count) {
            if (plan->node_count > SIZE_MAX / sizeof(*cpu->cache))
                return RS_OUT_OF_MEMORY;
            cpu->cache = calloc(plan->node_count, sizeof(*cpu->cache));
            if (!cpu->cache) return RS_OUT_OF_MEMORY;
        }
    }
    for (size_t i = 0; i < plan->node_count; ++i)
        if (!state_encode(cpu->state.blocks[i], &cpu->cache[i].block_state))
            return RS_INTERNAL_ERROR;
    cpu->revision = world_revision(world);
    cpu->valid = true;
    return RS_OK;
}

static RsResult use_cpu(World *world, BlockPos min, BlockPos max,
                         const SimCModel *model, bool append)
{
    RsResult result = backend_change_ready(world);
    if (result != RS_OK) return result;
    if (world->sim->piston_motions.active) return RS_UNSUPPORTED;
    CpuExecutor *tail = NULL;
    if (append && world->sim->executor.ops == &cpu_ops) {
        for (CpuExecutor *item = world->sim->executor.context; item;
             item = item->next) {
            if (overlaps(item->bounds, (RedstonePlanBounds){min, max}))
                return RS_INVALID_ARGUMENT;
            tail = item;
        }
    }
    CpuExecutor *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) return RS_OUT_OF_MEMORY;
    cpu->bounds = (RedstonePlanBounds){min, max};
    cpu->topology = calloc(1, sizeof(*cpu->topology));
    if (!cpu->topology) { result = RS_OUT_OF_MEMORY; goto fail; }
    cpu->topology->references = 1;
    result = redstone_plan_build(world, cpu->bounds, &cpu->topology->plan);
    if (result != RS_OK) goto fail;
    if (model && !redstone_c_matches(&cpu->topology->plan, model)) {
        result = RS_UNSUPPORTED;
        goto fail;
    }
    cpu_build_lookup(cpu->topology);
    cpu_build_neighbor_masks(world, cpu);
    result = world_result(world);
    if (result != RS_OK) goto fail;
    cpu->model = model;
    result = cpu_prepare(world, cpu, false);
    if (result != RS_OK) goto fail;
    if (tail) {
        CpuExecutor *head = world->sim->executor.context;
        if (head->revision != world_revision(world)) cpu_invalidate_all(head);
        head->revision = world_revision(world);
    }
    if (tail) {
        tail->next = cpu;
        cpu_regions_changed(world->sim->executor.context);
    } else {
        sim_executor_destroy(world);
        world->sim->executor = (SimExecutor){&cpu_ops, cpu};
    }
    return RS_OK;
fail:
    cpu_destroy(NULL, cpu);
    return result;
}

RsResult sim_use_cpu(World *world, BlockPos min, BlockPos max)
{
    return use_cpu(world, min, max, NULL, false);
}

RsResult sim_add_cpu(World *world, BlockPos min, BlockPos max)
{
    return use_cpu(world, min, max, NULL, true);
}

RsResult sim_use_c(World *world, BlockPos min, BlockPos max,
                   const SimCModel *model)
{
    if (!model || model->abi_version != SIM_C_ABI_VERSION ||
        !model->topology || !model->evaluate)
        return RS_INVALID_ARGUMENT;
    return use_cpu(world, min, max, model, false);
}

RsResult sim_add_c(World *world, BlockPos min, BlockPos max,
                   const SimCModel *model)
{
    if (!model || model->abi_version != SIM_C_ABI_VERSION ||
        !model->topology || !model->evaluate)
        return RS_INVALID_ARGUMENT;
    return use_cpu(world, min, max, model, true);
}

RsResult sim_add_cpu_instance(World *world, BlockPos prototype_min,
                               BlockPos instance_min)
{
    RsResult result = backend_change_ready(world);
    if (result != RS_OK) return result;
    if (world->sim->executor.ops != &cpu_ops ||
        world->sim->piston_motions.active) return RS_UNSUPPORTED;
    CpuExecutor *head = world->sim->executor.context, *source = NULL, *tail = head;
    if (head->revision != world_revision(world)) return RS_UNSUPPORTED;
    for (CpuExecutor *cpu = head; cpu; cpu = cpu->next) {
        if (pos_equal(cpu->bounds.min, prototype_min)) source = cpu;
        tail = cpu;
    }
    if (!source || !cpu_valid(world, source)) return RS_UNSUPPORTED;
    RedstonePlanBounds bounds = {.min = instance_min};
    int64_t x = (int64_t)instance_min.x + source->bounds.max.x - source->bounds.min.x;
    int64_t y = (int64_t)instance_min.y + source->bounds.max.y - source->bounds.min.y;
    int64_t z = (int64_t)instance_min.z + source->bounds.max.z - source->bounds.min.z;
    if (x > INT_MAX || y > INT_MAX || z > INT_MAX) return RS_OUT_OF_BOUNDS;
    bounds.max = (BlockPos){(int)x,(int)y,(int)z};
    if (!world_in_bounds(world, bounds.min) || !world_in_bounds(world, bounds.max))
        return RS_OUT_OF_BOUNDS;
    for (CpuExecutor *cpu = head; cpu; cpu = cpu->next)
        if (overlaps(cpu->bounds, bounds)) return RS_INVALID_ARGUMENT;
    result = cpu_shape(world, source, bounds);
    if (result != RS_OK) return result;
    CpuExecutor *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) return RS_OUT_OF_MEMORY;
    if (source->topology->references == SIZE_MAX) {
        free(cpu);
        return RS_OUT_OF_MEMORY;
    }
    cpu->topology = source->topology;
    ++cpu->topology->references;
    cpu->bounds = bounds;
    cpu->model = source->model;
    result = cpu_prepare(world, cpu, true);
    if (result != RS_OK) { cpu_destroy(NULL, cpu); return result; }
    tail->next = cpu;
    cpu_regions_changed(head);
    return RS_OK;
}

RsResult sim_write_c(const World *world, BlockPos min, BlockPos max,
                     FILE *output, const char *symbol)
{
    if (!output || !symbol) return RS_INVALID_ARGUMENT;
    RsResult result = backend_change_ready(world);
    if (result != RS_OK) return result;
    if (world->sim->piston_motions.active) return RS_UNSUPPORTED;
    RedstonePlan plan = {0};
    result = redstone_plan_build(world, (RedstonePlanBounds){min, max}, &plan);
    if (result == RS_OK) result = redstone_c_write(&plan, output, symbol);
    redstone_plan_destroy(&plan);
    return result;
}

RsResult sim_use_reference(World *world)
{
    RsResult result = backend_change_ready(world);
    if (result != RS_OK) return result;
    if (world->sim->executor.ops == &reference_ops) return RS_UNCHANGED;
    sim_executor_destroy(world);
    sim_executor_init_reference(world);
    return RS_OK;
}

bool sim_cpu_active(const World *world)
{
    if (!world || world->sim->executor.ops != &cpu_ops) return false;
    const CpuExecutor *head = world->sim->executor.context;
    if (head->revision != world_revision(world)) return false;
    for (const CpuExecutor *cpu = head; cpu; cpu = cpu->next)
        if (cpu_valid(world, cpu)) return true;
    return false;
}

bool sim_c_active(const World *world)
{
    if (!sim_cpu_active(world)) return false;
    for (const CpuExecutor *cpu = world->sim->executor.context; cpu; cpu = cpu->next)
        if (cpu->model && cpu_valid(world, cpu)) return true;
    return false;
}
