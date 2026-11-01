#include "storage.h"
#include <stdlib.h>
#include <string.h>

enum {
    STATE_GLASS = 13,
    STATE_HONEY = 14,
    STATE_SLIME = 15,
    STATE_REPEATER_FIRST = 8208,
    STATE_REPEATER_COUNT = 64,
    STATE_COMPARATOR_FIRST = STATE_REPEATER_FIRST + STATE_REPEATER_COUNT,
    STATE_COMPARATOR_COUNT = 256,
    STATE_POWERED_RAIL_FIRST = STATE_COMPARATOR_FIRST + STATE_COMPARATOR_COUNT,
    STATE_RAIL_COUNT = 16,
    STATE_ACTIVATOR_RAIL_FIRST = STATE_POWERED_RAIL_FIRST + STATE_RAIL_COUNT,
    STATE_OBSERVER_FIRST = STATE_ACTIVATOR_RAIL_FIRST + STATE_RAIL_COUNT,
    STATE_OBSERVER_COUNT = DIR_COUNT * 2,
    STATE_PISTON_FIRST = STATE_OBSERVER_FIRST + STATE_OBSERVER_COUNT,
    STATE_PISTON_COUNT = DIR_COUNT * 4,
    STATE_PISTON_HEAD_FIRST = STATE_PISTON_FIRST + STATE_PISTON_COUNT,
    STATE_PISTON_HEAD_COUNT = DIR_COUNT * 2,
    STATE_STICKY_PISTON_FIRST =
        STATE_PISTON_HEAD_FIRST + STATE_PISTON_HEAD_COUNT,
    STATE_STICKY_PISTON_COUNT = DIR_COUNT * 4,
    STATE_STICKY_PISTON_HEAD_FIRST =
        STATE_STICKY_PISTON_FIRST + STATE_STICKY_PISTON_COUNT,
    STATE_STICKY_PISTON_HEAD_COUNT = DIR_COUNT * 2,
    STATE_BARREL_FIRST =
        STATE_STICKY_PISTON_HEAD_FIRST + STATE_STICKY_PISTON_HEAD_COUNT,
    STATE_BARREL_COUNT = DIR_COUNT * 16 * 2,
    STATE_NOTE_BLOCK_FIRST = STATE_BARREL_FIRST + STATE_BARREL_COUNT,
    STATE_NOTE_BLOCK_COUNT = NOTE_COUNT * NOTE_INSTRUMENT_COUNT * 2,
    STATE_TARGET = STATE_NOTE_BLOCK_FIRST + STATE_NOTE_BLOCK_COUNT,
    /* Keep the former target-state range reserved in v1 palettes. */
    STATE_HOPPER_FIRST = STATE_TARGET + 16,
    STATE_HOPPER_COUNT = 5 * 2 * 16,
    STATE_DROPPER_FIRST = STATE_HOPPER_FIRST + STATE_HOPPER_COUNT,
    STATE_DROPPER_COUNT = DIR_COUNT * 2 * 16,
    STATE_LAMP_FIRST = STATE_DROPPER_FIRST + STATE_DROPPER_COUNT,
    STATE_LAMP_COUNT = 2,
    STATE_MOVING_PISTON_FIRST = STATE_LAMP_FIRST + STATE_LAMP_COUNT,
    STATE_MOVING_PISTON_COUNT = DIR_COUNT * 2,
    STATE_LEVER_FIRST =
        STATE_MOVING_PISTON_FIRST + STATE_MOVING_PISTON_COUNT,
    STATE_LEVER_COUNT = 3 * 4 * 2
};

_Static_assert(STATE_LEVER_FIRST + STATE_LEVER_COUNT <=
               STATE_ID_LIMIT,
               "block states must fit the palette state id");

