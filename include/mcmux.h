#ifndef MCMUX_H
#define MCMUX_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "rs/session.h"
#include "rs/structure.h"
#include "rs/sim.h"

/* Load an MCS structure with its saved anchor placed at anchor. On failure,
 * report the error and return NULL without terminating the host process. */
World *load_mcs(const char *path, BlockPos anchor);

/* Save world to an MCW file. Action failures are retained in world. NULL
 * worlds and worlds with a previous error are ignored by action functions. */
void save_mcw(World *world, const char *path);

/* Write world back to the original MCS selection in the MCW file. The anchor
 * must match the one passed to load_mcs(). */
void writeback_mcs(World *world, const char *mcs_path,
                   BlockPos anchor, const char *mcw_path);

/* Place a block & Remove a block */
// void world_place(World *world, BlockPos pos, BlockId id, Direction support_dir);
// void world_remove(World *world, BlockPos pos);

/* Read a block and its state. Out-of-bounds reads return air; check bounds
 * separately when the distinction matters. */
// BlockState world_get(const World *world, BlockPos pos);
// bool world_in_bounds(const World *world, BlockPos pos);

/* Toggle wire dot/cross mode; constrained connections may return RS_UNCHANGED. */
// RsResult world_toggle_wire_dot(World *world, BlockPos pos);

/* Cycle the repeater delay. */
// RsResult world_cycle_repeater_delay(World *world, BlockPos pos);

/* Toggle the comparator between compare and subtract modes. */
// RsResult world_toggle_comparator_mode(World *world, BlockPos pos);

/* Cycle the note block's note. */
// RsResult world_cycle_note(World *world, BlockPos pos);

/* Set barrel fullness to 0..15 by replacing its contents when the level changes. */
// RsResult world_set_barrel_level(World *world, BlockPos pos, uint8_t level);

/* Inspect container slots. The slot count is zero for a non-container;
 * indices are zero based. Formatting or iterating the results is up to the caller. */
// size_t world_container_size(const World *world, BlockPos pos);

// RsResult world_get_container_slot(const World *world, BlockPos pos, size_t slot, ContainerStack *stack);
// RsResult world_set_container_slot(World *world, BlockPos pos, size_t slot, ContainerStack stack);

/* Set the world's dropper RNG seed, which is saved with the world. */
// RsResult world_set_dropper_seed(World *world, uint64_t seed);

/* Advance a fixed number of game ticks synchronously. */
// void sim_advance_ticks(World *world, uint32_t ticks);

/* Register a read-only boolean predicate. after_ticks sets the initial delay;
 * after that it runs at every complete tick. The library records failures.
 * Context/message are optional; pass (SimCheckConfig){0} to detach. */
// void sim_set_check(World *world, SimCheckConfig config);

/* Check any read-only C condition at the current game tick. False retains
 * RS_EXPECTATION_FAILED and reports a printf-style message. This does not
 * advance time or register a future/continuous check; arguments are evaluated
 * by C even after a previous error. NULL/empty format uses a default message. */
// void world_expect(World *world, bool condition, const char *format, ...);

/* Inspect the first failure at the end of a sequence of actions, or handle a
 * recoverable error and clear it before continuing interactive editing. */
// RsResult world_result(const World *world);
// const char *world_error_message(const World *world);
// void world_clear_error(World *world);

/* Read simulation statistics and world counters. */
// SimStats sim_stats(const World *world);
// uint64_t world_revision(const World *world);
// size_t world_loaded_chunks(const World *world);
// size_t world_stored_chunks(const World *world);

/* Release an owned World without saving it. */
// void world_destroy(World *world);
#endif
