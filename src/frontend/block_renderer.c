#include "block_renderer.h"
#include "rlgl.h"
#include "hopper_shape.h"

#include <stddef.h>
#include <string.h>

enum {
    TILE_SIZE = 32,
    OBSERVER_TEXTURES = 4,
    OBSERVER_STATES = 2,
    OBSERVER_TILE_FIRST = 2,
    PISTON_TILE_FIRST = OBSERVER_TILE_FIRST +
                        OBSERVER_TEXTURES*OBSERVER_STATES,
    PISTON_TEXTURES = 5,
    STICKY_PISTON_TILE_FIRST = PISTON_TILE_FIRST + PISTON_TEXTURES,
    BARREL_TILE_FIRST = STICKY_PISTON_TILE_FIRST + PISTON_TEXTURES,
    BARREL_TEXTURES = 3,
    SLIME_TILE = BARREL_TILE_FIRST + BARREL_TEXTURES,
    NOTE_BLOCK_TILE = SLIME_TILE + 1,
    HONEY_TILE = NOTE_BLOCK_TILE + 1,
    TARGET_TILE = HONEY_TILE + 1,
    HOPPER_OUTSIDE_TILE = TARGET_TILE + 1,
    HOPPER_INSIDE_TILE = HOPPER_OUTSIDE_TILE + 1,
    DROPPER_FRONT_TILE = HOPPER_INSIDE_TILE + 1,
    DROPPER_BACK0_TILE = DROPPER_FRONT_TILE + 1,
    DROPPER_BACK1_TILE = DROPPER_BACK0_TILE + 1,
    DROPPER_SIDE0_TILE = DROPPER_BACK1_TILE + 1,
    DROPPER_SIDE1_TILE = DROPPER_SIDE0_TILE + 1,
    LAMP_OFF_TILE = DROPPER_SIDE1_TILE + 1,
    LAMP_ON_TILE = LAMP_OFF_TILE + 1,
    TILE_COUNT = LAMP_ON_TILE + 1
};
enum { OBSERVER_BACK, OBSERVER_FRONT, OBSERVER_SIDE, OBSERVER_TOP };
enum {
    PISTON_BOTTOM0, PISTON_BOTTOM1,
    PISTON_SIDE0, PISTON_SIDE1, PISTON_TOP
};
enum { BARREL_SIDE, BARREL_TOP, BARREL_TOP_OPEN };
enum {
    WIRE_SHAPES = 16, WIRE_STATES = 16,
    WIRE_SIDE_FIRST = WIRE_SHAPES*WIRE_STATES,
    WIRE_TILES = WIRE_SIDE_FIRST + WIRE_STATES,
    WIRE_COLUMNS = 16,
    WIRE_ROWS = (WIRE_TILES + WIRE_COLUMNS - 1)/WIRE_COLUMNS
};
enum {
    REPEATER_DELAYS = 4,
    REPEATER_STATES = 2,
    REPEATER_COLUMNS = REPEATER_DELAYS,
    REPEATER_ROWS = REPEATER_STATES
};
enum {
    COMPARATOR_MODES = 2,
    COMPARATOR_STATES = 2,
    COMPARATOR_COLUMNS = COMPARATOR_MODES,
    COMPARATOR_ROWS = COMPARATOR_STATES
};
enum { RAIL_STATES = 2 };

static const float wire_offset = 0.002f;
static const float redstone_gate_height = 2.0f/16.0f;
static const float rail_offset = 0.003f;
static const float lever_offset = 0.002f;
static const Direction wire_directions[] = {
    DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH
};

/* Match raylib's default desktop vertex shader; only binary-alpha pixels
 * survive, so transparent texels cannot write depth or cover other blocks. */
static const char cutout_fragment_shader[] =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 texel = texture(texture0, fragTexCoord);\n"
    "    if (texel.a < 0.5) discard;\n"
    "    finalColor = texel*fragColor*colDiffuse;\n"
    "}\n";

typedef struct {
    Vector3 normal;
    Vector3 vertices[4];
} CubeFace;

/* Counterclockwise vertices as seen from outside each face. */
static const CubeFace cube_faces[] = {
    {{ 0,  0,  1}, {{-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f},
                    { 0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f}}},
    {{ 0,  0, -1}, {{ 0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f},
                    {-0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}}},
    {{ 1,  0,  0}, {{ 0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f, -0.5f},
                    { 0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f}}},
    {{-1,  0,  0}, {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f,  0.5f},
                    {-0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f, -0.5f}}},
    {{ 0,  1,  0}, {{-0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f},
                    { 0.5f,  0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f}}},
    {{ 0, -1,  0}, {{-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f},
                    { 0.5f, -0.5f,  0.5f}, {-0.5f, -0.5f,  0.5f}}}
};

/* Vanilla 1.20.4-style torch: four full-texture side sheets and two caps.
 * Each source pixel covers 1/32 of a block. The supplied sprites have a
 * four-pixel-wide stem (x=14..17) and a body top at row 7. The lit sprite's
 * extra pixels extend beyond these sheets' central stem, including above
 * its cap; alpha discard removes every transparent pixel. */
static const CubeFace torch_faces[] = {
    {{ 0,  0,  1}, {{-0.5f, 0,  0.0625f}, { 0.5f, 0,  0.0625f},
                    { 0.5f, 1,  0.0625f}, {-0.5f, 1,  0.0625f}}},
    {{ 0,  0, -1}, {{ 0.5f, 0, -0.0625f}, {-0.5f, 0, -0.0625f},
                    {-0.5f, 1, -0.0625f}, { 0.5f, 1, -0.0625f}}},
    {{ 1,  0,  0}, {{ 0.0625f, 0,  0.5f}, { 0.0625f, 0, -0.5f},
                    { 0.0625f, 1, -0.5f}, { 0.0625f, 1,  0.5f}}},
    {{-1,  0,  0}, {{-0.0625f, 0, -0.5f}, {-0.0625f, 0,  0.5f},
                    {-0.0625f, 1,  0.5f}, {-0.0625f, 1, -0.5f}}},
    {{ 0,  1,  0}, {{-0.0625f, 0.78125f,  0.0625f}, { 0.0625f, 0.78125f,  0.0625f},
                    { 0.0625f, 0.78125f, -0.0625f}, {-0.0625f, 0.78125f, -0.0625f}}},
    {{ 0, -1,  0}, {{-0.0625f, 0, -0.0625f}, { 0.0625f, 0, -0.0625f},
                    { 0.0625f, 0,  0.0625f}, {-0.0625f, 0,  0.0625f}}}
};

typedef struct {
    Vector3 origin;
    Vector3 x_axis, y_axis, z_axis;
} TorchTransform;

static Image load_tile(const char *path)
{
    Image image = LoadImage(path);
    if (!image.data) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot load %s", path);
        return (Image){0};
    }
    if (image.width != TILE_SIZE || image.height != TILE_SIZE) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: %s must be %dx%d (got %dx%d)",
                 path, TILE_SIZE, TILE_SIZE, image.width, image.height);
        UnloadImage(image);
        return (Image){0};
    }
    return image;
}

static Image create_solid_atlas(void)
{
    static const char *const paths[TILE_COUNT] = {
        "res/textures/block/iron_block.png",
        "res/textures/block/redstone_block.png",
        "res/textures/block/observer_back0.png",
        "res/textures/block/observer_front0.png",
        "res/textures/block/observer_side0.png",
        "res/textures/block/observer_top0.png",
        "res/textures/block/observer_back1.png",
        "res/textures/block/observer_front1.png",
        "res/textures/block/observer_side1.png",
        "res/textures/block/observer_top1.png",
        "res/textures/block/piston_bottom0.png",
        "res/textures/block/piston_bottom1.png",
        "res/textures/block/piston_side0.png",
        "res/textures/block/piston_side1.png",
        "res/textures/block/piston_top.png",
        "res/textures/block/sticky_piston_bottom0.png",
        "res/textures/block/sticky_piston_bottom1.png",
        "res/textures/block/sticky_piston_side0.png",
        "res/textures/block/sticky_piston_side1.png",
        "res/textures/block/sticky_piston_top.png",
        "res/textures/block/barrel_side.png",
        "res/textures/block/barrel_top.png",
        "res/textures/block/barrel_top_open.png",
        "res/textures/block/slime_block.png",
        "res/textures/block/note_block.png",
        "res/textures/block/honey_block.png",
        "res/textures/block/target_block.png",
        "res/textures/block/hopper_outside.png",
        "res/textures/block/hopper_inside.png",
        "res/textures/block/dropper_front.png",
        "res/textures/block/dropper_back0.png",
        "res/textures/block/dropper_back1.png",
        "res/textures/block/dropper_side0.png",
        "res/textures/block/dropper_side1.png",
        "res/textures/block/redstone_lamp0.png",
        "res/textures/block/redstone_lamp1.png"
    };
    Image sources[TILE_COUNT] = {0};
    Image atlas = {0};
    Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};

    for (int tile = 0; tile < TILE_COUNT; ++tile) {
        sources[tile] = load_tile(paths[tile]);
        if (!sources[tile].data) goto cleanup;
    }
    atlas = GenImageColor(TILE_COUNT*TILE_SIZE, TILE_SIZE, BLANK);
    if (atlas.data)
        for (int tile = 0; tile < TILE_COUNT; ++tile) {
            Rectangle destination = {
                tile*TILE_SIZE, 0, TILE_SIZE, TILE_SIZE
            };
            ImageDraw(&atlas, sources[tile], full, destination, WHITE);
        }

