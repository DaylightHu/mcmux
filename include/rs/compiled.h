#ifndef RS_COMPILED_H
#define RS_COMPILED_H

#include "world.h"
#include <stdio.h>

/* Pure query results. Scheduling, notifications and writes belong to the
 * simulator runtime, including when this query is generated C. */
typedef struct {
    uint8_t input, side, target_power;
    bool target_on, locked;
} SimCEvaluation;

enum { SIM_C_ABI_VERSION = 3 };
typedef RsResult (*SimCEvaluate)(const BlockState *blocks, uint32_t node,
                                SimCEvaluation *output);
typedef struct {
    uint32_t abi_version;
    size_t node_count, topology_word_count;
    const uint32_t *topology;
    SimCEvaluate evaluate;
    /* Optional reverse index of pure-query state reads. A write to source s
     * invalidates read_targets[read_offsets[s]..read_offsets[s+1]). Conducting
     * relays are already expanded. Include every state that can affect any
     * result field, including conditional reads and the query's own state.
     * Offsets have node_count+1 entries; the final offset is read_count.
     * All three fields zero selects conservative graph invalidation. */
    size_t read_count;
    const size_t *read_offsets;
    const uint32_t *read_targets;
} SimCModel;

/* Emit a C11 translation unit defining `const SimCModel SYMBOL`. SYMBOL must
 * be a non-keyword C identifier starting with an ASCII letter (max 128 chars).
 * Include the generated file, or compile it and declare its symbol extern.
 * The stream is borrowed; caller closes it and discards partial output on
 * error. No compiler or shell is launched by the library. Call between actions.
 * No time, block state, pending events or backend selection is changed.
 * Unsupported queries are emitted as explicit RS_UNSUPPORTED results. */
RsResult sim_write_c(const World *world, BlockPos min, BlockPos max,
                     FILE *output, const char *symbol);

/* Attach a trusted, compiled-in model to the inclusive region. The complete
 * query topology is compared, not just a hash or node count. Dynamic power,
 * repeater delay and comparator mode may differ from export-time values.
 * Mismatch returns RS_UNSUPPORTED and leaves the previous backend selected.
 * The model, code and metadata must stay alive and immutable until detached.
 * Never pass models compiled against a different version of the core.
 * Uses the reverse read index when present, otherwise conservative CPU graph
 * invalidation. Reference fallback rules and original tick/notification order
 * are kept; topology changes invalidate the attachment. */
RsResult sim_use_c(World *world, BlockPos min, BlockPos max,
                   const SimCModel *model);
/* Attach another independent instance of a compiled model. Regions cannot
 * overlap. Its model may be the same pointer as another region's model;
 * each attachment owns its own state and query cache. Existing attachments
 * survive a failed call. sim_use_reference() detaches all regions. */
RsResult sim_add_c(World *world, BlockPos min, BlockPos max,
                  const SimCModel *model);
bool sim_c_active(const World *world);

#endif
