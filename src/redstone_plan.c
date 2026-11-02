#include "redstone_plan.h"

#include "blocks/block_internal.h"
#include "sim_internal.h"

#include <limits.h>
#include <stdlib.h>

typedef struct {
    RedstonePlan plan;
    size_t node_capacity, edge_capacity;
} PlanBuilder;

static bool bounds_contain(RedstonePlanBounds bounds, BlockPos pos)
{
    return pos.x >= bounds.min.x && pos.x <= bounds.max.x &&
           pos.y >= bounds.min.y && pos.y <= bounds.max.y &&
           pos.z >= bounds.min.z && pos.z <= bounds.max.z;
}

static bool bounds_valid(const World *world, RedstonePlanBounds bounds)
{
    return bounds.min.x <= bounds.max.x &&
           bounds.min.y <= bounds.max.y &&
           bounds.min.z <= bounds.max.z &&
           world_in_bounds(world, bounds.min) &&
           world_in_bounds(world, bounds.max);
}

static bool reserve_nodes(PlanBuilder *builder, size_t needed)
{
    if (needed <= builder->node_capacity) return true;
    size_t capacity = builder->node_capacity ? builder->node_capacity : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX/2) { capacity = needed; break; }
        capacity *= 2;
    }
    if (capacity > UINT32_MAX ||
        capacity > SIZE_MAX/sizeof(*builder->plan.nodes)) return false;
    RedstonePlanNode *nodes = realloc(builder->plan.nodes,
                                     capacity*sizeof(*nodes));
    if (!nodes) return false;
    builder->plan.nodes = nodes;
    builder->node_capacity = capacity;
    return true;
}

static bool reserve_edges(PlanBuilder *builder, size_t needed)
{
    if (needed <= builder->edge_capacity) return true;
    size_t capacity = builder->edge_capacity ? builder->edge_capacity : 128;
    while (capacity < needed) {
        if (capacity > SIZE_MAX/2) { capacity = needed; break; }
        capacity *= 2;
    }
    if (capacity > UINT32_MAX ||
        capacity > SIZE_MAX/sizeof(*builder->plan.edges)) return false;
    RedstonePlanEdge *edges = realloc(builder->plan.edges,
                                     capacity*sizeof(*edges));
    if (!edges) return false;
    builder->plan.edges = edges;
    builder->edge_capacity = capacity;
    return true;
}

static uint8_t node_flags(BlockState state)
{
    const BlockDef *definition = block_definition((BlockId)state.id);
    uint8_t flags = 0;
    if (definition->conductor) flags |= REDSTONE_NODE_CONDUCTOR;
    if (definition->signal_source) flags |= REDSTONE_NODE_SIGNAL_SOURCE;
    if (definition->diode) flags |= REDSTONE_NODE_DIODE;
    if (definition->scheduled_tick) flags |= REDSTONE_NODE_TIMED;
    if (state.id == BLOCK_REDSTONE_WIRE) flags |= REDSTONE_NODE_WIRE;
    if (state.id == BLOCK_MOVING_PISTON || state.id == BLOCK_HOPPER ||
        state.id == BLOCK_DROPPER || state.id == BLOCK_BARREL)
        flags |= REDSTONE_NODE_RUNTIME;
    return flags;
}

static bool append_node(PlanBuilder *builder, BlockPos pos, BlockState state)
{
    RedstonePlan *plan = &builder->plan;
    if (plan->node_count == UINT32_MAX ||
        !reserve_nodes(builder, plan->node_count + 1) ||
        !posmap_set(&plan->positions, pos, plan->node_count)) return false;
    RedstonePlanNode node = {
        .pos = pos,
        .initial_state = state,
        .flags = node_flags(state)
    };
    if (node.flags & REDSTONE_NODE_TIMED) ++plan->timed_node_count;
    if (node.flags & REDSTONE_NODE_RUNTIME) ++plan->runtime_node_count;
    plan->nodes[plan->node_count++] = node;
    return true;
}

static bool append_edge(PlanBuilder *builder, RedstoneNodeId target,
                        RedstoneEdgeKind kind, Direction direction,
                        uint8_t attenuation)
{
    RedstonePlan *plan = &builder->plan;
    if (plan->edge_count == UINT32_MAX ||
        !reserve_edges(builder, plan->edge_count + 1)) return false;
    plan->edges[plan->edge_count++] = (RedstonePlanEdge){
        .target = target,
        .kind = (uint8_t)kind,
        .direction = (uint8_t)direction,
        .attenuation = attenuation
    };
    if (kind == REDSTONE_EDGE_WIRE) ++plan->wire_edge_count;
    else ++plan->adjacent_edge_count;
    return true;
}