cleanup:
    for (int tile = 0; tile < TILE_COUNT; ++tile)
        if (sources[tile].data) UnloadImage(sources[tile]);
    return atlas;
}

static Rectangle wire_tile_rectangle(int tile)
{
    return (Rectangle){(tile%WIRE_COLUMNS)*TILE_SIZE,
                       (tile/WIRE_COLUMNS)*TILE_SIZE, TILE_SIZE, TILE_SIZE};
}

static Color wire_color(int power)
{
    /* RedStoneWireBlock's color curve. Power changes need no per-frame image
     * processing: all sixteen levels are baked into the atlas once. */
    float strength = power/15.0f;
    float red = strength*0.6f + (power ? 0.4f : 0.3f);
    float green = strength*strength*0.7f - 0.5f;
    float blue = strength*strength*0.6f - 0.7f;
    if (green < 0) green = 0;
    if (blue < 0) blue = 0;
    return (Color){(unsigned char)(red*255), (unsigned char)(green*255),
                   (unsigned char)(blue*255), 255};
}

static bool wire_mask(Image *image)
{
    Color *pixels = LoadImageColors(*image);
    if (!pixels) return false;
    /* The supplied powered sprites are already red. Recover a neutral
     * intensity mask so the color curve also supplies green/blue channels. */
    for (int i = 0; i < image->width*image->height; ++i) {
        unsigned char value = pixels[i].r;
        if (pixels[i].g > value) value = pixels[i].g;
        if (pixels[i].b > value) value = pixels[i].b;
        pixels[i] = (Color){value, value, value, pixels[i].a};
    }
    UnloadImage(*image);
    *image = (Image){pixels, TILE_SIZE, TILE_SIZE, 1,
                     PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    return true;
}

static Image create_wire_atlas(void)
{
    static const char *const paths[] = {
        "res/textures/block/redstone_dot0.png",
        "res/textures/block/redstone_dot1.png",
        "res/textures/block/redstone_wire0.png", 
        "res/textures/block/redstone_wire1.png"
    };
    /* The source arm points down in image space (+Z / south in the world).
     * CW turns map south -> west -> north -> east. */
    static const int clockwise_turns[] = {3, 1, 0, 2};
    Image sources[4] = {0};
    Image arms[4] = {0};
    Image atlas = {0};
    Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};
    bool complete = false;

    for (int index = 0; index < 4; ++index) {
        sources[index] = load_tile(paths[index]);
        if (!sources[index].data) goto cleanup;
        if ((index & 1) && !wire_mask(&sources[index])) goto cleanup;
    }
    atlas = GenImageColor(WIRE_COLUMNS*TILE_SIZE, WIRE_ROWS*TILE_SIZE, BLANK);
    if (!atlas.data) goto cleanup;

    for (int power = 0; power < WIRE_STATES; ++power) {
        int powered = power > 0;
        /* Keep the authored unpowered texture; tint the neutral powered mask. */
        Color tint = powered ? wire_color(power) : WHITE;
        for (int direction = 0; direction < 4; ++direction) {
            arms[direction] = ImageCopy(sources[2 + powered]);
            if (!arms[direction].data) goto cleanup;
            for (int turn = 0; turn < clockwise_turns[direction]; ++turn)
                ImageRotateCW(&arms[direction]);
        }
        for (int mask = 0; mask < WIRE_SHAPES; ++mask) {
            Rectangle destination = wire_tile_rectangle(power*WIRE_SHAPES + mask);
            for (int direction = 0; direction < 4; ++direction) {
                if (mask & (1 << direction))
                    ImageDraw(&atlas, arms[direction], full, destination, tint);
            }
            /* Opposite pairs form straight lines without a center dot. */
            bool straight = (mask == 3 || mask == 12);
            if (!straight)
                ImageDraw(&atlas, sources[powered], full, destination, tint);
        }

        /* Climbing strips use a full line without a dot in the middle. */
        Rectangle side = wire_tile_rectangle(WIRE_SIDE_FIRST + power);
        ImageDraw(&atlas, arms[2], full, side, tint);
        ImageDraw(&atlas, arms[3], full, side, tint);
        for (int direction = 0; direction < 4; ++direction) {
            UnloadImage(arms[direction]);
            arms[direction] = (Image){0};
        }
    }
    complete = true;

cleanup:
    for (int index = 0; index < 4; ++index) {
        if (sources[index].data) UnloadImage(sources[index]);
        if (arms[index].data) UnloadImage(arms[index]);
    }
    if (!complete) {
        if (atlas.data) UnloadImage(atlas);
        return (Image){0};
    }
    return atlas;
}

static Image create_torch_atlas(void)
{
    static const char *const paths[] = {
        "res/textures/block/redstone_torch0.png", 
        "res/textures/block/redstone_torch1.png"
    };
    Image sources[2] = {0};
    Image atlas = {0};
    for (int lit = 0; lit < 2; ++lit) {
        sources[lit] = load_tile(paths[lit]);
        if (!sources[lit].data) goto cleanup;
    }

    atlas = GenImageColor(2*TILE_SIZE, TILE_SIZE, BLANK);
    if (atlas.data) {
        Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};
        for (int lit = 0; lit < 2; ++lit) {
            Rectangle destination = {lit*TILE_SIZE, 0, TILE_SIZE, TILE_SIZE};
            ImageDraw(&atlas, sources[lit], full, destination, WHITE);
        }
    }

cleanup:
    for (int lit = 0; lit < 2; ++lit)
        if (sources[lit].data) UnloadImage(sources[lit]);
    return atlas;
}

static Image create_repeater_atlas(void)
{
    static const char *const paths[REPEATER_STATES][REPEATER_DELAYS] = {
        {
            "res/textures/block/repeater1_0.png",
            "res/textures/block/repeater2_0.png",
            "res/textures/block/repeater3_0.png",
            "res/textures/block/repeater4_0.png"
        },
        {
            "res/textures/block/repeater1_1.png",
            "res/textures/block/repeater2_1.png",
            "res/textures/block/repeater3_1.png",
            "res/textures/block/repeater4_1.png"
        }
    };
    Image sources[REPEATER_STATES][REPEATER_DELAYS] = {0};
    Image atlas = {0};
    Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};

    for (int powered = 0; powered < REPEATER_STATES; ++powered)
        for (int delay = 0; delay < REPEATER_DELAYS; ++delay) {
            sources[powered][delay] = load_tile(paths[powered][delay]);
            if (!sources[powered][delay].data) goto cleanup;
        }

    atlas = GenImageColor(REPEATER_COLUMNS*TILE_SIZE,
                          REPEATER_ROWS*TILE_SIZE, BLANK);
    if (atlas.data)
        for (int powered = 0; powered < REPEATER_STATES; ++powered)
            for (int delay = 0; delay < REPEATER_DELAYS; ++delay) {
                Rectangle destination = {
                    delay*TILE_SIZE, powered*TILE_SIZE,
                    TILE_SIZE, TILE_SIZE
                };
                ImageDraw(&atlas, sources[powered][delay], full,
                          destination, WHITE);
            }

cleanup:
    for (int powered = 0; powered < REPEATER_STATES; ++powered)
        for (int delay = 0; delay < REPEATER_DELAYS; ++delay)
            if (sources[powered][delay].data)
                UnloadImage(sources[powered][delay]);
    return atlas;
}

