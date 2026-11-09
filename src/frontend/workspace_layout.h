#ifndef MC_WORKSPACE_LAYOUT_H
#define MC_WORKSPACE_LAYOUT_H

#include <stdbool.h>

enum { WORKSPACE_TEXT_LIMIT = 8, WORKSPACE_NODE_LIMIT = 15,
       WORKSPACE_DIVIDER = 3, WORKSPACE_MIN_WIDTH = 180,
       WORKSPACE_MIN_HEIGHT = 112, WORKSPACE_TEXT_WIDTH = 620};
typedef struct { int x, y, width, height; } WorkspaceRect;
typedef enum { WORKSPACE_LEFT, WORKSPACE_RIGHT, WORKSPACE_UP, WORKSPACE_DOWN } WorkspaceDirection;
typedef struct {
    bool used, split_x;
    int parent, first, second, pane, ratio;
} WorkspaceNode;
typedef struct {
    WorkspaceNode nodes[WORKSPACE_NODE_LIMIT];
    int root, focus, zoom; /* pane 0 is GUI; text panes are 1..8; zoom -1 is off */
    int gui_width; /* Remember the GUI width while text panes occupy the right side. */
    bool gui_visible;
} WorkspaceLayout;
typedef struct {
    WorkspaceRect gui, text, panes[WORKSPACE_TEXT_LIMIT];
    bool visible[WORKSPACE_TEXT_LIMIT], compact;
} WorkspaceGeometry;

/* Start with one GUI pane; text nodes are allocated by the first split. */
void workspace_layout_init(WorkspaceLayout *layout);
void workspace_layout_geometry(const WorkspaceLayout *layout, int width, int height,
                                WorkspaceGeometry *geometry);
bool workspace_layout_has_pane(const WorkspaceLayout *layout, int pane);
/* Split only the text-pane tree. GUI focus targets its first pane (or creates one).
 * Return the new pane ID, or -1 without changing layout when there is no room. */
int workspace_layout_split(WorkspaceLayout *layout, bool split_x, int width, int height);
bool workspace_layout_close(WorkspaceLayout *layout);
void workspace_layout_next(WorkspaceLayout *layout);
bool workspace_layout_focus(WorkspaceLayout *layout, WorkspaceDirection direction,
                             int width, int height);
bool workspace_layout_resize(WorkspaceLayout *layout, WorkspaceDirection direction,
                              int pixels, int width, int height);
bool workspace_layout_zoom(WorkspaceLayout *layout);
void workspace_layout_show_gui(WorkspaceLayout *layout);
/* Divider hit testing uses window coordinates. -1 means none; -2 is the
 * GUI/text divider; nonnegative values identify text-tree split nodes. */
int workspace_layout_divider_at(const WorkspaceLayout *layout, int width, int height,
                                int x, int y);
bool workspace_layout_drag_divider(WorkspaceLayout *layout, int divider,
                                   int position, int width, int height);

#endif
