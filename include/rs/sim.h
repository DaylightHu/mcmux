#ifndef RS_SIM_H
#define RS_SIM_H
#include "world.h"

typedef struct {
    uint64_t neighbor_checks, components_solved; /* components: graph backend passes. */
    uint64_t wires_visited; /* Individual dust-strength evaluations. */
    uint64_t wire_changes;
    uint64_t rail_components_solved; /* Graph passes; zero for synchronous rails. */
    uint64_t rails_visited; /* Individual synchronous rail-state evaluations. */
    uint64_t rail_changes, events_processed;
    size_t pending_events; /* Scheduled ticks, block events and active motions. */
} SimStats;
typedef struct {
    uint8_t received;  /* Any incident signal, e.g. dust can turn off a torch. */
    uint8_t strong;    /* Direct signal with ALL wire outputs suppressed. */
} SolidPower;
/* Runs after a complete game tick, including block events and block entities.
 * May read the world and call world_expect(). It must not edit, save, destroy,
 * advance time, clear errors or change the check while it is running.
 * Context may hold caller-owned observation history and timing conditions. */
typedef void (*SimCheck)(World *world, void *context);

/* One optional runtime check per world. Replace it or pass NULL to detach.
 * The callback/context are borrowed until replacement, detachment or destroy;
 * they are not saved in MCW/MCS files. Registration does not invoke the check.
 * Checks run once after each tick's timed phases finish without an engine
 * error, whether advanced by sim_step() or sim_advance_ticks(). No check runs
 * on zero ticks, flush, edits or a step blocked by freezing. A failed check
 * stops further ticks, retaining the completed tick's state and time.
 * world_result()/world_error_message() report the first error.
 * Register between ticks/checks. Failures use the world's action error policy.
 * A NULL world or a world with a previous error is ignored. */
void sim_set_check(World *world, SimCheck check, void *context);

/* Edits synchronously settle neighbor/shape notifications, dust and rails.
 * flush reports the current status without advancing time.
 * step checks that status and the freeze/manual-step controls, advances ONE
 * permitted game tick, then runs due events in
 * priority/insertion order from a frozen due batch, then piston block events,
 * then moving pistons and hoppers in one block-entity insertion order. Entries
 * created during that last phase first tick on the next game tick. flush does
 * not execute these timed phases or advance time.
 * Each callback's zero-delay consequences settle before the next callback.
 * Torch delay = 2 game ticks (100 ms at 20 TPS). */
RsResult sim_flush(World *world);
RsResult sim_step(World *world);
/* Synchronously advance a fixed number of game ticks without real-time pacing.
 * Advances even when frozen, preserving the freeze flag and previously queued
 * manual steps. Zero ticks does not advance time.
 * Calls during tick processing/checks and requests that overflow time with
 * queued steps are rejected before advancing. Stops at the first error,
 * including an error raised by the registered check;
 * completed time and state changes are retained. Errors are retained by the
 * world; read world_result() and world_error_message(). Read world_tick() for
 * progress. A NULL world is ignored. */
void sim_advance_ticks(World *world, uint32_t ticks);
/* Frozen worlds return RS_UNCHANGED from sim_step() unless manual steps are
 * pending. Changing freeze state cancels pending steps. Only the freeze flag
 * is saved; pending steps reset on load. */
bool sim_is_frozen(const World *world);
RsResult sim_set_frozen(World *world, bool frozen);
uint64_t sim_pending_steps(const World *world);
RsResult sim_request_steps(World *world, uint32_t count);
/* Suppress block callbacks and neighbor/shape propagation, retaining storage,
 * container/motion cleanup and existing timed work. Enabling does not replay
 * skipped updates. This runtime switch defaults to enabled on world creation. */
bool sim_updates_enabled(const World *world);
RsResult sim_set_updates_enabled(World *world, bool enabled);
SimStats sim_stats(const World *world);
void sim_reset_stats(World *world);
SolidPower redstone_solid_power(const World *world, BlockPos pos);
uint8_t redstone_signal(const World *world, BlockPos emitter, Direction toward);
bool redstone_torch_burned_out(const World *world, BlockPos pos);
#endif
