#include "redstone_eval.h"

#include <limits.h>
#include <stdlib.h>

static bool valid_state(const RedstoneState *state, RedstoneNodeId node)
{
    return state && state->plan && state->blocks &&
           state->node_count == state->plan->node_count &&
           state->plan->nodes && node < state->node_count;
}

static bool input_range_valid(const RedstonePlan *plan, RedstonePlanNode node)
{
    return node.first_input <= plan->input_count &&
           node.input_count <= plan->input_count - node.first_input &&
           (!node.input_count || plan->inputs);
}

RsResult redstone_state_build(const RedstonePlan *plan, RedstoneState *state)
{
    if (!plan || !state || state->plan || state->blocks || state->node_count ||
        (plan->node_count && !plan->nodes) || plan->node_count > UINT32_MAX ||
        (plan->external_count && (!plan->live_inputs || !plan->external_sources)) ||
        plan->node_count > SIZE_MAX/sizeof(*state->blocks))
        return RS_INVALID_ARGUMENT;
    for (size_t i = 0; i < plan->node_count; ++i) {
        StateId encoded;
        RedstonePlanNode node = plan->nodes[i];
        if (!state_encode(node.initial_state, &encoded) ||
            !input_range_valid(plan, node)) return RS_INVALID_ARGUMENT;
        for (size_t j = 0; j < node.input_count; ++j) {
            RedstonePlanInput input = plan->inputs[node.first_input + j];
            if ((input.source >= plan->node_count &&
                 !redstone_plan_is_external(plan, input.source)) ||
                input.kind > REDSTONE_INPUT_DIRECT ||
                input.source_face > DIR_COUNT || input.attenuation > 15)
                return RS_INVALID_ARGUMENT;
        }
    }
    BlockState *blocks = NULL;
    if (plan->node_count) {
        blocks = malloc(plan->node_count*sizeof(*blocks));
        if (!blocks) return RS_OUT_OF_MEMORY;
        for (size_t i = 0; i < plan->node_count; ++i)
            blocks[i] = plan->nodes[i].initial_state;
    }
    *state = (RedstoneState){plan, blocks, plan->node_count};
    return RS_OK;
}

void redstone_state_destroy(RedstoneState *state)
{
    if (!state) return;
    free(state->blocks);
    *state = (RedstoneState){0};
}

bool redstone_state_compatible(BlockState initial, BlockState block)
{
    StateId before, after;
    if (block.id != initial.id || !state_encode(block, &after) ||
        !state_encode(initial, &before)) return false;
    switch ((BlockId)block.id) {
    case BLOCK_REDSTONE_WIRE:
        if (block.data.wire.connections != initial.data.wire.connections ||
            block.data.wire.up_connections != initial.data.wire.up_connections ||
            block.data.wire.prefer_dot != initial.data.wire.prefer_dot)
            return false;
        break;
    case BLOCK_REDSTONE_TORCH:
        if (block.data.torch.support_dir != initial.data.torch.support_dir)
            return false;
        break;
    case BLOCK_REDSTONE_REPEATER:
        if (block.data.repeater.facing != initial.data.repeater.facing)
            return false;
        break;
    case BLOCK_REDSTONE_COMPARATOR:
        if (block.data.comparator.facing != initial.data.comparator.facing)
            return false;
        break;
    case BLOCK_OBSERVER:
        if (block.data.observer.facing != initial.data.observer.facing)
            return false;
        break;
    case BLOCK_LEVER:
        if (block.data.lever.support_dir != initial.data.lever.support_dir ||
            block.data.lever.facing != initial.data.lever.facing)
            return false;
        break;
    case BLOCK_REDSTONE_LAMP:
    case BLOCK_NOTE_BLOCK:
    case BLOCK_BARREL:
    case BLOCK_HOPPER:
    case BLOCK_DROPPER:
        /* These states have no native redstone output or changing support
         * geometry. Runtime/analog evaluation still uses reference queries. */
        break;
    case BLOCK_POWERED_RAIL:
    case BLOCK_ACTIVATOR_RAIL:
        return initial.data.rail.facing == block.data.rail.facing &&
               initial.data.rail.ascending == block.data.rail.ascending;
    case BLOCK_PISTON:
    case BLOCK_STICKY_PISTON:
        return initial.data.piston.facing == block.data.piston.facing &&
               initial.data.piston.extended == block.data.piston.extended;
    default:
        return before == after;
    }
    return true;
}

RsResult redstone_state_set(RedstoneState *state, RedstoneNodeId node,
                            BlockState block)
{
    if (!valid_state(state, node)) return RS_INVALID_ARGUMENT;
    StateId before, after;
    if (!state_encode(block, &after)) return RS_INVALID_ARGUMENT;
    if (!redstone_state_compatible(state->plan->nodes[node].initial_state, block))
        return RS_UNSUPPORTED;
    if (!state_encode(state->blocks[node], &before)) return RS_INTERNAL_ERROR;
    if (before == after) return RS_UNCHANGED;
    state->blocks[node] = block;
    return RS_OK;
}

