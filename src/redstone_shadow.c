#include "redstone_shadow.h"

#include "blocks/block_internal.h"

#include <stdio.h>
#include <stdlib.h>

typedef enum {
    SHADOW_LINK,
    SHADOW_WIRE,
    SHADOW_CONDUCTOR,
    SHADOW_REPEATER_SIDE,
    SHADOW_COMPARATOR_OUTPUT,
    SHADOW_COMPARATOR_POWERED,
    SHADOW_PISTON,
    SHADOW_HOPPER,
    SHADOW_DROPPER,
    SHADOW_NOTE_BLOCK,
    SHADOW_LAMP
} ShadowCheck;

static const char *check_name(ShadowCheck check)
{
    static const char *const names[] = {
        "input link", "wire power", "conductor power", "repeater side",
        "comparator output", "comparator powered", "piston input",
        "hopper input", "dropper input", "note-block input", "lamp input"
    };
    return (unsigned)check < sizeof(names)/sizeof(*names) ?
           names[check] : "unknown check";
}

static RsResult mismatch(World *world, BlockPos pos, ShadowCheck check,
                         uint8_t expected, uint8_t actual)
{
    char detail[160];
    snprintf(detail, sizeof(detail),
             "%s mismatch: plan=%u reference=%u",
             check_name(check), expected, actual);
    world_record_error(world, RS_INTERNAL_ERROR, "redstone shadow", &pos,
                       detail);
    return world_result(world);
}

static bool topology_state_equal(BlockState first, BlockState second)
{
    if (first.id != second.id) return false;
    switch ((BlockId)first.id) {
    case BLOCK_REDSTONE_WIRE:
        return first.data.wire.connections == second.data.wire.connections &&
               first.data.wire.up_connections ==
                   second.data.wire.up_connections;
    case BLOCK_REDSTONE_TORCH:
        return first.data.torch.support_dir == second.data.torch.support_dir;
    case BLOCK_REDSTONE_REPEATER:
        return first.data.repeater.facing == second.data.repeater.facing;
    case BLOCK_REDSTONE_COMPARATOR:
        return first.data.comparator.facing == second.data.comparator.facing;
    case BLOCK_POWERED_RAIL:
    case BLOCK_ACTIVATOR_RAIL:
        return first.data.rail.facing == second.data.rail.facing &&
               first.data.rail.ascending == second.data.rail.ascending;
    case BLOCK_OBSERVER:
        return first.data.observer.facing == second.data.observer.facing;
    case BLOCK_PISTON:
    case BLOCK_STICKY_PISTON:
    case BLOCK_PISTON_HEAD:
    case BLOCK_STICKY_PISTON_HEAD:
        return first.data.piston.facing == second.data.piston.facing &&
               first.data.piston.extended == second.data.piston.extended;
    case BLOCK_LEVER:
        return first.data.lever.support_dir == second.data.lever.support_dir &&
               first.data.lever.facing == second.data.lever.facing;
    default:
        return true;
    }
}

static bool near_bounds(RedstonePlanBounds bounds, BlockPos pos)
{
    int64_t x = pos.x, y = pos.y, z = pos.z;
    return x >= (int64_t)bounds.min.x - 2 &&
           x <= (int64_t)bounds.max.x + 2 &&
           y >= (int64_t)bounds.min.y - 2 &&
           y <= (int64_t)bounds.max.y + 2 &&
           z >= (int64_t)bounds.min.z - 2 &&
           z <= (int64_t)bounds.max.z + 2;
}

void redstone_shadow_init(RedstoneShadow *shadow)
{
    if (shadow) *shadow = (RedstoneShadow){0};
}

void redstone_shadow_destroy(RedstoneShadow *shadow)
{
    if (!shadow) return;
    redstone_plan_destroy(&shadow->plan);
    free(shadow->states);
    *shadow = (RedstoneShadow){0};
}

static bool reserve_states(RedstoneShadow *shadow, size_t count)
{
    if (count <= shadow->state_capacity) return true;
    if (count > SIZE_MAX/sizeof(*shadow->states)) return false;
    BlockState *states = realloc(shadow->states, count*sizeof(*states));
    if (!states) return false;
    shadow->states = states;
    shadow->state_capacity = count;
    return true;
}

