#ifndef RS_REDSTONE_EVAL_H
#define RS_REDSTONE_EVAL_H

#include "redstone_plan.h"
#include "rs/compiled.h"
#include "blocks/block_internal.h"

/* Independent block-state storage for one run. Pending ticks, torch burnout
 * history and block entities are not cloned here. The borrowed plan must
 * remain alive and unchanged. Initialize with {0}; do not modify blocks
 * directly. */
typedef struct {
    const RedstonePlan *plan;
    BlockState *blocks;
    size_t node_count;
} RedstoneState;

/* A pure input/target query, NOT a scheduled action. input is the default
 * input maximum (after dust attenuation), or direct power for a passive
 * conductor. side is the separate diode side-input maximum. target_power is dust
 * strength, torch/repeater level, comparator cache, or passive conductor
 * relay. A comparator's target_on gates its cache independently. A locked
 * repeater retains its current output. Torch burnout, scheduled events,
 * notifications and their order remain the executor's responsibility. */
typedef SimCEvaluation RedstoneEvaluation;

/* Copy the actual initial states retained by a plan into an empty destination.
 * No reset, settling pass, world write, event execution or tick advance. */
RsResult redstone_state_build(const RedstonePlan *plan, RedstoneState *state);
void redstone_state_destroy(RedstoneState *state);

/* Conservative compatibility check shared with live-plan invalidation. */
bool redstone_state_compatible(BlockState initial, BlockState block);

/* Change a supported block's state in this run only. Direction, attachment,
 * dust shape and block-type edits return RS_UNSUPPORTED: rebuild the plan.
 * This does not enqueue work; the executor must preserve each notification's
 * ordering and choose the appropriate dirty-queue boundary. */
RsResult redstone_state_set(RedstoneState *state, RedstoneNodeId node,
                            BlockState block);

/* World-independent reads. On failure *output is unchanged. Boundary inputs,
 * analog container overrides and unsupported node rules return RS_UNSUPPORTED
 * so callers can use the authoritative world algorithm. These functions never
 * turn incomplete dependencies into zero or execute delayed transitions. */
RsResult redstone_state_signal(const RedstoneState *state, RedstoneNodeId node,
                               Direction face, SignalMode mode, bool direct,
                               uint8_t *output);
RsResult redstone_evaluate(const RedstoneState *state, RedstoneNodeId node,
                           RedstoneEvaluation *output);

#endif
