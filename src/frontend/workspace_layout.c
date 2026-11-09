#include "workspace_layout.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static int larger(int a, int b) { return a > b ? a : b; }
static int smaller(int a, int b) { return a < b ? a : b; }
static int first_pane(const WorkspaceLayout *layout, int node)
{
    if (node < 0) return -1;
    const WorkspaceNode *n = &layout->nodes[node];
    return n->first < 0 ? n->pane : first_pane(layout, n->first);
}
static int pane_node(const WorkspaceLayout *layout, int pane)
{
    for (int i = 0; i < WORKSPACE_NODE_LIMIT; ++i)
        if (layout->nodes[i].used && layout->nodes[i].first < 0 &&
            layout->nodes[i].pane == pane) return i;
    return -1;
}
static int free_node(const WorkspaceLayout *layout)
{
    for (int i = 0; i < WORKSPACE_NODE_LIMIT; ++i)
        if (!layout->nodes[i].used) return i;
    return -1;
}
static void leaf(WorkspaceLayout *layout, int node, int pane, int parent)
{
    layout->nodes[node] = (WorkspaceNode){.used=true, .parent=parent,
        .first=-1, .second=-1, .pane=pane, .ratio=5000};
}
bool workspace_layout_has_pane(const WorkspaceLayout *layout, int pane)
{
    return pane == 0 ? layout->gui_visible : pane_node(layout, pane) >= 0;
}
void workspace_layout_init(WorkspaceLayout *layout)
{
    memset(layout, 0, sizeof(*layout));
    layout->gui_visible = true;
    layout->zoom = -1;
    layout->root = -1;
}
static int minimum(const WorkspaceLayout *layout, int node, bool x)
{
    const WorkspaceNode *n = &layout->nodes[node];
    if (n->first < 0) return x ? WORKSPACE_MIN_WIDTH : WORKSPACE_MIN_HEIGHT;
    int a = minimum(layout, n->first, x), b = minimum(layout, n->second, x);
    return x == n->split_x ? a + b + WORKSPACE_DIVIDER : larger(a, b);
}
static void child_rects(const WorkspaceLayout *layout, int node, WorkspaceRect box,
                         WorkspaceRect *a, WorkspaceRect *b)
{
    const WorkspaceNode *n = &layout->nodes[node];
    *a = *b = box;
    int total = (n->split_x ? box.width : box.height) - WORKSPACE_DIVIDER;
    if (total < 0) total = 0;
    int length = (int)((int64_t)total*n->ratio/10000);
    int low = minimum(layout, n->first, n->split_x);
    int high = total - minimum(layout, n->second, n->split_x);
    if (high >= low) length = larger(low, smaller(high, length));
    if (n->split_x) {
        a->width = length; b->x += length + WORKSPACE_DIVIDER; b->width = total - length;
    } else {
        a->height = length; b->y += length + WORKSPACE_DIVIDER; b->height = total - length;
    }
}
static void geometry_node(const WorkspaceLayout *layout, int node, WorkspaceRect box,
                            WorkspaceGeometry *geometry)
{
    const WorkspaceNode *n = &layout->nodes[node];
    if (n->first < 0) {
        geometry->panes[n->pane - 1] = box;
        geometry->visible[n->pane - 1] = true;
        return;
    }
    WorkspaceRect a, b;
    child_rects(layout, node, box, &a, &b);
    geometry_node(layout, n->first, a, geometry);
    geometry_node(layout, n->second, b, geometry);
}
void workspace_layout_geometry(const WorkspaceLayout *layout, int width, int height,
                                WorkspaceGeometry *geometry)
{
    memset(geometry, 0, sizeof(*geometry));
    width = larger(1, width); height = larger(1, height);
    if (layout->root < 0) {
        if (layout->gui_visible) geometry->gui = (WorkspaceRect){0, 0, width, height};
        return;
    }
    int available = larger(1, width - WORKSPACE_DIVIDER - WORKSPACE_MIN_WIDTH);
    int gui_width = layout->gui_visible ? smaller(larger(1, layout->gui_width), available) : 0;
    geometry->gui = (WorkspaceRect){0, 0, gui_width, height};
    int start = layout->gui_visible ? smaller(width, gui_width + WORKSPACE_DIVIDER) : 0;
    geometry->text = (WorkspaceRect){start, 0, width - start, height};
    geometry->compact = minimum(layout, layout->root, true) > geometry->text.width ||
                        minimum(layout, layout->root, false) > geometry->text.height;
    int zoom = layout->zoom;
    if (geometry->compact && zoom < 0)
        zoom = layout->focus > 0 ? layout->focus : first_pane(layout, layout->root);
    if (zoom > 0) {
        geometry->panes[zoom - 1] = geometry->text;
        geometry->visible[zoom - 1] = true;
    } else geometry_node(layout, layout->root, geometry->text, geometry);
}
int workspace_layout_split(WorkspaceLayout *layout, bool split_x, int width, int height)
{
    int pane;
    for (pane = 1; pane <= WORKSPACE_TEXT_LIMIT; ++pane)
        if (!workspace_layout_has_pane(layout, pane)) break;
    if (pane > WORKSPACE_TEXT_LIMIT) return -1;
    if (layout->root < 0) {
        if (layout->gui_visible && layout->gui_width <= 0) layout->gui_width = width;
        layout->root = free_node(layout);
        leaf(layout, layout->root, pane, -1);
        layout->focus = pane;
        return pane;
    }
    WorkspaceGeometry geometry;
    workspace_layout_geometry(layout, width, height, &geometry);
    if (geometry.compact) return -1;
    /* Unzooming and splitting must also fit the original pane's geometry. */
    WorkspaceLayout unzoomed = *layout;
    unzoomed.zoom = -1;
    workspace_layout_geometry(&unzoomed, width, height, &geometry);
    int target = layout->focus > 0 ? layout->focus : first_pane(layout, layout->root);
    WorkspaceRect box = geometry.panes[target - 1];
    if ((split_x ? box.width : box.height) <
        2*(split_x ? WORKSPACE_MIN_WIDTH : WORKSPACE_MIN_HEIGHT) + WORKSPACE_DIVIDER) return -1;
    int node = pane_node(layout, target);
    WorkspaceNode old = layout->nodes[node];
    int a = free_node(layout);
    leaf(layout, a, target, node);
    int b = free_node(layout);
    leaf(layout, b, pane, node);
    layout->nodes[node] = (WorkspaceNode){.used=true, .split_x=split_x,
        .parent=old.parent, .first=a, .second=b, .pane=-1, .ratio=5000};
    layout->focus = pane;
    layout->zoom = -1;
    return pane;
}
bool workspace_layout_close(WorkspaceLayout *layout)
{
    if (!layout->focus) {
        if (layout->root < 0) return false;
        layout->gui_visible = false;
        layout->focus = first_pane(layout, layout->root);
        return true;
    }
    int node = pane_node(layout, layout->focus);
    if (node < 0) return false;
    int parent = layout->nodes[node].parent;
    if (parent < 0) {
        if (!layout->gui_visible) return false;
        layout->nodes[node].used = false;
        layout->root = -1;
        layout->focus = 0;
    } else {
        int sibling = layout->nodes[parent].first == node ?
                      layout->nodes[parent].second : layout->nodes[parent].first;
        int grand = layout->nodes[parent].parent;
        if (grand < 0) layout->root = sibling;
        else if (layout->nodes[grand].first == parent) layout->nodes[grand].first = sibling;
        else layout->nodes[grand].second = sibling;
        layout->nodes[sibling].parent = grand;
        layout->nodes[node].used = layout->nodes[parent].used = false;
        layout->focus = first_pane(layout, sibling);
    }
    layout->zoom = -1;
    return true;
}
static void focus_pane(WorkspaceLayout *layout, int pane)
{
    layout->focus = pane;
    if (layout->zoom > 0 && pane > 0) layout->zoom = pane;
}
void workspace_layout_next(WorkspaceLayout *layout)
{
    for (int i = 1; i <= WORKSPACE_TEXT_LIMIT + 1; ++i) {
        int pane = (layout->focus + i) % (WORKSPACE_TEXT_LIMIT + 1);
        if (workspace_layout_has_pane(layout, pane)) { focus_pane(layout, pane); return; }
    }
}
bool workspace_layout_focus(WorkspaceLayout *layout, WorkspaceDirection direction,
                             int width, int height)
{
    WorkspaceGeometry geometry;
    workspace_layout_geometry(layout, width, height, &geometry);
    WorkspaceRect from = layout->focus ? geometry.panes[layout->focus - 1] : geometry.gui;
    int64_t x = 2LL*from.x + from.width, y = 2LL*from.y + from.height;
    int best = -1;
    int64_t score = INT64_MAX;
    for (int pane = 0; pane <= WORKSPACE_TEXT_LIMIT; ++pane) {
        if (pane == layout->focus || !workspace_layout_has_pane(layout, pane) ||
            (pane && !geometry.visible[pane - 1])) continue;
        WorkspaceRect to = pane ? geometry.panes[pane - 1] : geometry.gui;
        int64_t dx = 2LL*to.x + to.width - x, dy = 2LL*to.y + to.height - y;
        if ((direction == WORKSPACE_LEFT && dx >= 0) ||
            (direction == WORKSPACE_RIGHT && dx <= 0) ||
            (direction == WORKSPACE_UP && dy >= 0) ||
            (direction == WORKSPACE_DOWN && dy <= 0)) continue;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        int64_t distance = direction <= WORKSPACE_RIGHT ? dx + 4*dy : dy + 4*dx;
        if (distance < score) { score = distance; best = pane; }
    }
    if (best < 0) return false;
    focus_pane(layout, best);
    return true;
}
static bool find_rect(const WorkspaceLayout *layout, int node, int target,
                       WorkspaceRect box, WorkspaceRect *found)
{
    if (node == target) { *found = box; return true; }
    const WorkspaceNode *n = &layout->nodes[node];
    if (n->first < 0) return false;
    WorkspaceRect a, b;
    child_rects(layout, node, box, &a, &b);
    return find_rect(layout, n->first, target, a, found) ||
           find_rect(layout, n->second, target, b, found);
}
bool workspace_layout_resize(WorkspaceLayout *layout, WorkspaceDirection direction,
                              int pixels, int width, int height)
{
    if (layout->focus <= 0 || layout->zoom > 0 || pixels <= 0) return false;
    WorkspaceGeometry geometry;
    workspace_layout_geometry(layout, width, height, &geometry);
    if (geometry.compact) return false;
    int node = pane_node(layout, layout->focus);
    bool x = direction <= WORKSPACE_RIGHT;
    while (node >= 0) {
        node = layout->nodes[node].parent;
        if (node >= 0 && layout->nodes[node].split_x == x) break;
    }
    if (node < 0) return false;
    WorkspaceRect box;
    if (!find_rect(layout, layout->root, node, geometry.text, &box)) return false;
    WorkspaceNode *n = &layout->nodes[node];
    int total = (x ? box.width : box.height) - WORKSPACE_DIVIDER;
    int low = minimum(layout, n->first, x), high = total - minimum(layout, n->second, x);
    int length = larger(low, smaller(high, (int)((int64_t)total*n->ratio/10000)));
    int delta = direction == WORKSPACE_LEFT || direction == WORKSPACE_UP ? -pixels : pixels;
    int next = larger(low, smaller(high, length + delta));
    if (next == length) return false;
    n->ratio = (int)(((int64_t)next*10000 + total/2)/total);
    return true;
}
bool workspace_layout_zoom(WorkspaceLayout *layout)
{
    if (!layout->focus) return false;
    layout->zoom = layout->zoom < 0 ? layout->focus : -1;
    return true;
}
void workspace_layout_show_gui(WorkspaceLayout *layout)
{
    layout->gui_visible = true;
    layout->focus = 0;
}