static bool native_output_supported(BlockState block)
{
    switch ((BlockId)block.id) {
    case BLOCK_REDSTONE_BLOCK:
    case BLOCK_REDSTONE_WIRE:
    case BLOCK_REDSTONE_TORCH:
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
    case BLOCK_OBSERVER:
    case BLOCK_LEVER:
        return true;
    default: {
        const BlockDef *definition = block_definition((BlockId)block.id);
        return !definition->signal && !definition->direct_signal;
    }
    }
}

typedef struct {
    const World *world;
    int64_t dx, dy, dz;
    bool read_external;
} LiveInputs;

static bool external_position(const RedstonePlan *plan, RedstoneNodeId source,
                               LiveInputs *live, BlockPos *pos)
{
    if (!live || !redstone_plan_is_external(plan, source)) return false;
    BlockPos original = plan->external_sources[REDSTONE_NODE_NONE - 1 - source];
    int64_t x = original.x + live->dx, y = original.y + live->dy;
    int64_t z = original.z + live->dz;
    if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX ||
        z < INT_MIN || z > INT_MAX) return false;
    *pos = (BlockPos){(int)x,(int)y,(int)z};
    live->read_external = true;
    return true;
}

static RsResult signal_at(const RedstoneState *state, RedstoneNodeId node,
                           Direction face, SignalMode mode, bool direct,
                           LiveInputs *live, uint8_t *output)
{
    if (!valid_state(state, node)) return RS_INVALID_ARGUMENT;
    const RedstonePlan *plan = state->plan;
    RedstonePlanNode info = plan->nodes[node];
    if (!live && (info.flags & REDSTONE_NODE_BOUNDARY)) return RS_UNSUPPORTED;
    BlockState block = state->blocks[node];
    if (!native_output_supported(block)) return RS_UNSUPPORTED;
    if (block.id == BLOCK_REDSTONE_WIRE)
        block.data.wire.connections = info.wire_output_faces;
    uint8_t best = block_state_signal(block, face, mode, direct);
    /* Direct queries never relay a conductor, so this recursion is bounded
     * to one conductor plus its direct sources. Solid chains cannot conduct. */
    if (!direct && (info.flags & REDSTONE_NODE_CONDUCTOR)) {
        if (!input_range_valid(plan, info)) return RS_INTERNAL_ERROR;
        for (size_t i = 0; i < info.input_count; ++i) {
            RedstonePlanInput input = plan->inputs[info.first_input + i];
            if (input.kind != REDSTONE_INPUT_DIRECT) continue;
            if (input.source_face >= DIR_COUNT) return RS_INTERNAL_ERROR;
            uint8_t power;
            RsResult result;
            if (input.source < state->node_count) {
                result = signal_at(state, input.source,
                    (Direction)input.source_face, mode, true, live, &power);
            } else {
                BlockPos pos;
                if (!external_position(plan, input.source, live, &pos)) return RS_UNSUPPORTED;
                power = power_direct(live->world, pos, (Direction)input.source_face, mode);
                result = world_result(live->world);
            }
            if (result != RS_OK) return result;
            if (power > best) best = power;
        }
    }
    *output = best;
    return RS_OK;
}

RsResult redstone_state_signal(const RedstoneState *state, RedstoneNodeId node,
                               Direction face, SignalMode mode, bool direct,
                               uint8_t *output)
{
    if (!output || (unsigned)face >= DIR_COUNT ||
        (mode != SIGNAL_ALL && mode != SIGNAL_WITHOUT_WIRE))
        return RS_INVALID_ARGUMENT;
    uint8_t power;
    RsResult result = signal_at(state, node, face, mode, direct, NULL, &power);
    if (result == RS_OK) *output = power;
    return result;
}

