#ifndef RS_SIM_INTERNAL_H
#define RS_SIM_INTERNAL_H
#include "rs/sim.h"
#include "world_internal.h"
#include "scheduler.h"
#include "container.h"
#include "blocks/block_internal.h"
typedef struct {
    uint64_t off_times[8], burned_until;
    uint8_t off_count, off_head;
    BlockPos pos;
    size_t next_free;
    bool used;
} TorchRuntime;
typedef struct {
    BlockPos pos;
    uint8_t block, type, facing;
} PistonEvent;
typedef struct {
    PistonEvent *items;
    size_t head, count, capacity;
    PosMap pending;
} PistonEvents;
typedef struct {
    BlockPos pos;
    BlockState moved_state;
    uint64_t last_ticked;
    size_t tick_slot;
    uint8_t direction;
    /* Half-step units: 0, 1, 2 are vanilla progress 0, 0.5, 1. */
    uint8_t progress, previous_progress;
    bool extending, source, used;
} PistonMotion;
typedef struct {
    PistonMotion *items;
    size_t count, active, capacity;
    PosMap slots;
} PistonMotions;
typedef struct {
    BlockPos pos;
    uint8_t block;
    bool used;
} BlockEntityTick;
typedef struct {
    BlockEntityTick *items;
    size_t count, active, capacity;
    bool ticking;
} BlockEntityTicks;
typedef struct {
    BlockPos pos;
    BlockState previous, current;
    unsigned flags, remaining_shapes;
} StateChange;
struct Simulator {
    Scheduler scheduler;
    PistonEvents piston_events;
    PistonMotions piston_motions;
    BlockEntityTicks block_entities;
    ContainerTable containers;
    PosMap torch_slots;
    TorchRuntime *torches;
    size_t torch_used, torch_capacity, free_torch;
    SimStats stats;
    SimCheck check;
    void *check_context;
    uint64_t check_from;
    char check_message[384];
    RsResult error;
    uint64_t manual_steps;
    bool handling_tick;
    bool checking;
    bool frozen;
    bool updates_suppressed;
};
bool sim_init_internal(World *);
void sim_destroy_internal(World *);
RsResult sim_prepare_torch(World *);
bool sim_attach_torch(World *, BlockPos);
void sim_forget_cell(World *, BlockPos);
/* Cancel destination work and release inventories once for a batch edit. */
void sim_forget_edit_cells(World *, const PosMap *positions);
void sim_forget_outside_plot(World *);
void sim_notify_change(World *, BlockPos);
void sim_notify_analog(World *, BlockPos);
void sim_update_neighbors_at(World *, BlockPos, Direction except);
void sim_update_neighbors_around(World *, BlockPos);
void sim_neighbor_changed(World *, BlockPos);
void sim_update_output_neighbors(World *, BlockPos, Direction output);
void sim_update_shape_at(World *, BlockPos, Direction toward_source,
                          BlockPos source, unsigned flags, unsigned remaining_shapes);
void sim_update_neighbor_shapes(World *, BlockPos, unsigned flags, unsigned remaining_shapes);
BlockState sim_update_from_neighbor_shapes(World *, BlockPos, BlockState);
void sim_state_changed(World *, StateChange);
/* Runtime resources have already been installed for every edited cell. */
void sim_edit_state_changed(World *, StateChange);
bool block_states_equal(BlockState, BlockState);
void sim_mark_wire(World *, BlockPos);
void sim_mark_input(World *, BlockPos);
void sim_schedule(World *, BlockPos, uint32_t delay);
void sim_schedule_state(World *, BlockPos, BlockState, uint32_t delay);
void sim_schedule_priority(World *, BlockPos, uint32_t delay,
                           TickPriority priority);
void sim_cancel(World *, BlockPos);
bool sim_has_scheduled_tick(const World *, BlockPos);
bool sim_will_tick(const World *, BlockPos);
bool sim_queue_piston_event(World *, PistonEvent);
void piston_trigger_event(World *, PistonEvent);
bool sim_prepare_piston_motions(World *, size_t additional);
bool sim_add_piston_motion(World *, PistonMotion);
const PistonMotion *sim_piston_motion(const World *, BlockPos);
void sim_discard_piston_motion(World *, BlockPos);
bool sim_finish_piston_motion(World *, BlockPos, bool forced);
void sim_tick_piston_motion(World *, BlockPos);
bool sim_prepare_block_entities(World *, size_t additional);
bool sim_add_block_entity(World *, BlockPos, BlockId, size_t *slot);
void sim_discard_block_entity(World *, size_t slot);
bool sim_block_entity_order_valid(const World *);
TorchRuntime *sim_torch_runtime(const World *, BlockPos);
bool sim_chunk_pinned(const World *, BlockPos chunk);
#endif