RsResult redstone_shadow_build(RedstoneShadow *shadow, const World *world,
                               RedstonePlanBounds bounds)
{
    if (!shadow || !world) return RS_INVALID_ARGUMENT;
    RedstonePlan next;
    redstone_plan_init(&next);
    RsResult result = redstone_plan_build(world, bounds, &next);
    if (result != RS_OK) return result;
    if (!reserve_states(shadow, next.node_count)) {
        redstone_plan_destroy(&next);
        return RS_OUT_OF_MEMORY;
    }
    redstone_plan_destroy(&shadow->plan);
    shadow->plan = next;
    shadow->bounds = bounds;
    shadow->dirty = false;
    ++shadow->stats.rebuilds;
    return RS_OK;
}

void redstone_shadow_state_changed(RedstoneShadow *shadow, StateChange change)
{
    if (!shadow || shadow->dirty || !near_bounds(shadow->bounds, change.pos))
        return;
    if (!topology_state_equal(change.previous, change.current))
        shadow->dirty = true;
}

static uint8_t maximum(uint8_t first, uint8_t second)
{
    return first > second ? first : second;
}

static uint8_t analog_output(const RedstoneShadow *shadow,
                             RedstoneNodeId node)
{
    BlockState state = shadow->states[node];
    if (state.id == BLOCK_BARREL) return state.data.barrel.level;
    if (state.id == BLOCK_HOPPER) return state.data.hopper.level;
    if (state.id == BLOCK_DROPPER) return state.data.dropper.level;
    return 0;
}

static uint8_t direct_signal(const RedstoneShadow *shadow,
                             RedstoneNodeId node, Direction face,
                             SignalMode mode)
{
    if ((unsigned)face >= DIR_COUNT) return 0;
    BlockState state = shadow->states[node];
    switch ((BlockId)state.id) {
    case BLOCK_REDSTONE_WIRE:
        if (mode == SIGNAL_WITHOUT_WIRE || face == DIR_UP ||
            !state.data.wire.power) return 0;
        return face == DIR_DOWN ||
               (state.data.wire.connections & (1u << face)) ?
               state.data.wire.power : 0;
    case BLOCK_REDSTONE_TORCH:
        return face == DIR_UP && state.data.torch.lit ? 15 : 0;
    case BLOCK_REDSTONE_REPEATER:
        return state.data.repeater.powered &&
               face == (Direction)state.data.repeater.facing ? 15 : 0;
    case BLOCK_REDSTONE_COMPARATOR:
        return state.data.comparator.powered &&
               face == (Direction)state.data.comparator.facing ?
               state.data.comparator.output : 0;
    case BLOCK_OBSERVER:
        return state.data.observer.powered &&
               face == (Direction)state.data.observer.facing ? 15 : 0;
    case BLOCK_LEVER:
        return state.data.lever.powered &&
               face == (Direction)state.data.lever.support_dir ? 15 : 0;
    default:
        return 0;
    }
}

static uint8_t conducted_power(const RedstoneShadow *shadow,
                               RedstoneNodeId node, SignalMode mode)
{
    const RedstonePlanNode *target = &shadow->plan.nodes[node];
    uint8_t best = 0;
    for (size_t offset = 0; offset < target->input_count; ++offset) {
        RedstonePlanInput input =
            shadow->plan.inputs[target->first_input + offset];
        if (input.kind != REDSTONE_INPUT_DIRECT ||
            input.source_face >= DIR_COUNT) continue;
        best = maximum(best, direct_signal(shadow, input.source,
                       (Direction)input.source_face, mode));
    }
    return best;
}

static uint8_t own_signal(const RedstoneShadow *shadow, RedstoneNodeId node,
                          Direction face, SignalMode mode)
{
    if ((unsigned)face >= DIR_COUNT) return 0;
    BlockState state = shadow->states[node];
    switch ((BlockId)state.id) {
    case BLOCK_REDSTONE_BLOCK:
        return 15;
    case BLOCK_REDSTONE_WIRE:
        return direct_signal(shadow, node, face, mode);
    case BLOCK_REDSTONE_TORCH:
        return state.data.torch.lit &&
               face != (Direction)state.data.torch.support_dir ? 15 : 0;
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
    case BLOCK_OBSERVER:
        return direct_signal(shadow, node, face, mode);
    case BLOCK_LEVER:
        return state.data.lever.powered ? 15 : 0;
    default:
        return 0;
    }
}

