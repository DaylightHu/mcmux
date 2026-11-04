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

enum { SIM_C_ABI_VERSION = 1 };
typedef RsResult (*SimCEvaluate)(const BlockState *blocks, uint32_t node,
                                SimCEvaluation *output);
typedef struct {
    uint32_t abi_version;
    size_t node_count, topology_word_count;
    const uint32_t *topology;
    SimCEvaluate evaluate;
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
 * Reuses the CPU cache/invalidation policy and reference fallback rules;
 * topology changes invalidate it. Original tick/notification order is kept. */
RsResult sim_use_c(World *world, BlockPos min, BlockPos max,
                   const SimCModel *model);
bool sim_c_active(const World *world);

#endif