static Image create_comparator_atlas(void)
{
    static const char *const paths[COMPARATOR_STATES][COMPARATOR_MODES] = {
        {
            "res/textures/block/comparator1_0.png",
            "res/textures/block/comparator2_0.png"
        },
        {
            "res/textures/block/comparator1_1.png",
            "res/textures/block/comparator2_1.png"
        }
    };
    Image sources[COMPARATOR_STATES][COMPARATOR_MODES] = {0};
    Image atlas = {0};
    Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};

    for (int powered = 0; powered < COMPARATOR_STATES; ++powered)
        for (int mode = 0; mode < COMPARATOR_MODES; ++mode) {
            sources[powered][mode] = load_tile(paths[powered][mode]);
            if (!sources[powered][mode].data) goto cleanup;
        }

    atlas = GenImageColor(COMPARATOR_COLUMNS*TILE_SIZE,
                          COMPARATOR_ROWS*TILE_SIZE, BLANK);
    if (atlas.data)
        for (int powered = 0; powered < COMPARATOR_STATES; ++powered)
            for (int mode = 0; mode < COMPARATOR_MODES; ++mode) {
                Rectangle destination = {
                    mode*TILE_SIZE, powered*TILE_SIZE,
                    TILE_SIZE, TILE_SIZE
                };
                ImageDraw(&atlas, sources[powered][mode], full,
                          destination, WHITE);
            }

cleanup:
    for (int powered = 0; powered < COMPARATOR_STATES; ++powered)
        for (int mode = 0; mode < COMPARATOR_MODES; ++mode)
            if (sources[powered][mode].data)
                UnloadImage(sources[powered][mode]);
    return atlas;
}

static Image create_two_state_atlas(const char *unpowered_path,
                                    const char *powered_path)
{
    const char *const paths[RAIL_STATES] = {
        unpowered_path, powered_path
    };
    Image sources[RAIL_STATES] = {0};
    Image atlas = {0};
    for (int powered = 0; powered < RAIL_STATES; ++powered) {
        sources[powered] = load_tile(paths[powered]);
        if (!sources[powered].data) goto cleanup;
    }

    atlas = GenImageColor(RAIL_STATES*TILE_SIZE, TILE_SIZE, BLANK);
    if (atlas.data) {
        Rectangle full = {0, 0, TILE_SIZE, TILE_SIZE};
        for (int powered = 0; powered < RAIL_STATES; ++powered) {
            Rectangle destination = {
                powered*TILE_SIZE, 0, TILE_SIZE, TILE_SIZE
            };
            ImageDraw(&atlas, sources[powered], full, destination, WHITE);
        }
    }

cleanup:
    for (int powered = 0; powered < RAIL_STATES; ++powered)
        if (sources[powered].data) UnloadImage(sources[powered]);
    return atlas;
}

bool block_renderer_init(BlockRenderer *renderer)
{
    if (!renderer) return false;
    *renderer = (BlockRenderer){0};

    Image atlas = create_solid_atlas();
    if (atlas.data) {
        renderer->atlas = LoadTextureFromImage(atlas);
        UnloadImage(atlas);
    }

    if (!renderer->atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the block atlas");
        return false;
    }

    SetTextureFilter(renderer->atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->atlas, TEXTURE_WRAP_CLAMP);

    Image glass = load_tile("res/textures/block/glass.png");
    if (glass.data) {
        renderer->glass_texture = LoadTextureFromImage(glass);
        UnloadImage(glass);
    }
    if (!renderer->glass_texture.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot load the glass texture");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->glass_texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->glass_texture, TEXTURE_WRAP_CLAMP);

    Image wire_atlas = create_wire_atlas();
    if (wire_atlas.data) {
        renderer->wire_atlas = LoadTextureFromImage(wire_atlas);
        UnloadImage(wire_atlas);
    }
    if (!renderer->wire_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the redstone dust atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->wire_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->wire_atlas, TEXTURE_WRAP_CLAMP);

    Image torch_atlas = create_torch_atlas();
    if (torch_atlas.data) {
        renderer->torch_atlas = LoadTextureFromImage(torch_atlas);
        UnloadImage(torch_atlas);
    }
    if (!renderer->torch_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the redstone torch atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->torch_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->torch_atlas, TEXTURE_WRAP_CLAMP);

    Image repeater_atlas = create_repeater_atlas();
    if (repeater_atlas.data) {
        renderer->repeater_atlas = LoadTextureFromImage(repeater_atlas);
        UnloadImage(repeater_atlas);
    }
    if (!renderer->repeater_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the repeater atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->repeater_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->repeater_atlas, TEXTURE_WRAP_CLAMP);

    Image comparator_atlas = create_comparator_atlas();
    if (comparator_atlas.data) {
        renderer->comparator_atlas = LoadTextureFromImage(comparator_atlas);
        UnloadImage(comparator_atlas);
    }
    if (!renderer->comparator_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the comparator atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->comparator_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->comparator_atlas, TEXTURE_WRAP_CLAMP);

    Image powered_rail_atlas = create_two_state_atlas(
        "res/textures/block/powered_rail0.png",
        "res/textures/block/powered_rail1.png");
    if (powered_rail_atlas.data) {
        renderer->powered_rail_atlas = LoadTextureFromImage(powered_rail_atlas);
        UnloadImage(powered_rail_atlas);
    }
    if (!renderer->powered_rail_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the powered rail atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->powered_rail_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->powered_rail_atlas, TEXTURE_WRAP_CLAMP);

    Image activator_rail_atlas = create_two_state_atlas(
        "res/textures/block/activator_rail0.png",
        "res/textures/block/activator_rail1.png");
    if (activator_rail_atlas.data) {
        renderer->activator_rail_atlas = LoadTextureFromImage(activator_rail_atlas);
        UnloadImage(activator_rail_atlas);
    }
    if (!renderer->activator_rail_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the activator rail atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->activator_rail_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->activator_rail_atlas, TEXTURE_WRAP_CLAMP);

    Image lever_atlas = create_two_state_atlas(
        "res/textures/block/lever0.png",
        "res/textures/block/lever1.png");
    if (lever_atlas.data) {
        renderer->lever_atlas = LoadTextureFromImage(lever_atlas);
        UnloadImage(lever_atlas);
    }
    if (!renderer->lever_atlas.id) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot create the lever atlas");
        block_renderer_unload(renderer);
        return false;
    }
    SetTextureFilter(renderer->lever_atlas, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->lever_atlas, TEXTURE_WRAP_CLAMP);

    renderer->cutout_shader = LoadShaderFromMemory(NULL, cutout_fragment_shader);
    if (renderer->cutout_shader.id == rlGetShaderIdDefault()) {
        /* LoadShaderFromMemory allocates its own locations even on fallback. */
        MemFree(renderer->cutout_shader.locs);
        renderer->cutout_shader = (Shader){0};
    }
    if (!renderer->cutout_shader.id || !renderer->cutout_shader.locs) {
        TraceLog(LOG_ERROR, "BLOCK TEXTURE: Cannot load the cutout shader (OpenGL 3.3)");
        block_renderer_unload(renderer);
        return false;
    }
    return true;
}

void block_renderer_unload(BlockRenderer *renderer)
{
    if (!renderer) return;
    if (renderer->cutout_shader.id) UnloadShader(renderer->cutout_shader);
    if (renderer->lever_atlas.id) UnloadTexture(renderer->lever_atlas);
    if (renderer->activator_rail_atlas.id)
        UnloadTexture(renderer->activator_rail_atlas);
    if (renderer->powered_rail_atlas.id)
        UnloadTexture(renderer->powered_rail_atlas);
    if (renderer->comparator_atlas.id) UnloadTexture(renderer->comparator_atlas);
    if (renderer->repeater_atlas.id) UnloadTexture(renderer->repeater_atlas);
    if (renderer->torch_atlas.id) UnloadTexture(renderer->torch_atlas);
    if (renderer->wire_atlas.id) UnloadTexture(renderer->wire_atlas);
    if (renderer->glass_texture.id) UnloadTexture(renderer->glass_texture);
    if (renderer->atlas.id) UnloadTexture(renderer->atlas);
    *renderer = (BlockRenderer){0};
}

