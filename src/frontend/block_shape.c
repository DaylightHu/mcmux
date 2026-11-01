#include "block_shape.h"
#include "hopper_shape.h"

#include <string.h>

static void box(BlockShape *shape, float x0, float y0, float z0,
                 float x1, float y1, float z1)
{
    shape->boxes[shape->count++] = (BlockShapeBox){
        {x0, y0, z0}, {x1, y1, z1}
    };
}

void block_outline_shape(BlockState state, BlockShape *shape)
{
    *shape = (BlockShape){0};
    switch ((BlockId)state.id) {
    case BLOCK_AIR: return;
    case BLOCK_MOVING_PISTON: return;
    case BLOCK_REDSTONE_WIRE: {
        box(shape, 3, 0, 3, 13, 1, 13);
        static const Direction directions[] = {
            DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH
        };
        static const BlockShapeBox arms[] = {
            {{13,0,3}, {16,1,13}}, {{0,0,3}, {3,1,13}},
            {{3,0,13}, {13,1,16}}, {{3,0,0}, {13,1,3}}
        };
        static const BlockShapeBox sides[] = {
            {{15,0,3}, {16,16,13}}, {{0,0,3}, {1,16,13}},
            {{3,0,15}, {13,16,16}}, {{3,0,0}, {13,16,1}}
        };
        for (unsigned i = 0; i < 4; ++i) {
            unsigned bit = 1u << directions[i];
            if (!(state.data.wire.connections & bit)) continue;
            shape->boxes[shape->count++] = arms[i];
            if (state.data.wire.up_connections & bit)
                shape->boxes[shape->count++] = sides[i];
        }
        return;
    }
    case BLOCK_REDSTONE_REPEATER:
    case BLOCK_REDSTONE_COMPARATOR:
        box(shape, 0, 0, 0, 16, 2, 16);
        return;
    case BLOCK_LEVER: {
        Direction support = (Direction)state.data.lever.support_dir;
        Direction facing = (Direction)state.data.lever.facing;
        bool x_axis = facing == DIR_EAST || facing == DIR_WEST;
        if (support == DIR_DOWN) {
            if (x_axis) box(shape, 4,0,5, 12,6,11);
            else box(shape, 5,0,4, 11,6,12);
        } else if (support == DIR_UP) {
            if (x_axis) box(shape, 4,10,5, 12,16,11);
            else box(shape, 5,10,4, 11,16,12);
        } else if (support == DIR_EAST) {
            box(shape, 10,4,5, 16,12,11);
        } else if (support == DIR_WEST) {
            box(shape, 0,4,5, 6,12,11);
        } else if (support == DIR_SOUTH) {
            box(shape, 5,4,10, 11,12,16);
        } else {
            box(shape, 5,4,0, 11,12,6);
        }
        return;
    }
    case BLOCK_POWERED_RAIL:
    case BLOCK_ACTIVATOR_RAIL:
        box(shape, 0, 0, 0, 16, state.data.rail.ascending ? 8 : 2, 16);
        return;
    case BLOCK_REDSTONE_TORCH:
        switch ((Direction)state.data.torch.support_dir) {
        case DIR_EAST:  box(shape, 11,3,5.5f, 16,13,10.5f); break;
        case DIR_WEST:  box(shape, 0,3,5.5f, 5,13,10.5f); break;
        case DIR_SOUTH: box(shape, 5.5f,3,11, 10.5f,13,16); break;
        case DIR_NORTH: box(shape, 5.5f,3,0, 10.5f,13,5); break;
        default:       box(shape, 6,0,6, 10,10,10); break;
        }
        return;
    case BLOCK_HOPPER: {
        HopperBox boxes[HOPPER_BOX_COUNT];
        hopper_shape((Direction)state.data.hopper.facing, boxes);
        for (unsigned i = 0; i < HOPPER_BOX_COUNT; ++i)
            box(shape, boxes[i].minimum[0], boxes[i].minimum[1], boxes[i].minimum[2],
                        boxes[i].maximum[0], boxes[i].maximum[1], boxes[i].maximum[2]);
        return;
    }
    default: break;
    }

    box(shape, 0, 0, 0, 16, 16, 16);
    unsigned facing = state.data.piston.facing;
    if (facing >= DIR_COUNT) return;
    unsigned axis = facing / 2;
    bool positive = (facing & 1u) == 0;
    if (block_is_piston_base((BlockId)state.id) && state.data.piston.extended) {
        if (positive) shape->boxes[0].maximum[axis] = 12;
        else shape->boxes[0].minimum[axis] = 4;
    } else if (block_is_piston_head((BlockId)state.id)) {
        if (positive) shape->boxes[0].minimum[axis] = 12;
        else shape->boxes[0].maximum[axis] = 4;
        box(shape, 6, 6, 6, 10, 10, 10);
        shape->boxes[1].minimum[axis] = positive ? -4 : 4;
        shape->boxes[1].maximum[axis] = positive ? 12 : 20;
    }
}

