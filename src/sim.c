#include "sim_internal.h"
#include "wire_solver.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

bool sim_init_internal(World *w)
{
    w->sim = calloc(1, sizeof(*w->sim));
    if (!w->sim) return false;
    w->sim->containers.dropper_random = UINT64_C(0x5deece66d); /* Java seed 0. */
    w->sim->free_torch = SIZE_MAX; scheduler_init(&w->sim->scheduler); return true;
}
void sim_destroy_internal(World *w)
{
    Simulator *s = w->sim;
    if (!s) return;
    container_destroy(w);
    free(s->block_entities.items);
    free(s->piston_events.items); posmap_destroy(&s->piston_events.pending);
    free(s->piston_motions.items); posmap_destroy(&s->piston_motions.slots);
    scheduler_destroy(&s->scheduler); posmap_destroy(&s->torch_slots);
    free(s->torches); free(s); w->sim = NULL;
}
static void discard_torch(Simulator *s, size_t slot)
{
    posmap_remove(&s->torch_slots, s->torches[slot].pos);
    s->torches[slot].used = false;
    s->torches[slot].next_free = s->free_torch;
    s->free_torch = slot;
}
static bool torch_history_recent(const World *w, const TorchRuntime *t)
{
    return t->off_count &&
           w->tick - t->off_times[(t->off_head + 7u) % 8u] <= 60;
}
RsResult sim_prepare_torch(World *w)
{
    Simulator *s = w->sim;
    if (s->free_torch == SIZE_MAX && s->torch_used == s->torch_capacity)
        for (size_t i = 0; i < s->torch_used; ++i)
            if (s->torches[i].used && !torch_history_recent(w, &s->torches[i]) &&
                world_get(w, s->torches[i].pos).id != BLOCK_REDSTONE_TORCH)
                discard_torch(s, i);
    if (!posmap_reserve(&s->torch_slots, s->torch_slots.count + 1)) return RS_OUT_OF_MEMORY;
    if (s->free_torch != SIZE_MAX || s->torch_used < s->torch_capacity) return RS_OK;
    size_t capacity = s->torch_capacity ? s->torch_capacity * 2 : 8;
    if (capacity < s->torch_capacity || capacity > SIZE_MAX / sizeof(TorchRuntime)) return RS_OUT_OF_MEMORY;
    TorchRuntime *next = realloc(s->torches, capacity * sizeof(*next));
    if (!next) return RS_OUT_OF_MEMORY;
    s->torches = next; s->torch_capacity = capacity; return RS_OK;
}
bool sim_attach_torch(World *w, BlockPos p)
{
    Simulator *s = w->sim;
    TorchRuntime *existing = sim_torch_runtime(w, p);
    if (existing) {
        existing->burned_until = 0;
        return true;
    }
    if (sim_prepare_torch(w) != RS_OK) return false;
    size_t slot = s->free_torch != SIZE_MAX ? s->free_torch : s->torch_used;
    if (!posmap_set(&s->torch_slots, p, slot)) return false;
    if (s->free_torch != SIZE_MAX) s->free_torch = s->torches[slot].next_free;
    else ++s->torch_used;
    s->torches[slot] = (TorchRuntime){.pos = p, .used = true}; return true;
}
void sim_forget_cell(World *w, BlockPos p)
{
    container_forget(w, p);
    sim_discard_piston_motion(w, p);
    Simulator *s = w->sim; size_t slot;
    if (posmap_get(&s->torch_slots, p, &slot) &&
        !torch_history_recent(w, &s->torches[slot]))
        discard_torch(s, slot);
}
TorchRuntime *sim_torch_runtime(const World *w, BlockPos p)
{
    size_t slot;
    return posmap_get(&w->sim->torch_slots, p, &slot) ? &w->sim->torches[slot] : NULL;
}
void sim_mark_wire(World *w, BlockPos p)
{
    if (w->sim->updates_suppressed) return;
    if (world_get(w, p).id == BLOCK_REDSTONE_WIRE)
        wire_update_power(w, p, world_get(w, p));
}
void sim_mark_input(World *w, BlockPos p)
{
    if (w->sim->error != RS_OK || w->sim->updates_suppressed) return;
    const BlockDef *d = block_definition((BlockId)world_get(w, p).id);
    if (d->input_changed) d->input_changed(w, p);
}
void sim_notify_change(World *w, BlockPos p)
{
    sim_update_neighbors_at(w, p, DIR_COUNT);
}
void sim_notify_analog(World *w, BlockPos p)
{
    if (w->sim->updates_suppressed) return;
    static const Direction horizontal[] = {DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST};
    for (unsigned i = 0; i < 4; ++i) {
        Direction direction = horizontal[i];
        BlockPos next = block_offset(p, direction);
        if (world_get(w, next).id == BLOCK_REDSTONE_COMPARATOR)
            sim_neighbor_changed(w, next);
        else if (block_is_conductor(w, next)) {
            next = block_offset(next, direction);
            if (world_get(w, next).id == BLOCK_REDSTONE_COMPARATOR)
                sim_neighbor_changed(w, next);
        }
    }
}
static void schedule_state(World *w, BlockPos p, BlockState state,
                            uint32_t delay, TickPriority priority)
{
    if (!delay || UINT64_MAX - w->tick < delay) { w->sim->error = RS_INTERNAL_ERROR; return; }
    if (!scheduler_add_priority(&w->sim->scheduler, p,
                                scheduler_block_type(state), w->tick + delay,
                                priority))
        w->sim->error = RS_OUT_OF_MEMORY;
}
void sim_schedule(World *w, BlockPos p, uint32_t delay)
{
    schedule_state(w, p, world_get(w, p), delay, TICK_NORMAL);
}
void sim_schedule_state(World *w, BlockPos p, BlockState state, uint32_t delay)
{
    schedule_state(w, p, state, delay, TICK_NORMAL);
}
void sim_schedule_priority(World *w, BlockPos p, uint32_t delay,
                           TickPriority priority)
{
    schedule_state(w, p, world_get(w, p), delay, priority);
}
void sim_cancel(World *w, BlockPos p)
{
    scheduler_cancel(&w->sim->scheduler, p, scheduler_block_type(world_get(w, p)));
}
bool sim_has_scheduled_tick(const World *w, BlockPos p)
{
    return scheduler_pending(&w->sim->scheduler, p,
                             scheduler_block_type(world_get(w, p)));
}
bool sim_will_tick(const World *w, BlockPos p)
{
    return scheduler_will_tick(&w->sim->scheduler, p,
                               scheduler_block_type(world_get(w, p)));
}