void block_renderer_draw_cube(const BlockRenderer *renderer, BlockId id,
                              Vector3 center)
{
    if (!renderer || !renderer->atlas.id) return;

    int tile = 0;
    Texture2D texture = renderer->atlas;
    switch (id) {
    case BLOCK_IRON:           tile = 0; break;
    case BLOCK_REDSTONE_BLOCK: tile = 1; break;
    case BLOCK_SLIME:          tile = SLIME_TILE; break;
    case BLOCK_NOTE_BLOCK:     tile = NOTE_BLOCK_TILE; break;
    case BLOCK_HONEY:          tile = HONEY_TILE; break;
    case BLOCK_TARGET:         tile = TARGET_TILE; break;
    case BLOCK_REDSTONE_LAMP:  tile = LAMP_OFF_TILE; break;
    case BLOCK_GLASS:
        texture = renderer->glass_texture;
        if (!texture.id) return;
        break;
    default: return;
    }

    /* Inset by half a texel so tile edges cannot sample their neighbor.
     * Image row zero maps to v=0; bottom vertices use the larger v. */
    float u0 = (tile*TILE_SIZE + 0.5f)/texture.width;
    float u1 = ((tile + 1)*TILE_SIZE - 0.5f)/texture.width;
    float v0 = 0.5f/texture.height;
    float v1 = (TILE_SIZE - 0.5f)/texture.height;
    const Vector2 uv[4] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};

    bool cutout = id == BLOCK_GLASS && renderer->cutout_shader.id;
    if (cutout) BeginShaderMode(renderer->cutout_shader);
    rlCheckRenderBatchLimit(24);
    rlSetTexture(texture.id);
    rlBegin(RL_QUADS);
    /* White tint preserves the source colors instead of multiplying them
     * by the prototype's LIGHTGRAY/red block colors. */
    rlColor4ub(255, 255, 255, 255);
    for (int face = 0; face < 6; ++face) {
        const CubeFace *quad = &cube_faces[face];
        rlNormal3f(quad->normal.x, quad->normal.y, quad->normal.z);
        for (int vertex = 0; vertex < 4; ++vertex) {
            rlTexCoord2f(uv[vertex].x, uv[vertex].y);
            rlVertex3f(center.x + quad->vertices[vertex].x,
                       center.y + quad->vertices[vertex].y,
                       center.z + quad->vertices[vertex].z);
        }
    }
    rlEnd();
    rlSetTexture(0);
    if (cutout) EndShaderMode();
}

static void wire_uv(Texture2D atlas, int tile, Vector2 uv[4])
{
    Rectangle rectangle = wire_tile_rectangle(tile);
    float u0 = (rectangle.x + 0.5f)/atlas.width;
    float u1 = (rectangle.x + TILE_SIZE - 0.5f)/atlas.width;
    float v0 = (rectangle.y + 0.5f)/atlas.height;
    float v1 = (rectangle.y + TILE_SIZE - 0.5f)/atlas.height;
    uv[0] = (Vector2){u0, v1};
    uv[1] = (Vector2){u1, v1};
    uv[2] = (Vector2){u1, v0};
    uv[3] = (Vector2){u0, v0};
}

static int direction_turns(Direction right, Direction down,
                           Direction desired, bool source_points_down)
{
    if (source_points_down) {
        if (desired == down) return 0;
        if (desired == direction_opposite(right)) return 1;
        if (desired == direction_opposite(down)) return 2;
        if (desired == right) return 3;
    } else {
        if (desired == right) return 0;
        if (desired == down) return 1;
        if (desired == direction_opposite(right)) return 2;
        if (desired == direction_opposite(down)) return 3;
    }
    return 0;
}

static void solid_tile_region_uv(Texture2D atlas, int tile,
                                 float x, float y, float width, float height,
                                 int turns, Vector2 uv[4])
{
    float u0 = (tile*TILE_SIZE + x + 0.5f)/atlas.width;
    float u1 = (tile*TILE_SIZE + x + width - 0.5f)/atlas.width;
    float v0 = (y + 0.5f)/atlas.height;
    float v1 = (y + height - 0.5f)/atlas.height;
    const Vector2 base[4] = {{u0,v1},{u1,v1},{u1,v0},{u0,v0}};
    for (int vertex = 0; vertex < 4; ++vertex)
        uv[vertex] = base[(vertex + turns) & 3];
}

static void solid_tile_uv(Texture2D atlas, int tile, int turns,
                          Vector2 uv[4])
{
    solid_tile_region_uv(atlas, tile, 0.0f, 0.0f,
                         TILE_SIZE, TILE_SIZE, turns, uv);
}

