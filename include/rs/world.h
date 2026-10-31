#ifndef RS_WORLD_H
#define RS_WORLD_H
#include <stddef.h>
#include "block.h"
#include "item.h"

typedef struct World World;
/* One optional observer, called after a cell is written. It must not edit the
 * world or retain pointers into it. Passing NULL detaches the observer. */
typedef void (*WorldChangeObserver)(void *context, BlockPos pos,
                                    BlockState previous, BlockState current);
typedef enum {
    RS_OK = 0, RS_UNCHANGED, RS_OUT_OF_BOUNDS, RS_UNSUPPORTED,
    RS_INVALID_ARGUMENT, RS_OUT_OF_MEMORY, RS_INTERNAL_ERROR,
    RS_IO_ERROR, RS_CORRUPT_DATA, RS_EXPECTATION_FAILED
} RsResult;
enum { CHUNK_SIZE = 16, SECTION_VOLUME = 4096, WORLD_MIN_Y = -64,
       WORLD_MAX_Y = 319, SECTION_COUNT = 24, STATE_ID_LIMIT = 32768,
       WORLD_PLOT_SIZE = 512 };
typedef struct { int ground_layers; } WorldConfig;
typedef struct { int min_x, min_z, max_x, max_z; } WorldPlotBounds;
typedef struct {
    WorldPlotBounds before, after, removed;
    bool has_edits;
} WorldShrinkPreview;
typedef ItemStack ContainerStack;
typedef void (*WorldSectionVisitor)(void *context, BlockPos origin);
typedef struct {
    BlockState moved_state;
    uint64_t last_ticked;
    Direction direction; /* Piston facing; retracting cargo moves opposite. */
    /* Discrete half-block units: 0, 1, 2 mean progress 0, 0.5, 1. */
    uint8_t progress, previous_progress;
    bool extending, source;
} WorldPistonMotion;

/* Fixed iron ground, no RNG, no seed, no immovable bedrock. */
WorldConfig world_config_default(void);
World *world_create(WorldConfig config);
void world_destroy(World *world);
/* Void action functions retain and report the first failure. Later edits,
 * ticks and saves are ignored; host C control flow continues normally.
 * Read the result once at the end. RS_UNCHANGED is successful and is not
 * retained as an error. Actions ignore NULL; these queries return
 * RS_INVALID_ARGUMENT and an explanatory message for NULL worlds. */
RsResult world_result(const World *world);
/* Empty when healthy. The returned message is borrowed until clear/destroy. */
const char *world_error_message(const World *world);
/* Clear a validation, expectation or file error after handling it, without
 * undoing edits. Internal/allocation failures and errors raised during tick
 * processing cannot be cleared. No effect during tick processing. A GUI can
 * clear a rejected edit before accepting the next one. */
void world_clear_error(World *world);
/* Check a C condition immediately, without advancing time or changing blocks.
 * False retains RS_EXPECTATION_FAILED and reports the current game tick plus
 * the printf-style message. NULL/empty format uses "condition is false";
 * long messages are truncated. Checks remain active when NDEBUG is defined.
 * C evaluates the condition and message arguments before this call, even if
 * the world already has an error. Use read-only expressions/predicate calls.
 * An error already present in the world takes precedence. Later actions are
 * ignored until explicitly cleared. A NULL world is ignored like other void
 * actions. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void world_expect(World *world, bool condition, const char *format, ...);
bool world_in_bounds(const World *world, BlockPos pos);
WorldPlotBounds world_plot_bounds(const World *world);
RsResult world_expand_plot(World *world, Direction direction, uint32_t count);
/* Preview does not change blocks or bounds. Default terrain is not an edit. */
RsResult world_preview_shrink(World *world, Direction direction, uint32_t count,
                               WorldShrinkPreview *preview);
/* Permanently discard the specified outer strips, leaving at least one plot
 * on both axes. Callers own confirmation; freeze/noupdate retain their meaning. */
RsResult world_shrink_plot(World *world, Direction direction, uint32_t count);
BlockState world_get(const World *world, BlockPos pos); /* Outside vertical/coordinate limits = air; reads may restore archived chunks. */
/* Returns false unless pos contains an active moving-piston placeholder. */
bool world_get_piston_motion(const World *world, BlockPos pos,
                             WorldPistonMotion *motion);
/* direction is the supporting block relative to a torch; the output direction
 * for a repeater/comparator/observer; the push direction for a piston; the lid
 * direction for a barrel; the outlet for a dropper or hopper (hopper up maps to down);
 * and the horizontal orientation for a rail (opposite directions share a shape). */
void world_place(World *world, BlockPos pos, BlockId id, Direction support_dir);
void world_remove(World *world, BlockPos pos);
RsResult world_toggle_wire_dot(World *world, BlockPos pos);
RsResult world_cycle_repeater_delay(World *world, BlockPos pos);
RsResult world_toggle_comparator_mode(World *world, BlockPos pos);
RsResult world_cycle_note(World *world, BlockPos pos);
RsResult world_set_barrel_level(World *world, BlockPos pos, uint8_t level);
RsResult world_open_barrel(World *world, BlockPos pos);
RsResult world_close_barrel(World *world, BlockPos pos);
/* Five slots in a hopper, nine in a dropper, 27 in a barrel. Slots are zero based.
 * Empty stacks use {0,0,0}; nonempty stacks require count <= limit.
 * Identities supplied by the caller include any item/tag distinctions. */
size_t world_container_size(const World *world, BlockPos pos);
RsResult world_get_container_slot(const World *world, BlockPos pos, size_t slot,
                                  ContainerStack *stack);
RsResult world_set_container_slot(World *world, BlockPos pos, size_t slot,
                                  ContainerStack stack);
uint8_t world_hopper_cooldown(const World *world, BlockPos pos);
/* Java Random slot selection, shared by this world's droppers. Default seed 0;
 * saved with the world. A non-container outlet discards the selected item. */
RsResult world_set_dropper_seed(World *world, uint64_t seed);
uint64_t world_tick(const World *world);
uint64_t world_revision(const World *world);
/* Loaded block capacity, not the extent of the horizontal world. */
size_t world_volume(const World *world);
size_t world_loaded_chunks(const World *world);
size_t world_stored_chunks(const World *world);
/* Stream a square window; pending simulation work pins its surrounding chunks.
 * Inactive edits are spilled to a temporary backing file, then saved in .mcw. */
RsResult world_stream(World *world, BlockPos focus, int radius);
/* Visit nonempty loaded sections within the current view window only. */
void world_visit_sections(const World *world, WorldSectionVisitor visitor, void *context);
uint64_t world_topology_revision(const World *world);
WorldConfig world_config(const World *world);
void world_set_change_observer(World *world, WorldChangeObserver observer,
                               void *context);
const char *rs_result_name(RsResult result);
#endif