static RsResult sample_input(const RedstoneState *state, BlockId target,
                              RedstonePlanInput input, LiveInputs *live,
                              uint8_t *output)
{
    bool external = input.source >= state->node_count;
    BlockPos external_pos;
    BlockState source;
    if (external) {
        if (!redstone_plan_is_external(state->plan, input.source)) return RS_INTERNAL_ERROR;
        if (!external_position(state->plan, input.source, live, &external_pos))
            return RS_UNSUPPORTED;
        source = world_get(live->world, external_pos);
        RsResult result = world_result(live->world);
        if (result != RS_OK) return result;
    } else {
        if (!live && (state->plan->nodes[input.source].flags & REDSTONE_NODE_BOUNDARY))
            return RS_UNSUPPORTED;
        source = state->blocks[input.source];
    }
    bool diode = target == BLOCK_REDSTONE_REPEATER ||
                 target == BLOCK_REDSTONE_COMPARATOR;
    /* Analog overrides require block-entity state not owned by this backend.
     * In particular, max(rear inputs) is not the comparator override rule. */
    if (target == BLOCK_REDSTONE_COMPARATOR &&
        block_definition((BlockId)source.id)->analog_output)
        return RS_UNSUPPORTED;
    uint8_t power;
    if (source.id == BLOCK_REDSTONE_WIRE &&
        (diode || input.source_face == DIR_COUNT)) {
        power = source.data.wire.power;
    } else if (input.kind == REDSTONE_INPUT_SIDE &&
               target == BLOCK_REDSTONE_COMPARATOR &&
               source.id == BLOCK_REDSTONE_BLOCK) {
        power = 15;
    } else {
        if (input.source_face >= DIR_COUNT) return RS_UNSUPPORTED;
        SignalMode mode = target == BLOCK_REDSTONE_WIRE ? SIGNAL_WITHOUT_WIRE : SIGNAL_ALL;
        Direction face = (Direction)input.source_face;
        bool direct = input.kind != REDSTONE_INPUT_DEFAULT;
        RsResult result;
        if (external) {
            power = direct ? power_direct(live->world, external_pos, face, mode) :
                             power_signal(live->world, external_pos, face, mode);
            result = world_result(live->world);
        } else result = signal_at(state, input.source, face, mode, direct, live, &power);
        if (result != RS_OK) return result;
    }
    *output = power > input.attenuation ?
              (uint8_t)(power - input.attenuation) : 0;
    return RS_OK;
}

/* Specialize the ordinary evaluator with no live input reader. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline RsResult evaluate(const RedstoneState *state, RedstoneNodeId node,
                                 LiveInputs *live, RedstoneEvaluation *output)
{
    if (!output || !valid_state(state, node)) return RS_INVALID_ARGUMENT;
    RedstonePlanNode info = state->plan->nodes[node];
    if ((info.flags & (REDSTONE_NODE_RUNTIME | REDSTONE_NODE_INPUT_FALLBACK)) ||
        (!live && (info.flags & REDSTONE_NODE_BOUNDARY))) return RS_UNSUPPORTED;
    BlockState block = state->blocks[node];
    switch ((BlockId)block.id) {
    case BLOCK_REDSTONE_WIRE:
    case BLOCK_REDSTONE_TORCH:
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
    case BLOCK_IRON:
    case BLOCK_SLIME:
    case BLOCK_TARGET:
        break;
    default:
        return RS_UNSUPPORTED;
    }
    if (!input_range_valid(state->plan, info)) return RS_INTERNAL_ERROR;
    RedstoneEvaluation value = {0};
    for (size_t i = 0; i < info.input_count; ++i) {
        RedstonePlanInput input = state->plan->inputs[info.first_input + i];
        uint8_t power;
        RsResult result = sample_input(state, (BlockId)block.id, input, live, &power);
        if (result != RS_OK) return result;
        uint8_t *channel = input.kind == REDSTONE_INPUT_SIDE ?
                           &value.side : &value.input;
        if (power > *channel) *channel = power;
    }
    value.target_power = value.input;
    value.target_on = value.input > 0;
    if (block.id == BLOCK_REDSTONE_TORCH) {
        value.target_on = !value.target_on;
        value.target_power = value.target_on ? 15 : 0;
    } else if (block.id == BLOCK_REDSTONE_REPEATER) {
        value.locked = value.side > 0;
        if (value.locked) value.target_on = block.data.repeater.powered;
        value.target_power = value.target_on ? 15 : 0;
    } else if (block.id == BLOCK_REDSTONE_COMPARATOR) {
        value.target_on = value.input > 0 &&
            (value.input > value.side ||
             (value.input == value.side && !block.data.comparator.subtract));
        if (block.data.comparator.subtract)
            value.target_power = value.input > value.side ?
                (uint8_t)(value.input - value.side) : 0;
    }
    *output = value;
    return RS_OK;
}

RsResult redstone_evaluate(const RedstoneState *state, RedstoneNodeId node,
                           RedstoneEvaluation *output)
{
    return evaluate(state, node, NULL, output);
}

RsResult redstone_evaluate_live(const RedstoneState *state, RedstoneNodeId node,
                                const World *world, BlockPos origin,
                                RedstoneEvaluation *output, bool *read_external)
{
    if (!valid_state(state, node) || !state->plan->live_inputs || !world ||
        !read_external) return RS_INVALID_ARGUMENT;
    BlockPos min = state->plan->bounds.min;
    LiveInputs live = {world, (int64_t)origin.x-min.x, (int64_t)origin.y-min.y,
                       (int64_t)origin.z-min.z, false};
    RsResult result = evaluate(state, node, &live, output);
    *read_external = live.read_external;
    return result;
}