static BlockPos piston_event_key(PistonEvent event)
{
    BlockPos key = event.pos;
    unsigned type = ((event.block == BLOCK_STICKY_PISTON ? 3u : 0u) +
                     event.type)*DIR_COUNT + event.facing;
    key.y = (key.y - WORLD_MIN_Y)*(6*DIR_COUNT) + (int)type;
    return key;
}

bool sim_queue_piston_event(World *w, PistonEvent event)
{
    if (!world_in_bounds(w, event.pos) ||
        !block_is_piston_base((BlockId)event.block) ||
        event.type > 2 || event.facing >= DIR_COUNT) {
        w->sim->error = RS_INTERNAL_ERROR;
        return false;
    }
    PistonEvents *q = &w->sim->piston_events;
    BlockPos key = piston_event_key(event);
    if (posmap_get(&q->pending, key, NULL)) return true;
    if (q->head && q->count == q->capacity) {
        memmove(q->items, q->items + q->head,
                (q->count - q->head)*sizeof(*q->items));
        q->count -= q->head;
        q->head = 0;
    }
    if (q->count == q->capacity) {
        size_t capacity = q->capacity ? q->capacity*2 : 16;
        if (capacity < q->capacity || capacity > SIZE_MAX/sizeof(*q->items))
            goto fail;
        PistonEvent *next = realloc(q->items, capacity*sizeof(*next));
        if (!next) goto fail;
        q->items = next;
        q->capacity = capacity;
    }
    if (!posmap_set(&q->pending, key, 0)) goto fail;
    q->items[q->count++] = event;
    return true;
fail:
    w->sim->error = RS_OUT_OF_MEMORY;
    return false;
}

static bool pop_piston_event(PistonEvents *q, PistonEvent *event)
{
    if (q->head == q->count) return false;
    *event = q->items[q->head++];
    posmap_remove(&q->pending, piston_event_key(*event));
    if (q->head == q->count) q->head = q->count = 0;
    return true;
}

