#ifndef MC_BLOCK_RENDERER_H
#define MC_BLOCK_RENDERER_H

#include "raylib.h"
#include "rs/block.h"

typedef struct{
    Texture2D atlas;
    Texture2D iron_texture;
    Texture2D glass_texture;
    Texture2D wire_atlas;
    Texture2D torch_atlas;
    Texture2D repeater_atlas;
    Texture2D comparator_atlas;
    Texture2D powered_rail_atlas;
    Texture2D activator_rail_atlas;
    Texture2D lever_atlas;
    Shader cutout_shader;
} BlockRenderer;

enum {
    BLOCK_GEOMETRY_QUADS = 32,
    REDSTONE_GATE_GEOMETRY_QUADS = 5
};
typedef struct {
    Vector3 vertices[BLOCK_GEOMETRY_QUADS][4];
    Vector2 uv[BLOCK_GEOMETRY_QUADS][4];
    int quads;
} BlockGeometry;

bool block_renderer_init(BlockRenderer *renderer);
void block_renderer_unload(BlockRenderer *renderer);
void block_renderer_draw_cube(const BlockRenderer *renderer, BlockId id, Vector3 center);

/* Draw glass and redstone cutouts after opaque solids. */
void block_renderer_begin_cutouts(const BlockRenderer *renderer);
void block_renderer_draw_wire(const BlockRenderer *renderer, BlockState state,
                              Vector3 center);
void block_renderer_draw_torch(const BlockRenderer *renderer, BlockState state,
                               Vector3 center);
void block_renderer_draw_repeater(const BlockRenderer *renderer, BlockState state,
                                  Vector3 center);
void block_renderer_draw_comparator(const BlockRenderer *renderer,
                                    BlockState state, Vector3 center);
void block_renderer_draw_rail(const BlockRenderer *renderer, BlockState state,
                              Vector3 center);
void block_renderer_end_cutouts(void);

/* CPU geometry for persistent meshes. Solid face bits follow the renderer's
 * +Z,-Z,+X,-X,+Y,-Y order; cutouts ignore the mask. UV generation does not
 * calculate positions, so state changes can update only the GPU UV buffer. */
int block_renderer_build_uv(const BlockRenderer *renderer, BlockState state,
                            unsigned solid_faces,
                            Vector2 uv[BLOCK_GEOMETRY_QUADS][4]);
void block_renderer_build_geometry(const BlockRenderer *renderer, BlockState state,
                                   Vector3 center, unsigned solid_faces,
                                   BlockGeometry *geometry);

#endif