void block_interaction_shape(BlockState state, BlockShape *shape)
{
    *shape = (BlockShape){0};
    if (state.id != BLOCK_HOPPER) return;
    /* HopperBlock.getInteractionShape can override the hit face, but never
     * turns a miss through the open bowl into a hit. */
    box(shape, 2, 11, 2, 14, 16, 14);
    switch ((Direction)state.data.hopper.facing) {
    case DIR_EAST:  box(shape, 12,8,6, 16,10,10); break;
    case DIR_WEST:  box(shape, 0,8,6, 4,10,10); break;
    case DIR_SOUTH: box(shape, 6,8,12, 10,10,16); break;
    case DIR_NORTH: box(shape, 6,8,0, 10,10,4); break;
    default: break;
    }
}

enum { GRID_COORDS = BLOCK_SHAPE_BOXES * 2, GRID_CELLS = GRID_COORDS - 1 };

static void coordinate(float values[GRID_COORDS], unsigned *count, float value)
{
    unsigned i = 0;
    while (i < *count && values[i] < value) ++i;
    if (i < *count && values[i] == value) return;
    memmove(values + i + 1, values + i, (*count - i)*sizeof(*values));
    values[i] = value;
    ++*count;
}

static unsigned coordinate_index(const float *values, float value)
{
    unsigned i = 0;
    while (values[i] != value) ++i;
    return i;
}

static unsigned cell_index(const unsigned cells[3], const unsigned size[3])
{
    return (cells[0]*size[1] + cells[1])*size[2] + cells[2];
}

unsigned block_shape_edges(const BlockShape *shape,
                           BlockShapeEdge edges[BLOCK_SHAPE_EDGES])
{
    if (!shape->count || shape->count > BLOCK_SHAPE_BOXES) return 0;
    float positions[3][GRID_COORDS];
    unsigned count[3] = {0};
    for (unsigned b = 0; b < shape->count; ++b)
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (shape->boxes[b].minimum[axis] >= shape->boxes[b].maximum[axis])
                return 0;
            coordinate(positions[axis], &count[axis], shape->boxes[b].minimum[axis]);
            coordinate(positions[axis], &count[axis], shape->boxes[b].maximum[axis]);
        }
    unsigned size[3] = {count[0]-1, count[1]-1, count[2]-1};
    unsigned char occupied[GRID_CELLS*GRID_CELLS*GRID_CELLS] = {0};
    for (unsigned b = 0; b < shape->count; ++b) {
        unsigned low[3], high[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            low[axis] = coordinate_index(positions[axis], shape->boxes[b].minimum[axis]);
            high[axis] = coordinate_index(positions[axis], shape->boxes[b].maximum[axis]);
        }
        for (unsigned x = low[0]; x < high[0]; ++x)
            for (unsigned y = low[1]; y < high[1]; ++y)
                for (unsigned z = low[2]; z < high[2]; ++z) {
                    unsigned cells[3] = {x,y,z};
                    occupied[cell_index(cells, size)] = 1;
                }
    }

    unsigned output = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        unsigned u = (axis + 1)%3, v = (axis + 2)%3;
        for (unsigned j = 0; j < count[u]; ++j)
            for (unsigned k = 0; k < count[v]; ++k) {
                unsigned start = 0;
                bool running = false;
                for (unsigned i = 0; i <= size[axis]; ++i) {
                    unsigned mask = 0;
                    if (i < size[axis]) {
                        for (unsigned a = 0; a < 2; ++a)
                            for (unsigned b = 0; b < 2; ++b) {
                                if ((!a && !j) || (!b && !k)) continue;
                                unsigned cells[3];
                                cells[axis] = i;
                                cells[u] = j + a - 1;
                                cells[v] = k + b - 1;
                                if (cells[u] < size[u] && cells[v] < size[v] &&
                                    occupied[cell_index(cells, size)])
                                    mask |= 1u << (a + b*2);
                            }
                    }
                    /* Two adjacent occupied quadrants form a flat face;
                     * one, three or two diagonal quadrants form an edge. */
                    bool edge = mask && mask != 15 && mask != 3 && mask != 5 &&
                                mask != 10 && mask != 12;
                    if (edge && !running) { start = i; running = true; }
                    if (!edge && running) {
                        if (output == BLOCK_SHAPE_EDGES) return 0;
                        BlockShapeEdge *line = &edges[output++];
                        line->start[axis] = positions[axis][start];
                        line->end[axis] = positions[axis][i];
                        line->start[u] = line->end[u] = positions[u][j];
                        line->start[v] = line->end[v] = positions[v][k];
                        running = false;
                    }
                }
            }
    }
    return output;
}