static uint8_t signal(const RedstoneShadow *shadow, RedstoneNodeId node,
                      Direction face, SignalMode mode)
{
    uint8_t power = own_signal(shadow, node, face, mode);
    if (power < 15 &&
        (shadow->plan.nodes[node].flags & REDSTONE_NODE_CONDUCTOR))
        power = maximum(power, conducted_power(shadow, node, mode));
    return power;
}

static uint8_t side_value(const RedstoneShadow *shadow,
                          RedstoneNodeId target, RedstonePlanInput input)
{
    if (input.source_face >= DIR_COUNT) return 0;
    BlockState target_state = shadow->states[target];
    BlockState source_state = shadow->states[input.source];
    if (target_state.id == BLOCK_REDSTONE_COMPARATOR) {
        if (source_state.id == BLOCK_REDSTONE_BLOCK) return 15;
        if (source_state.id == BLOCK_REDSTONE_WIRE)
            return source_state.data.wire.power;
    }
    return direct_signal(shadow, input.source,
                         (Direction)input.source_face, SIGNAL_ALL);
}

static uint8_t default_value(const RedstoneShadow *shadow,
                             RedstonePlanInput input,
                             SignalMode mode)
{
    if (input.attenuation) {
        uint8_t power = shadow->states[input.source].data.wire.power;
        return power > input.attenuation ?
               (uint8_t)(power - input.attenuation) : 0;
    }
    if (input.source_face >= DIR_COUNT)
        return analog_output(shadow, input.source);
    return signal(shadow, input.source, (Direction)input.source_face, mode);
}

static uint8_t maximum_input(const RedstoneShadow *shadow,
                             RedstoneNodeId target, RedstoneInputKind kind,
                             SignalMode mode)
{
    const RedstonePlanNode *node = &shadow->plan.nodes[target];
    uint8_t best = 0;
    for (size_t offset = 0; offset < node->input_count; ++offset) {
        RedstonePlanInput input =
            shadow->plan.inputs[node->first_input + offset];
        if (input.kind != kind) continue;
        uint8_t value = kind == REDSTONE_INPUT_SIDE ?
            side_value(shadow, target, input) :
            default_value(shadow, input, mode);
        best = maximum(best, value);
    }
    return best;
}

static bool node_at(const RedstoneShadow *shadow, BlockPos pos,
                    RedstoneNodeId *node)
{
    return redstone_plan_find(&shadow->plan, pos, node);
}

static uint8_t comparator_rear(const RedstoneShadow *shadow,
                               RedstoneNodeId target)
{
    BlockState state = shadow->states[target];
    Direction facing = (Direction)state.data.comparator.facing;
    if (!direction_horizontal(facing)) return 0;
    Direction rear = direction_opposite(facing);
    BlockPos source_pos = block_offset(shadow->plan.nodes[target].pos, rear);
    RedstoneNodeId source;
    if (!node_at(shadow, source_pos, &source)) return 0;

    uint8_t power = signal(shadow, source, facing, SIGNAL_ALL);
    BlockState source_state = shadow->states[source];
    if (source_state.id == BLOCK_REDSTONE_WIRE)
        power = maximum(power, source_state.data.wire.power);
    const BlockDef *definition =
        block_definition((BlockId)source_state.id);
    if (definition->analog_output) return analog_output(shadow, source);
    if (power < 15 && definition->conductor) {
        BlockPos second_pos = block_offset(source_pos, rear);
        RedstoneNodeId second;
        if (node_at(shadow, second_pos, &second) &&
            block_definition((BlockId)shadow->states[second].id)->analog_output)
            return analog_output(shadow, second);
    }
    return power;
}

static uint8_t comparator_side(const RedstoneShadow *shadow,
                               RedstoneNodeId target)
{
    return maximum_input(shadow, target, REDSTONE_INPUT_SIDE, SIGNAL_ALL);
}