bool redstone_plan_find(const RedstonePlan *plan, BlockPos pos,
                        RedstoneNodeId *node)
{
    if (!plan) return false;
    size_t index;
    if (!posmap_get(&plan->positions, pos, &index) ||
        index >= plan->node_count || index > UINT32_MAX)
        return false;
    if (node) *node = (RedstoneNodeId)index;
    return true;
}

static Direction direction_between(BlockPos source, BlockPos target)
{
    if (target.x > source.x) return DIR_EAST;
    if (target.x < source.x) return DIR_WEST;
    if (target.z > source.z) return DIR_SOUTH;
    if (target.z < source.z) return DIR_NORTH;
    return target.y > source.y ? DIR_UP : DIR_DOWN;
}

static bool mark_boundary(const World *world, RedstonePlanBounds bounds,
                          RedstonePlanNode *node, BlockPos neighbor)
{
    if (bounds_contain(bounds, neighbor) ||
        world_get(world, neighbor).id == BLOCK_AIR) return true;
    node->flags |= REDSTONE_NODE_BOUNDARY;
    return world_result(world) == RS_OK;
}

static bool build_edges(const World *world, PlanBuilder *builder)
{
    RedstonePlan *plan = &builder->plan;
    for (size_t index = 0; index < plan->node_count; ++index) {
        RedstonePlanNode *node = &plan->nodes[index];
        node->first_edge = (uint32_t)plan->edge_count;
        for (Direction direction = 0; direction < DIR_COUNT; ++direction) {
            BlockPos neighbor = block_offset(node->pos, direction);
            RedstoneNodeId target;
            if (redstone_plan_find(plan, neighbor, &target)) {
                if (!append_edge(builder, target, REDSTONE_EDGE_ADJACENT,
                                 direction, 0)) return false;
            } else if (!mark_boundary(world, plan->bounds, node, neighbor)) {
                return false;
            }
        }
        if (node->initial_state.id == BLOCK_REDSTONE_WIRE) {
            BlockPos neighbors[12];
            size_t count = wire_neighbors(world, node->pos, neighbors);
            for (size_t i = 0; i < count; ++i) {
                RedstoneNodeId target;
                if (redstone_plan_find(plan, neighbors[i], &target)) {
                    if (!append_edge(builder, target, REDSTONE_EDGE_WIRE,
                            direction_between(node->pos, neighbors[i]), 1))
                        return false;
                } else if (!mark_boundary(world, plan->bounds, node,
                                          neighbors[i])) {
                    return false;
                }
            }
        }
        size_t count = plan->edge_count - node->first_edge;
        if (count > UINT16_MAX) return false;
        node->edge_count = (uint16_t)count;
        if (node->flags & REDSTONE_NODE_BOUNDARY)
            ++plan->boundary_node_count;
    }
    return true;
}

void redstone_plan_init(RedstonePlan *plan)
{
    if (plan) *plan = (RedstonePlan){0};
}

void redstone_plan_destroy(RedstonePlan *plan)
{
    if (!plan) return;
    free(plan->nodes);
    free(plan->edges);
    posmap_destroy(&plan->positions);
    *plan = (RedstonePlan){0};
}

RsResult redstone_plan_build(const World *world, RedstonePlanBounds bounds,
                             RedstonePlan *destination)
{
    if (!world || !destination || destination->nodes || destination->edges ||
        destination->positions.entries || destination->node_count ||
        destination->edge_count || !bounds_valid(world, bounds))
        return RS_INVALID_ARGUMENT;
    RsResult result = world_result(world);
    if (result != RS_OK) return result;

    PlanBuilder builder = {0};
    builder.plan.bounds = bounds;
    builder.plan.source_tick = world_tick(world);
    builder.plan.source_revision = world_revision(world);

    for (int64_t y = bounds.min.y; y <= bounds.max.y; ++y)
        for (int64_t z = bounds.min.z; z <= bounds.max.z; ++z)
            for (int64_t x = bounds.min.x; x <= bounds.max.x; ++x) {
                BlockPos pos = {(int)x, (int)y, (int)z};
                BlockState state = world_get(world, pos);
                result = world_result(world);
                if (result != RS_OK) goto fail;
                if (state.id != BLOCK_AIR &&
                    !append_node(&builder, pos, state)) {
                    result = RS_OUT_OF_MEMORY;
                    goto fail;
                }
            }
    if (!build_edges(world, &builder)) {
        result = world_result(world);
        if (result == RS_OK) result = RS_OUT_OF_MEMORY;
        goto fail;
    }
    result = world_result(world);
    if (result != RS_OK) goto fail;
    *destination = builder.plan;
    return RS_OK;

fail:
    redstone_plan_destroy(&builder.plan);
    return result;
}