void sim_forget_outside_plot(World *w)
{
    Simulator *s = w->sim;
    for (size_t i = 0; i < s->scheduler.count;) {
        ScheduledEvent e = s->scheduler.heap[i];
        if (!world_in_bounds(w, e.pos)) scheduler_cancel(&s->scheduler, e.pos, e.block);
        else ++i;
    }
    PistonEvents *q = &s->piston_events;
    size_t output = 0;
    for (size_t i = q->head; i < q->count; ++i) {
        PistonEvent e = q->items[i];
        if (!world_in_bounds(w, e.pos)) posmap_remove(&q->pending, piston_event_key(e));
        else q->items[output++] = e;
    }
    q->head = 0; q->count = output;
    for (size_t i = 0; i < s->piston_motions.count; ++i)
        if (s->piston_motions.items[i].used &&
            !world_in_bounds(w, s->piston_motions.items[i].pos))
            sim_discard_piston_motion(w, s->piston_motions.items[i].pos);
    container_forget_outside_plot(w);
    for (size_t i = 0; i < s->torch_used; ++i)
        if (s->torches[i].used && !world_in_bounds(w, s->torches[i].pos))
            discard_torch(s, i);
}

void sim_forget_edit_cells(World *w, const PosMap *positions)
{
    Simulator *s = w->sim;
    scheduler_cancel_positions(&s->scheduler, positions);
    PistonEvents *q = &s->piston_events;
    size_t output = 0;
    for (size_t i = q->head; i < q->count; ++i) {
        PistonEvent event = q->items[i];
        if (posmap_get(positions, event.pos, NULL))
            posmap_remove(&q->pending, piston_event_key(event));
        else q->items[output++] = event;
    }
    q->head = 0; q->count = output;
    container_forget_edit_cells(w, positions);
}

static size_t *block_entity_slot(const World *w, BlockEntityTick entity)
{
    if (entity.block == BLOCK_HOPPER) {
        ContainerRuntime *c = container_runtime(w, entity.pos);
        return c && c->id == BLOCK_HOPPER ? &c->tick_slot : NULL;
    }
    size_t index;
    if (entity.block == BLOCK_MOVING_PISTON &&
        posmap_get(&w->sim->piston_motions.slots, entity.pos, &index))
        return &w->sim->piston_motions.items[index].tick_slot;
    return NULL;
}

static bool compact_block_entities(World *w)
{
    BlockEntityTicks *ticks = &w->sim->block_entities;
    if (ticks->active == ticks->count) return true;
    size_t output = 0;
    for (size_t i = 0; i < ticks->count; ++i) {
        BlockEntityTick entity = ticks->items[i];
        if (!entity.used) continue;
        size_t *slot = block_entity_slot(w, entity);
        if (!slot) { w->sim->error = RS_INTERNAL_ERROR; return false; }
        ticks->items[output] = entity;
        *slot = output++;
    }
    ticks->count = output;
    return true;
}

bool sim_prepare_block_entities(World *w, size_t additional)
{
    if (!w || w->sim->error != RS_OK) return false;
    BlockEntityTicks *ticks = &w->sim->block_entities;
    if (additional <= ticks->capacity - ticks->count) return true;
    if (!ticks->ticking && ticks->active < ticks->count &&
        !compact_block_entities(w)) return false;
    if (additional <= ticks->capacity - ticks->count) return true;
    if (additional > SIZE_MAX - ticks->count) goto fail;
    size_t needed = ticks->count + additional;
    size_t capacity = ticks->capacity ? ticks->capacity : 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX/2) { capacity = needed; break; }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX/sizeof(*ticks->items)) goto fail;
    BlockEntityTick *next = realloc(ticks->items, capacity*sizeof(*next));
    if (!next) goto fail;
    ticks->items = next; ticks->capacity = capacity;
    return true;
fail:
    w->sim->error = RS_OUT_OF_MEMORY;
    return false;
}

bool sim_add_block_entity(World *w, BlockPos pos, BlockId block, size_t *slot)
{
    if (!slot || (block != BLOCK_HOPPER && block != BLOCK_MOVING_PISTON)) {
        w->sim->error = RS_INTERNAL_ERROR;
        return false;
    }
    if (!sim_prepare_block_entities(w, 1)) return false;
    BlockEntityTicks *ticks = &w->sim->block_entities;
    *slot = ticks->count;
    ticks->items[ticks->count++] = (BlockEntityTick){pos, (uint8_t)block, true};
    ++ticks->active;
    return true;
}

void sim_discard_block_entity(World *w, size_t slot)
{
    BlockEntityTicks *ticks = &w->sim->block_entities;
    if (slot >= ticks->count) { w->sim->error = RS_INTERNAL_ERROR; return; }
    if (ticks->items[slot].used) {
        ticks->items[slot].used = false;
        --ticks->active;
    }
}