static void observer_face_uv(Texture2D atlas, BlockState state, int face,
                             Vector2 uv[4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    static const Direction right[6] = {
        DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_EAST
    };
    static const Direction down[6] = {
        DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_SOUTH, DIR_NORTH
    };
    Direction output = (Direction)state.data.observer.facing;
    Direction face_direction = directions[face];
    int texture;
    Direction desired;
    bool points_down;

    if (face_direction == output) {
        texture = OBSERVER_BACK;
        desired = output == DIR_UP ? DIR_NORTH :
                  output == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
        points_down = true;
    } else if (face_direction == direction_opposite(output)) {
        texture = OBSERVER_FRONT;
        desired = output == DIR_UP ? DIR_NORTH :
                  output == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
        points_down = true;
    } else {
        bool top_face = direction_horizontal(output) ?
                        (face_direction == DIR_UP || face_direction == DIR_DOWN) :
                        (face_direction == DIR_NORTH || face_direction == DIR_SOUTH);
        texture = top_face ? OBSERVER_TOP : OBSERVER_SIDE;
        desired = top_face ? direction_opposite(output) : output;
        points_down = top_face;
    }

    int powered = state.data.observer.powered ? 1 : 0;
    int tile = OBSERVER_TILE_FIRST + powered*OBSERVER_TEXTURES + texture;
    solid_tile_uv(atlas, tile,
                  direction_turns(right[face], down[face], desired,
                                  points_down),
                  uv);
}

static void barrel_face_uv(Texture2D atlas, BlockState state, int face,
                           Vector2 uv[4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    static const Direction right[6] = {
        DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_EAST
    };
    static const Direction down[6] = {
        DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_SOUTH, DIR_NORTH
    };
    Direction facing = (Direction)state.data.barrel.facing;
    if ((unsigned)facing >= DIR_COUNT) facing = DIR_NORTH;
    Direction face_direction = directions[face];
    int texture;
    Direction desired;
    bool source_points_down = true;

    if (face_direction == facing) {
        texture = state.data.barrel.open ? BARREL_TOP_OPEN : BARREL_TOP;
        desired = facing == DIR_UP ? DIR_NORTH :
                  facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
    } else if (face_direction == direction_opposite(facing)) {
        /* This project intentionally reuses the closed lid as the bottom. */
        texture = BARREL_TOP;
        desired = facing == DIR_UP ? DIR_NORTH :
                  facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
    } else {
        texture = BARREL_SIDE;
        /* The source image's horizontal axis follows the barrel axis. */
        desired = direction_opposite(facing);
        source_points_down = false;
    }

    solid_tile_uv(atlas, BARREL_TILE_FIRST + texture,
                  direction_turns(right[face], down[face], desired,
                                  source_points_down),
                  uv);
}

static void dropper_face_uv(Texture2D atlas, BlockState state, int face,
                            Vector2 uv[4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    static const Direction right[6] = {
        DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_EAST
    };
    static const Direction down[6] = {
        DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_SOUTH, DIR_NORTH
    };
    Direction facing = (Direction)state.data.dropper.facing;
    bool front = directions[face] == facing;
    bool back = directions[face] == direction_opposite(facing);
    int tile = front ? DROPPER_FRONT_TILE :
               back ? DROPPER_BACK0_TILE + state.data.dropper.triggered :
                      DROPPER_SIDE0_TILE + state.data.dropper.triggered;
    Direction desired = front || back ?
        (facing == DIR_UP ? DIR_NORTH : facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN) :
        facing;
    solid_tile_uv(atlas, tile,
                  direction_turns(right[face], down[face], desired, front || back),
                  uv);
}

static int piston_tile_first(BlockId id)
{
    return id == BLOCK_STICKY_PISTON ||
           id == BLOCK_STICKY_PISTON_HEAD ?
           STICKY_PISTON_TILE_FIRST : PISTON_TILE_FIRST;
}

static void piston_face_uv(Texture2D atlas, BlockState state, int face,
                           Vector2 uv[4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    static const Direction right[6] = {
        DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_EAST
    };
    static const Direction down[6] = {
        DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_SOUTH, DIR_NORTH
    };
    Direction facing = (Direction)state.data.piston.facing;
    Direction face_direction = directions[face];
    bool powered = state.data.piston.powered;
    int texture;
    Direction desired;
    bool points_down;
    float region_x = 0.0f, region_width = TILE_SIZE;

    if (face_direction == facing) {
        texture = state.data.piston.extended ?
                  (powered ? PISTON_BOTTOM1 : PISTON_BOTTOM0) : PISTON_TOP;
        desired = facing == DIR_UP ? DIR_NORTH :
                  facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
        points_down = true;
    } else if (face_direction == direction_opposite(facing)) {
        texture = powered ? PISTON_BOTTOM1 : PISTON_BOTTOM0;
        desired = facing == DIR_UP ? DIR_NORTH :
                  facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
        points_down = true;
    } else {
        texture = powered ? PISTON_SIDE1 : PISTON_SIDE0;
        desired = facing;
        points_down = false;
        /* The rightmost eight source pixels are the wooden head. Once the
         * piston extends, only the 12/16-deep stone body remains here. */
        if (state.data.piston.extended) region_width = 24.0f;
    }

    int tile = piston_tile_first((BlockId)state.id) + texture;
    int turns = direction_turns(right[face], down[face], desired,
                                points_down);
    solid_tile_region_uv(atlas, tile, region_x, 0.0f, region_width,
                         TILE_SIZE, turns, uv);
}

static int piston_head_uv(Texture2D atlas, BlockState state,
                          unsigned solid_faces,
                          Vector2 uv[BLOCK_GEOMETRY_QUADS][4])
{
    static const Direction directions[6] = {
        DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
    };
    static const Direction right[6] = {
        DIR_EAST, DIR_WEST, DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_EAST
    };
    static const Direction down[6] = {
        DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_DOWN, DIR_SOUTH, DIR_NORTH
    };
    Direction facing = (Direction)state.data.piston.facing;
    int first = piston_tile_first((BlockId)state.id);
    int side = first +
               (state.data.piston.powered ? PISTON_SIDE1 : PISTON_SIDE0);
    int top = first + PISTON_TOP;
    int count = 0;

    for (int face = 0; face < 6; ++face) {
        if (!(solid_faces & (1u << face))) continue;
        Direction face_direction = directions[face];
        if (face_direction == facing) {
            Direction upright = facing == DIR_UP ? DIR_NORTH :
                                facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
            solid_tile_uv(atlas, top,
                          direction_turns(right[face], down[face], upright,
                                          true), uv[count]);
        } else if (face_direction == direction_opposite(facing)) {
            /* Vanilla's moving plate is wooden on both broad faces. */
            Direction upright = facing == DIR_UP ? DIR_NORTH :
                                facing == DIR_DOWN ? DIR_SOUTH : DIR_DOWN;
            solid_tile_uv(atlas, top,
                          direction_turns(right[face], down[face], upright,
                                          true), uv[count]);
        } else {
            int turns = direction_turns(right[face], down[face], facing,
                                        false);
            solid_tile_region_uv(atlas, side, 24.0f, 0.0f, 8.0f,
                                 TILE_SIZE, turns, uv[count]);
        }
        ++count;
    }

    for (int face = 0; face < 6; ++face) {
        Direction face_direction = directions[face];
        if (face_direction == facing ||
            face_direction == direction_opposite(facing)) continue;
        /* Use a bolt-free strip of the wooden top texture for the narrow
         * piston arm, with its long texture axis following the arm. */
        int turns = direction_turns(right[face], down[face], facing, false);
        solid_tile_region_uv(atlas, top, 5.0f, 4.0f, 6.0f, 24.0f,
                             turns, uv[count++]);
    }
    return count;
}

static void emit_wire_face(const CubeFace *face, Vector3 center,
                           const Vector2 uv[4], bool climbing)
{
    rlNormal3f(face->normal.x, face->normal.y, face->normal.z);
    for (int vertex = 0; vertex < 4; ++vertex) {
        Vector3 position = face->vertices[vertex];
        /* Keep the lower wall edge just outside its support. The upper edge
         * meets the higher wire exactly; this avoids a gap at the step. */
        if (climbing && position.y < 0) {
            position.x += face->normal.x*wire_offset;
            position.z += face->normal.z*wire_offset;
        }
        rlTexCoord2f(uv[vertex].x, uv[vertex].y);
        rlVertex3f(center.x + position.x, center.y + position.y,
                   center.z + position.z);
    }
}

void block_renderer_begin_cutouts(const BlockRenderer *renderer)
{
    if (renderer && renderer->cutout_shader.id)
        BeginShaderMode(renderer->cutout_shader);
}

void block_renderer_end_cutouts(void)
{
    EndShaderMode();
}

void block_renderer_draw_wire(const BlockRenderer *renderer, BlockState state,
                              Vector3 center)
{
    if (!renderer || !renderer->wire_atlas.id || state.id != BLOCK_REDSTONE_WIRE)
        return;

    int mask = 0;
    for (int direction = 0; direction < 4; ++direction) {
        if (state.data.wire.connections & (1u << wire_directions[direction]))
            mask |= 1 << direction;
    }
    int power = state.data.wire.power;
    Vector2 uv[4];
    wire_uv(renderer->wire_atlas, power*WIRE_SHAPES + mask, uv);

    rlCheckRenderBatchLimit(20);
    rlSetTexture(renderer->wire_atlas.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    Vector3 top = {center.x, center.y - 1.0f + wire_offset, center.z};
    emit_wire_face(&cube_faces[4], top, uv, false);

    wire_uv(renderer->wire_atlas, WIRE_SIDE_FIRST + power, uv);
    static const int side_faces[] = {3, 2, 1, 0};
    static const Vector3 offsets[] = {{1,0,0}, {-1,0,0}, {0,0,1}, {0,0,-1}};
    for (int direction = 0; direction < 4; ++direction) {
        unsigned bit = 1u << wire_directions[direction];
        if (!(state.data.wire.up_connections & state.data.wire.connections & bit))
            continue;
        Vector3 side = {center.x + offsets[direction].x,
                        center.y + wire_offset,
                        center.z + offsets[direction].z};
        emit_wire_face(&cube_faces[side_faces[direction]], side, uv, true);
    }
    rlEnd();
    rlSetTexture(0);
}

static TorchTransform torch_transform(Vector3 center, Direction support)
{
    TorchTransform transform = {
        {center.x, center.y - 0.5f, center.z},
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}
    };
    /* Facing is away from the support, whose direction points toward it. */
    Vector3 facing;
    switch (support) {
    case DIR_EAST:  facing = (Vector3){-1, 0,  0}; break;
    case DIR_WEST:  facing = (Vector3){ 1, 0,  0}; break;
    case DIR_SOUTH: facing = (Vector3){ 0, 0, -1}; break;
    case DIR_NORTH: facing = (Vector3){ 0, 0,  1}; break;
    default: return transform;
    }

    /* Vanilla's wall mount is 3.5/16 above the cell bottom, tilted 22.5
     * degrees outward. The stem's bottom intersects the supporting face. */
    const float sine = 0.3826834324f, cosine = 0.9238795325f;
    transform.origin.x -= facing.x*0.5f;
    transform.origin.y += 3.5f/16.0f;
    transform.origin.z -= facing.z*0.5f;
    transform.x_axis = (Vector3){facing.x*cosine, -sine, facing.z*cosine};
    transform.y_axis = (Vector3){facing.x*sine, cosine, facing.z*sine};
    transform.z_axis = (Vector3){-facing.z, 0, facing.x};
    return transform;
}

static Vector3 torch_rotate(TorchTransform transform, Vector3 vector)
{
    return (Vector3){
        transform.x_axis.x*vector.x + transform.y_axis.x*vector.y + transform.z_axis.x*vector.z,
        transform.x_axis.y*vector.x + transform.y_axis.y*vector.y + transform.z_axis.y*vector.z,
        transform.x_axis.z*vector.x + transform.y_axis.z*vector.y + transform.z_axis.z*vector.z
    };
}

static void torch_uv(Texture2D atlas, int lit, Rectangle source, Vector2 uv[4])
{
    /* Use texel boundaries so the four-pixel stem meets its caps exactly.
     * Point filtering preserves pixels; tile sides are transparent gutters. */
    float u0 = (lit*TILE_SIZE + source.x)/atlas.width;
    float u1 = (lit*TILE_SIZE + source.x + source.width)/atlas.width;
    float v0 = source.y/atlas.height;
    float v1 = (source.y + source.height)/atlas.height;
    uv[0] = (Vector2){u0, v1};
    uv[1] = (Vector2){u1, v1};
    uv[2] = (Vector2){u1, v0};
    uv[3] = (Vector2){u0, v0};
}

void block_renderer_draw_torch(const BlockRenderer *renderer, BlockState state,
                               Vector3 center)
{
    if (!renderer || !renderer->torch_atlas.id || state.id != BLOCK_REDSTONE_TORCH)
        return;

    TorchTransform transform = torch_transform(center, (Direction)state.data.torch.support_dir);
    int lit = state.data.torch.lit ? 1 : 0;
    Vector2 uv[4];
    torch_uv(renderer->torch_atlas, lit, (Rectangle){0, 0, TILE_SIZE, TILE_SIZE}, uv);

    rlCheckRenderBatchLimit(24);
    rlSetTexture(renderer->torch_atlas.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    for (int face = 0; face < 6; ++face) {
        if (face == 4)
            torch_uv(renderer->torch_atlas, lit, (Rectangle){14, 7, 4, 4}, uv);
        else if (face == 5)
            torch_uv(renderer->torch_atlas, lit, (Rectangle){14, 28, 4, 4}, uv);

        const CubeFace *quad = &torch_faces[face];
        Vector3 normal = torch_rotate(transform, quad->normal);
        rlNormal3f(normal.x, normal.y, normal.z);
        for (int vertex = 0; vertex < 4; ++vertex) {
            Vector3 position = torch_rotate(transform, quad->vertices[vertex]);
            rlTexCoord2f(uv[vertex].x, uv[vertex].y);
            rlVertex3f(transform.origin.x + position.x,
                       transform.origin.y + position.y,
                       transform.origin.z + position.z);
        }
    }
    rlEnd();
    rlSetTexture(0);
}

static void redstone_gate_top_uv(Texture2D atlas, int column, int row,
                                 Direction facing, Vector2 uv[4])
{
    float u0 = (column*TILE_SIZE + 0.5f)/atlas.width;
    float u1 = ((column + 1)*TILE_SIZE - 0.5f)/atlas.width;
    float v0 = (row*TILE_SIZE + 0.5f)/atlas.height;
    float v1 = ((row + 1)*TILE_SIZE - 0.5f)/atlas.height;
    const Vector2 base[4] = {{u0,v1},{u1,v1},{u1,v0},{u0,v0}};

    /* The top edge of every gate source image is its output/front. */
    int turn = 0;
    switch (facing) {
    case DIR_EAST:  turn = 1; break;
    case DIR_SOUTH: turn = 2; break;
    case DIR_WEST:  turn = 3; break;
    default: break; /* north */
    }
    for (int vertex = 0; vertex < 4; ++vertex)
        uv[vertex] = base[(vertex + turn) & 3];
}

static void redstone_gate_side_uv(Texture2D atlas, int column, int row,
                                  Vector2 uv[4])
{
    float u0 = (column*TILE_SIZE + 0.5f)/atlas.width;
    float u1 = ((column + 1)*TILE_SIZE - 0.5f)/atlas.width;
    float v = (row*TILE_SIZE + 0.5f)/atlas.height;

    /* Stretch the uniform outermost pixel row into the two-pixel-high base. */
    uv[0] = (Vector2){u0, v};
    uv[1] = (Vector2){u1, v};
    uv[2] = (Vector2){u1, v};
    uv[3] = (Vector2){u0, v};
}

static void repeater_uv(Texture2D atlas, BlockState state,
                        Vector2 uv[REDSTONE_GATE_GEOMETRY_QUADS][4])
{
    int delay = state.data.repeater.delay;
    if (delay < 1 || delay > REPEATER_DELAYS) delay = 1;
    int column = delay - 1;
    int row = state.data.repeater.powered ? 1 : 0;
    redstone_gate_top_uv(atlas, column, row,
                         (Direction)state.data.repeater.facing, uv[0]);
    for (int face = 1; face < REDSTONE_GATE_GEOMETRY_QUADS; ++face)
        redstone_gate_side_uv(atlas, column, row, uv[face]);
}

static void comparator_uv(Texture2D atlas, BlockState state,
                          Vector2 uv[REDSTONE_GATE_GEOMETRY_QUADS][4])
{
    int column = state.data.comparator.subtract ? 1 : 0;
    int row = state.data.comparator.powered ? 1 : 0;
    redstone_gate_top_uv(atlas, column, row,
                         (Direction)state.data.comparator.facing, uv[0]);
    for (int face = 1; face < REDSTONE_GATE_GEOMETRY_QUADS; ++face)
        redstone_gate_side_uv(atlas, column, row, uv[face]);
}

static Texture2D rail_texture(const BlockRenderer *renderer, BlockId id)
{
    return id == BLOCK_POWERED_RAIL ? renderer->powered_rail_atlas :
           renderer->activator_rail_atlas;
}

static void rail_uv(Texture2D atlas, BlockState state, Vector2 uv[4])
{
    int column = state.data.rail.powered ? 1 : 0;
    redstone_gate_top_uv(atlas, column, 0,
                         (Direction)state.data.rail.facing, uv);
}

static bool rail_vertex_high(Direction facing, Vector3 point)
{
    switch (facing) {
    case DIR_EAST:  return point.x > 0.0f;
    case DIR_WEST:  return point.x < 0.0f;
    case DIR_SOUTH: return point.z > 0.0f;
    case DIR_NORTH: return point.z < 0.0f;
    default: return false;
    }
}

static void rail_geometry(BlockState state, Vector3 center,
                          Vector3 vertices[4])
{
    Direction facing = (Direction)state.data.rail.facing;
    float bottom = center.y - 0.5f + rail_offset;
    for (int vertex = 0; vertex < 4; ++vertex) {
        Vector3 point = cube_faces[4].vertices[vertex];
        float rise = state.data.rail.ascending &&
                     rail_vertex_high(facing, point) ? 1.0f : 0.0f;
        vertices[vertex] = (Vector3){
            center.x + point.x, bottom + rise, center.z + point.z
        };
    }
}

static void redstone_gate_geometry(
    Vector3 center,
    Vector3 vertices[REDSTONE_GATE_GEOMETRY_QUADS][4])
{
    float bottom = center.y - 0.5f;
    float top = bottom + redstone_gate_height;

    for (int vertex = 0; vertex < 4; ++vertex) {
        Vector3 point = cube_faces[4].vertices[vertex];
        vertices[0][vertex] = (Vector3){
            center.x + point.x, top, center.z + point.z
        };
    }
    for (int face = 0; face < 4; ++face)
        for (int vertex = 0; vertex < 4; ++vertex) {
            Vector3 point = cube_faces[face].vertices[vertex];
            vertices[face + 1][vertex] = (Vector3){
                center.x + point.x,
                point.y < 0.0f ? bottom : top,
                center.z + point.z
            };
        }
}

static void draw_redstone_gate(
    Texture2D atlas,
    Vector2 uv[REDSTONE_GATE_GEOMETRY_QUADS][4],
    Vector3 center)
{
    Vector3 vertices[REDSTONE_GATE_GEOMETRY_QUADS][4];
    redstone_gate_geometry(center, vertices);

    rlCheckRenderBatchLimit(REDSTONE_GATE_GEOMETRY_QUADS*4);
    rlSetTexture(atlas.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    for (int face = 0; face < REDSTONE_GATE_GEOMETRY_QUADS; ++face) {
        Vector3 normal = face ? cube_faces[face - 1].normal : cube_faces[4].normal;
        rlNormal3f(normal.x, normal.y, normal.z);
        for (int vertex = 0; vertex < 4; ++vertex) {
            rlTexCoord2f(uv[face][vertex].x, uv[face][vertex].y);
            rlVertex3f(vertices[face][vertex].x,
                       vertices[face][vertex].y,
                       vertices[face][vertex].z);
        }
    }
    rlEnd();
    rlSetTexture(0);
}

void block_renderer_draw_repeater(const BlockRenderer *renderer,
                                  BlockState state, Vector3 center)
{
    if (!renderer || !renderer->repeater_atlas.id ||
        state.id != BLOCK_REDSTONE_REPEATER) return;
    Vector2 uv[REDSTONE_GATE_GEOMETRY_QUADS][4];
    repeater_uv(renderer->repeater_atlas, state, uv);
    draw_redstone_gate(renderer->repeater_atlas, uv, center);
}

void block_renderer_draw_comparator(const BlockRenderer *renderer,
                                    BlockState state, Vector3 center)
{
    if (!renderer || !renderer->comparator_atlas.id ||
        state.id != BLOCK_REDSTONE_COMPARATOR) return;
    Vector2 uv[REDSTONE_GATE_GEOMETRY_QUADS][4];
    comparator_uv(renderer->comparator_atlas, state, uv);
    draw_redstone_gate(renderer->comparator_atlas, uv, center);
}

void block_renderer_draw_rail(const BlockRenderer *renderer, BlockState state,
                              Vector3 center)
{
    if (!renderer || (state.id != BLOCK_POWERED_RAIL &&
                      state.id != BLOCK_ACTIVATOR_RAIL)) return;
    Texture2D atlas = rail_texture(renderer, (BlockId)state.id);
    if (!atlas.id) return;
    Vector2 uv[4];
    Vector3 vertices[4];
    rail_uv(atlas, state, uv);
    rail_geometry(state, center, vertices);

    rlCheckRenderBatchLimit(4);
    rlSetTexture(atlas.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    rlNormal3f(0.0f, 1.0f, 0.0f);
    for (int vertex = 0; vertex < 4; ++vertex) {
        rlTexCoord2f(uv[vertex].x, uv[vertex].y);
        rlVertex3f(vertices[vertex].x, vertices[vertex].y,
                   vertices[vertex].z);
    }
    rlEnd();
    rlSetTexture(0);
}

static int hopper_geometry(const BlockRenderer *renderer, BlockState state,
                            Vector3 center, BlockGeometry *geometry);

int block_renderer_build_uv(const BlockRenderer *renderer, BlockState state,
                            unsigned solid_faces,
                            Vector2 uv[BLOCK_GEOMETRY_QUADS][4])
{
    int count = 0;
    if (state.id == BLOCK_HOPPER) {
        BlockGeometry geometry;
        count = hopper_geometry(renderer, state, (Vector3){0}, &geometry);
        memcpy(uv, geometry.uv, (size_t)count * sizeof(geometry.uv[0]));
        return count;
    }
    if (state.id == BLOCK_IRON || state.id == BLOCK_REDSTONE_BLOCK ||
        state.id == BLOCK_GLASS || state.id == BLOCK_SLIME ||
        state.id == BLOCK_NOTE_BLOCK || state.id == BLOCK_HONEY ||
        state.id == BLOCK_TARGET || state.id == BLOCK_REDSTONE_LAMP) {
        bool glass = state.id == BLOCK_GLASS;
        Texture2D texture = glass ? renderer->glass_texture : renderer->atlas;
        int tile = state.id == BLOCK_REDSTONE_BLOCK ? 1 :
                   state.id == BLOCK_SLIME ? SLIME_TILE :
                   state.id == BLOCK_NOTE_BLOCK ? NOTE_BLOCK_TILE :
                   state.id == BLOCK_HONEY ? HONEY_TILE :
                   state.id == BLOCK_TARGET ? TARGET_TILE :
                   state.id == BLOCK_REDSTONE_LAMP ?
                       (state.data.lamp.lit ? LAMP_ON_TILE : LAMP_OFF_TILE) : 0;
        float u0 = (tile*TILE_SIZE + 0.5f)/texture.width;
        float u1 = ((tile + 1)*TILE_SIZE - 0.5f)/texture.width;
        float v0 = 0.5f/texture.height;
        float v1 = (TILE_SIZE - 0.5f)/texture.height;
        Vector2 face_uv[4] = {{u0,v1},{u1,v1},{u1,v0},{u0,v0}};
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                memcpy(uv[count++], face_uv, sizeof(face_uv));
    } else if (state.id == BLOCK_OBSERVER) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                observer_face_uv(renderer->atlas, state, face, uv[count++]);
    } else if (state.id == BLOCK_DROPPER) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                dropper_face_uv(renderer->atlas, state, face, uv[count++]);
    } else if (state.id == BLOCK_BARREL) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                barrel_face_uv(renderer->atlas, state, face, uv[count++]);
    } else if (block_is_piston_base((BlockId)state.id)) {
        for (int face = 0; face < 6; ++face)
            if (solid_faces & (1u << face))
                piston_face_uv(renderer->atlas, state, face, uv[count++]);
    } else if (block_is_piston_head((BlockId)state.id)) {
        count = piston_head_uv(renderer->atlas, state, solid_faces, uv);
    } else if (state.id == BLOCK_REDSTONE_WIRE) {
        int mask = 0;
        for (int direction = 0; direction < 4; ++direction)
            if (state.data.wire.connections & (1u << wire_directions[direction]))
                mask |= 1 << direction;
        int power = state.data.wire.power;
        wire_uv(renderer->wire_atlas, power*WIRE_SHAPES + mask, uv[count++]);
        for (int direction = 0; direction < 4; ++direction) {
            unsigned bit = 1u << wire_directions[direction];
            if (state.data.wire.up_connections & state.data.wire.connections & bit)
                wire_uv(renderer->wire_atlas, WIRE_SIDE_FIRST + power, uv[count++]);
        }
    } else if (state.id == BLOCK_REDSTONE_TORCH) {
        int lit = state.data.torch.lit ? 1 : 0;
        for (int face = 0; face < 6; ++face) {
            Rectangle source = {0,0,TILE_SIZE,TILE_SIZE};
            if (face == 4) source = (Rectangle){14,7,4,4};
            if (face == 5) source = (Rectangle){14,28,4,4};
            torch_uv(renderer->torch_atlas, lit, source, uv[count++]);
        }
    } else if (state.id == BLOCK_LEVER) {
        solid_tile_uv(renderer->lever_atlas,
                      state.data.lever.powered ? 1 : 0, 0, uv[count++]);
    } else if (state.id == BLOCK_REDSTONE_REPEATER) {
        repeater_uv(renderer->repeater_atlas, state, uv);
        count = REDSTONE_GATE_GEOMETRY_QUADS;
    } else if (state.id == BLOCK_REDSTONE_COMPARATOR) {
        comparator_uv(renderer->comparator_atlas, state, uv);
        count = REDSTONE_GATE_GEOMETRY_QUADS;
    } else if (state.id == BLOCK_POWERED_RAIL ||
               state.id == BLOCK_ACTIVATOR_RAIL) {
        rail_uv(rail_texture(renderer, (BlockId)state.id), state, uv[0]);
        count = 1;
    }
    return count;
}

static void box_face_vertices(Vector3 center, Vector3 minimum,
                              Vector3 maximum, int face, Vector3 output[4])
{
    for (int vertex = 0; vertex < 4; ++vertex) {
        Vector3 point = cube_faces[face].vertices[vertex];
        output[vertex] = (Vector3){
            center.x + (point.x < 0.0f ? minimum.x : maximum.x),
            center.y + (point.y < 0.0f ? minimum.y : maximum.y),
            center.z + (point.z < 0.0f ? minimum.z : maximum.z)
        };
    }
}

static int hopper_geometry(const BlockRenderer *renderer, BlockState state,
                            Vector3 center, BlockGeometry *geometry)
{
    HopperBox boxes[HOPPER_BOX_COUNT];
    hopper_shape((Direction)state.data.hopper.facing, boxes);
    int count = 0;
    for (int b = 0; b < HOPPER_BOX_COUNT; ++b) {
        const HopperBox *box = &boxes[b];
        Vector3 minimum = {box->minimum[0]/16-0.5f, box->minimum[1]/16-0.5f, box->minimum[2]/16-0.5f};
        Vector3 maximum = {box->maximum[0]/16-0.5f, box->maximum[1]/16-0.5f, box->maximum[2]/16-0.5f};
        for (int face = 0; face < 6; ++face) {
            if (!(box->faces & (1u << face))) continue;
            box_face_vertices(center, minimum, maximum, face, geometry->vertices[count]);
            int tile = b == 0 && face == 4 ? HOPPER_INSIDE_TILE : HOPPER_OUTSIDE_TILE;
            for (int v = 0; v < 4; ++v) {
                Vector3 vertex = geometry->vertices[count][v];
                float x = vertex.x-center.x+0.5f;
                float y = vertex.y-center.y+0.5f;
                float z = vertex.z-center.z+0.5f;
                float u = face == 0 || face >= 4 ? x : face == 1 ? 1-x : face == 2 ? 1-z : z;
                float t = face == 4 ? z : face == 5 ? 1-z : 1-y;
                geometry->uv[count][v] = (Vector2){
                    (tile*TILE_SIZE+0.5f+u*(TILE_SIZE-1))/renderer->atlas.width,
                    (0.5f+t*(TILE_SIZE-1))/renderer->atlas.height};
            }
            ++count;
        }
    }
    geometry->quads = count;
    return count;
}

static void piston_base_bounds(BlockState state, Vector3 *minimum,
                               Vector3 *maximum)
{
    *minimum = (Vector3){-0.5f, -0.5f, -0.5f};
    *maximum = (Vector3){ 0.5f,  0.5f,  0.5f};
    if (!state.data.piston.extended) return;
    switch ((Direction)state.data.piston.facing) {
    case DIR_EAST:  maximum->x =  0.25f; break;
    case DIR_WEST:  minimum->x = -0.25f; break;
    case DIR_UP:    maximum->y =  0.25f; break;
    case DIR_DOWN:  minimum->y = -0.25f; break;
    case DIR_SOUTH: maximum->z =  0.25f; break;
    case DIR_NORTH: minimum->z = -0.25f; break;
    default: break;
    }
}

static Vector3 direction_vector(Direction direction)
{
    BlockPos offset = block_offset((BlockPos){0}, direction);
    return (Vector3){(float)offset.x, (float)offset.y, (float)offset.z};
}

static void lever_geometry(BlockState state, Vector3 center,
                           Vector3 vertices[4])
{
    Direction support = (Direction)state.data.lever.support_dir;
    Direction facing = (Direction)state.data.lever.facing;
    if ((unsigned)support >= DIR_COUNT) support = DIR_DOWN;
    if (!direction_horizontal(facing)) facing = DIR_NORTH;
    Vector3 toward_support = direction_vector(support);
    Vector3 outward = {-toward_support.x, -toward_support.y,
                       -toward_support.z};
    Vector3 up = direction_horizontal(support) ?
                 (Vector3){0, 1, 0} : direction_vector(facing);
    Vector3 right = {
        up.y*outward.z - up.z*outward.y,
        up.z*outward.x - up.x*outward.z,
        up.x*outward.y - up.y*outward.x
    };
    Vector3 plane = {
        center.x + toward_support.x*(0.5f - lever_offset),
        center.y + toward_support.y*(0.5f - lever_offset),
        center.z + toward_support.z*(0.5f - lever_offset)
    };
    static const float horizontal[] = {-0.5f, 0.5f, 0.5f, -0.5f};
    static const float vertical[] = {-0.5f, -0.5f, 0.5f, 0.5f};
    for (int vertex = 0; vertex < 4; ++vertex) {
        vertices[vertex] = (Vector3){
            plane.x + right.x*horizontal[vertex] + up.x*vertical[vertex],
            plane.y + right.y*horizontal[vertex] + up.y*vertical[vertex],
            plane.z + right.z*horizontal[vertex] + up.z*vertical[vertex]
        };
    }
}

static void piston_head_bounds(Direction facing,
                               Vector3 *plate_minimum,
                               Vector3 *plate_maximum,
                               Vector3 *rod_minimum,
                               Vector3 *rod_maximum)
{
    *plate_minimum = (Vector3){-0.5f, -0.5f, -0.5f};
    *plate_maximum = (Vector3){ 0.5f,  0.5f,  0.5f};
    /* A slim 3/16-wide wooden arm. It reaches 4/16 into the shortened base,
     * so the head never appears detached from the piston body. */
    *rod_minimum = (Vector3){-0.09375f, -0.09375f, -0.09375f};
    *rod_maximum = (Vector3){ 0.09375f,  0.09375f,  0.09375f};
    switch (facing) {
    case DIR_EAST:
        plate_minimum->x = 0.25f;
        rod_minimum->x = -0.75f; rod_maximum->x = 0.25f;
        break;
    case DIR_WEST:
        plate_maximum->x = -0.25f;
        rod_minimum->x = -0.25f; rod_maximum->x = 0.75f;
        break;
    case DIR_UP:
        plate_minimum->y = 0.25f;
        rod_minimum->y = -0.75f; rod_maximum->y = 0.25f;
        break;
    case DIR_DOWN:
        plate_maximum->y = -0.25f;
        rod_minimum->y = -0.25f; rod_maximum->y = 0.75f;
        break;
    case DIR_SOUTH:
        plate_minimum->z = 0.25f;
        rod_minimum->z = -0.75f; rod_maximum->z = 0.25f;
        break;
    case DIR_NORTH:
        plate_maximum->z = -0.25f;
        rod_minimum->z = -0.25f; rod_maximum->z = 0.75f;
        break;
    default: break;
    }
}

void block_renderer_build_geometry(const BlockRenderer *renderer, BlockState state,
                                   Vector3 center, unsigned solid_faces,
                                   BlockGeometry *geometry)
{
    if (state.id == BLOCK_HOPPER) {
        (void)hopper_geometry(renderer, state, center, geometry);
        return;
    }
    geometry->quads = block_renderer_build_uv(renderer, state, solid_faces, geometry->uv);
    int count = 0;
    if (state.id == BLOCK_IRON || state.id == BLOCK_REDSTONE_BLOCK ||
        state.id == BLOCK_GLASS || state.id == BLOCK_OBSERVER ||
        state.id == BLOCK_BARREL || state.id == BLOCK_SLIME ||
        state.id == BLOCK_NOTE_BLOCK || state.id == BLOCK_HONEY ||
        state.id == BLOCK_TARGET || state.id == BLOCK_DROPPER ||
        state.id == BLOCK_REDSTONE_LAMP) {
        for (int face = 0; face < 6; ++face) {
            if (!(solid_faces & (1u << face))) continue;
            for (int vertex = 0; vertex < 4; ++vertex) {
                Vector3 p = cube_faces[face].vertices[vertex];
                geometry->vertices[count][vertex] = (Vector3){center.x+p.x,center.y+p.y,center.z+p.z};
            }
            ++count;
        }
    } else if (block_is_piston_base((BlockId)state.id)) {
        Vector3 minimum, maximum;
        piston_base_bounds(state, &minimum, &maximum);
        for (int face = 0; face < 6; ++face) {
            if (!(solid_faces & (1u << face))) continue;
            box_face_vertices(center, minimum, maximum, face,
                              geometry->vertices[count++]);
        }
    } else if (block_is_piston_head((BlockId)state.id)) {
        static const Direction directions[6] = {
            DIR_SOUTH, DIR_NORTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN
        };
        Direction facing = (Direction)state.data.piston.facing;
        Vector3 plate_minimum, plate_maximum, rod_minimum, rod_maximum;
        piston_head_bounds(facing, &plate_minimum, &plate_maximum,
                           &rod_minimum, &rod_maximum);
        for (int face = 0; face < 6; ++face) {
            if (!(solid_faces & (1u << face))) continue;
            box_face_vertices(center, plate_minimum, plate_maximum, face,
                              geometry->vertices[count++]);
        }
        for (int face = 0; face < 6; ++face) {
            Direction face_direction = directions[face];
            if (face_direction == facing ||
                face_direction == direction_opposite(facing)) continue;
            box_face_vertices(center, rod_minimum, rod_maximum, face,
                              geometry->vertices[count++]);
        }
    } else if (state.id == BLOCK_REDSTONE_WIRE) {
        for (int vertex = 0; vertex < 4; ++vertex) {
            Vector3 p = cube_faces[4].vertices[vertex];
            geometry->vertices[0][vertex] = (Vector3){center.x+p.x,center.y-1+wire_offset+p.y,center.z+p.z};
        }
        count = 1;
        static const int side_faces[] = {3,2,1,0};
        static const Vector3 offsets[] = {{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
        for (int direction = 0; direction < 4; ++direction) {
            unsigned bit = 1u << wire_directions[direction];
            if (!(state.data.wire.up_connections & state.data.wire.connections & bit)) continue;
            const CubeFace *face = &cube_faces[side_faces[direction]];
            for (int vertex = 0; vertex < 4; ++vertex) {
                Vector3 p = face->vertices[vertex];
                if (p.y < 0) {
                    p.x += face->normal.x*wire_offset;
                    p.z += face->normal.z*wire_offset;
                }
                geometry->vertices[count][vertex] = (Vector3){
                    center.x+offsets[direction].x+p.x,
                    center.y+wire_offset+p.y,center.z+offsets[direction].z+p.z};
            }
            ++count;
        }
    } else if (state.id == BLOCK_REDSTONE_TORCH) {
        TorchTransform transform = torch_transform(center, (Direction)state.data.torch.support_dir);
        for (int face = 0; face < 6; ++face)
            for (int vertex = 0; vertex < 4; ++vertex) {
                Vector3 p = torch_rotate(transform, torch_faces[face].vertices[vertex]);
                geometry->vertices[face][vertex] = (Vector3){transform.origin.x+p.x,transform.origin.y+p.y,transform.origin.z+p.z};
            }
    } else if (state.id == BLOCK_LEVER) {
        lever_geometry(state, center, geometry->vertices[0]);
    } else if (state.id == BLOCK_REDSTONE_REPEATER) {
        redstone_gate_geometry(center, geometry->vertices);
    } else if (state.id == BLOCK_REDSTONE_COMPARATOR) {
        redstone_gate_geometry(center, geometry->vertices);
    } else if (state.id == BLOCK_POWERED_RAIL ||
               state.id == BLOCK_ACTIVATOR_RAIL) {
        rail_geometry(state, center, geometry->vertices[0]);
    }
}
