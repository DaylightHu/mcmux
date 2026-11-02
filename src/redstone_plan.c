#include "redstone_plan.h"

#include "blocks/block_internal.h"
#include "sim_internal.h"

#include <limits.h>
#include <stdlib.h>

typedef struct {
    RedstonePlan plan;
    size_t node_capacity, edge_capacity, input_capacity;
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

static bool reserve_inputs(PlanBuilder *builder, size_t needed)
{
    if (needed <= builder->input_capacity) return true;
    size_t capacity = builder->input_capacity ? builder->input_capacity : 128;
    while (capacity < needed) {
        if (capacity > SIZE_MAX/2) { capacity = needed; break; }
        capacity *= 2;
    }
    if (capacity > UINT32_MAX ||
        capacity > SIZE_MAX/sizeof(*builder->plan.inputs)) return false;
    RedstonePlanInput *inputs = realloc(builder->plan.inputs,
                                       capacity*sizeof(*inputs));
    if (!inputs) return false;
    builder->plan.inputs = inputs;
    builder->input_capacity = capacity;
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
    /* Rail-to-rail power uses a stateful, range-limited walk. Its ordinary
     * external power inputs are still listed, but the specialized walk stays
     * authoritative until a dedicated semantic representation exists. */
    if (block_is_rail((BlockId)state.id))
        flags |= REDSTONE_NODE_INPUT_FALLBACK;
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
    if (node.flags & REDSTONE_NODE_INPUT_FALLBACK)
        ++plan->input_fallback_node_count;
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

static bool append_input(PlanBuilder *builder, RedstoneNodeId source,
                         RedstoneInputKind kind, uint8_t source_face,
                         uint8_t attenuation)
{
    RedstonePlan *plan = &builder->plan;
    if (plan->input_count == UINT32_MAX ||
        !reserve_inputs(builder, plan->input_count + 1)) return false;
    plan->inputs[plan->input_count++] = (RedstonePlanInput){
        .source = source,
        .kind = (uint8_t)kind,
        .source_face = source_face,
        .attenuation = attenuation
    };
    if (kind == REDSTONE_INPUT_DEFAULT) ++plan->default_input_count;
    else if (kind == REDSTONE_INPUT_SIDE) ++plan->side_input_count;
    else ++plan->direct_input_count;
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

static uint8_t queried_face(BlockPos source, BlockPos target)
{
    int64_t dx = (int64_t)target.x - source.x;
    int64_t dy = (int64_t)target.y - source.y;
    int64_t dz = (int64_t)target.z - source.z;
    if ((dx != 0) + (dy != 0) + (dz != 0) != 1 ||
        dx < -1 || dx > 1 || dy < -1 || dy > 1 || dz < -1 || dz > 1)
        return DIR_COUNT;
    return (uint8_t)direction_between(source, target);
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
    }
    return true;
}


typedef enum {
    INPUT_SOURCE_DEFAULT,
    INPUT_SOURCE_DEFAULT_NO_WIRE,
    INPUT_SOURCE_DIRECT,
    INPUT_SOURCE_REPEATER_SIDE,
    INPUT_SOURCE_COMPARATOR_SIDE,
    INPUT_SOURCE_ANALOG,
    INPUT_SOURCE_WIRE
} InputSourceRule;

static bool input_source_matches(BlockState state, InputSourceRule rule)
{
    const BlockDef *definition = block_definition((BlockId)state.id);
    switch (rule) {
    case INPUT_SOURCE_DEFAULT:
        return definition->signal_source || definition->conductor;
    case INPUT_SOURCE_DEFAULT_NO_WIRE:
        return state.id != BLOCK_REDSTONE_WIRE &&
               (definition->signal_source || definition->conductor);
    case INPUT_SOURCE_DIRECT:
        return definition->direct_signal != NULL;
    case INPUT_SOURCE_REPEATER_SIDE:
        return definition->diode;
    case INPUT_SOURCE_COMPARATOR_SIDE:
        return state.id == BLOCK_REDSTONE_BLOCK ||
               state.id == BLOCK_REDSTONE_WIRE || definition->signal_source;
    case INPUT_SOURCE_ANALOG:
        return definition->analog_output != NULL;
    case INPUT_SOURCE_WIRE:
        return state.id == BLOCK_REDSTONE_WIRE;
    }
    return false;
}

static bool add_input_with_face(const World *world, PlanBuilder *builder,
                                RedstonePlanNode *target, BlockPos source_pos,
                                RedstoneInputKind kind, InputSourceRule rule,
                                uint8_t source_face, uint8_t attenuation)
{
    if (!world_in_bounds(world, source_pos)) return true;
    BlockState source_state = world_get(world, source_pos);
    if (world_result(world) != RS_OK) return false;
    if (!input_source_matches(source_state, rule)) return true;

    RedstoneNodeId source;
    if (!redstone_plan_find(&builder->plan, source_pos, &source)) {
        /* A matched source outside the selected region makes the semantic
         * input list deliberately incomplete rather than silently constant. */
        if (!bounds_contain(builder->plan.bounds, source_pos))
            target->flags |= REDSTONE_NODE_BOUNDARY;
        return true;
    }
    for (size_t index = target->first_input;
         index < builder->plan.input_count; ++index) {
        RedstonePlanInput existing = builder->plan.inputs[index];
        if (existing.source == source && existing.kind == (uint8_t)kind &&
            existing.source_face == source_face &&
            existing.attenuation == attenuation) return true;
    }
    return append_input(builder, source, kind, source_face, attenuation);
}

static bool add_input_at(const World *world, PlanBuilder *builder,
                         RedstonePlanNode *target, BlockPos source_pos,
                         RedstoneInputKind kind, InputSourceRule rule,
                         uint8_t attenuation)
{
    return add_input_with_face(
        world, builder, target, source_pos, kind, rule,
        queried_face(source_pos, target->pos), attenuation);
}

static bool add_input_sampled_at(const World *world, PlanBuilder *builder,
                                 RedstonePlanNode *target, BlockPos source_pos,
                                 BlockPos sampled_pos, RedstoneInputKind kind,
                                 InputSourceRule rule, uint8_t attenuation)
{
    return add_input_with_face(
        world, builder, target, source_pos, kind, rule,
        queried_face(source_pos, sampled_pos), attenuation);
}

static bool add_non_face_input(const World *world, PlanBuilder *builder,
                               RedstonePlanNode *target, BlockPos source_pos,
                               RedstoneInputKind kind, InputSourceRule rule,
                               uint8_t attenuation)
{
    return add_input_with_face(world, builder, target, source_pos, kind, rule,
                               DIR_COUNT, attenuation);
}

static bool add_adjacent_inputs(const World *world, PlanBuilder *builder,
                                RedstonePlanNode *target,
                                RedstoneInputKind kind, InputSourceRule rule,
                                int skipped_direction)
{
    for (Direction direction = 0; direction < DIR_COUNT; ++direction) {
        if ((int)direction == skipped_direction) continue;
        if (!add_input_at(world, builder, target,
                          block_offset(target->pos, direction), kind, rule, 0))
            return false;
    }
    return true;
}

static bool add_diode_side_inputs(const World *world, PlanBuilder *builder,
                                  RedstonePlanNode *target, Direction facing,
                                  InputSourceRule rule)
{
    if (!direction_horizontal(facing)) return true;
    const Direction east_west[] = {DIR_NORTH, DIR_SOUTH};
    const Direction north_south[] = {DIR_EAST, DIR_WEST};
    const Direction *sides = facing == DIR_EAST || facing == DIR_WEST ?
                             east_west : north_south;
    for (unsigned index = 0; index < 2; ++index)
        if (!add_input_at(world, builder, target,
                          block_offset(target->pos, sides[index]),
                          REDSTONE_INPUT_SIDE, rule, 0)) return false;
    return true;
}

static bool add_comparator_rear(const World *world, PlanBuilder *builder,
                                RedstonePlanNode *target, Direction facing)
{
    if (!direction_horizontal(facing)) return true;
    Direction rear = direction_opposite(facing);
    BlockPos source = block_offset(target->pos, rear);
    if (!add_input_at(world, builder, target, source,
                      REDSTONE_INPUT_DEFAULT, INPUT_SOURCE_DEFAULT, 0) ||
        !add_input_at(world, builder, target, source,
                      REDSTONE_INPUT_DEFAULT, INPUT_SOURCE_ANALOG, 0))
        return false;

    /* The second override is a separate dependency. At execution time the
     * comparator rule still decides whether the intervening conductor and
     * its signal allow this override to be observed. */
    if (!world_in_bounds(world, source)) return true;
    BlockState state = world_get(world, source);
    if (world_result(world) != RS_OK) return false;
    if (!block_definition((BlockId)state.id)->conductor) return true;
    return add_non_face_input(world, builder, target,
                              block_offset(source, rear),
                              REDSTONE_INPUT_DEFAULT, INPUT_SOURCE_ANALOG, 0);
}

static bool build_semantic_inputs(const World *world, PlanBuilder *builder)
{
    RedstonePlan *plan = &builder->plan;
    for (size_t index = 0; index < plan->node_count; ++index) {
        RedstonePlanNode *node = &plan->nodes[index];
        BlockState state = node->initial_state;
        node->first_input = (uint32_t)plan->input_count;

        if (node->flags & REDSTONE_NODE_CONDUCTOR)
            if (!add_adjacent_inputs(world, builder, node,
                                     REDSTONE_INPUT_DIRECT,
                                     INPUT_SOURCE_DIRECT, -1)) return false;

        switch ((BlockId)state.id) {
        case BLOCK_REDSTONE_WIRE: {
            if (!add_adjacent_inputs(world, builder, node,
                                     REDSTONE_INPUT_DEFAULT,
                                     INPUT_SOURCE_DEFAULT_NO_WIRE, -1))
                return false;
            BlockPos neighbors[12];
            size_t count = wire_neighbors(world, node->pos, neighbors);
            for (size_t input = 0; input < count; ++input)
                if (!add_non_face_input(world, builder, node,
                                        neighbors[input],
                                        REDSTONE_INPUT_DEFAULT,
                                        INPUT_SOURCE_WIRE, 1)) return false;
            break;
        }
        case BLOCK_REDSTONE_TORCH: {
            Direction support = (Direction)state.data.torch.support_dir;
            if ((unsigned)support < DIR_COUNT &&
                !add_input_at(world, builder, node,
                              block_offset(node->pos, support),
                              REDSTONE_INPUT_DEFAULT,
                              INPUT_SOURCE_DEFAULT, 0)) return false;
            break;
        }
        case BLOCK_REDSTONE_REPEATER: {
            Direction facing = (Direction)state.data.repeater.facing;
            if (direction_horizontal(facing)) {
                Direction rear = direction_opposite(facing);
                if (!add_input_at(world, builder, node,
                                  block_offset(node->pos, rear),
                                  REDSTONE_INPUT_DEFAULT,
                                  INPUT_SOURCE_DEFAULT, 0) ||
                    !add_diode_side_inputs(world, builder, node, facing,
                                           INPUT_SOURCE_REPEATER_SIDE))
                    return false;
            }
            break;
        }
        case BLOCK_REDSTONE_COMPARATOR: {
            Direction facing = (Direction)state.data.comparator.facing;
            if (!add_comparator_rear(world, builder, node, facing) ||
                !add_diode_side_inputs(world, builder, node, facing,
                                       INPUT_SOURCE_COMPARATOR_SIDE))
                return false;
            break;
        }
        case BLOCK_POWERED_RAIL:
        case BLOCK_ACTIVATOR_RAIL:
        case BLOCK_HOPPER:
        case BLOCK_DROPPER:
        case BLOCK_NOTE_BLOCK:
        case BLOCK_REDSTONE_LAMP:
            if (!add_adjacent_inputs(world, builder, node,
                                     REDSTONE_INPUT_DEFAULT,
                                     INPUT_SOURCE_DEFAULT, -1)) return false;
            break;
        case BLOCK_PISTON:
        case BLOCK_STICKY_PISTON: {
            Direction facing = (Direction)state.data.piston.facing;
            if (!add_adjacent_inputs(world, builder, node,
                                     REDSTONE_INPUT_DEFAULT,
                                     INPUT_SOURCE_DEFAULT,
                                     (unsigned)facing < DIR_COUNT ?
                                     (int)facing : -1))
                return false;
            BlockPos above = block_offset(node->pos, DIR_UP);
            for (Direction direction = 0; direction < DIR_COUNT; ++direction) {
                if (direction == DIR_DOWN) continue;
                if (!add_input_sampled_at(world, builder, node,
                                          block_offset(above, direction),
                                          above, REDSTONE_INPUT_DEFAULT,
                                          INPUT_SOURCE_DEFAULT, 0)) return false;
            }
            break;
        }
        default:
            break;
        }

        if (state.id == BLOCK_DROPPER) {
            BlockPos above = block_offset(node->pos, DIR_UP);
            for (Direction direction = 0; direction < DIR_COUNT; ++direction) {
                BlockPos source = block_offset(above, direction);
                if (pos_equal(source, node->pos)) continue;
                if (!add_input_sampled_at(world, builder, node, source, above,
                                          REDSTONE_INPUT_DEFAULT,
                                          INPUT_SOURCE_DEFAULT, 0)) return false;
            }
        }

        size_t count = plan->input_count - node->first_input;
        if (count > UINT16_MAX) return false;
        node->input_count = (uint16_t)count;
    }
    return true;
}

static bool build_consumers(PlanBuilder *builder)
{
    RedstonePlan *plan = &builder->plan;
    if (!plan->node_count) return plan->input_count == 0;
    if (plan->input_count > SIZE_MAX/sizeof(*plan->consumers) ||
        plan->node_count > SIZE_MAX/sizeof(size_t)) return false;

    size_t *counts = calloc(plan->node_count, sizeof(*counts));
    size_t *cursors = malloc(plan->node_count*sizeof(*cursors));
    if (!counts || !cursors) {
        free(counts);
        free(cursors);
        return false;
    }

    for (size_t target = 0; target < plan->node_count; ++target) {
        RedstonePlanNode *node = &plan->nodes[target];
        for (size_t offset = 0; offset < node->input_count; ++offset) {
            RedstonePlanInput input =
                plan->inputs[node->first_input + offset];
            if (input.source >= plan->node_count ||
                counts[input.source] == UINT16_MAX) goto fail;
            ++counts[input.source];
        }
    }

    size_t first = 0;
    for (size_t source = 0; source < plan->node_count; ++source) {
        RedstonePlanNode *node = &plan->nodes[source];
        if (first > UINT32_MAX) goto fail;
        node->first_consumer = (uint32_t)first;
        node->consumer_count = (uint16_t)counts[source];
        cursors[source] = first;
        if (counts[source] > SIZE_MAX - first) goto fail;
        first += counts[source];
    }
    if (first != plan->input_count) goto fail;

    if (first) {
        plan->consumers = malloc(first*sizeof(*plan->consumers));
        if (!plan->consumers) goto fail;
    }
    for (size_t target = 0; target < plan->node_count; ++target) {
        RedstonePlanNode *node = &plan->nodes[target];
        for (size_t offset = 0; offset < node->input_count; ++offset) {
            RedstonePlanInput input =
                plan->inputs[node->first_input + offset];
            plan->consumers[cursors[input.source]++] =
                (RedstonePlanConsumer){
                    .target = (RedstoneNodeId)target,
                    .input_offset = (uint16_t)offset
                };
        }
    }
    plan->consumer_count = first;
    free(counts);
    free(cursors);
    return true;

fail:
    free(counts);
    free(cursors);
    return false;
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
    free(plan->inputs);
    free(plan->consumers);
    posmap_destroy(&plan->positions);
    *plan = (RedstonePlan){0};
}

RsResult redstone_plan_build(const World *world, RedstonePlanBounds bounds,
                             RedstonePlan *destination)
{
    if (!world || !destination || destination->nodes || destination->edges ||
        destination->inputs || destination->consumers ||
        destination->positions.entries || destination->node_count ||
        destination->edge_count || destination->input_count ||
        destination->consumer_count ||
        !bounds_valid(world, bounds))
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
    if (!build_semantic_inputs(world, &builder)) {
        result = world_result(world);
        if (result == RS_OK) result = RS_OUT_OF_MEMORY;
        goto fail;
    }
    if (!build_consumers(&builder)) {
        result = RS_OUT_OF_MEMORY;
        goto fail;
    }
    for (size_t index = 0; index < builder.plan.node_count; ++index)
        if (builder.plan.nodes[index].flags & REDSTONE_NODE_BOUNDARY)
            ++builder.plan.boundary_node_count;
    result = world_result(world);
    if (result != RS_OK) goto fail;
    *destination = builder.plan;
    return RS_OK;

fail:
    redstone_plan_destroy(&builder.plan);
    return result;
}