bool sim_block_entity_order_valid(const World *w)
{
    const BlockEntityTicks *ticks = &w->sim->block_entities;
    size_t count = 0, expected = w->sim->piston_motions.active;
    for (size_t i = 0; i < w->sim->containers.count; ++i)
        if (w->sim->containers.items[i].id == BLOCK_HOPPER) ++expected;
    for (size_t i = 0; i < ticks->count; ++i) {
        BlockEntityTick entity = ticks->items[i];
        if (!entity.used) continue;
        size_t *slot = block_entity_slot(w, entity);
        if (!slot || *slot != i || world_get(w, entity.pos).id != entity.block)
            return false;
        ++count;
    }
    return count == ticks->active && count == expected;
}

static void tick_block_entities(World *w)
{
    BlockEntityTicks *ticks = &w->sim->block_entities;
    size_t limit = ticks->count;
    ticks->ticking = true;
    /* Level.tickBlockEntities walks one insertion-ordered list. Entries
     * created during this phase are appended but wait until the next tick. */
    for (size_t i = 0; i < limit && w->sim->error == RS_OK; ++i) {
        BlockEntityTick entity = ticks->items[i];
        if (!entity.used) continue;
        if (entity.block == BLOCK_MOVING_PISTON)
            sim_tick_piston_motion(w, entity.pos);
        else
            container_tick_hopper(w, entity.pos);
        (void)sim_flush(w);
    }
    ticks->ticking = false;
    if (w->sim->error == RS_OK) (void)compact_block_entities(w);
}

