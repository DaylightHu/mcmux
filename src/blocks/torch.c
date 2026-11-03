#include "block_internal.h"
#include "sim_internal.h"
#include "sim_executor.h"

enum {
    TORCH_DELAY = 2,
    BURNOUT_THRESHOLD = 8,
    BURNOUT_WINDOW = 60,
    BURNOUT_RECOVERY = 160
};

bool torch_supported(const World *world, BlockPos position, Direction support)
{
    return (support == DIR_DOWN || direction_horizontal(support)) &&
           block_supports_torch(world, block_offset(position, support),
                                 direction_opposite(support));
}

static bool input_powered(const World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    RedstoneEvaluation value;
    if (sim_executor_evaluate(world, position, state, &value) == RS_OK)
        return value.input > 0;
    Direction support_dir = (Direction)state.data.torch.support_dir;
    BlockPos support = block_offset(position, support_dir);

    return power_signal(
        world,
        support,
        direction_opposite(support_dir),
        SIGNAL_ALL
    ) > 0;
}

static uint8_t torch_signal(const World *world, BlockPos position,
                            Direction face, SignalMode mode)
{
    return block_state_signal(world_get(world, position), face, mode, false);
}

static uint8_t torch_direct(const World *world, BlockPos position,
                            Direction face, SignalMode mode)
{
    return face == DIR_UP ?
           block_state_signal(world_get(world, position), face, mode, true) : 0;
}

void torch_check_input(World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    bool should_be_lit = !input_powered(world, position);
    if (state.data.torch.lit != should_be_lit && !sim_will_tick(world, position)) {
        sim_schedule(world, position, TORCH_DELAY);
    }
}

static void torch_neighbor(World *world, BlockPos position)
{
    sim_mark_input(world, position);
}

static bool recently_burned(const TorchRuntime *runtime, uint64_t tick)
{
    if (runtime->off_count < BURNOUT_THRESHOLD) {
        return false;
    }
    for (size_t index = 0; index < BURNOUT_THRESHOLD; ++index) {
        if (tick - runtime->off_times[index] > BURNOUT_WINDOW) {
            return false;
        }
    }
    return true;
}

static TickAction torch_tick(World *world, BlockPos position)
{
    BlockState state = world_get(world, position);
    TickAction action = {state, false, 0};
    TorchRuntime *runtime = sim_torch_runtime(world, position);
    if (!runtime) {
        world->sim->error = RS_INTERNAL_ERROR;
        return action;
    }

    bool powered = input_powered(world, position);
    if (state.data.torch.lit && powered) {
        action.state.data.torch.lit = false;
        action.changed = true;
        runtime->off_times[runtime->off_head] = world->tick;
        runtime->off_head = (uint8_t)((runtime->off_head + 1) % BURNOUT_THRESHOLD);
        if (runtime->off_count < BURNOUT_THRESHOLD) {
            ++runtime->off_count;
        }
        if (recently_burned(runtime, world->tick)) {
            if (UINT64_MAX - world->tick < BURNOUT_RECOVERY) {
                world->sim->error = RS_INTERNAL_ERROR;
                return action;
            }
            runtime->burned_until = world->tick + BURNOUT_RECOVERY;
            action.next_delay = BURNOUT_RECOVERY;
        }
    } else if (!state.data.torch.lit && !powered && !recently_burned(runtime, world->tick)) {
        action.state.data.torch.lit = true;
        action.changed = true;
        runtime->burned_until = 0;
    }
    return action;
}

bool redstone_torch_burned_out(const World *world, BlockPos position)
{
    if (!world || world_get(world, position).id != BLOCK_REDSTONE_TORCH) {
        return false;
    }
    TorchRuntime *runtime = sim_torch_runtime(world, position);
    return runtime && runtime->burned_until > world->tick;
}

const BlockDef rs_torch_def = {
    .name = "redstone_torch",
    .signal_source = true,
    .push_reaction = PUSH_DESTROY,
    .signal = torch_signal,
    .direct_signal = torch_direct,
    .neighbor_changed = torch_neighbor,
    .input_changed = torch_check_input,
    .scheduled_tick = torch_tick
};