static int divider_at(const WorkspaceLayout *layout, int node, WorkspaceRect box,
                       int x, int y)
{
    const WorkspaceNode *n = &layout->nodes[node];
    if (n->first < 0 || x < box.x || y < box.y ||
        x >= box.x + box.width || y >= box.y + box.height) return -1;
    WorkspaceRect a, b;
    child_rects(layout, node, box, &a, &b);
    int edge = n->split_x ? b.x : b.y;
    int point = n->split_x ? x : y;
    if (point >= edge - WORKSPACE_DIVIDER - 2 && point < edge + 2) return node;
    int found = divider_at(layout, n->first, a, x, y);
    return found >= 0 ? found : divider_at(layout, n->second, b, x, y);
}

int workspace_layout_divider_at(const WorkspaceLayout *layout, int width, int height,
                                int x, int y)
{
    if (x < 0 || y < 0 || x >= width || y >= height || layout->root < 0) return -1;
    WorkspaceGeometry geometry;
    workspace_layout_geometry(layout, width, height, &geometry);
    if (layout->gui_visible && x >= geometry.text.x - WORKSPACE_DIVIDER - 2 &&
        x < geometry.text.x + 2) return -2;
    if (layout->zoom > 0 || geometry.compact) return -1;
    return divider_at(layout, layout->root, geometry.text, x, y);
}

