#include "mcmux.h"

static const BlockPos INPUT_A = {31, 4, 9};
static const BlockPos INPUT_B = {28, 4, 9};
static const BlockPos OUTPUT = {30, 3, 13};
static const BlockPos ANCHOR = {31, 4, 10};

static bool valid(const World *world, void *unused)
{
    (void)unused;

    BlockState a = world_get(world, INPUT_A);
    BlockState b = world_get(world, INPUT_B);
    BlockState output = world_get(world, OUTPUT);
    SolidPower output_power = redstone_solid_power(world, OUTPUT);

    bool a_high = a.id == BLOCK_LEVER && a.data.lever.powered;
    bool b_high = b.id == BLOCK_LEVER && b.data.lever.powered;

    uint8_t expected_power = a_high != b_high ? 15u : 0u;

    return a.id == BLOCK_LEVER &&
           b.id == BLOCK_LEVER &&
           output.id == BLOCK_IRON &&
           output_power.received == expected_power;
}

int main(void)
{
    World *world = load_mcs("saves/examples/xor_gate1.mcs", ANCHOR);
    
    sim_set_check(world, (SimCheckConfig){
        .check = valid, 
        .after_ticks = 2,
        .message = "Error:XOR output does not match its lever inputs"
    });
    
    sim_advance_ticks(world, 2);
    
    RsResult result = world_result(world);
    world_destroy(world);
    return result;
}