int floor_div16(int x) { int q = x / 16; return q - (x % 16 < 0); }
int floor_mod16(int x) { int r = x % 16; return r < 0 ? r + 16 : r; }
size_t section_index(BlockPos p)
{
    return (size_t)floor_mod16(p.y) * 256 + (size_t)floor_mod16(p.z) * 16 +
           (size_t)floor_mod16(p.x);
}
static unsigned compact_faces(unsigned value)
{
    return (value & 3u) | ((value >> 2) & 12u);
}
static uint8_t expand_faces(unsigned value)
{
    return (uint8_t)((value & 3u) | ((value & 12u) << 2));
}
static bool horizontal_index(unsigned face, unsigned *index)
{
    switch ((Direction)face) {
    case DIR_EAST:  *index = 0; return true;
    case DIR_WEST:  *index = 1; return true;
    case DIR_SOUTH: *index = 2; return true;
    case DIR_NORTH: *index = 3; return true;
    default: return false;
    }
}
static Direction horizontal_face(unsigned index)
{
    static const Direction faces[] = {
        DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH
    };
    return faces[index & 3u];
}
bool state_encode(BlockState s, StateId *id)
{
    if (s.id <= BLOCK_REDSTONE_BLOCK) { *id = s.id; return true; }
    if (s.id == BLOCK_GLASS) { *id = STATE_GLASS; return true; }
    if (s.id == BLOCK_HONEY) { *id = STATE_HONEY; return true; }
    if (s.id == BLOCK_SLIME) { *id = STATE_SLIME; return true; }
    if (s.id == BLOCK_REDSTONE_TORCH) {
        unsigned face = s.data.torch.support_dir;
        if (face == DIR_DOWN) face = 4;
        else if (face == DIR_SOUTH) face = 2;
        else if (face == DIR_NORTH) face = 3;
        else if (face > DIR_WEST) return false;
        *id = (StateId)(3 + face * 2 + s.data.torch.lit); return true;
    }
    if (s.id == BLOCK_REDSTONE_REPEATER) {
        unsigned facing;
        if (!horizontal_index(s.data.repeater.facing, &facing) ||
            s.data.repeater.delay < 1 || s.data.repeater.delay > 4) return false;
        *id = (StateId)(STATE_REPEATER_FIRST + facing +
              (unsigned)(s.data.repeater.delay - 1)*4u +
              (s.data.repeater.powered ? 16u : 0u) +
              (s.data.repeater.locked ? 32u : 0u));
        return true;
    }
    if (s.id == BLOCK_REDSTONE_COMPARATOR) {
        unsigned facing;
        if (!horizontal_index(s.data.comparator.facing, &facing) ||
            s.data.comparator.output > 15) return false;
        *id = (StateId)(STATE_COMPARATOR_FIRST + facing +
              (unsigned)s.data.comparator.output*4u +
              (s.data.comparator.subtract ? 64u : 0u) +
              (s.data.comparator.powered ? 128u : 0u));
        return true;
    }
    if (s.id == BLOCK_POWERED_RAIL || s.id == BLOCK_ACTIVATOR_RAIL) {
        unsigned facing;
        if (!horizontal_index(s.data.rail.facing, &facing)) return false;
        StateId first = s.id == BLOCK_POWERED_RAIL ?
                        STATE_POWERED_RAIL_FIRST : STATE_ACTIVATOR_RAIL_FIRST;
        *id = (StateId)(first + facing +
              (s.data.rail.ascending ? 4u : 0u) +
              (s.data.rail.powered ? 8u : 0u));
        return true;
    }
    if (s.id == BLOCK_OBSERVER) {
        if (s.data.observer.facing >= DIR_COUNT) return false;
        *id = (StateId)(STATE_OBSERVER_FIRST + s.data.observer.facing +
              (s.data.observer.powered ? DIR_COUNT : 0));
        return true;
    }
    if (block_is_piston_base((BlockId)s.id)) {
        if (s.data.piston.facing >= DIR_COUNT) return false;
        StateId first = s.id == BLOCK_STICKY_PISTON ?
                        STATE_STICKY_PISTON_FIRST : STATE_PISTON_FIRST;
        *id = (StateId)(first + s.data.piston.facing +
              (s.data.piston.powered ? DIR_COUNT : 0) +
              (s.data.piston.extended ? DIR_COUNT*2 : 0));
        return true;
    }
    if (block_is_piston_head((BlockId)s.id)) {
        if (s.data.piston.facing >= DIR_COUNT || s.data.piston.extended)
            return false;
        StateId first = s.id == BLOCK_STICKY_PISTON_HEAD ?
                        STATE_STICKY_PISTON_HEAD_FIRST :
                        STATE_PISTON_HEAD_FIRST;
        *id = (StateId)(first + s.data.piston.facing +
              (s.data.piston.powered ? DIR_COUNT : 0));
        return true;
    }
    if (s.id == BLOCK_BARREL) {
        if (s.data.barrel.facing >= DIR_COUNT ||
            s.data.barrel.level > 15) return false;
        *id = (StateId)(STATE_BARREL_FIRST + s.data.barrel.facing +
              (unsigned)s.data.barrel.level*DIR_COUNT +
              (s.data.barrel.open ? DIR_COUNT*16 : 0));
        return true;
    }
    if (s.id == BLOCK_NOTE_BLOCK) {
        if (s.data.note_block.note >= NOTE_COUNT ||
            s.data.note_block.instrument >= NOTE_INSTRUMENT_COUNT) return false;
        *id = (StateId)(STATE_NOTE_BLOCK_FIRST + s.data.note_block.note +
              s.data.note_block.instrument*NOTE_COUNT +
              (s.data.note_block.powered ? NOTE_COUNT*NOTE_INSTRUMENT_COUNT : 0));
        return true;
    }
    if (s.id == BLOCK_TARGET) {
        *id = STATE_TARGET;
        return true;
    }
    if (s.id == BLOCK_HOPPER) {
        static const Direction faces[] = {DIR_DOWN, DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH};
        if (s.data.hopper.level > 15) return false;
        for (unsigned i = 0; i < 5; ++i) if (s.data.hopper.facing == faces[i]) {
            *id = (StateId)(STATE_HOPPER_FIRST + i +
                  (s.data.hopper.enabled ? 5u : 0u) + s.data.hopper.level * 10u);
            return true;
        }
        return false;
    }
    if (s.id == BLOCK_DROPPER) {
        if (s.data.dropper.facing >= DIR_COUNT || s.data.dropper.level > 15)
            return false;
        *id = (StateId)(STATE_DROPPER_FIRST + s.data.dropper.facing +
              (s.data.dropper.triggered ? DIR_COUNT : 0) +
              s.data.dropper.level * (DIR_COUNT * 2));
        return true;
    }
    if (s.id == BLOCK_REDSTONE_LAMP) {
        *id = (StateId)(STATE_LAMP_FIRST + (s.data.lamp.lit ? 1u : 0u));
        return true;
    }
    if (s.id == BLOCK_LEVER) {
        unsigned facing, face;
        if (!horizontal_index(s.data.lever.facing, &facing)) return false;
        Direction support = (Direction)s.data.lever.support_dir;
        if (support == DIR_DOWN) face = 0;
        else if (direction_horizontal(support) &&
                 support == direction_opposite((Direction)s.data.lever.facing))
            face = 1;
        else if (support == DIR_UP) face = 2;
        else return false;
        *id = (StateId)(STATE_LEVER_FIRST + facing + face*4u +
              (s.data.lever.powered ? 12u : 0u));
        return true;
    }
    if (s.id == BLOCK_MOVING_PISTON) {
        if (s.data.moving_piston.facing >= DIR_COUNT) return false;
        *id = (StateId)(STATE_MOVING_PISTON_FIRST +
              s.data.moving_piston.facing +
              (s.data.moving_piston.sticky ? DIR_COUNT : 0));
        return true;
    }
    if (s.id != BLOCK_REDSTONE_WIRE || s.data.wire.power > 15 ||
        (s.data.wire.connections & ~0x33u) ||
        (s.data.wire.up_connections & ~s.data.wire.connections)) return false;
    *id = (StateId)(16 + s.data.wire.power + (compact_faces(s.data.wire.connections) << 4) +
         (compact_faces(s.data.wire.up_connections) << 8) + (s.data.wire.prefer_dot ? 4096 : 0));
    return true;
}
bool state_decode(StateId id, BlockState *s)
{
    if (id <= 2) { *s = block_default((BlockId)id); return true; }
    if (id >= 3 && id <= 12) {
        static const Direction faces[] = {DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH, DIR_DOWN};
        *s = block_default(BLOCK_REDSTONE_TORCH);
        s->data.torch.support_dir = (uint8_t)faces[(id - 3) / 2];
        s->data.torch.lit = ((id - 3) & 1u) != 0; return true;
    }
    if (id == STATE_GLASS) { *s = block_default(BLOCK_GLASS); return true; }
    if (id == STATE_HONEY) { *s = block_default(BLOCK_HONEY); return true; }
    if (id == STATE_SLIME) { *s = block_default(BLOCK_SLIME); return true; }
    if (id >= STATE_REPEATER_FIRST &&
        id < STATE_REPEATER_FIRST + STATE_REPEATER_COUNT) {
        unsigned value = id - STATE_REPEATER_FIRST;
        *s = block_default(BLOCK_REDSTONE_REPEATER);
        s->data.repeater.facing = (uint8_t)horizontal_face(value);
        s->data.repeater.delay = (uint8_t)(((value >> 2) & 3u) + 1u);
        s->data.repeater.powered = (value & 16u) != 0;
        s->data.repeater.locked = (value & 32u) != 0;
        return true;
    }
    if (id >= STATE_COMPARATOR_FIRST &&
        id < STATE_COMPARATOR_FIRST + STATE_COMPARATOR_COUNT) {
        unsigned value = id - STATE_COMPARATOR_FIRST;
        *s = block_default(BLOCK_REDSTONE_COMPARATOR);
        s->data.comparator.facing = (uint8_t)horizontal_face(value);
        s->data.comparator.output = (uint8_t)((value >> 2) & 15u);
        s->data.comparator.subtract = (value & 64u) != 0;
        s->data.comparator.powered = (value & 128u) != 0;
        return true;
    }
    StateId rail_first;
    BlockId rail_id;
    if (id >= STATE_POWERED_RAIL_FIRST &&
        id < STATE_POWERED_RAIL_FIRST + STATE_RAIL_COUNT) {
        rail_first = STATE_POWERED_RAIL_FIRST;
        rail_id = BLOCK_POWERED_RAIL;
    } else if (id >= STATE_ACTIVATOR_RAIL_FIRST &&
               id < STATE_ACTIVATOR_RAIL_FIRST + STATE_RAIL_COUNT) {
        rail_first = STATE_ACTIVATOR_RAIL_FIRST;
        rail_id = BLOCK_ACTIVATOR_RAIL;
    } else {
        rail_first = 0;
        rail_id = BLOCK_AIR;
    }
    if (rail_id != BLOCK_AIR) {
        unsigned value = id - rail_first;
        *s = block_default(rail_id);
        s->data.rail.facing = (uint8_t)horizontal_face(value);
        s->data.rail.ascending = (value & 4u) != 0;
        s->data.rail.powered = (value & 8u) != 0;
        return true;
    }
    if (id >= STATE_OBSERVER_FIRST &&
        id < STATE_OBSERVER_FIRST + STATE_OBSERVER_COUNT) {
        unsigned value = id - STATE_OBSERVER_FIRST;
        *s = block_default(BLOCK_OBSERVER);
        s->data.observer.facing = (uint8_t)(value % DIR_COUNT);
        s->data.observer.powered = value >= DIR_COUNT;
        return true;
    }
    if (id >= STATE_PISTON_FIRST &&
        id < STATE_PISTON_FIRST + STATE_PISTON_COUNT) {
        unsigned value = id - STATE_PISTON_FIRST;
        *s = block_default(BLOCK_PISTON);
        s->data.piston.facing = (uint8_t)(value % DIR_COUNT);
        s->data.piston.powered = ((value / DIR_COUNT) & 1u) != 0;
        s->data.piston.extended = value >= DIR_COUNT*2;
        return true;
    }
    if (id >= STATE_PISTON_HEAD_FIRST &&
        id < STATE_PISTON_HEAD_FIRST + STATE_PISTON_HEAD_COUNT) {
        unsigned value = id - STATE_PISTON_HEAD_FIRST;
        *s = block_default(BLOCK_PISTON_HEAD);
        s->data.piston.facing = (uint8_t)(value % DIR_COUNT);
        s->data.piston.powered = value >= DIR_COUNT;
        return true;
    }
    if (id >= STATE_STICKY_PISTON_FIRST &&
        id < STATE_STICKY_PISTON_FIRST + STATE_STICKY_PISTON_COUNT) {
        unsigned value = id - STATE_STICKY_PISTON_FIRST;
        *s = block_default(BLOCK_STICKY_PISTON);
        s->data.piston.facing = (uint8_t)(value % DIR_COUNT);
        s->data.piston.powered = ((value / DIR_COUNT) & 1u) != 0;
        s->data.piston.extended = value >= DIR_COUNT*2;
        return true;
    }
    if (id >= STATE_STICKY_PISTON_HEAD_FIRST &&
        id < STATE_STICKY_PISTON_HEAD_FIRST +
             STATE_STICKY_PISTON_HEAD_COUNT) {
        unsigned value = id - STATE_STICKY_PISTON_HEAD_FIRST;
        *s = block_default(BLOCK_STICKY_PISTON_HEAD);
        s->data.piston.facing = (uint8_t)(value % DIR_COUNT);
        s->data.piston.powered = value >= DIR_COUNT;
        return true;
    }
    if (id >= STATE_BARREL_FIRST &&
        id < STATE_BARREL_FIRST + STATE_BARREL_COUNT) {
        unsigned value = id - STATE_BARREL_FIRST;
        *s = block_default(BLOCK_BARREL);
        s->data.barrel.facing = (uint8_t)(value % DIR_COUNT);
        s->data.barrel.level = (uint8_t)((value / DIR_COUNT) % 16);
        s->data.barrel.open = value >= DIR_COUNT*16;
        return true;
    }
    if (id >= STATE_NOTE_BLOCK_FIRST &&
        id < STATE_NOTE_BLOCK_FIRST + STATE_NOTE_BLOCK_COUNT) {
        unsigned value = id - STATE_NOTE_BLOCK_FIRST;
        *s = block_default(BLOCK_NOTE_BLOCK);
        s->data.note_block.note = (uint8_t)(value % NOTE_COUNT);
        s->data.note_block.instrument = (uint8_t)((value / NOTE_COUNT) % NOTE_INSTRUMENT_COUNT);
        s->data.note_block.powered = value >= NOTE_COUNT*NOTE_INSTRUMENT_COUNT;
        return true;
    }
    if (id == STATE_TARGET) {
        *s = block_default(BLOCK_TARGET);
        return true;
    }
    if (id >= STATE_HOPPER_FIRST && id < STATE_HOPPER_FIRST + STATE_HOPPER_COUNT) {
        static const Direction faces[] = {DIR_DOWN, DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH};
        unsigned value = id - STATE_HOPPER_FIRST;
        *s = block_default(BLOCK_HOPPER);
        s->data.hopper.facing = (uint8_t)faces[value % 5];
        s->data.hopper.enabled = (value % 10) >= 5;
        s->data.hopper.level = (uint8_t)(value / 10);
        return true;
    }
    if (id >= STATE_DROPPER_FIRST && id < STATE_DROPPER_FIRST + STATE_DROPPER_COUNT) {
        unsigned value = id - STATE_DROPPER_FIRST;
        *s = block_default(BLOCK_DROPPER);
        s->data.dropper.facing = (uint8_t)(value % DIR_COUNT);
        s->data.dropper.triggered = (value % (DIR_COUNT * 2)) >= DIR_COUNT;
        s->data.dropper.level = (uint8_t)(value / (DIR_COUNT * 2));
        return true;
    }
    if (id >= STATE_LAMP_FIRST && id < STATE_LAMP_FIRST + STATE_LAMP_COUNT) {
        *s = block_default(BLOCK_REDSTONE_LAMP);
        s->data.lamp.lit = id != STATE_LAMP_FIRST;
        return true;
    }
    if (id >= STATE_MOVING_PISTON_FIRST &&
        id < STATE_MOVING_PISTON_FIRST + STATE_MOVING_PISTON_COUNT) {
        unsigned value = id - STATE_MOVING_PISTON_FIRST;
        *s = block_default(BLOCK_MOVING_PISTON);
        s->data.moving_piston.facing = (uint8_t)(value % DIR_COUNT);
        s->data.moving_piston.sticky = value >= DIR_COUNT;
        return true;
    }
    if (id >= STATE_LEVER_FIRST &&
        id < STATE_LEVER_FIRST + STATE_LEVER_COUNT) {
        unsigned value = id - STATE_LEVER_FIRST;
        unsigned orientation = value % 12u;
        unsigned face = orientation / 4u;
        Direction facing = horizontal_face(orientation % 4u);
        *s = block_default(BLOCK_LEVER);
        s->data.lever.facing = (uint8_t)facing;
        s->data.lever.support_dir = (uint8_t)(
            face == 0 ? DIR_DOWN :
            face == 1 ? direction_opposite(facing) : DIR_UP);
        s->data.lever.powered = value >= 12u;
        return true;
    }
    if (id < 16 || id > 8207) return false;
    unsigned v = id - 16;
    unsigned connections = (v >> 4) & 15u, up = (v >> 8) & 15u;
    if (up & ~connections) return false;
    *s = block_default(BLOCK_REDSTONE_WIRE);
    s->data.wire.power = (uint8_t)(v & 15u);
    s->data.wire.connections = expand_faces(connections);
    s->data.wire.up_connections = expand_faces(up);
    s->data.wire.prefer_dot = (v & 4096u) != 0; return true;
}
/* Like MCHPRS, entries never straddle a 64-bit word. */
size_t palette_word_count(unsigned bits)
{
    size_t per_word = 64 / bits;
    return (SECTION_VOLUME + per_word - 1) / per_word;
}
static unsigned entry(const PalettedBitBuffer *b, size_t index)
{
    size_t per_word = 64 / b->bits;
    unsigned shift = (unsigned)(index % per_word) * b->bits;
    return (unsigned)((b->data[index / per_word] >> shift) & ((1u << b->bits) - 1));
}
static void put(PalettedBitBuffer *b, size_t index, unsigned value)
{
    size_t per_word = 64 / b->bits, word = index / per_word;
    unsigned shift = (unsigned)(index % per_word) * b->bits;
    uint64_t mask = ((UINT64_C(1) << b->bits) - 1) << shift;
    b->data[word] = (b->data[word] & ~mask) | ((uint64_t)value << shift);
}
bool palette_init(PalettedBitBuffer *b)
{
    *b = (PalettedBitBuffer){0};
    b->data = calloc(palette_word_count(4), sizeof(uint64_t));
    b->palette = calloc(16, sizeof(StateId));
    if (!b->data || !b->palette) { palette_destroy(b); return false; }
    b->bits = 4; b->palette_count = 1; return true;
}
void palette_destroy(PalettedBitBuffer *b) { free(b->data); free(b->palette); *b = (PalettedBitBuffer){0}; }
StateId palette_get(const PalettedBitBuffer *b, size_t i)
{
    unsigned v = entry(b, i); return b->bits == 15 ? (StateId)v : b->palette[v];
}
bool palette_set(PalettedBitBuffer *b, size_t i, StateId state)
{
    if (i >= SECTION_VOLUME || state >= STATE_ID_LIMIT) return false;
    if (b->bits == 15) { put(b, i, state); return true; }
    size_t p = 0;
    while (p < b->palette_count && b->palette[p] != state) ++p;
    if (p == b->palette_count && p == (size_t)(1u << b->bits)) {
        /* Allocate the complete replacement before changing the live buffer. */
        unsigned bits = b->bits == 8 ? 15 : b->bits + 1;
        PalettedBitBuffer next = {0}; next.bits = (uint8_t)bits;
        next.data = calloc(palette_word_count(bits), sizeof(uint64_t));
        if (bits != 15) next.palette = malloc((size_t)(1u << bits) * sizeof(StateId));
        if (!next.data || (bits != 15 && !next.palette)) { palette_destroy(&next); return false; }
        if (bits != 15) {
            memcpy(next.palette, b->palette, p * sizeof(StateId)); next.palette_count = p;
        }
        for (size_t j = 0; j < SECTION_VOLUME; ++j)
            put(&next, j, bits == 15 ? palette_get(b, j) : entry(b, j));
        palette_destroy(b); *b = next;
        if (bits == 15) { put(b, i, state); return true; }
    }
    if (p == b->palette_count) b->palette[b->palette_count++] = state;
    put(b, i, (unsigned)p); return true;
}
ChunkSection *section_create(void)
{
    ChunkSection *s = calloc(1, sizeof(*s));
    if (s && !palette_init(&s->blocks)) { free(s); return NULL; }
    return s;
}
void section_destroy(ChunkSection *s)
{
    if (!s) return;
    palette_destroy(&s->blocks); free(s->changed_blocks); free(s);
}
StateId section_get(const ChunkSection *s, size_t i)
{
    if (!s) return 0;
    return s->changed_blocks && s->changed_blocks[i] != UINT16_MAX ?
           s->changed_blocks[i] : palette_get(&s->blocks, i);
}
bool section_set(ChunkSection *s, size_t i, StateId state)
{
    if (i >= SECTION_VOLUME || state >= STATE_ID_LIMIT) return false;
    StateId old = section_get(s, i);
    if (old == state) return true;
    if (!s->changed_blocks) {
        s->changed_blocks = malloc(SECTION_VOLUME * sizeof(StateId));
        if (!s->changed_blocks) return false;
        for (size_t j = 0; j < SECTION_VOLUME; ++j) s->changed_blocks[j] = UINT16_MAX;
    }
    s->changed_blocks[i] = state;
    if (!old) ++s->block_count;
    if (!state) --s->block_count;
    return true;
}
bool section_compact(ChunkSection *s)
{
    if (!s->changed_blocks) return true;
    PalettedBitBuffer next;
    if (!palette_init(&next)) return false;
    for (size_t i = 0; i < SECTION_VOLUME; ++i) if (!palette_set(&next, i, section_get(s, i))) {
        palette_destroy(&next); return false;
    }
    palette_destroy(&s->blocks); s->blocks = next;
    free(s->changed_blocks); s->changed_blocks = NULL; return true;
}