static uint8_t comparator_output(const RedstoneShadow *shadow,
                                 RedstoneNodeId target)
{
    BlockState state = shadow->states[target];
    uint8_t rear = comparator_rear(shadow, target);
    if (!state.data.comparator.subtract) return rear;
    uint8_t side = comparator_side(shadow, target);
    return rear > side ? (uint8_t)(rear - side) : 0;
}

static bool comparator_powered(const RedstoneShadow *shadow,
                               RedstoneNodeId target)
{
    BlockState state = shadow->states[target];
    uint8_t rear = comparator_rear(shadow, target);
    if (!rear) return false;
    uint8_t side = comparator_side(shadow, target);
    return rear > side ||
           (rear == side && !state.data.comparator.subtract);
}

static RsResult validate_link(RedstoneShadow *shadow, World *world,
                              RedstoneNodeId target, RedstonePlanInput input)
{
    if (input.attenuation) return RS_OK;
    BlockPos source_pos = shadow->plan.nodes[input.source].pos;
    uint8_t expected = 0, actual = 0;
    if (input.kind == REDSTONE_INPUT_SIDE) {
        expected = side_value(shadow, target, input);
        BlockState target_state = shadow->states[target];
        BlockState source_state = world_get(world, source_pos);
        if (target_state.id == BLOCK_REDSTONE_COMPARATOR &&
            source_state.id == BLOCK_REDSTONE_BLOCK) actual = 15;
        else if (target_state.id == BLOCK_REDSTONE_COMPARATOR &&
                 source_state.id == BLOCK_REDSTONE_WIRE)
            actual = source_state.data.wire.power;
        else if (input.source_face < DIR_COUNT)
            actual = power_direct(world, source_pos,
                                  (Direction)input.source_face, SIGNAL_ALL);
    } else if (input.source_face >= DIR_COUNT) {
        expected = analog_output(shadow, input.source);
        actual = block_analog_output(world, source_pos);
    } else if (input.kind == REDSTONE_INPUT_DIRECT) {
        expected = direct_signal(shadow, input.source,
                                 (Direction)input.source_face, SIGNAL_ALL);
        actual = power_direct(world, source_pos,
                              (Direction)input.source_face, SIGNAL_ALL);
    } else {
        SignalMode mode = shadow->states[target].id == BLOCK_REDSTONE_WIRE ?
                          SIGNAL_WITHOUT_WIRE : SIGNAL_ALL;
        expected = signal(shadow, input.source,
                          (Direction)input.source_face, mode);
        actual = power_signal(world, source_pos,
                              (Direction)input.source_face, mode);
    }
    ++shadow->stats.links_checked;
    return expected == actual ? RS_OK :
           mismatch(world, shadow->plan.nodes[target].pos,
                    SHADOW_LINK, expected, actual);
}

