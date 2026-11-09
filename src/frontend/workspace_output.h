#ifndef MC_WORKSPACE_OUTPUT_H
#define MC_WORKSPACE_OUTPUT_H

#include "raylib.h"
#include <stddef.h>

typedef struct WorkspaceOutput WorkspaceOutput;
typedef void (*WorkspaceOutputReceiver)(void *source, const char *text,
                                        bool error, bool finished);

WorkspaceOutput *workspace_output_create(void);
void workspace_output_destroy(WorkspaceOutput *output);
void workspace_output_message(WorkspaceOutput *output, const char *text, bool error);
bool workspace_output_start(WorkspaceOutput *output, const char *command,
    const char *label, void *source, WorkspaceOutputReceiver receiver,
    char *error, size_t error_capacity);
/* Arguments are passed literally; directory is the child's working directory. */
bool workspace_output_start_program(WorkspaceOutput *output,
    const char *const *arguments, const char *directory, const char *label,
    void *source, WorkspaceOutputReceiver receiver,
    char *error, size_t error_capacity);
void workspace_output_detach(WorkspaceOutput *output, void *source);
bool workspace_output_is_running(const WorkspaceOutput *output);
/* Argument-based program tasks (:make/:run/:model), including cancellation cleanup. */
bool workspace_output_is_program_running(const WorkspaceOutput *output);
void workspace_output_poll(WorkspaceOutput *output);
void workspace_output_cancel(WorkspaceOutput *output);
void workspace_output_update(WorkspaceOutput *output);
/* Routed by the workspace to the hovered output pane, independently of focus. */
void workspace_output_scroll(WorkspaceOutput *output, Vector2 wheel);
void workspace_output_draw(WorkspaceOutput *output, Rectangle box, int pane,
                            bool focused, bool zoomed);

#endif
