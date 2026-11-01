#include "flexible_placement.h"

#include <math.h>

static Direction clockwise(Direction direction)
{
    switch (direction) {
    case DIR_NORTH: return DIR_EAST;
    case DIR_EAST: return DIR_SOUTH;
    case DIR_SOUTH: return DIR_WEST;
    case DIR_WEST: return DIR_NORTH;
    default: return DIR_COUNT;
    }
}

static double component(PickVec3 point, Direction direction)
{
    BlockPos axis = block_offset((BlockPos){0}, direction);
    return point.x*axis.x + point.y*axis.y + point.z*axis.z;
}

PlacementTarget flexible_placement_target(VoxelHit hit, BlockId block,
    Direction horizontal, Direction looking, bool offset, bool rotate)
{
    PlacementTarget target = {.placement = DIR_COUNT, .right = DIR_COUNT,
        .up = DIR_COUNT, .direction = DIR_COUNT};
    if (!hit.hit || !hit.has_face || (unsigned)hit.face >= DIR_COUNT ||
        !direction_horizontal(horizontal) || (unsigned)looking >= DIR_COUNT ||
        block <= BLOCK_AIR || block >= BLOCK_INTERNAL_FIRST ||
        !isfinite(hit.local.x) || !isfinite(hit.local.y) || !isfinite(hit.local.z))
        return target;

    if (direction_horizontal(hit.face)) {
        target.right = direction_opposite(clockwise(hit.face));
        target.up = DIR_UP;
    } else {
        target.right = clockwise(horizontal);
        target.up = hit.face == DIR_UP ? horizontal : direction_opposite(horizontal);
    }
    double u = component(hit.local, target.right);
    double v = component(hit.local, target.up);
    target.region = PLACEMENT_CENTER;
    target.direction = hit.face;
    if (fabs(u) > 0.25 || fabs(v) > 0.25) {
        if (fabs(u) > fabs(v)) {
            target.region = u < 0 ? PLACEMENT_LEFT : PLACEMENT_RIGHT;
            target.direction = u < 0 ? direction_opposite(target.right) : target.right;
        } else {
            target.region = v < 0 ? PLACEMENT_BOTTOM : PLACEMENT_TOP;
            target.direction = v < 0 ? direction_opposite(target.up) : target.up;
        }
    }
    target.pos = block_offset(hit.pos, hit.face);
    if (offset) target.pos = block_offset(target.pos, target.direction);
    /* Center rotation points inward; its placement offset still points out. */
    if (target.region == PLACEMENT_CENTER)
        target.direction = direction_opposite(hit.face);

    bool horizontal_block = block == BLOCK_REDSTONE_REPEATER ||
        block == BLOCK_REDSTONE_COMPARATOR || block == BLOCK_POWERED_RAIL ||
        block == BLOCK_ACTIVATOR_RAIL;
    bool six_axis = block == BLOCK_OBSERVER || block == BLOCK_BARREL ||
        block == BLOCK_DROPPER || block_is_piston_base(block);
    target.directional = horizontal_block || six_axis ||
        block == BLOCK_REDSTONE_TORCH || block == BLOCK_LEVER ||
        block == BLOCK_HOPPER;
    target.placement = six_axis ? direction_opposite(looking) :
        horizontal_block ? horizontal : direction_opposite(hit.face);
    target.valid = true;
    if (rotate && target.directional) {
        target.placement = (block == BLOCK_REDSTONE_TORCH ||
                            block == BLOCK_LEVER) ?
            direction_opposite(target.direction) : target.direction;
        if ((horizontal_block && !direction_horizontal(target.placement)) ||
            (block == BLOCK_HOPPER && target.placement == DIR_UP) ||
            (block == BLOCK_REDSTONE_TORCH && target.placement == DIR_UP))
            target.valid = false;
    }
    return target;
}