static RsResult validate_node(RedstoneShadow *shadow, World *world,
                              RedstoneNodeId node)
{
    RedstonePlanNode planned = shadow->plan.nodes[node];
    BlockState state = shadow->states[node];
    if (planned.flags &
        (REDSTONE_NODE_BOUNDARY | REDSTONE_NODE_INPUT_FALLBACK)) {
        ++shadow->stats.nodes_skipped;
        return RS_OK;
    }

    if (planned.flags & REDSTONE_NODE_CONDUCTOR) {
        uint8_t expected = conducted_power(shadow, node, SIGNAL_ALL);
        uint8_t actual = power_conducted(world, planned.pos, SIGNAL_ALL);
        if (expected != actual)
            return mismatch(world, planned.pos, SHADOW_CONDUCTOR,
                            expected, actual);
        expected = conducted_power(shadow, node, SIGNAL_WITHOUT_WIRE);
        actual = power_conducted(world, planned.pos, SIGNAL_WITHOUT_WIRE);
        if (expected != actual)
            return mismatch(world, planned.pos, SHADOW_CONDUCTOR,
                            expected, actual);
    }

    uint8_t input = maximum_input(shadow, node, REDSTONE_INPUT_DEFAULT,
                                  state.id == BLOCK_REDSTONE_WIRE ?
                                  SIGNAL_WITHOUT_WIRE : SIGNAL_ALL);
    switch ((BlockId)state.id) {
    case BLOCK_REDSTONE_WIRE:
        if (input != state.data.wire.power)
            return mismatch(world, planned.pos, SHADOW_WIRE, input,
                            state.data.wire.power);
        break;
    case BLOCK_REDSTONE_REPEATER: {
        uint8_t expected = maximum_input(shadow, node, REDSTONE_INPUT_SIDE,
                                         SIGNAL_ALL) > 0;
        uint8_t actual = repeater_is_locked(world, planned.pos, state);
        if (expected != actual)
            return mismatch(world, planned.pos, SHADOW_REPEATER_SIDE,
                            expected, actual);
        break;
    }
    case BLOCK_REDSTONE_COMPARATOR: {
        uint8_t expected = comparator_output(shadow, node);
        uint8_t actual = comparator_desired_output(world, planned.pos, state);
        if (expected != actual)
            return mismatch(world, planned.pos, SHADOW_COMPARATOR_OUTPUT,
                            expected, actual);
        expected = comparator_powered(shadow, node);
        actual = comparator_should_turn_on(world, planned.pos, state);
        if (expected != actual)
            return mismatch(world, planned.pos, SHADOW_COMPARATOR_POWERED,
                            expected, actual);
        break;
    }
    case BLOCK_PISTON:
    case BLOCK_STICKY_PISTON:
        if ((input > 0) != state.data.piston.powered)
            return mismatch(world, planned.pos, SHADOW_PISTON, input > 0,
                            state.data.piston.powered);
        break;
    case BLOCK_HOPPER:
        if ((input == 0) != state.data.hopper.enabled)
            return mismatch(world, planned.pos, SHADOW_HOPPER, input == 0,
                            state.data.hopper.enabled);
        break;
    case BLOCK_DROPPER:
        if ((input > 0) != state.data.dropper.triggered)
            return mismatch(world, planned.pos, SHADOW_DROPPER, input > 0,
                            state.data.dropper.triggered);
        break;
    case BLOCK_NOTE_BLOCK:
        if ((input > 0) != state.data.note_block.powered)
            return mismatch(world, planned.pos, SHADOW_NOTE_BLOCK, input > 0,
                            state.data.note_block.powered);
        break;
    case BLOCK_REDSTONE_LAMP:
        if (input > 0 && !state.data.lamp.lit)
            return mismatch(world, planned.pos, SHADOW_LAMP, 1, 0);
        break;
    default:
        break;
    }
    ++shadow->stats.nodes_checked;
    return RS_OK;
}

static RsResult refresh_states(RedstoneShadow *shadow, World *world)
{
    for (size_t node = 0; node < shadow->plan.node_count; ++node) {
        BlockState state = world_get(world, shadow->plan.nodes[node].pos);
        if (!topology_state_equal(state,
                                  shadow->plan.nodes[node].initial_state)) {
            shadow->dirty = true;
            return RS_UNCHANGED;
        }
        shadow->states[node] = state;
    }
    return world_result(world);
}

RsResult redstone_shadow_validate(RedstoneShadow *shadow, World *world)
{
    if (!shadow || !world) return RS_INVALID_ARGUMENT;
    if (!sim_updates_enabled(world)) return world_result(world);
    if (shadow->dirty) {
        RsResult result = redstone_shadow_build(shadow, world, shadow->bounds);
        if (result != RS_OK) return result;
    }
    RsResult result = refresh_states(shadow, world);
    if (result == RS_UNCHANGED) {
        result = redstone_shadow_build(shadow, world, shadow->bounds);
        if (result != RS_OK) return result;
        result = refresh_states(shadow, world);
    }
    if (result != RS_OK) return result;

    ++shadow->stats.validations;
    for (RedstoneNodeId node = 0; node < shadow->plan.node_count; ++node) {
        RedstonePlanNode planned = shadow->plan.nodes[node];
        for (size_t offset = 0; offset < planned.input_count; ++offset) {
            RedstonePlanInput input =
                shadow->plan.inputs[planned.first_input + offset];
            result = validate_link(shadow, world, node, input);
            if (result != RS_OK) return result;
        }
        result = validate_node(shadow, world, node);
        if (result != RS_OK) return result;
    }
    return world_result(world);
}
