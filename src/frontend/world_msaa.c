#include "world_msaa.h"
#include "rlgl.h"
#include "external/glad.h"

enum { WORLD_MSAA_SAMPLES = 4 };

void world_msaa_destroy(WorldMsaa *target)
{
    if (target->id) glDeleteFramebuffers(1, &target->id);
    if (target->color) glDeleteRenderbuffers(1, &target->color);
    if (target->depth) glDeleteRenderbuffers(1, &target->depth);
    *target = (WorldMsaa){0};
}

void world_msaa_prepare(WorldMsaa *target, int width, int height)
{
    if (target->width == width && target->height == height) return;
    world_msaa_destroy(target);
    target->width = width;
    target->height = height;
    if (width <= 0 || height <= 0) return;

    GLint max_samples = 0, max_size = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
    if (max_samples < WORLD_MSAA_SAMPLES || width > max_size || height > max_size) return;

    WorldMsaa next = {.width = width, .height = height};
    glGenFramebuffers(1, &next.id);
    glGenRenderbuffers(1, &next.color);
    glGenRenderbuffers(1, &next.depth);
    if (!next.id || !next.color || !next.depth) {
        world_msaa_destroy(&next);
        return;
    }

    GLint draw = 0, read = 0, renderbuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, next.id);
    glBindRenderbuffer(GL_RENDERBUFFER, next.color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, WORLD_MSAA_SAMPLES,
                                     GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, next.color);
    glBindRenderbuffer(GL_RENDERBUFFER, next.depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, WORLD_MSAA_SAMPLES,
                                     GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, next.depth);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)renderbuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    if (complete) *target = next;
    else world_msaa_destroy(&next);
}

void world_msaa_resolve(const WorldMsaa *target, unsigned int destination)
{
    rlBindFramebuffer(RL_READ_FRAMEBUFFER, target->id);
    rlBindFramebuffer(RL_DRAW_FRAMEBUFFER, destination);
    rlBlitFramebuffer(0, 0, target->width, target->height,
                      0, 0, target->width, target->height, GL_COLOR_BUFFER_BIT);
    rlEnableFramebuffer(destination);
}
