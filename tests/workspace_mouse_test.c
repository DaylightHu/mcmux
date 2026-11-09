/* Headless input/geometry regression tests. Run with ./nob test-workspace. */
#include "../src/frontend/workspace_editor.c"
#include "../src/frontend/workspace_output.c"
#include "../src/frontend/workspace_layout.c"
#include "../src/frontend/gui.c"
#include <assert.h>

static Vector2 test_mouse, test_wheel;
static bool test_pressed, test_down, test_released, test_focused = true;
static bool test_keys[512];
static int test_character, test_cursor, test_captures;
static char test_clipboard[4096];
bool IsWindowFocused(void) { return test_focused; }
int GetScreenWidth(void) { return 1800; }
int GetScreenHeight(void) { return 900; }
Vector2 GetMousePosition(void) { return test_mouse; }
Vector2 GetMouseWheelMoveV(void) { return test_wheel; }
bool IsMouseButtonPressed(int b) { return b == MOUSE_BUTTON_LEFT && test_pressed; }
bool IsMouseButtonDown(int b) { return b == MOUSE_BUTTON_LEFT && test_down; }
bool IsMouseButtonReleased(int b) { return b == MOUSE_BUTTON_LEFT && test_released; }
bool IsKeyPressed(int k) { return k < 512 && test_keys[k]; }
bool IsKeyDown(int k) { (void)k; return false; }
bool IsKeyPressedRepeat(int k) { (void)k; return false; }
int GetCharPressed(void) { int c=test_character; test_character=0; return c; }
double GetTime(void) { return 1.0; }
void EnableCursor(void) {}
void DisableCursor(void) { ++test_captures; }
void SetMouseCursor(int c) { test_cursor=c; }
void SetWindowMinSize(int w,int h) { (void)w; (void)h; }
bool CheckCollisionPointRec(Vector2 p,Rectangle r)
{ return p.x>=r.x && p.y>=r.y && p.x<r.x+r.width && p.y<r.y+r.height; }
const char *GetClipboardText(void) { return test_clipboard; }
void SetClipboardText(const char *s) { snprintf(test_clipboard,sizeof(test_clipboard),"%s",s); }
const char *CodepointToUTF8(int c,int *size)
{ static char s[2]; s[0]=(char)c; s[1]=0; *size=1; return s; }
int ui_text_width(const char *s,int size) { return (int)strlen(s)*(size/2); }
void ui_text_draw(const char *s,int x,int y,int size,Color c)
{ (void)s; (void)x; (void)y; (void)size; (void)c; }
void DrawRectangle(int x,int y,int w,int h,Color c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; }
void DrawRectangleRec(Rectangle r,Color c) { (void)r; (void)c; }
void DrawRectangleLines(int x,int y,int w,int h,Color c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; }
void DrawRectangleLinesEx(Rectangle r,float t,Color c) { (void)r; (void)t; (void)c; }
void BeginScissorMode(int x,int y,int w,int h) { (void)x; (void)y; (void)w; (void)h; }
void EndScissorMode(void) {}
void workspace_code_detach(WorkspaceCode *c,void *s) { (void)c; (void)s; }
bool workspace_code_query(WorkspaceCode *c,const char *root,
 const WorkspaceCodeDocument *d,size_t n,size_t a,size_t row,size_t col,
 WorkspaceCodeKind k,void *s,WorkspaceCodeReceiver r,char *e,size_t cap)
{ (void)c; (void)root; (void)d; (void)n; (void)a; (void)row; (void)col;
  (void)k; (void)s; (void)r; (void)e; (void)cap; return false; }