RsResult sim_flush(World *w)
{
    if (!w) return RS_INVALID_ARGUMENT;
    return w->sim->error;
}
void sim_set_check(World *w, SimCheck check, void *context)
{
    if (!world_action_ready(w, "sim_set_check", NULL)) return;
    Simulator *s = w->sim;
    if (s->handling_tick || s->block_entities.ticking || s->checking) {
        world_record_error(w, RS_INVALID_ARGUMENT, "sim_set_check", NULL,
                           "cannot change a check during tick processing or a check");
        return;
    }
    s->check = check;
    s->check_context = check ? context : NULL;
}
bool sim_is_frozen(const World *w)
{
    return w && w->sim->frozen;
}
RsResult sim_set_frozen(World *w, bool frozen)
{
    if (!w || w->sim->handling_tick || w->sim->block_entities.ticking)
        return RS_INVALID_ARGUMENT;
    if (w->sim->error != RS_OK) return w->sim->error;
    if (w->sim->frozen == frozen) return RS_UNCHANGED;
    w->sim->frozen = frozen;
    w->sim->manual_steps = 0;
    ++w->revision;
    return RS_OK;
}
uint64_t sim_pending_steps(const World *w)
{
    return w ? w->sim->manual_steps : 0;
}
RsResult sim_request_steps(World *w, uint32_t count)
{
    if (!w || !count || w->sim->handling_tick || w->sim->block_entities.ticking)
        return RS_INVALID_ARGUMENT;
    Simulator *s = w->sim;
    if (s->error != RS_OK) return s->error;
    if (!s->frozen || s->manual_steps > UINT64_MAX - w->tick ||
        count > UINT64_MAX - w->tick - s->manual_steps)
        return RS_INVALID_ARGUMENT;
    s->manual_steps += count;
    return RS_OK;
}
bool sim_updates_enabled(const World *w)
{
    return w && !w->sim->updates_suppressed;
}
RsResult sim_set_updates_enabled(World *w, bool enabled)
{
    if (!w || w->sim->handling_tick || w->sim->block_entities.ticking)
        return RS_INVALID_ARGUMENT;
    if (w->sim->error != RS_OK) return w->sim->error;
    if (sim_updates_enabled(w) == enabled) return RS_UNCHANGED;
    w->sim->updates_suppressed = !enabled;
    return RS_OK;
}
RsResult sim_step(World *w)
{
    RsResult result = sim_flush(w);
    if (result != RS_OK) return result;
    Simulator *s = w->sim;
    if (s->checking) {
        world_record_error(w, RS_INVALID_ARGUMENT, "sim_step", NULL,
                           "cannot advance time from a simulation check");
        return s->error;
    }
    if (s->frozen && !s->manual_steps) return RS_UNCHANGED;
    if (w->tick == UINT64_MAX) return RS_INTERNAL_ERROR;
    ++w->tick;
    /* Consume the permit when time advances, even if a later callback fails. */
    if (s->manual_steps) --s->manual_steps;
    s->handling_tick = true;
    if (!scheduler_begin_tick(&s->scheduler, w->tick)) {
        s->handling_tick = false;
        return s->error = RS_OUT_OF_MEMORY;
    }
    ScheduledEvent event;
    while (scheduler_next_tick(&s->scheduler, &event)) {
        BlockState state = world_get(w, event.pos);
        if (scheduler_block_type(state) != event.block) continue;
        const BlockDef *d = block_definition((BlockId)state.id);
        if (!d->scheduled_tick) continue;
        TickAction action = d->scheduled_tick(w, event.pos);
        ++s->stats.events_processed;
        if (action.changed)
            (void)world_set_block(w, event.pos, action.state,
                state.id == BLOCK_REDSTONE_TORCH ? 3 : 2);
        if (action.next_delay)
            sim_schedule(w, event.pos, action.next_delay);
        if (state.id == BLOCK_OBSERVER)
            sim_update_output_neighbors(w, event.pos, (Direction)state.data.observer.facing);
        result = sim_flush(w);
        if (result != RS_OK) {
            s->handling_tick = false;
            return result;
        }
    }
    /* ServerLevel runs block events after the complete scheduled-tick batch,
     * before block entities. A block event may append more events to this FIFO. */
    PistonEvent piston;
    while (pop_piston_event(&s->piston_events, &piston)) {
        if (world_get(w, piston.pos).id != piston.block) continue;
        piston_trigger_event(w, piston);
        ++s->stats.events_processed;
        result = sim_flush(w);
        if (result != RS_OK) {
            s->handling_tick = false;
            return result;
        }
    }
    /* ServerLevel clears handlingTick before ticking block entities. */
    s->handling_tick = false;
    tick_block_entities(w);
    if (s->error == RS_OK && s->check) {
        s->checking = true;
        s->check(w, s->check_context);
        s->checking = false;
        (void)world_action_ready(w, "sim_check", NULL);
    }
    return s->error;
}
static RsResult advance_ticks(World *w, uint32_t ticks)
{
    if (!w || w->sim->handling_tick || w->sim->block_entities.ticking ||
        w->sim->checking)
        return RS_INVALID_ARGUMENT;
    RsResult result = sim_flush(w);
    if (result != RS_OK) return result;
    Simulator *s = w->sim;
    if (s->manual_steps > UINT64_MAX - w->tick ||
        ticks > UINT64_MAX - w->tick - s->manual_steps)
        return RS_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < ticks; ++i) {
        if (s->frozen) {
            result = sim_request_steps(w, 1);
            if (result != RS_OK) return result;
        }
        result = sim_step(w);
        if (result != RS_OK) return result;
    }
    return RS_OK;
}
void sim_advance_ticks(World *w, uint32_t ticks)
{
    if (!world_action_ready(w, "sim_advance_ticks", NULL)) return;
    RsResult result = advance_ticks(w, ticks);
    world_record_error(w, result, "sim_advance_ticks", NULL, rs_result_name(result));
}
SimStats sim_stats(const World *w)
{
    SimStats stats = {0};
    if (w) {
        const Simulator *s = w->sim;
        stats = s->stats;
        stats.pending_events = s->scheduler.count +
            s->scheduler.ticking_count - s->scheduler.ticking_head +
            s->piston_events.count - s->piston_events.head +
            s->piston_motions.active;
    }
    return stats;
}
void sim_reset_stats(World *w) { if (w) memset(&w->sim->stats, 0, sizeof(SimStats)); }
static bool near_chunk(BlockPos p, BlockPos chunk)
{
    int64_t dx = (int64_t)floor_div16(p.x) - chunk.x, dz = (int64_t)floor_div16(p.z) - chunk.z;
    return dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1;
}
bool sim_chunk_pinned(const World *w, BlockPos chunk)
{
    if (container_chunk_pinned(w, chunk)) return true;
    const Simulator *s = w->sim;
    for (size_t i = 0; i < s->scheduler.count; ++i) if (near_chunk(s->scheduler.heap[i].pos, chunk)) return true;
    for (size_t i = s->scheduler.ticking_head; i < s->scheduler.ticking_count; ++i)
        if (near_chunk(s->scheduler.ticking[i].pos, chunk)) return true;
    for (size_t i = s->piston_events.head; i < s->piston_events.count; ++i)
        if (near_chunk(s->piston_events.items[i].pos, chunk)) return true;
    for (size_t i = 0; i < s->piston_motions.count; ++i)
        if (s->piston_motions.items[i].used &&
            near_chunk(s->piston_motions.items[i].pos, chunk)) return true;
    return false;
}
