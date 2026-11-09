#ifndef MC_WORKSPACE_EDITOR_H
#define MC_WORKSPACE_EDITOR_H

#include "raylib.h"

typedef struct WorkspaceEditor WorkspaceEditor;
typedef struct WorkspaceOutput WorkspaceOutput;
typedef struct WorkspaceCode WorkspaceCode;
typedef enum {
    WORKSPACE_EDITOR_CONTINUE,
    WORKSPACE_EDITOR_QUIT,
    WORKSPACE_EDITOR_QUIT_FORCE
} WorkspaceEditorAction;
/* Called before launching :run; false aborts. :make does not call it. */
typedef bool (*WorkspaceEditorPrepareRun)(void *context,
    WorkspaceEditor *editor);

WorkspaceEditor *workspace_editor_create(void);
void workspace_editor_destroy(WorkspaceEditor *editor);
void workspace_editor_set_output(WorkspaceEditor *editor, WorkspaceOutput *output,
                                  int pane);
void workspace_editor_set_code(WorkspaceEditor *editor, WorkspaceCode *code);
bool workspace_editor_has_unsaved(const WorkspaceEditor *editor);
WorkspaceEditorAction workspace_editor_update(WorkspaceEditor *editor,
    WorkspaceEditorPrepareRun prepare_run, void *context);
typedef struct {
    Vector2 position, wheel;
    bool pressed, down, released;
    double time;
} WorkspaceEditorPointer;
/* Call for the hovered pane, or the pane owning an ongoing drag. Returns true
 * while a text drag owns the pointer, even outside the pane rectangle. */
bool workspace_editor_pointer(WorkspaceEditor *editor, Rectangle box,
                              WorkspaceEditorPointer pointer);
void workspace_editor_cancel_pointer(WorkspaceEditor *editor);
void workspace_editor_draw(WorkspaceEditor *editor, Rectangle box, int pane,
                           bool focused, bool zoomed);
void workspace_editor_message(WorkspaceEditor *editor, const char *text,
                              bool error);

#endif
