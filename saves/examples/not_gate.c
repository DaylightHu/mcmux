#include "mcmux.h"

static const BlockPos INPUT = {41, 4, 9};
static const BlockPos OUTPUT = {41, 6, 12};
static const BlockPos ANCHOR = {41, 4, 9};

typedef struct {
    uint64_t input_tick;
} NotCheckContext;

static bool valid(const World *world, void *context)
{
    const NotCheckContext *check = context;

    BlockState input = world_get(world, INPUT);
    BlockState output = world_get(world, OUTPUT);
    SolidPower output_power = redstone_solid_power(world, OUTPUT);

    uint64_t elapsed = world_tick(world) - check->input_tick;
    uint8_t expected_power = elapsed < 2 ? 15u : 0u;

    return input.id == BLOCK_LEVER &&
           input.data.lever.powered &&
           output.id == BLOCK_IRON &&
           output_power.received == expected_power;
}

int main(void)
{
    World *world = load_mcs("saves/examples/not_gate.mcs", ANCHOR);

    BlockState input = world_get(world, INPUT);
    BlockState output = world_get(world, OUTPUT);
    SolidPower output_power = redstone_solid_power(world, OUTPUT);

    world_expect(world,
                 input.id == BLOCK_LEVER &&
                 !input.data.lever.powered,
                 "NOT input must initially be off");

    world_expect(world,
                 output.id == BLOCK_IRON &&
                 output_power.received == 15u,
                 "NOT output must initially be powered");

    NotCheckContext check = {
        .input_tick = world_tick(world),
    };

    sim_set_check(world, (SimCheckConfig){
        .check = valid,
        .after_ticks = 0,
        .context = &check,
        .message = "NOT output changed at the wrong game tick",
    });

    world_set_lever_powered(world, INPUT, true);
    sim_advance_ticks(world, 2);

    writeback_mcs(world,
                  "saves/examples/not_gate.mcs",
                  ANCHOR,
                  "saves/examples/examples.mcw");

    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}