#ifndef MC_WORLD_MSAA_H
#define MC_WORLD_MSAA_H

typedef struct {
    unsigned int id, color, depth;
    int width, height;
} WorldMsaa;

/* An unavailable target has id == 0. Cache the attempted size to avoid
 * retrying an unsupported allocation every frame. */
void world_msaa_prepare(WorldMsaa *target, int width, int height);
void world_msaa_destroy(WorldMsaa *target);

/* Resolve color into an existing single-sample target of the same size
 * and leave that framebuffer bound for the 2D overlay. */
void world_msaa_resolve(const WorldMsaa *target, unsigned int destination);

#endif