bool workspace_layout_drag_divider(WorkspaceLayout *layout, int divider,
                                   int position, int width, int height)
{
    if (layout->root < 0) return false;
    WorkspaceGeometry geometry;
    workspace_layout_geometry(layout, width, height, &geometry);
    if (divider == -2) {
        if (!layout->gui_visible) return false;
        int high = width - WORKSPACE_DIVIDER - minimum(layout, layout->root, true);
        int low = 640;
        if (high < low) return false;
        int next = larger(low, smaller(high, position));
        if (layout->gui_width == next) return false;
        layout->gui_width = next;
        return true;
    }
    if (divider < 0 || divider >= WORKSPACE_NODE_LIMIT || layout->zoom > 0 ||
        geometry.compact || !layout->nodes[divider].used ||
        layout->nodes[divider].first < 0) return false;
    WorkspaceRect box;
    if (!find_rect(layout, layout->root, divider, geometry.text, &box)) return false;
    WorkspaceNode *n = &layout->nodes[divider];
    int total = (n->split_x ? box.width : box.height) - WORKSPACE_DIVIDER;
    int low = minimum(layout, n->first, n->split_x);
    int high = total - minimum(layout, n->second, n->split_x);
    if (high < low || total <= 0) return false;
    int length = position - (n->split_x ? box.x : box.y);
    length = larger(low, smaller(high, length));
    int ratio = (int)(((int64_t)length*10000 + total/2)/total);
    if (n->ratio == ratio) return false;
    n->ratio = ratio;
    return true;
}