static WorkspaceEditor *fixture(void)
{
    WorkspaceEditor *e=workspace_editor_create(); assert(e);
    char text[8192]="\t你ab\nsecond line\n";
    for (int i=0;i<100;++i) strcat(text,"another line\n");
    buffer_destroy(e->buffers[0]);
    e->buffers[0]=buffer_from_bytes(NULL,text,strlen(text)); assert(e->buffers[0]);
    return e;
}
static WorkspaceEditorPointer point(EditorView v,int row,int column,bool press,bool down,bool release)
{
    return (WorkspaceEditorPointer){.position={v.text_x+column*v.cell_width+1,
        v.body_top+row*EDITOR_ROW_HEIGHT+1},.pressed=press,.down=down,.released=release,.time=1};
}
static void editor_tests(void)
{
    WorkspaceEditor *e=fixture(); EditorBuffer *b=current_buffer(e);
    Rectangle box={0,0,600,300}; EditorView v=editor_view(e,box);
    WorkspaceEditorPointer p=point(v,0,4,true,true,false);
    assert(workspace_editor_pointer(e,box,p)); assert(b->column==1); /* UTF-8 after tab */
    p.pressed=false; p.down=false; p.released=true;
    assert(!workspace_editor_pointer(e,box,p)); assert(e->mode==EDITOR_NORMAL);
    p=point(v,0,5,true,true,false); workspace_editor_pointer(e,box,p);
    assert(b->column==4); /* Never inside the three-byte character. */
    p=point(v,1,2,false,true,false); workspace_editor_pointer(e,box,p);
    assert(e->mode==EDITOR_VISUAL);
    char *selected=range_text(b,selected_range(e,b));
    assert(!strcmp(selected,"ab\nsec")); free(selected);
    p.down=false; p.released=true; workspace_editor_pointer(e,box,p);
    assert(!e->pointer_down && e->mode==EDITOR_VISUAL);
    test_character='y'; workspace_editor_update(e,NULL,NULL);
    assert(!strcmp(test_clipboard,"ab\nsec") && e->mode==EDITOR_NORMAL);
    /* Reverse selection uses the same inclusive range as keyboard Visual mode. */
    p=point(v,1,2,true,true,false); workspace_editor_pointer(e,box,p);
    p=point(v,0,5,false,true,false); workspace_editor_pointer(e,box,p);
    selected=range_text(b,selected_range(e,b));
    assert(!strcmp(selected,"ab\nsec")); free(selected);
    p.down=false; p.released=true; workspace_editor_pointer(e,box,p);
    test_keys[KEY_ESCAPE]=true; workspace_editor_update(e,NULL,NULL); test_keys[KEY_ESCAPE]=false;
    /* Insert mode survives click and resumes after a temporary mouse selection. */
    e->mode=EDITOR_INSERT;
    p=point(v,0,40,true,true,false); workspace_editor_pointer(e,box,p);
    assert(b->column==b->lines[0].length && e->mode==EDITOR_INSERT);
    p=point(v,1,1,false,true,false); workspace_editor_pointer(e,box,p);
    p.down=false; p.released=true; workspace_editor_pointer(e,box,p);
    test_character='y'; workspace_editor_update(e,NULL,NULL);
    assert(e->mode==EDITOR_INSERT);
    e->mode=EDITOR_NORMAL; b->row=b->column=b->top=b->left=0; e->follow_cursor=false;
    p=(WorkspaceEditorPointer){.wheel={0,-0.1f}};
    for(int i=0;i<4;++i) workspace_editor_pointer(e,box,p);
    assert(b->top==1 && b->row==0);
    draw_buffer(e,v,true); assert(b->top==1); /* Drawing must not undo scrolling. */
    test_keys[KEY_RIGHT]=true; workspace_editor_update(e,NULL,NULL); test_keys[KEY_RIGHT]=false;
    draw_buffer(e,v,true); assert(b->top==0); /* Keyboard movement reveals cursor. */
    /* Dragging beyond the bottom scrolls and stays owned until release. */
    p=point(v,0,0,true,true,false); workspace_editor_pointer(e,box,p);
    p=point(v,v.rows+3,2,false,true,false); p.time=2;
    workspace_editor_pointer(e,box,p); assert(b->top==1 && e->pointer_down);
    p.time=2.01; workspace_editor_pointer(e,box,p); assert(b->top==1);
    p.time=2.1; workspace_editor_pointer(e,box,p); assert(b->top==2);
    workspace_editor_cancel_pointer(e); assert(!e->pointer_down);
    workspace_editor_destroy(e);
}
static void layout_tests(void)
{
    WorkspaceLayout l; workspace_layout_init(&l); l.gui_width=700;
    assert(workspace_layout_split(&l,true,1800,900)==1);
    assert(workspace_layout_split(&l,true,1800,900)==2);
    WorkspaceGeometry g; workspace_layout_geometry(&l,1800,900,&g);
    int divider=workspace_layout_divider_at(&l,1800,900,g.panes[1].x-2,200);
    assert(divider>=0);
    assert(workspace_layout_drag_divider(&l,divider,100000,1800,900));
    workspace_layout_geometry(&l,1800,900,&g);
    assert(g.panes[0].width>=WORKSPACE_MIN_WIDTH && g.panes[1].width>=WORKSPACE_MIN_WIDTH);
    assert(workspace_layout_divider_at(&l,1800,900,g.text.x-2,200)==-2);
    assert(workspace_layout_drag_divider(&l,-2,0,1800,900)); assert(l.gui_width==640);
    l.focus=2; assert(workspace_layout_zoom(&l));
    assert(workspace_layout_divider_at(&l,1800,900,1200,200)==-1);
    assert(!workspace_layout_drag_divider(&l,divider,1000,1800,900));
    assert(!workspace_layout_drag_divider(&l,-99,1000,1800,900));
    workspace_layout_zoom(&l);
    assert(workspace_layout_split(&l,false,1800,900)==3);
    workspace_layout_geometry(&l,1800,900,&g);
    divider=workspace_layout_divider_at(&l,1800,900,g.panes[2].x+20,g.panes[2].y-2);
    assert(divider>=0 && !l.nodes[divider].split_x);
    workspace_layout_drag_divider(&l,divider,-10000,1800,900);
    workspace_layout_geometry(&l,1800,900,&g);
    assert(g.panes[1].height>=WORKSPACE_MIN_HEIGHT && g.panes[2].height>=WORKSPACE_MIN_HEIGHT);
}
static void routing_tests(void)
{
    GuiApp app={0}; workspace_layout_init(&app.workspace); app.workspace.gui_width=700;
    workspace_layout_split(&app.workspace,true,1800,900);
    workspace_layout_split(&app.workspace,true,1800,900);
    app.editors[0]=fixture(); app.editors[1]=fixture(); app.workspace.focus=1;
    WorkspaceGeometry g; workspace_geometry(&app,&g);
    EditorView v=editor_view(app.editors[1],pane_rectangle(g.panes[1]));
    test_mouse=(Vector2){v.text_x+10,v.body_top+10}; test_wheel=(Vector2){0,-1};
    assert(update_workspace_pointer(&app)); assert(app.workspace.focus==1);
    assert(current_buffer(app.editors[1])->top==3 && current_buffer(app.editors[0])->top==0);
    test_wheel=(Vector2){0}; test_pressed=test_down=true;
    assert(update_workspace_pointer(&app)); assert(app.workspace.focus==2 && app.pointer_pane==2);
    test_pressed=false; test_mouse=(Vector2){100,600};
    assert(update_workspace_pointer(&app)); assert(app.workspace.focus==2 && app.pointer_pane==2);
    test_down=false; test_released=true; update_workspace_pointer(&app); assert(!app.pointer_pane);
    test_released=false;
    /* The first click returning to the world is consumed through release. */
    app.screen=GUI_WORLD; app.session=(GameSession *)(uintptr_t)1;
    test_pressed=test_down=true; test_mouse=(Vector2){100,200};
    int captures=test_captures;
    assert(update_workspace_pointer(&app));
    assert(app.workspace.focus==0 && app.mouse_captured && app.pointer_guard && test_captures==captures+1);
    test_pressed=false; assert(update_workspace_pointer(&app));
    test_down=false; test_released=true; assert(update_workspace_pointer(&app));
    assert(!app.pointer_guard); test_released=false;
    assert(!update_workspace_pointer(&app));
    app.mouse_captured=false; app.session=NULL; app.screen=GUI_TITLE;
    app.pane_prefix=true;
    test_mouse=(Vector2){g.text.x-2,200}; test_pressed=test_down=true;
    assert(update_workspace_pointer(&app) && app.pointer_resizing);
    test_pressed=false; test_mouse.x=650; assert(update_workspace_pointer(&app));
    assert(app.workspace.gui_width==650 && test_cursor==MOUSE_CURSOR_RESIZE_EW);
    assert(app.pane_prefix); /* Resizing must not recapture the world pointer. */
    test_focused=false; update_workspace_pointer(&app); assert(!app.pointer_resizing);
    test_focused=true; test_down=false;
    workspace_editor_destroy(app.editors[0]); workspace_editor_destroy(app.editors[1]);
}
static void output_tests(void)
{
    WorkspaceOutput out={.count=200,.visible_rows=10};
    for(int i=0;i<4;++i) workspace_output_scroll(&out,(Vector2){0,0.1f});
    assert(out.scroll==1);
    workspace_output_scroll(&out,(Vector2){-1,0}); assert(out.left==3 && out.scroll==1);
}
int main(void)
{
    editor_tests(); layout_tests(); routing_tests(); output_tests();
    puts("workspace mouse: PASS (UTF-8, tabs, selection, modes, scrolling, dividers, focus, world click guard)");
    return 0;
}
