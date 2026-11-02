#ifndef RS_REDSTONE_PLAN_H
#define RS_REDSTONE_PLAN_H

#include "posmap.h"
#include "rs/world.h"

#include <stddef.h>
#include <stdint.h>

typedef uint32_t RedstoneNodeId;
enum { REDSTONE_NODE_NONE = UINT32_MAX };

typedef struct {
    BlockPos min, max; /* Inclusive. */
} RedstonePlanBounds;

typedef enum {
    REDSTONE_EDGE_ADJACENT,
    REDSTONE_EDGE_WIRE
} RedstoneEdgeKind;

/* A semantic input describes how one node is sampled by another. Keeping the
 * channels separate is important: a repeater side-lock is not an ordinary
 * rear input, and a conducting block only relays direct power. */
typedef enum {
    REDSTONE_INPUT_DEFAULT,
    REDSTONE_INPUT_SIDE,
    REDSTONE_INPUT_DIRECT
} RedstoneInputKind;

enum {
    REDSTONE_NODE_CONDUCTOR = 1u << 0,
    REDSTONE_NODE_SIGNAL_SOURCE = 1u << 1,
    REDSTONE_NODE_DIODE = 1u << 2,
    REDSTONE_NODE_WIRE = 1u << 3,
    REDSTONE_NODE_TIMED = 1u << 4,
    REDSTONE_NODE_RUNTIME = 1u << 5,
    REDSTONE_NODE_BOUNDARY = 1u << 6,
    /* At least one power dependency still requires the authoritative world
     * algorithm. An executor must not silently evaluate only plan inputs. */
    REDSTONE_NODE_INPUT_FALLBACK = 1u << 7
};

typedef struct {
    RedstoneNodeId target;
    uint8_t kind;        /* RedstoneEdgeKind. */
    uint8_t direction;   /* From the source node toward target. */
    uint8_t attenuation; /* Dust-to-dust decay; zero for physical adjacency. */
} RedstonePlanEdge;

typedef struct {
    RedstoneNodeId source;
    uint8_t kind;        /* RedstoneInputKind. */
    /* Face of the source sampled by the target. DIR_COUNT means that the
     * relationship is not a face query (currently dust-to-dust transfer or
     * a comparator override through a conducting block). */
    uint8_t source_face;
    uint8_t attenuation; /* One for each retained dust-to-dust step. */
} RedstonePlanInput;

typedef struct {
    BlockPos pos;
    /* Initial state is copied out of the world. Backends own mutable runtime
     * state separately, so one plan can serve many state batches. */
    BlockState initial_state;
    uint32_t first_edge;
    uint32_t first_input;
    uint16_t edge_count;
    uint16_t input_count;
    uint8_t flags;
} RedstonePlanNode;

typedef struct {
    RedstonePlanBounds bounds;
    RedstonePlanNode *nodes;
    RedstonePlanEdge *edges;
    RedstonePlanInput *inputs;
    size_t node_count, edge_count, input_count;
    size_t adjacent_edge_count, wire_edge_count;
    size_t default_input_count, side_input_count, direct_input_count;
    size_t boundary_node_count, timed_node_count, runtime_node_count;
    size_t input_fallback_node_count;
    uint64_t source_tick, source_revision;
    PosMap positions; /* BlockPos -> RedstoneNodeId. */
} RedstonePlan;

void redstone_plan_init(RedstonePlan *plan);
void redstone_plan_destroy(RedstonePlan *plan);

/* Build a logically read-only, lossless block topology and semantic input
 * table for an inclusive region. world_get may page stored chunks into memory,
 * but blocks, simulation time and scheduled work are not changed. The
 * destination must be empty. Every non-air block is retained; no constant
 * folding or node coalescing is performed. Nodes depending on non-air blocks
 * outside the region are marked as boundary nodes so an executor cannot
 * silently treat an open plan as closed. */
RsResult redstone_plan_build(const World *world, RedstonePlanBounds bounds,
                             RedstonePlan *plan);

bool redstone_plan_find(const RedstonePlan *plan, BlockPos pos,
                        RedstoneNodeId *node);

#endif
