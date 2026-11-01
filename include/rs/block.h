#ifndef RS_BLOCK_H
#define RS_BLOCK_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { int x, y, z; } BlockPos;
typedef enum {
    BLOCK_AIR = 0, /* Empty storage cell, never a placeable block. */
    BLOCK_IRON, BLOCK_REDSTONE_BLOCK, BLOCK_REDSTONE_WIRE, BLOCK_REDSTONE_TORCH,
    BLOCK_GLASS, BLOCK_REDSTONE_REPEATER, BLOCK_REDSTONE_COMPARATOR,
    BLOCK_POWERED_RAIL, BLOCK_ACTIVATOR_RAIL, BLOCK_OBSERVER,
    BLOCK_PISTON, BLOCK_STICKY_PISTON, BLOCK_BARREL, BLOCK_SLIME,
    BLOCK_NOTE_BLOCK, BLOCK_HONEY, BLOCK_TARGET, BLOCK_HOPPER, BLOCK_DROPPER,
    BLOCK_REDSTONE_LAMP, BLOCK_LEVER,
    BLOCK_INTERNAL_FIRST,
    BLOCK_PISTON_HEAD = BLOCK_INTERNAL_FIRST,
    BLOCK_STICKY_PISTON_HEAD,
    BLOCK_MOVING_PISTON,
    BLOCK_COUNT
} BlockId;

/* All faces are FROM the emitting block TOWARD the receiving block.
 * Torch support_dir is FROM the torch TOWARD its supporting block. */
typedef enum {
    DIR_EAST = 0, DIR_WEST, DIR_UP, DIR_DOWN, DIR_SOUTH, DIR_NORTH, DIR_COUNT
} Direction;

typedef enum {
    NOTE_HARP, NOTE_BASEDRUM, NOTE_SNARE, NOTE_HAT, NOTE_BASS,
    NOTE_FLUTE, NOTE_BELL, NOTE_GUITAR, NOTE_CHIME, NOTE_XYLOPHONE,
    NOTE_IRON_XYLOPHONE, NOTE_COW_BELL, NOTE_DIDGERIDOO, NOTE_BIT,
    NOTE_BANJO, NOTE_PLING, NOTE_INSTRUMENT_COUNT
} NoteInstrument;
enum { NOTE_COUNT = 25 };

typedef struct {
    uint8_t id; /* BlockId: C enums themselves need not occupy one byte. */
    union {
        struct {
            uint8_t power;           /* Derived 0..15, simulation writes it. */
            uint8_t connections;     /* Direction bits; horizontal only. */
            uint8_t up_connections;  /* Subset of connections. */
            bool prefer_dot;         /* User choice for an isolated wire. */
        } wire;
        struct {
            uint8_t support_dir;     /* DIR_DOWN or a horizontal face. */
            bool lit;
        } torch;
        struct {
            uint8_t facing;         /* Horizontal output direction. */
            uint8_t delay;          /* 1..4 redstone ticks (2..8 game ticks). */
            bool powered;
            bool locked;
        } repeater;
        struct {
            uint8_t facing;         /* Horizontal output direction. */
            uint8_t output;         /* Block-entity cache, 0..15; not a block property. */
            bool subtract;          /* false: compare, true: subtract. */
            bool powered;          /* Gates the cache; independent of its value. */
        } comparator;
        struct {
            uint8_t facing;         /* Texture top; uphill direction on slopes. */
            bool ascending;
            bool powered;
        } rail;
        struct {
            uint8_t facing;
            bool powered;
        } observer;
        struct {
            uint8_t facing;
            bool powered;
            bool extended;
        } piston;
        struct {
            uint8_t facing;
            bool sticky;
        } moving_piston;
        struct {
            uint8_t facing;         /* Direction of the configurable lid. */
            uint8_t level;          /* Derived comparator fullness, 0..15. */
            bool open;
        } barrel;
        struct {
            bool powered;
            uint8_t note, instrument;
        } note_block;
        struct {
            uint8_t facing;         /* Down or horizontal outlet direction. */
            uint8_t level;          /* Derived comparator fullness, 0..15. */
            bool enabled;          /* False when receiving redstone power. */
        } hopper;
        struct {
            uint8_t facing;         /* Six-direction item outlet. */
            uint8_t level;          /* Derived comparator fullness, 0..15. */
            bool triggered;        /* Input latch, separate from the pending tick. */
        } dropper;
        struct {
            bool lit;
        } lamp;
        struct {
            uint8_t support_dir;    /* From lever toward its supporting block. */
            uint8_t facing;         /* Vanilla horizontal FACING property. */
            bool powered;
        } lever;
    } data;
} BlockState;

BlockState block_default(BlockId id);
const char *block_name(BlockId id);
const char *note_instrument_name(NoteInstrument instrument);
BlockPos block_offset(BlockPos pos, Direction direction);
Direction direction_opposite(Direction direction);
bool direction_horizontal(Direction direction);
bool block_is_piston_base(BlockId id);
bool block_is_piston_head(BlockId id);
bool block_piston_pair(BlockId base, BlockId head);
#endif
