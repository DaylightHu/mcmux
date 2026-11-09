#include "gui_view.h"
#include "gui.h"
#include "ui_font.h"

#include "block_renderer.h"
#include "block_shape.h"
#include "command_completion.h"
#include "edit_command.h"
#include "flexible_placement.h"
#include "hotbar.h"
#include "inventory.h"
#include "plot_command.h"
#include "tick_command.h"
#include "voxel_pick.h"
#include "world_renderer.h"
#include "world_msaa.h"
#include "workspace_layout.h"
#include "workspace_editor.h"
#include "workspace_output.h"
#include "workspace_code.h"
#include "world_session_manager.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "rs/session.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SCREEN_WIDTH = 1080,
    SCREEN_HEIGHT = 800,
    WORKSPACE_STATUS_HEIGHT = 26,
    WORKSPACE_HEADER_HEIGHT = 25,
    MESSAGE_CAPACITY = 192,
    COMMAND_CAPACITY = 128,
    CHAT_HISTORY_CAPACITY = 100,
    CHAT_TEXT_CAPACITY = MESSAGE_CAPACITY + 8,
    CHAT_FONT_SIZE = 18,
    CHAT_LINE_HEIGHT = 22,
    MAX_VISIBLE_SAVE_ROWS = 7
};

static const char save_directory[] = "saves";

static void begin_gui_target(RenderTexture2D target)
{
    BeginTextureMode(target);
    /* Keep widgets in logical units while the viewport uses target pixels. */
    rlSetMatrixProjection(MatrixOrtho(0, gui_view_width(), gui_view_height(),
                                      0, 0.0, 1.0));
}

static void begin_gui_scissor(int x, int y, int width, int height)
{
    float scale_x = (float)GetRenderWidth()/gui_view_width();
    float scale_y = (float)GetRenderHeight()/gui_view_height();
    int left = (int)roundf(x*scale_x);
    int top = (int)roundf(y*scale_y);
    int right = (int)roundf((x + width)*scale_x);
    int bottom = (int)roundf((y + height)*scale_y);
    BeginScissorMode(left, top, right - left, bottom - top);
}

typedef enum {
    GUI_TITLE,
    GUI_WORLD,
    GUI_COMMAND,
    GUI_INVENTORY,
    GUI_BARREL,
    GUI_HOPPER,
    GUI_DROPPER,
    GUI_PAUSE,
    GUI_SAVE_ERROR
} GuiScreen;

typedef enum {
    AFTER_SAVE_NONE,
    AFTER_SAVE_TITLE,
    AFTER_SAVE_GUI_DETACH,
    AFTER_SAVE_HOST_EXIT
} AfterSave;

typedef struct {
    char text[CHAT_TEXT_CAPACITY];
    double time;
    bool error;
} ChatMessage;

typedef struct {
    BlockRenderer blocks;
    Inventory inventory;
    Hotbar hotbar;
    GameSession *session;
    WorldRenderer *world_renderer;
    Camera3D camera;
    GameSaveList saves;
    GuiScreen screen;
    GuiScreen screen_before_error;
    AfterSave after_save;
    int selected_save;
    int save_scroll;
    char world_name[GAME_SESSION_NAME_CAPACITY];
    size_t world_name_length;
    char message[MESSAGE_CAPACITY];
    char command[COMMAND_CAPACITY];
    size_t command_length;
    CommandCompletion completion;
    PlotCommandState plot_command;
    ChatMessage chat[CHAT_HISTORY_CAPACITY];
    size_t chat_count;
    int chat_scroll;
    BlockPos barrel_pos;
    BlockPos container_pos;
    bool mouse_captured;
    bool close_latched;
    bool running;
    bool fatal_error;
    WorkspaceLayout workspace;
    WorkspaceEditor *editors[WORKSPACE_TEXT_LIMIT];
    WorkspaceOutput *output;
    WorkspaceCode *code;
    int output_pane;
    /* Borrowed from the host; frontends may select sessions but do not own them. */
    WorldSessionManager *world_sessions;
    RenderTexture2D gui_target;
    WorldMsaa world_msaa;
    Texture2D title_background;
    bool pane_prefix, pane_help;
    bool window_gui_visible, window_text_visible, window_resize_pending;
    bool program_active, program_gui_visible, skip_world_advance;
    int window_width, window_height, window_text_width;
    char pane_message[MESSAGE_CAPACITY];
} GuiApp;

static void add_chat_message(GuiApp *app, const char *text, bool error)
{
    if (!text[0]) return;
    if (app->chat_count == CHAT_HISTORY_CAPACITY) {
        memmove(app->chat, app->chat + 1,
                (CHAT_HISTORY_CAPACITY - 1)*sizeof(app->chat[0]));
        --app->chat_count;
    }
    ChatMessage *message = &app->chat[app->chat_count++];
    snprintf(message->text, sizeof(message->text), "%s%s",
             error ? "error: " : "", text);
    message->time = GetTime();
    message->error = error;
    app->chat_scroll = 0;
}

typedef struct {
    Rectangle panel;
    float grid_x;
    float grid_y;
    int columns;
    int rows;
} ContainerLayout;

enum {
    CONTAINER_SLOT_SIZE = 54,
    CONTAINER_SLOT_GAP = 4,
    SIGNAL_BUTTON_SIZE = 29,
    SIGNAL_BUTTON_GAP = 3
};

static bool point_in(Rectangle rectangle)
{
    return CheckCollisionPointRec(gui_view_mouse(), rectangle);
}

static bool button_with_font(Rectangle rectangle, const char *label, bool enabled,
                             int font_size)
{
    bool hovered = enabled && point_in(rectangle);
    Color fill = enabled ? (hovered ? (Color){78, 101, 116, 255}
                                    : (Color){55, 72, 84, 255})
                         : (Color){48, 48, 48, 190};
    Color border = enabled ? (hovered ? (Color){244, 190, 88, 255}
                                      : (Color){142, 158, 168, 255})
                           : (Color){85, 85, 85, 180};
    Color text = enabled ? RAYWHITE : (Color){125, 125, 125, 255};
    DrawRectangleRec(rectangle, fill);
    DrawRectangleLinesEx(rectangle, 2.0f, border);
    int width = ui_text_width(label, font_size);
    ui_text_draw(label, (int)(rectangle.x + (rectangle.width - width)*0.5f),
             (int)(rectangle.y + (rectangle.height - font_size)*0.5f),
             font_size, text);
    return hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

static bool button(Rectangle rectangle, const char *label, bool enabled)
{
    return button_with_font(rectangle, label, enabled, 20);
}

static void draw_centered(const char *text, int y, int font_size, Color color)
{
    ui_text_draw(text, (gui_view_width() - ui_text_width(text, font_size))/2,
             y, font_size, color);
}

static void draw_texture_cover(Texture2D texture)
{
    float view_width = (float)gui_view_width();
    float view_height = (float)gui_view_height();
    Rectangle source = {0, 0, (float)texture.width, (float)texture.height};
    if ((float)texture.width*view_height > (float)texture.height*view_width) {
        source.width = (float)texture.height*view_width/view_height;
        source.x = ((float)texture.width - source.width)*0.5f;
    } else {
        source.height = (float)texture.width*view_height/view_width;
        source.y = ((float)texture.height - source.height)*0.5f;
    }
    DrawTexturePro(texture, source, (Rectangle){0, 0, view_width, view_height},
                   (Vector2){0}, 0.0f, WHITE);
}

static bool names_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

static bool save_name_exists(const GuiApp *app, const char *name)
{
    for (size_t index = 0; index < app->saves.count; ++index)
        if (names_equal(app->saves.names[index], name)) return true;
    return false;
}

static GameSession *managed_world_find(const GuiApp *app, const char *name)
{
    return world_session_manager_find(app->world_sessions, name);
}

static size_t managed_world_count(const GuiApp *app)
{
    return world_session_manager_count(app->world_sessions);
}

static const char *managed_world_name(const GuiApp *app,
                                      const GameSession *session)
{
    const char *name = world_session_manager_name(app->world_sessions, session);
    return name ? name : "world";
}

static bool managed_world_add(GuiApp *app, const char *name, GameSession *session)
{
    return world_session_manager_add(app->world_sessions, name, session);
}

static bool any_world_dirty(const GuiApp *app)
{
    return world_session_manager_any_dirty(app->world_sessions);
}

static GameSessionResult save_all_worlds(GuiApp *app, GameSession **failed_session)
{
    return world_session_manager_save_dirty(app->world_sessions, failed_session);
}

static GameSessionResult list_saved_worlds(GameSaveList *saves)
{
    GameSessionResult result = game_session_list_saves(save_directory, saves);
    return result;
}

static bool saved_world_exists(const char *name, GameSessionResult *list_result)
{
    GameSaveList saves;
    GameSessionResult result = list_saved_worlds(&saves);
    if (list_result) *list_result = result;
    if (result != GAME_SESSION_OK && result != GAME_SESSION_TOO_MANY_SAVES)
        return false;
    for (size_t index = 0; index < saves.count; ++index)
        if (names_equal(saves.names[index], name)) return true;
    return false;
}

static void refresh_saves(GuiApp *app)
{
    GameSessionResult result = game_session_list_saves(save_directory,
                                                       &app->saves);
    if (result != GAME_SESSION_OK && result != GAME_SESSION_TOO_MANY_SAVES) {
        app->saves = (GameSaveList){0};
        snprintf(app->message, sizeof(app->message),
                 "Cannot read saves: %s", game_session_result_name(result));
    } else if (result == GAME_SESSION_TOO_MANY_SAVES) {
        snprintf(app->message, sizeof(app->message),
                 "Only the first %d saves are shown", GAME_SESSION_MAX_SAVES);
    }
    if (!app->saves.count) {
        app->selected_save = -1;
        app->save_scroll = 0;
    } else {
        if (app->selected_save < 0) app->selected_save = 0;
        if ((size_t)app->selected_save >= app->saves.count)
            app->selected_save = (int)app->saves.count - 1;
    }
}

static void release_mouse(GuiApp *app)
{
    if (app->mouse_captured) EnableCursor();
    app->mouse_captured = false;
}

static void capture_mouse(GuiApp *app)
{
    DisableCursor();
    app->mouse_captured = true;
}

static ContainerLayout container_layout(BlockId id)
{
    bool barrel = id == BLOCK_BARREL;
    int columns = barrel ? 9 : id == BLOCK_HOPPER ? 5 : 3;
    int rows = id == BLOCK_HOPPER ? 1 : 3;
    int grid_width = columns*CONTAINER_SLOT_SIZE +
                     (columns - 1)*CONTAINER_SLOT_GAP;
    int grid_height = rows*CONTAINER_SLOT_SIZE +
                      (rows - 1)*CONTAINER_SLOT_GAP;
    int header = barrel ? 76 : 58;
    int panel_width = barrel ? grid_width + 40 : 480;
    int panel_height = header + grid_height + (barrel ? 42 : 0) + 18;
    ContainerLayout layout = {0};
    layout.panel = (Rectangle){
        (gui_view_width() - panel_width)*0.5f,
        (gui_view_height() - 80 - panel_height)*0.5f,
        (float)panel_width, (float)panel_height
    };
    layout.grid_x = layout.panel.x + (panel_width - grid_width)*0.5f;
    layout.grid_y = layout.panel.y + header;
    layout.columns = columns;
    layout.rows = rows;
    return layout;
}

static Rectangle container_slot_rectangle(ContainerLayout layout, int index)
{
    int column = index % layout.columns;
    int row = index / layout.columns;
    return (Rectangle){
        layout.grid_x + column*(CONTAINER_SLOT_SIZE + CONTAINER_SLOT_GAP),
        layout.grid_y + row*(CONTAINER_SLOT_SIZE + CONTAINER_SLOT_GAP),
        CONTAINER_SLOT_SIZE, CONTAINER_SLOT_SIZE
    };
}

static Rectangle barrel_signal_rectangle(ContainerLayout layout, int level)
{
    int width = 16*SIGNAL_BUTTON_SIZE + 15*SIGNAL_BUTTON_GAP;
    return (Rectangle){
        layout.panel.x + (layout.panel.width - width)*0.5f +
        level*(SIGNAL_BUTTON_SIZE + SIGNAL_BUTTON_GAP),
        layout.grid_y + layout.rows*(CONTAINER_SLOT_SIZE + CONTAINER_SLOT_GAP) + 8,
        SIGNAL_BUTTON_SIZE, 30
    };
}

static void open_barrel(GuiApp *app, BlockPos pos)
{
    RsResult result = world_open_barrel(game_session_world(app->session), pos);
    if (result != RS_OK) {
        snprintf(app->message, sizeof(app->message),
                 "Open barrel: %s", rs_result_name(result));
        return;
    }
    app->barrel_pos = pos;
    release_mouse(app);
    app->screen = GUI_BARREL;
    snprintf(app->message, sizeof(app->message),
             "Barrel: store items or use the signal shortcuts");
}

static void close_barrel(GuiApp *app)
{
    (void)world_close_barrel(game_session_world(app->session), app->barrel_pos);
    app->screen = GUI_WORLD;
    capture_mouse(app);
}

static void close_world(GuiApp *app)
{
    release_mouse(app);
    app->pane_prefix = app->pane_help = false;
    gui_view_reset();
    world_msaa_destroy(&app->world_msaa);
    world_renderer_destroy(app->world_renderer);
    app->world_renderer = NULL;
    app->session = NULL;
    app->chat_count = 0;
    app->chat_scroll = 0;
    app->command[0] = '\0';
    app->command_length = 0;
    app->completion = (CommandCompletion){0};
    app->plot_command = (PlotCommandState){0};
}

static float camera_axis_within(float value, int minimum, int maximum)
{
    if ((double)value < minimum) return (float)minimum;
    if ((double)value > maximum) {
        float edge = (float)maximum;
        return (double)edge > maximum ? nextafterf(edge, -INFINITY) : edge;
    }
    return value;
}

static bool open_world(GuiApp *app, const char *name, GameSession *session)
{
    World *world = game_session_world(session);
    WorldPlotBounds bounds = world_plot_bounds(world);
    BlockPos spawn = {
        (int)camera_axis_within(16.0f, bounds.min_x, bounds.max_x), 3,
        (int)camera_axis_within(16.0f, bounds.min_z, bounds.max_z)
    };
    if (world_stream(world,spawn,5) != RS_OK) {
        snprintf(app->message,sizeof(app->message),"Cannot load world chunks");
        return false;
    }
    WorldRenderer *renderer = world_renderer_create(world, &app->blocks);
    if (!renderer || !world_renderer_sync(renderer)) {
        world_renderer_destroy(renderer);
        snprintf(app->message, sizeof(app->message),
                 "Cannot create the world render cache");
        return false;
    }
    if (!managed_world_add(app, name, session)) {
        world_renderer_destroy(renderer);
        snprintf(app->message, sizeof(app->message),
                 "Cannot register world session (limit reached or duplicate name)");
        return false;
    }

    app->session = session;
    app->world_renderer = renderer;
    app->camera = (Camera3D){0};
    app->camera.position = (Vector3){(float)spawn.x, 3.0f, (float)spawn.z};
    app->camera.target = (Vector3){(float)spawn.x, 2.0f, (float)spawn.z - 1.0f};
    app->camera.up = (Vector3){0.0f, 1.0f, 0.0f};
    app->camera.fovy = 65.0f;
    app->camera.projection = CAMERA_PERSPECTIVE;
    app->screen = GUI_WORLD;
    snprintf(app->message, sizeof(app->message),
             "LMB remove | RMB use/place | Shift+RMB place | MMB pick | E inventory | / command | ESC menu");
    capture_mouse(app);
    return true;
}

static void return_to_title(GuiApp *app)
{
    close_world(app);
    app->screen = GUI_TITLE;
    app->selected_save = -1;
    refresh_saves(app);
}

static bool close_pane(GuiApp *app, bool force);

static void detach_gui_frontend(GuiApp *app)
{
    close_world(app);
    app->screen = GUI_TITLE;
    workspace_layout_show_gui(&app->workspace);
    if (!close_pane(app, false))
        snprintf(app->pane_message, sizeof(app->pane_message),
                 "GUI remains open because no text pane could be created");
}

static void perform_after_save(GuiApp *app, AfterSave action)
{
    if (action == AFTER_SAVE_TITLE) {
        return_to_title(app);
    } else if (action == AFTER_SAVE_GUI_DETACH) {
        detach_gui_frontend(app);
    } else if (action == AFTER_SAVE_HOST_EXIT) {
        app->running = false;
    }
}

static void request_save_action(GuiApp *app, AfterSave action)
{
    GameSessionResult result = GAME_SESSION_OK;
    if (action == AFTER_SAVE_TITLE || action == AFTER_SAVE_GUI_DETACH) {
        if (app->session) {
            result = game_session_save(app->session);
        }
    } else {
        result = save_all_worlds(app, NULL);
    }
    if (result == GAME_SESSION_OK) {
        perform_after_save(app, action);
        return;
    }
    release_mouse(app);
    workspace_layout_show_gui(&app->workspace);
    app->screen_before_error = app->screen;
    app->screen = GUI_SAVE_ERROR;
    app->after_save = action;
    snprintf(app->message, sizeof(app->message), "Save failed: %s",
             game_session_result_name(result));
}

static void create_world(GuiApp *app)
{
    if (!game_session_valid_name(app->world_name)) {
        snprintf(app->message, sizeof(app->message),
                 "Use letters, numbers, spaces, '-' or '_'");
        return;
    }
    GameSessionResult list_result;
    bool exists = saved_world_exists(app->world_name, &list_result);
    if (managed_world_find(app, app->world_name) || exists) {
        snprintf(app->message, sizeof(app->message),
                 "A world with that name already exists");
        return;
    }
    if (list_result != GAME_SESSION_OK) {
        snprintf(app->message, sizeof(app->message), "Cannot list worlds: %s",
                 game_session_result_name(list_result));
        return;
    }
    if (managed_world_count(app) >= GAME_SESSION_MAX_SAVES) {
        snprintf(app->message, sizeof(app->message), "Too many open world sessions");
        return;
    }
    char path[GAME_SESSION_PATH_CAPACITY];
    GameSessionResult result = game_session_make_path(
        save_directory, app->world_name, path);
    if (result != GAME_SESSION_OK) {
        snprintf(app->message, sizeof(app->message), "Cannot create path: %s",
                 game_session_result_name(result));
        return;
    }
    GameSession *session = game_session_create(path, world_config_default(),
                                               &result);
    if (!session) {
        snprintf(app->message, sizeof(app->message), "Cannot create world: %s",
                 game_session_result_name(result));
        return;
    }
    result = game_session_save(session);
    if (result != GAME_SESSION_OK) {
        game_session_destroy(session);
        snprintf(app->message, sizeof(app->message), "Cannot save new world: %s",
                 game_session_result_name(result));
        return;
    }
    if (!open_world(app, app->world_name, session)) game_session_destroy(session);
}

static void load_selected_world(GuiApp *app)
{
    if (app->selected_save < 0 ||
        (size_t)app->selected_save >= app->saves.count) return;
    const char *name = app->saves.names[app->selected_save];
    GameSession *session = managed_world_find(app, name);
    if (session) {
        (void)open_world(app, name, session);
        return;
    }
    char path[GAME_SESSION_PATH_CAPACITY];
    GameSessionResult result = game_session_make_path(save_directory, name, path);
    if (result != GAME_SESSION_OK) {
        snprintf(app->message, sizeof(app->message), "Cannot open path: %s",
                 game_session_result_name(result));
        return;
    }
    session = game_session_load(path, &result);
    if (!session) {
        snprintf(app->message, sizeof(app->message), "Cannot load world: %s",
                 game_session_result_name(result));
        return;
    }
    if (!open_world(app, name, session)) game_session_destroy(session);
}

static void update_name_input(GuiApp *app)
{
    int character;
    while ((character = GetCharPressed()) != 0) {
        bool allowed = character < 128 &&
            (isalnum((unsigned char)character) || character == ' ' ||
             character == '-' || character == '_');
        if (allowed && app->world_name_length + 1 < sizeof(app->world_name)) {
            app->world_name[app->world_name_length++] = (char)character;
            app->world_name[app->world_name_length] = '\0';
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && app->world_name_length) {
        app->world_name[--app->world_name_length] = '\0';
    }
}

enum {
    TITLE_WIDTH = 840, TITLE_MARGIN = 40, TITLE_CONTROL_HEIGHT = 60,
    TITLE_TEXT_SIZE = 26, TITLE_LABEL_SIZE = 24, TITLE_ROW_HEIGHT = 44,
    TITLE_LIST_TOP = 180, TITLE_LIST_PADDING = 10, TITLE_GAP = 24,
    TITLE_FIXED_HEIGHT = TITLE_LIST_TOP + 2*TITLE_LIST_PADDING +
                         TITLE_GAP + TITLE_CONTROL_HEIGHT
};

typedef struct {
    Rectangle input, create, list, load, close;
    int top, rows;
} TitleLayout;

static int visible_save_rows(void)
{
    int available = gui_view_height() - 2*TITLE_MARGIN - TITLE_FIXED_HEIGHT;
    int rows = available/TITLE_ROW_HEIGHT;
    if (rows < 1) rows = 1;
    if (rows > MAX_VISIBLE_SAVE_ROWS) rows = MAX_VISIBLE_SAVE_ROWS;
    return rows;
}

static TitleLayout title_layout(void)
{
    int width = gui_view_width() - 2*TITLE_MARGIN;
    if (width > TITLE_WIDTH) width = TITLE_WIDTH;
    int left = (gui_view_width() - width)/2;
    int rows = visible_save_rows();
    int top = (gui_view_height() - TITLE_FIXED_HEIGHT - rows*TITLE_ROW_HEIGHT)/2;
    int create_width = (width - 20)*3/8;
    int input_width = width - 20 - create_width;
    int action_width = (width - TITLE_GAP)/2;
    TitleLayout layout = {.top=top, .rows=rows};
    layout.input = (Rectangle){(float)left, (float)(top + 40),
                              (float)input_width, TITLE_CONTROL_HEIGHT};
    layout.create = (Rectangle){(float)(left + input_width + 20), layout.input.y,
                               (float)create_width, TITLE_CONTROL_HEIGHT};
    layout.list = (Rectangle){(float)left, (float)(top + TITLE_LIST_TOP),
        (float)width, (float)(rows*TITLE_ROW_HEIGHT + 2*TITLE_LIST_PADDING)};
    float actions_y = layout.list.y + layout.list.height + TITLE_GAP;
    layout.load = (Rectangle){(float)left, actions_y,
                             (float)action_width, TITLE_CONTROL_HEIGHT};
    layout.close = (Rectangle){(float)(left + action_width + TITLE_GAP), actions_y,
        (float)(width - action_width - TITLE_GAP), TITLE_CONTROL_HEIGHT};
    return layout;
}

static void keep_selection_visible(GuiApp *app)
{
    int visible_rows = visible_save_rows();
    if (app->selected_save < app->save_scroll)
        app->save_scroll = app->selected_save;
    if (app->selected_save >= app->save_scroll + visible_rows)
        app->save_scroll = app->selected_save - visible_rows + 1;
    int maximum = (int)app->saves.count - visible_rows;
    if (maximum < 0) maximum = 0;
    if (app->save_scroll > maximum) app->save_scroll = maximum;
    if (app->save_scroll < 0) app->save_scroll = 0;
}

static void update_title(GuiApp *app)
{
    update_name_input(app);
    if (app->saves.count) {
        if (IsKeyPressed(KEY_DOWN)) {
            app->selected_save = (app->selected_save + 1) % (int)app->saves.count;
        } else if (IsKeyPressed(KEY_UP)) {
            app->selected_save = (app->selected_save + (int)app->saves.count - 1) %
                                 (int)app->saves.count;
        }
        float wheel = GetMouseWheelMove();
        if (wheel < 0 && app->selected_save + 1 < (int)app->saves.count)
            ++app->selected_save;
        else if (wheel > 0 && app->selected_save > 0)
            --app->selected_save;
        keep_selection_visible(app);
    }
    if (IsKeyPressed(KEY_ENTER)) create_world(app);
}

static void draw_title(GuiApp *app)
{
    const Color ink = {248, 226, 111, 255};
    const Color field_ink = {26, 50, 55, 255};
    const Color list_ink = {255, 244, 195, 255};
    bool active = app->workspace.focus == 0;
    draw_texture_cover(app->title_background);
    DrawRectangle(0, 0, gui_view_width(), gui_view_height(),
                  (Color){0, 27, 22, 72});
    TitleLayout layout = title_layout();
    Rectangle input = layout.input;
    ui_text_draw("CREATE WORLD", (int)input.x + 2, layout.top + 2,
                 TITLE_LABEL_SIZE, (Color){0, 35, 29, 230});
    ui_text_draw("CREATE WORLD", (int)input.x, layout.top, TITLE_LABEL_SIZE, ink);
    DrawRectangleRec(input, (Color){247, 243, 230, 255});
    DrawRectangleLinesEx(input, 2.0f, (Color){92, 111, 112, 255});
    const char *input_text = app->world_name;
    while (*input_text && ui_text_width(input_text, TITLE_TEXT_SIZE) > (int)input.width - 33)
        ++input_text;
    if (app->world_name_length)
        ui_text_draw(input_text, (int)input.x + 16, (int)input.y + 17,
                     TITLE_TEXT_SIZE, field_ink);
    else
        ui_text_draw("World name", (int)input.x + 16, (int)input.y + 17, TITLE_TEXT_SIZE,
                 (Color){137, 137, 125, 255});
    if (app->workspace.focus == 0 && ((int)(GetTime()*2.0)) % 2 == 0) {
        int cursor_x = (int)input.x + 16 + ui_text_width(input_text, TITLE_TEXT_SIZE);
        DrawLine(cursor_x, (int)input.y + 14, cursor_x, (int)input.y + 46, field_ink);
    }

    bool can_create = active && game_session_valid_name(app->world_name) &&
                      !save_name_exists(app, app->world_name);
    if (button_with_font(layout.create, "Create", can_create, TITLE_TEXT_SIZE))
        create_world(app);

    ui_text_draw("LOAD WORLD", (int)layout.list.x + 2, layout.top + 146,
                 TITLE_LABEL_SIZE, (Color){0, 35, 29, 230});
    ui_text_draw("LOAD WORLD", (int)layout.list.x, layout.top + 144,
                 TITLE_LABEL_SIZE, ink);
    Rectangle list = layout.list;
    DrawRectangleLinesEx(list, 2.0f, (Color){226, 207, 96, 255});
    begin_gui_scissor((int)list.x + 2, (int)list.y + 2,
                     (int)list.width - 4, (int)list.height - 4);
    if (!app->saves.count) {
        ui_text_draw("No saved worlds yet", (int)list.x + 22,
                 (int)list.y + 22, TITLE_LABEL_SIZE, (Color){232, 220, 155, 255});
    }
    for (int row = 0; row < layout.rows; ++row) {
        int index = app->save_scroll + row;
        if ((size_t)index >= app->saves.count) break;
        Rectangle item = {list.x + TITLE_LIST_PADDING,
            list.y + TITLE_LIST_PADDING + row*TITLE_ROW_HEIGHT,
            list.width - 2*TITLE_LIST_PADDING, TITLE_ROW_HEIGHT - 4};
        bool selected = index == app->selected_save;
        bool hovered = active && point_in(item);
        if (selected || hovered)
            DrawRectangleRec(item, selected ? (Color){80, 105, 108, 220}
                                            : (Color){177, 177, 157, 160});
        ui_text_draw(app->saves.names[index], (int)item.x + 12, (int)item.y + 8,
                 TITLE_LABEL_SIZE, selected ? RAYWHITE : list_ink);
        if (active && hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            app->selected_save = index;
    }
    EndScissorMode();

    if (button_with_font(layout.load, "Load selected",
                         active && app->selected_save >= 0, TITLE_TEXT_SIZE))
        load_selected_world(app);
    if (button_with_font(layout.close, "Close GUI", active, TITLE_TEXT_SIZE))
        request_save_action(app, AFTER_SAVE_GUI_DETACH);
    if (strcmp(app->message, "Create a world or load an existing .mcw file") != 0)
        draw_centered(app->message, gui_view_height() - 34, 17,
                      (Color){255, 190, 150, 255});
}

static Direction camera_horizontal_direction(const Camera3D *camera)
{
    float x = camera->target.x - camera->position.x;
    float z = camera->target.z - camera->position.z;
    if (fabsf(x) > fabsf(z)) return x >= 0.0f ? DIR_EAST : DIR_WEST;
    return z >= 0.0f ? DIR_SOUTH : DIR_NORTH;
}

static const char *horizontal_direction_name(Direction direction)
{
    switch (direction) {
    case DIR_EAST: return "East (+X)";
    case DIR_WEST: return "West (-X)";
    case DIR_SOUTH: return "South (+Z)";
    case DIR_NORTH: return "North (-Z)";
    default: return "Unknown";
    }
}

static Direction camera_direction(const Camera3D *camera)
{
    float x = camera->target.x - camera->position.x;
    float y = camera->target.y - camera->position.y;
    float z = camera->target.z - camera->position.z;
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    if (ay > ax && ay > az) return y >= 0.0f ? DIR_UP : DIR_DOWN;
    return camera_horizontal_direction(camera);
}

static EditCommandContext edit_context(const GuiApp *app)
{
    EditCommandContext context = {.facing=camera_direction(&app->camera)};
    double x=floor((double)app->camera.position.x+0.5);
    double y=floor((double)app->camera.position.y+0.5);
    double z=floor((double)app->camera.position.z+0.5);
    if (isfinite(x) && isfinite(y) && isfinite(z) &&
        x>=INT_MIN && x<=INT_MAX && y>=INT_MIN && y<=INT_MAX && z>=INT_MIN && z<=INT_MAX) {
        context.position=(BlockPos){(int)x,(int)y,(int)z};
        context.has_position=true;
    }
    return context;
}

static RsResult run_gui_edit(GuiApp *app, const EditCommand *command)
{
    return edit_command_run(app->session, command, edit_context(app),
                             app->message, sizeof(app->message));
}

static bool flexible_placement_held(BlockId selected)
{
    return selected > BLOCK_AIR && selected < BLOCK_INTERNAL_FIRST &&
           (IsKeyDown(KEY_G) || IsKeyDown(KEY_H));
}

static void apply_edit(GuiApp *app, VoxelHit hit, BlockId selected, bool place,
                       Direction facing)
{
    if (!hit.hit) return;
    if (place && selected == BLOCK_AIR) {
        snprintf(app->message, sizeof(app->message),
                 "Selected hotbar slot is empty");
        return;
    }

    World *world = game_session_world(app->session);
    RsResult result;
    uint64_t before_revision = world_revision(world);
    bool flexible = place && flexible_placement_held(selected);
    if (place) {
        if (!hit.has_face) {
            snprintf(app->message, sizeof(app->message),
                     "Cannot place while inside a block");
            if (flexible) add_chat_message(app, app->message, true);
            return;
        }
        PlacementTarget plan = flexible_placement_target(hit, selected, facing,
            camera_direction(&app->camera), IsKeyDown(KEY_G), IsKeyDown(KEY_H));
        if (!plan.valid) {
            snprintf(app->message, sizeof(app->message),
                     "%s does not support that direction", block_name(selected));
            if (flexible) add_chat_message(app, app->message, true);
            return;
        }
        BlockPos target = plan.pos;
        if (!world_in_bounds(world, target) ||
            world_get(world, target).id != BLOCK_AIR) {
            snprintf(app->message, sizeof(app->message),
                     "Target occupied or outside world");
            if (flexible) add_chat_message(app, app->message, true);
            return;
        }
        if ((selected == BLOCK_REDSTONE_WIRE ||
             selected == BLOCK_REDSTONE_REPEATER ||
             selected == BLOCK_REDSTONE_COMPARATOR ||
             selected == BLOCK_POWERED_RAIL ||
             selected == BLOCK_ACTIVATOR_RAIL) && hit.face != DIR_UP && !flexible) {
            snprintf(app->message, sizeof(app->message),
                     "%s must be placed on a top face", block_name(selected));
            return;
        }
        if (selected == BLOCK_LEVER)
            world_place_lever(world, target, plan.placement, facing);
        else
            world_place(world, target, selected, plan.placement);
    } else {
        world_remove(world, hit.pos);
    }
    result = world_result(world);
    if (result == RS_OK && world_revision(world) == before_revision)
        result = RS_UNCHANGED;
    world_clear_error(world);
    if (result == RS_OK || result == RS_UNCHANGED) {
        RsResult flushed = game_session_flush(app->session);
        if (flushed != RS_OK) result = flushed;
    }
    snprintf(app->message, sizeof(app->message), "%s: %s",
             place ? "Place" : "Remove", rs_result_name(result));
    if (flexible && result != RS_OK && result != RS_UNCHANGED)
        add_chat_message(app, app->message, true);
}

static BlockId pickable_block(BlockId id)
{
    if (id == BLOCK_PISTON_HEAD) return BLOCK_PISTON;
    if (id == BLOCK_STICKY_PISTON_HEAD) return BLOCK_STICKY_PISTON;
    return id > BLOCK_AIR && id < BLOCK_INTERNAL_FIRST ? id : BLOCK_AIR;
}

static void advance_world_sessions(GuiApp *app)
{
    if (app->program_active || app->skip_world_advance) return;
    GameSession *failed_session = NULL;
    RsResult result = world_session_manager_advance(app->world_sessions,
                                                     GetFrameTime(),
                                                     &failed_session);
    if (result != RS_OK && result != RS_UNCHANGED) {
        snprintf(app->message, sizeof(app->message), "%s simulation: %s",
                 managed_world_name(app, failed_session), rs_result_name(result));
        app->fatal_error = true;
        app->running = false;
    }
}

static void update_command(GuiApp *app)
{
    float wheel = GetMouseWheelMove();
    if (wheel > 0.0f || IsKeyPressed(KEY_PAGE_UP)) app->chat_scroll += 3;
    if (wheel < 0.0f || IsKeyPressed(KEY_PAGE_DOWN)) app->chat_scroll -= 3;
    if (app->chat_scroll < 0) app->chat_scroll = 0;
    if (app->chat_scroll > CHAT_HISTORY_CAPACITY*CHAT_TEXT_CAPACITY)
        app->chat_scroll = CHAT_HISTORY_CAPACITY*CHAT_TEXT_CAPACITY;
    int character;
    while ((character = GetCharPressed()) != 0) {
        if (character >= 32 && character < 127 &&
            app->command_length + 1 < sizeof(app->command)) {
            app->command[app->command_length++] = (char)character;
            app->command[app->command_length] = '\0';
            app->completion.active = false;
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && app->command_length) {
        app->command[--app->command_length] = '\0';
        app->completion.active = false;
    }
    if (IsKeyPressed(KEY_TAB) && !app->plot_command.pending) {
        command_complete(&app->completion, app->command, sizeof(app->command));
        app->command_length = strlen(app->command);
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (app->plot_command.pending) {
            (void)plot_command_answer(game_session_world(app->session), &app->plot_command,
                                       "n", app->message, sizeof(app->message));
            add_chat_message(app, app->message, false);
        }
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }
    if (!IsKeyPressed(KEY_ENTER)) return;
    if (!app->plot_command.pending && !app->command[strspn(app->command, " \t")]) {
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }
    PlotCommand command;
    TickCommand tick;
    EditCommand edit;
    bool error = false;
    if (app->plot_command.pending) {
        RsResult result = plot_command_answer(game_session_world(app->session), &app->plot_command,
                                               app->command, app->message, sizeof(app->message));
        error = result != RS_OK;
        if (error && !app->message[0])
            snprintf(app->message, sizeof(app->message), "Shrink: %s", rs_result_name(result));
    } else if (edit_command_parse(app->command, &edit)) {
        error = run_gui_edit(app, &edit) != RS_OK;
    } else if (tick_command_parse(app->command, &tick)) {
        RsResult result = tick_command_run(game_session_world(app->session),
            &tick, app->message, sizeof(app->message));
        error = result != RS_OK;
        if (result != RS_OK && !app->message[0])
            snprintf(app->message, sizeof(app->message), "Tick command: %s",
                     rs_result_name(result));
    } else if (plot_command_parse(app->command, &command)) {
        RsResult result = plot_command_run(game_session_world(app->session),
                                           &command, &app->plot_command, app->message,
                                           sizeof(app->message));
        error = result != RS_OK;
        if (result != RS_OK && !app->message[0])
            snprintf(app->message, sizeof(app->message), "Plot command: %s",
                     rs_result_name(result));
    } else {
        error = true;
        snprintf(app->message, sizeof(app->message),
                 "Use pos1|pos2|copy|paste|stack|move|undo|redo|schem, tick freeze|step|noupdate, or plot info|expand|shrink");
    }
    add_chat_message(app, app->message, error);
    if (app->plot_command.pending) {
        app->command[0] = '\0';
        app->command_length = 0;
        app->completion = (CommandCompletion){0};
        return;
    }
    app->screen = GUI_WORLD;
    capture_mouse(app);
}

static void constrain_camera(GuiApp *app, const World *world)
{
    WorldPlotBounds bounds = world_plot_bounds(world);
    float x = app->camera.position.x;
    float z = app->camera.position.z;
    x = camera_axis_within(x, bounds.min_x, bounds.max_x);
    z = camera_axis_within(z, bounds.min_z, bounds.max_z);
    app->camera.target.x += x - app->camera.position.x;
    app->camera.target.z += z - app->camera.position.z;
    app->camera.position.x = x;
    app->camera.position.z = z;
}

static VoxelHit update_world(GuiApp *app)
{
    VoxelHit hit = {0};
    if (IsKeyPressed(KEY_SLASH)) {
        release_mouse(app);
        while (GetCharPressed() != 0) {} /* Consume the key that opened chat. */
        app->command[0] = '\0';
        app->command_length = 0;
        app->completion = (CommandCompletion){0};
        app->chat_scroll = 0;
        app->screen = GUI_COMMAND;
        return hit;
    }
    if (IsKeyPressed(KEY_E)) {
        release_mouse(app);
        app->screen = GUI_INVENTORY;
        snprintf(app->message, sizeof(app->message),
                 "Inventory: choose a hotbar slot, then click an icon");
        return hit;
    }
    if (IsKeyPressed(KEY_ESCAPE) || !IsWindowFocused()) {
        release_mouse(app);
        app->screen = GUI_PAUSE;
        game_session_reset_clock(app->session);
        return hit;
    }

    hotbar_update(&app->hotbar);
    UpdateCamera(&app->camera, CAMERA_FIRST_PERSON);
    const float vertical_speed = 5.0f;
    float vertical_input = 0.0f;
    if (IsKeyDown(KEY_SPACE)) vertical_input += 1.0f;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
        vertical_input -= 1.0f;
    float vertical_delta = vertical_input*vertical_speed*GetFrameTime();
    app->camera.position.y += vertical_delta;
    app->camera.target.y += vertical_delta;

    World *world = game_session_world(app->session);
    constrain_camera(app, world);

    Ray ray = GetScreenToWorldRayEx(
        (Vector2){gui_view_width()*0.5f, gui_view_height()*0.5f}, app->camera,
        gui_view_width(), gui_view_height());
    double camera_x = floor((double)app->camera.position.x + 0.5);
    double camera_z = floor((double)app->camera.position.z + 0.5);
    if (!isfinite(camera_x) || !isfinite(camera_z) || camera_x < (double)INT32_MIN+16 ||
        camera_x > (double)INT32_MAX-16 || camera_z < (double)INT32_MIN+16 || camera_z > (double)INT32_MAX-16 ||
        world_stream(world,(BlockPos){(int)camera_x,0,(int)camera_z},5) != RS_OK) {
        snprintf(app->message,sizeof(app->message),"Cannot stream world chunks");
        app->fatal_error = true; app->running = false;
        return hit;
    }
    hit = voxel_pick(world,
        (PickVec3){ray.position.x, ray.position.y, ray.position.z},
        (PickVec3){ray.direction.x, ray.direction.y, ray.direction.z}, 6.0f);
    BlockId selected = hotbar_selected_block(&app->hotbar);
    Direction facing = camera_horizontal_direction(&app->camera);
    InventorySlot held = hotbar_selected(&app->hotbar);
    if (held.kind == INVENTORY_TOOL && held.tool == TOOL_WOODEN_AXE) {
        for (unsigned point = 1; point <= 2; ++point) {
            int button = point == 1 ? MOUSE_BUTTON_LEFT : MOUSE_BUTTON_RIGHT;
            if (!IsMouseButtonPressed(button)) continue;
            RsResult result = RS_INVALID_ARGUMENT;
            if (hit.hit) {
                EditCommand edit = {.kind = point == 1 ? EDIT_POS1 : EDIT_POS2,
                    .position = hit.pos, .has_position = true};
                result = run_gui_edit(app, &edit);
            } else {
                snprintf(app->message, sizeof(app->message), "Point at a block to select it");
            }
            add_chat_message(app, app->message, result != RS_OK);
        }
    } else {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            apply_edit(app, hit, selected, false, facing);
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) ||
                         IsKeyDown(KEY_RIGHT_SHIFT);
            BlockId target = hit.hit ? (BlockId)world_get(world, hit.pos).id :
                                      BLOCK_AIR;
            if (flexible_placement_held(selected)) {
                apply_edit(app, hit, selected, true, facing);
            } else if (hit.hit && !shift &&
                target == BLOCK_BARREL) {
                open_barrel(app, hit.pos);
            } else if (hit.hit && !shift &&
                       (target == BLOCK_HOPPER || target == BLOCK_DROPPER)) {
                app->container_pos = hit.pos;
                app->screen = target == BLOCK_HOPPER ? GUI_HOPPER : GUI_DROPPER;
                release_mouse(app);
            } else if (hit.hit && !shift && target == BLOCK_REDSTONE_WIRE) {
                RsResult result = world_toggle_wire_dot(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message), "Toggle dot: %s",
                         rs_result_name(result));
            } else if (hit.hit && !shift && target == BLOCK_LEVER) {
                BlockState state = world_get(world, hit.pos);
                uint64_t before_revision = world_revision(world);
                world_set_lever_powered(world, hit.pos,
                                        !state.data.lever.powered);
                RsResult result = world_result(world);
                if (result == RS_OK &&
                    world_revision(world) == before_revision)
                    result = RS_UNCHANGED;
                world_clear_error(world);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message),
                         "Toggle lever: %s", rs_result_name(result));
            } else if (hit.hit && !shift &&
                       target == BLOCK_REDSTONE_REPEATER) {
                RsResult result = world_cycle_repeater_delay(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message),
                         "Cycle repeater delay: %s", rs_result_name(result));
            } else if (hit.hit && !shift &&
                       target == BLOCK_REDSTONE_COMPARATOR) {
                RsResult result = world_toggle_comparator_mode(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                snprintf(app->message, sizeof(app->message),
                         "Toggle comparator mode: %s", rs_result_name(result));
            } else if (hit.hit && !shift && target == BLOCK_NOTE_BLOCK) {
                RsResult result = world_cycle_note(world, hit.pos);
                if (result == RS_OK || result == RS_UNCHANGED)
                    result = game_session_flush(app->session);
                BlockState state = world_get(world, hit.pos);
                if ((result == RS_OK || result == RS_UNCHANGED) && state.id == BLOCK_NOTE_BLOCK)
                    snprintf(app->message, sizeof(app->message), "Note: %u (%s)",
                        (unsigned)state.data.note_block.note,
                        note_instrument_name((NoteInstrument)state.data.note_block.instrument));
                else
                    snprintf(app->message, sizeof(app->message), "Tune note: %s", rs_result_name(result));
            } else {
                if (hotbar_selected(&app->hotbar).kind == INVENTORY_ITEM)
                    snprintf(app->message, sizeof(app->message),
                             "Open a hopper, dropper or barrel to store items");
                else apply_edit(app, hit, selected, true, facing);
            }
        }
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) && hit.hit) {
        BlockId picked = pickable_block(
            (BlockId)world_get(world, hit.pos).id);
        if (picked != BLOCK_AIR &&
            hotbar_pick_block(&app->hotbar, picked)) {
            snprintf(app->message, sizeof(app->message),
                     "Hotbar slot %d: %s", app->hotbar.selected_slot + 1,
                     block_name(picked));
        } else {
            snprintf(app->message, sizeof(app->message),
                     "This block cannot be picked");
        }
    }

    return hit;
}

static void update_inventory(GuiApp *app)
{
    if (IsKeyPressed(KEY_E) || IsKeyPressed(KEY_ESCAPE)) {
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }

    hotbar_update(&app->hotbar);
    InventorySlot picked = inventory_update(&app->inventory);
    if (picked.kind != INVENTORY_EMPTY) {
        bool changed = hotbar_pick(&app->hotbar, picked);
        InventorySlot selected = hotbar_selected(&app->hotbar);
        if (selected.kind == INVENTORY_ITEM)
            snprintf(app->message, sizeof(app->message),
                     "Hotbar slot %d: %s x%u%s", app->hotbar.selected_slot + 1,
                     inventory_slot_name(selected), (unsigned)selected.stack.count,
                     changed ? "" : " (full)");
        else
            snprintf(app->message, sizeof(app->message),
                     "Hotbar slot %d: %s", app->hotbar.selected_slot + 1,
                     inventory_slot_name(selected));
    }

}

static bool write_container_slot(World *world, BlockPos pos, size_t index,
                                 ItemStack contents, RsResult *result)
{
    *result = world_set_container_slot(world, pos, index, contents);
    /* A simulation error may be reported after the slot was written. */
    ContainerStack committed;
    if (world_get_container_slot(world, pos, index, &committed) == RS_OK &&
        committed.item == contents.item && committed.count == contents.count &&
        committed.limit == contents.limit) return true;
    if (*result == RS_OK || *result == RS_UNCHANGED)
        *result = RS_INTERNAL_ERROR;
    return false;
}

static unsigned store_hotbar_items(World *world, BlockPos pos,
                                    InventorySlot *slot, unsigned amount,
                                    RsResult *result)
{
    unsigned moved = 0;
    size_t count = world_container_size(world, pos);
    for (int pass = 0; pass < 2 && moved < amount; ++pass) {
        for (size_t index = 0; index < count && moved < amount; ++index) {
            ContainerStack contents;
            *result = world_get_container_slot(world, pos, index, &contents);
            if (*result != RS_OK) return moved;
            if ((contents.count == 0) != (pass == 1)) continue;
            ItemStack remaining = slot->stack;
            unsigned added = item_stack_transfer(&contents, &remaining,
                                                   amount - moved);
            if (!added) continue;
            if (!write_container_slot(world, pos, index, contents, result))
                return moved;
            *slot = remaining.count ? (InventorySlot){
                .kind = INVENTORY_ITEM, .stack = remaining
            } : (InventorySlot){0};
            moved += added;
            if (*result != RS_OK && *result != RS_UNCHANGED) return moved;
        }
    }
    return moved;
}

static unsigned take_container_items(World *world, BlockPos pos, size_t index,
                                      Hotbar *hotbar, ItemStack contents,
                                      unsigned amount, RsResult *result)
{
    Hotbar updated = *hotbar;
    unsigned moved = 0;
    for (int pass = 0; pass < 3 && moved < amount; ++pass) {
        for (int slot = 0; slot < HOTBAR_SLOT_COUNT && moved < amount; ++slot) {
            InventorySlot *destination = &updated.slots[slot];
            if (pass == 0 && destination->kind != INVENTORY_ITEM) continue;
            if (pass == 1 && destination->kind != INVENTORY_EMPTY) continue;
            /* Creative block icons can be replaced in the selected slot. */
            if (pass == 2 && (slot != updated.selected_slot ||
                             destination->kind != INVENTORY_BLOCK)) continue;
            ItemStack stack = destination->kind == INVENTORY_ITEM ?
                              destination->stack : (ItemStack){0};
            unsigned added = item_stack_transfer(&stack, &contents,
                                                   amount - moved);
            if (!added) continue;
            *destination = (InventorySlot){.kind = INVENTORY_ITEM, .stack = stack};
            moved += added;
        }
    }
    if (!moved || !write_container_slot(world, pos, index, contents, result))
        return 0;
    *hotbar = updated;
    return moved;
}

static void update_container_slots(GuiApp *app, BlockPos pos,
                                    ContainerLayout layout)
{
    hotbar_update(&app->hotbar);
    bool left = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool right = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!left && !(right && shift)) return;

    int hotbar_slot = hotbar_slot_at(gui_view_mouse());
    int container_slot = -1;
    if (hotbar_slot >= 0) app->hotbar.selected_slot = hotbar_slot;
    else {
        for (int index = 0; index < layout.columns*layout.rows; ++index) {
            if (point_in(container_slot_rectangle(layout, index))) {
                container_slot = index;
                break;
            }
        }
    }
    if (hotbar_slot < 0 && container_slot < 0) return;

    RsResult result = game_session_flush(app->session);
    if (result != RS_OK) {
        snprintf(app->message, sizeof(app->message), "Transfer failed: %s",
                 rs_result_name(result));
        return;
    }
    World *world = game_session_world(app->session);
    ItemStack source;
    if (hotbar_slot >= 0) {
        InventorySlot slot = app->hotbar.slots[hotbar_slot];
        if (slot.kind != INVENTORY_ITEM) return;
        source = slot.stack;
    } else {
        result = world_get_container_slot(world, pos, (size_t)container_slot,
                                          &source);
        if (result != RS_OK) return;
    }
    if (!source.count) return;
    unsigned amount = !shift ? 1u : left ? source.count :
                      ((unsigned)source.count + 1u)/2u;
    unsigned moved = hotbar_slot >= 0 ?
        store_hotbar_items(world, pos, &app->hotbar.slots[hotbar_slot], amount,
                            &result) :
        take_container_items(world, pos, (size_t)container_slot, &app->hotbar,
                               source, amount, &result);
    if (!moved && (result == RS_OK || result == RS_UNCHANGED)) {
        snprintf(app->message, sizeof(app->message),
                 "No items moved: destination has no free space");
        return;
    }
    if (result == RS_OK || result == RS_UNCHANGED)
        result = game_session_flush(app->session);
    snprintf(app->message, sizeof(app->message), "%s %u: %s",
             hotbar_slot >= 0 ? "Stored" : "Retrieved", moved,
             rs_result_name(result));
}

static void update_barrel(GuiApp *app)
{
    World *world = game_session_world(app->session);
    BlockState state = world_get(world, app->barrel_pos);
    if (state.id != BLOCK_BARREL) {
        close_barrel(app);
        snprintf(app->message, sizeof(app->message),
                 "Barrel is no longer available");
        return;
    }
    if (IsKeyPressed(KEY_E) || IsKeyPressed(KEY_ESCAPE)) {
        close_barrel(app);
        return;
    }

    ContainerLayout layout = container_layout(BLOCK_BARREL);
    update_container_slots(app, app->barrel_pos, layout);
    int level = -1;
    state = world_get(world, app->barrel_pos);
    if (IsKeyPressed(KEY_LEFT) && state.data.barrel.level > 0)
        level = state.data.barrel.level - 1;
    if (IsKeyPressed(KEY_RIGHT) && state.data.barrel.level < 15)
        level = state.data.barrel.level + 1;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        for (int value = 0; value <= 15; ++value) {
            if (point_in(barrel_signal_rectangle(layout, value))) {
                level = value;
                break;
            }
        }
    }
    if (level >= 0) {
        RsResult result = world_set_barrel_level(world, app->barrel_pos,
                                                 (uint8_t)level);
        if (result == RS_OK || result == RS_UNCHANGED)
            result = game_session_flush(app->session);
        snprintf(app->message, sizeof(app->message), "Barrel signal %d: %s",
                 level, rs_result_name(result));
    }
}

static void update_container(GuiApp *app)
{
    World *world = game_session_world(app->session);
    BlockId id = app->screen == GUI_DROPPER ? BLOCK_DROPPER : BLOCK_HOPPER;
    if (world_get(world, app->container_pos).id != id ||
        IsKeyPressed(KEY_E) || IsKeyPressed(KEY_ESCAPE)) {
        app->screen = GUI_WORLD;
        capture_mouse(app);
        return;
    }
    update_container_slots(app, app->container_pos, container_layout(id));
}

static void draw_container(const GuiApp *app, BlockId id)
{
    bool barrel = id == BLOCK_BARREL;
    World *world = game_session_world(app->session);
    BlockPos pos = barrel ? app->barrel_pos : app->container_pos;
    ContainerLayout layout = container_layout(id);
    DrawRectangle(0, 0, gui_view_width(), gui_view_height(),
                  (Color){10, 18, 20, 155});
    DrawRectangleRec(layout.panel, (Color){226, 218, 196, 250});
    DrawRectangleLinesEx(layout.panel, 3.0f, (Color){73, 48, 24, 255});
    draw_centered(barrel ? "BARREL" : id == BLOCK_HOPPER ? "HOPPER" : "DROPPER",
                  (int)layout.panel.y + 18,
                  26, (Color){73, 48, 24, 255});
    BlockState state = world_get(world, pos);
    int level = barrel && state.id == BLOCK_BARREL ? state.data.barrel.level : 0;
    if (barrel)
        draw_centered(TextFormat("Comparator output: %d", level),
                      (int)layout.panel.y + 49, 17, (Color){100, 71, 36, 255});

    for (int index = 0; index < layout.columns*layout.rows; ++index) {
        Rectangle slot = container_slot_rectangle(layout, index);
        bool hovered = point_in(slot);
        DrawRectangleRec(slot, hovered ? (Color){102, 116, 112, 255} :
                                        (Color){75, 75, 75, 235});
        DrawRectangleLinesEx(slot, hovered ? 2.0f : 1.0f,
                             hovered ? (Color){244, 190, 88, 255} :
                                       (Color){145, 145, 145, 255});
        ContainerStack stack;
        if (world_get_container_slot(world, pos, (size_t)index, &stack) == RS_OK)
            inventory_draw_stack(&app->inventory, stack, slot);
    }
    if (barrel) {
        for (int value = 0; value <= 15; ++value) {
            Rectangle shortcut = barrel_signal_rectangle(layout, value);
            bool hovered = point_in(shortcut);
            DrawRectangleRec(shortcut, hovered ? (Color){111, 79, 50, 255} :
                                                (Color){82, 58, 37, 255});
            DrawRectangleLinesEx(shortcut, value == level ? 3.0f : 1.0f,
                                 value == level ? (Color){244, 190, 88, 255} :
                                                  (Color){142, 119, 86, 255});
            const char *text = TextFormat("%d", value);
            ui_text_draw(text,
                     (int)(shortcut.x + (shortcut.width - ui_text_width(text, 16))*0.5f),
                     (int)shortcut.y + 7, 16, RAYWHITE);
        }
    }
    hotbar_draw(&app->hotbar, &app->inventory);
}

static void draw_block_outline(BlockState state, Vector3 center)
{
    static BlockShape cached;
    static BlockShapeEdge edges[BLOCK_SHAPE_EDGES];
    static unsigned edge_count;
    BlockShape shape;
    block_outline_shape(state, &shape);
    if (shape.count != cached.count ||
        memcmp(shape.boxes, cached.boxes, shape.count*sizeof(shape.boxes[0]))) {
        cached = shape;
        edge_count = block_shape_edges(&shape, edges);
    }
    for (unsigned i = 0; i < edge_count; ++i) {
        const BlockShapeEdge *edge = &edges[i];
        Vector3 start = {center.x + edge->start[0]/16.0f - 0.5f,
                         center.y + edge->start[1]/16.0f - 0.5f,
                         center.z + edge->start[2]/16.0f - 0.5f};
        Vector3 end = {center.x + edge->end[0]/16.0f - 0.5f,
                       center.y + edge->end[1]/16.0f - 0.5f,
                       center.z + edge->end[2]/16.0f - 0.5f};
        DrawLine3D(start, end, YELLOW);
    }
}

static Vector3 placement_axis(Direction direction)
{
    BlockPos axis = block_offset((BlockPos){0}, direction);
    return (Vector3){(float)axis.x, (float)axis.y, (float)axis.z};
}

static Vector3 placement_grid_point(VoxelHit hit, PlacementTarget plan,
                                    float u, float v, float lift)
{
    Vector3 normal = placement_axis(hit.face);
    Vector3 right = placement_axis(plan.right), up = placement_axis(plan.up);
    float plane = hit.local.x*normal.x + hit.local.y*normal.y + hit.local.z*normal.z;
    return (Vector3){
        hit.pos.x + normal.x*(plane + lift) + right.x*u + up.x*v,
        hit.pos.y + normal.y*(plane + lift) + right.y*u + up.y*v,
        hit.pos.z + normal.z*(plane + lift) + right.z*u + up.z*v};
}

static void draw_placement_quad(VoxelHit hit, PlacementTarget plan,
                                const float points[4][2], float lift, Color color)
{
    Vector3 vertices[4];
    for (int i = 0; i < 4; ++i)
        vertices[i] = placement_grid_point(hit, plan, points[i][0], points[i][1], lift);
    DrawTriangle3D(vertices[0], vertices[1], vertices[2], color);
    DrawTriangle3D(vertices[0], vertices[2], vertices[3], color);
}

static void draw_flexible_placement(GuiApp *app, VoxelHit hit)
{
    BlockId selected = hotbar_selected_block(&app->hotbar);
    if (app->screen != GUI_WORLD || !hit.hit || !hit.has_face ||
        !flexible_placement_held(selected)) return;
    PlacementTarget plan = flexible_placement_target(hit, selected,
        camera_horizontal_direction(&app->camera), camera_direction(&app->camera),
        IsKeyDown(KEY_G), IsKeyDown(KEY_H));
    if ((unsigned)plan.right >= DIR_COUNT) return;
    World *world = game_session_world(app->session);
    bool available = plan.valid && world_in_bounds(world, plan.pos) &&
                     world_get(world, plan.pos).id == BLOCK_AIR;
    Color color = available ? (Color){72, 48, 184, 255} : RED;
    static const float outer[4][2] = {
        {-0.5f,-0.5f}, {0.5f,-0.5f}, {0.5f,0.5f}, {-0.5f,0.5f}};
    static const float regions[5][4][2] = {
        {{-0.25f,-0.25f}, {0.25f,-0.25f}, {0.25f,0.25f}, {-0.25f,0.25f}},
        {{-0.5f,-0.5f}, {-0.25f,-0.25f}, {-0.25f,0.25f}, {-0.5f,0.5f}},
        {{0.25f,-0.25f}, {0.5f,-0.5f}, {0.5f,0.5f}, {0.25f,0.25f}},
        {{-0.25f,0.25f}, {0.25f,0.25f}, {0.5f,0.5f}, {-0.5f,0.5f}},
        {{-0.5f,-0.5f}, {0.5f,-0.5f}, {0.25f,-0.25f}, {-0.25f,-0.25f}}
    };
    Color background = color;
    background.a = 40;
    draw_placement_quad(hit, plan, outer, 0.002f, background);
    Color highlight = color;
    highlight.a = 160;
    draw_placement_quad(hit, plan, regions[plan.region], 0.004f, highlight);
    for (int i = 0; i < 4; ++i) {
        int next = (i + 1)%4;
        Vector3 corner = placement_grid_point(hit, plan, outer[i][0], outer[i][1], 0.006f);
        Vector3 inner = placement_grid_point(hit, plan,
            regions[PLACEMENT_CENTER][i][0], regions[PLACEMENT_CENTER][i][1], 0.006f);
        DrawLine3D(corner, placement_grid_point(hit, plan,
            outer[next][0], outer[next][1], 0.006f), color);
        DrawLine3D(inner, placement_grid_point(hit, plan,
            regions[PLACEMENT_CENTER][next][0], regions[PLACEMENT_CENTER][next][1], 0.006f), color);
        DrawLine3D(corner, inner, color);
    }
    Vector3 center = {(float)plan.pos.x, (float)plan.pos.y, (float)plan.pos.z};
    DrawCubeWires(center, 1.002f, 1.002f, 1.002f, color);
    if (IsKeyDown(KEY_H) && plan.directional) {
        Vector3 direction = placement_axis(plan.direction);
        Direction wing_axis = plan.direction == plan.up ||
            direction_opposite(plan.direction) == plan.up ? plan.right : plan.up;
        Vector3 wing = placement_axis(wing_axis);
        Vector3 start = {center.x - direction.x*0.3f, center.y - direction.y*0.3f,
                         center.z - direction.z*0.3f};
        Vector3 end = {center.x + direction.x*0.4f, center.y + direction.y*0.4f,
                       center.z + direction.z*0.4f};
        DrawLine3D(start, end, color);
        for (int side = -1; side <= 1; side += 2) {
            Vector3 tip = {end.x - direction.x*0.16f + wing.x*0.12f*side,
                           end.y - direction.y*0.16f + wing.y*0.12f*side,
                           end.z - direction.z*0.16f + wing.z*0.12f*side};
            DrawLine3D(end, tip, color);
        }
    }
}

static void draw_selection(GuiApp *app)
{
    RegionEditor *editor=game_session_editor(app->session);
    BlockPos first, second;
    unsigned selected=region_editor_selection(editor,&first,&second);
    Color color={72,48,184,255};
    RegionBounds bounds;
    if (region_editor_bounds(editor,&bounds)) {
        BoundingBox box={
            {(float)((double)bounds.min.x-0.505),(float)((double)bounds.min.y-0.505),(float)((double)bounds.min.z-0.505)},
            {(float)((double)bounds.max.x+0.505),(float)((double)bounds.max.y+0.505),(float)((double)bounds.max.z+0.505)}};
        DrawBoundingBox(box,color);
    } else if (selected) {
        BlockPos pos=(selected&1) ? first : second;
        DrawCubeWires((Vector3){(float)pos.x,(float)pos.y,(float)pos.z},1.01f,1.01f,1.01f,color);
    }
}

static void draw_world(GuiApp *app, VoxelHit hit)
{
    /* BeginTextureMode already set the viewport and matrices to this size. */
    if (app->world_msaa.id) rlEnableFramebuffer(app->world_msaa.id);
    ClearBackground((Color){184, 211, 214, 255});
    BeginMode3D(app->camera);
        world_renderer_draw(app->world_renderer);
        if (hit.hit) {
            Vector3 center = {(float)hit.pos.x, (float)hit.pos.y,
                              (float)hit.pos.z};
            draw_block_outline(world_get(game_session_world(app->session), hit.pos),
                                center);
        }
        if (app->workspace.focus == 0) draw_flexible_placement(app, hit);
        draw_selection(app);
    EndMode3D();
    if (app->world_msaa.id)
        world_msaa_resolve(&app->world_msaa, app->gui_target.id);

    if (app->screen == GUI_WORLD && app->workspace.focus == 0) {
        int cx = gui_view_width()/2;
        int cy = gui_view_height()/2;
        DrawLine(cx - 6, cy, cx + 6, cy, BLACK);
        DrawLine(cx, cy - 6, cx, cy + 6, BLACK);
    }
    ui_text_draw(TextFormat("Position: (%.2f, %.2f, %.2f)",
             app->camera.position.x,
             app->camera.position.y,
             app->camera.position.z),
             15, 15, 18, DARKBLUE);
    if (hit.hit) {
        ui_text_draw(TextFormat("Target: (%d, %d, %d)",
                 hit.pos.x, hit.pos.y, hit.pos.z),
                 15, 39, 18, DARKBLUE);
    } else {
        ui_text_draw("Target: none", 15, 39, 18, DARKBLUE);
    }
    ui_text_draw(TextFormat("Facing: %s", horizontal_direction_name(
                 camera_horizontal_direction(&app->camera))),
             15, 63, 18, DARKBLUE);
    hotbar_draw(&app->hotbar, &app->inventory);
}

/* Return wrapped ranges so resizing also reflows messages already in history. */
static int wrap_chat_message(const char *text, int width,
                             size_t starts[CHAT_TEXT_CAPACITY],
                             size_t lengths[CHAT_TEXT_CAPACITY])
{
    int rows = 0;
    size_t offset = 0;
    while (text[offset] && rows < CHAT_TEXT_CAPACITY) {
        char line[CHAT_TEXT_CAPACITY];
        size_t length = 0, word_end = 0;
        while (text[offset + length] && text[offset + length] != '\n' &&
               length + 1 < sizeof(line)) {
            line[length] = text[offset + length];
            line[length + 1] = '\0';
            if (length && ui_text_width(line, CHAT_FONT_SIZE) > width) break;
            ++length;
            if (line[length - 1] == ' ') word_end = length;
        }
        if (text[offset + length] && text[offset + length] != '\n' && word_end)
            length = word_end;
        starts[rows] = offset;
        lengths[rows] = length;
        while (lengths[rows] && text[offset + lengths[rows] - 1] == ' ')
            --lengths[rows];
        ++rows;
        offset += length;
        if (text[offset] == '\n') ++offset;
        while (text[offset] == ' ') ++offset;
    }
    return rows;
}

static void draw_chat(GuiApp *app)
{
    bool open = app->screen == GUI_COMMAND;
    int width = gui_view_width() - 36;
    if (width > 760) width = 760;
    int bottom = gui_view_height() - 100;
    int visible = (bottom - 100)/CHAT_LINE_HEIGHT;
    int limit = open ? 20 : 6;
    if (visible > limit) visible = limit;
    if (width < 24 || visible <= 0) return;

    size_t starts[CHAT_TEXT_CAPACITY], lengths[CHAT_TEXT_CAPACITY];
    if (open) {
        int total = 0;
        for (size_t i = 0; i < app->chat_count; ++i)
            total += wrap_chat_message(app->chat[i].text, width - 12,
                                       starts, lengths);
        int maximum = total > visible ? total - visible : 0;
        if (app->chat_scroll > maximum) app->chat_scroll = maximum;
    }
    int skip = open ? app->chat_scroll : 0;
    int drawn = 0;
    double now = GetTime();
    for (size_t i = app->chat_count; i > 0 && drawn < visible; --i) {
        const ChatMessage *message = &app->chat[i - 1];
        if (!open && now - message->time >= 10.0) continue;
        int rows = wrap_chat_message(message->text, width - 12, starts, lengths);
        for (int row = rows - 1; row >= 0 && drawn < visible; --row) {
            if (skip) { --skip; continue; }
            char line[CHAT_TEXT_CAPACITY];
            memcpy(line, message->text + starts[row], lengths[row]);
            line[lengths[row]] = '\0';
            int y = bottom - (++drawn)*CHAT_LINE_HEIGHT;
            DrawRectangle(18, y - 2, width, CHAT_LINE_HEIGHT,
                          (Color){15, 27, 30, open ? 230 : 190});
            ui_text_draw(line, 24, y, CHAT_FONT_SIZE,
                     message->error ? (Color){255, 125, 125, 255} : RAYWHITE);
        }
    }
}

static void draw_command(const GuiApp *app)
{
    if (app->plot_command.pending) {
        int panel_width = gui_view_width() - 36;
        if (panel_width > 660) panel_width = 660;
        if (panel_width >= 48) {
            WorldPlotBounds b = app->plot_command.preview.removed;
            char warning[CHAT_TEXT_CAPACITY];
            snprintf(warning, sizeof(warning),
                     "Shrinking deletes cropped blocks and inventories, and may break connected blocks.\n"
                     "X %d..%d, Z %d..%d\nType y to confirm, or n to cancel.",
                     b.min_x,b.max_x,b.min_z,b.max_z);
            size_t starts[CHAT_TEXT_CAPACITY], lengths[CHAT_TEXT_CAPACITY];
            int rows = wrap_chat_message(warning, panel_width - 32, starts, lengths);
            int height = rows*CHAT_LINE_HEIGHT + 32;
            int x = (gui_view_width() - panel_width)/2, top = (gui_view_height() - height)/2;
            DrawRectangle(x,top,panel_width,height,(Color){15,27,30,245});
            DrawRectangleLines(x,top,panel_width,height,(Color){244,190,88,255});
            for (int i = 0; i < rows; ++i) {
                char line[CHAT_TEXT_CAPACITY];
                memcpy(line,warning + starts[i],lengths[i]); line[lengths[i]] = '\0';
                ui_text_draw(line,x + 16,top + 16 + i*CHAT_LINE_HEIGHT,CHAT_FONT_SIZE,
                         (Color){244,190,88,255});
            }
        }
    }
    int y = gui_view_height() - 88;
    int width = gui_view_width() - 36;
    DrawRectangle(18, y, width, 52,
                  (Color){15, 27, 30, 235});
    DrawRectangleLines(18, y, width, 52,
                       (Color){244, 190, 88, 255});
    const char *text = TextFormat("%s%s", app->plot_command.pending ? "y/n > " : "/", app->command);
    int text_x = 32;
    int cursor_x = text_x + ui_text_width(text, 21);
    int right = 18 + width - 14;
    if (cursor_x > right) {
        text_x -= cursor_x - right;
        cursor_x = right;
    }
    begin_gui_scissor(20, y + 2, width - 4, 48);
    ui_text_draw(text, text_x, y + 15, 21, RAYWHITE);
    if (((int)(GetTime()*2.0)) % 2 == 0)
        DrawLine(cursor_x, y + 11, cursor_x, y + 40, RAYWHITE);
    EndScissorMode();
}

static void draw_pause(GuiApp *app)
{
    bool active = app->workspace.focus == 0;
    DrawRectangle(0, 0, gui_view_width(), gui_view_height(),
                  (Color){10, 18, 20, 150});

    int center = gui_view_width()/2;
    Rectangle panel = {(float)center - 190, 105, 380, 400};

    DrawRectangleRec(panel, (Color){226, 218, 196, 248});
    DrawRectangleLinesEx(panel, 3.0f, (Color){34, 63, 66, 255});
    draw_centered("GUI MENU", 137, 30, (Color){26, 50, 55, 255});

    Rectangle resume = {(float)center - 140, 205, 280, 44};
    Rectangle save = {(float)center - 140, 265, 280, 44};
    Rectangle title = {(float)center - 140, 325, 280, 44};
    Rectangle exit = {(float)center - 140, 385, 280, 44};
    if (button(resume, "Continue", active)) {
        app->screen = GUI_WORLD;
        if (app->workspace.focus == 0) capture_mouse(app);
        else release_mouse(app);
        return;
    }

    if (button(save, "Save", active)) {
        GameSessionResult result = game_session_save(app->session);

        snprintf(app->message, sizeof(app->message), "Save: %s",
                 game_session_result_name(result));

        if (result != GAME_SESSION_OK) {
            app->screen_before_error = GUI_PAUSE;
            app->screen = GUI_SAVE_ERROR;
            app->after_save = AFTER_SAVE_NONE;
        }

        return;
    }

    if (button(title, "Save and title", active)) {
        request_save_action(app, AFTER_SAVE_TITLE);
        return;
    }

    if (button(exit, "Save and close GUI", active)) {
        request_save_action(app, AFTER_SAVE_GUI_DETACH);
        return;
    }
}

static void draw_save_error(GuiApp *app)
{
    bool active = app->workspace.focus == 0;
    ClearBackground((Color){39, 48, 49, 255});
    draw_centered("SAVE FAILED", 130, 30, RAYWHITE);
    draw_centered(app->message, 185, 19, (Color){244, 190, 88, 255});
    draw_centered("The last valid .mcw file is still intact.",
                  220, 18, (Color){193, 205, 204, 255});
    int center = gui_view_width()/2;
    Rectangle retry = {(float)center - 160, 290, 320, 46};
    Rectangle cancel = {(float)center - 160, 352, 320, 46};
    Rectangle leave = {(float)center - 160, 414, 320, 46};
    if (button(retry, "Retry save", active)) {
        GameSession *failed_session = NULL;
        GameSessionResult result;
        if (app->after_save == AFTER_SAVE_NONE ||
            app->after_save == AFTER_SAVE_TITLE ||
            app->after_save == AFTER_SAVE_GUI_DETACH) {
            result = app->session ? game_session_save(app->session) : GAME_SESSION_OK;
        } else {
            result = save_all_worlds(app, &failed_session);
        }
        if (result == GAME_SESSION_OK) {
            if (app->after_save == AFTER_SAVE_NONE)
                app->screen = app->screen_before_error;
            else
                perform_after_save(app, app->after_save);
        } else {
            snprintf(app->message, sizeof(app->message), "Save failed: %s",
                     game_session_result_name(result));
        }
    }
    if (button(cancel, "Return to world", active)) {
        app->screen = app->screen_before_error;
        app->after_save = AFTER_SAVE_NONE;
    }
    const char *leave_label = app->after_save == AFTER_SAVE_TITLE ?
        "Return to title; keep session" :
        app->after_save == AFTER_SAVE_GUI_DETACH ?
        "Close GUI; keep session" : "Exit without saving";
    if (button(leave, leave_label,
               active && app->after_save != AFTER_SAVE_NONE)) {
        AfterSave action = app->after_save;
        if (action == AFTER_SAVE_TITLE) {
            return_to_title(app);
        } else if (action == AFTER_SAVE_GUI_DETACH) {
            detach_gui_frontend(app);
        } else if (action == AFTER_SAVE_HOST_EXIT) {
            close_world(app);
            app->running = false;
        }
    }
}

static bool initialize(GuiApp *app, WorldSessionManager *world_sessions)
{
    *app = (GuiApp){0};
    if (!world_sessions) {
        fprintf(stderr, "World session manager is unavailable\n");
        return false;
    }
    app->world_sessions = world_sessions;
    app->running = true;
    app->selected_save = -1;
    app->screen = GUI_TITLE;
    snprintf(app->message, sizeof(app->message),
             "Create a world or load an existing .mcw file");

    unsigned int window_flags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT;
#if defined(__APPLE__)
    window_flags |= FLAG_WINDOW_HIGHDPI;
#endif
    SetConfigFlags(window_flags);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "mcmux");
    SetWindowMinSize(1000, 650);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    if (!ui_font_init()) return false;
    app->title_background = LoadTexture(gui_asset_path("res/gui/title_background.png"));
    if (!app->title_background.id) {
        fprintf(stderr, "Cannot load title background; run from the project root\n");
        return false;
    }
    SetTextureFilter(app->title_background, TEXTURE_FILTER_BILINEAR);
    if (!block_renderer_init(&app->blocks)) {
        fprintf(stderr, "Cannot load block textures; run from the project root\n");
        return false;
    }
    if (!inventory_init(&app->inventory)) {
        fprintf(stderr, "Cannot load inventory textures; run from the project root\n");
        return false;
    }
    if (!hotbar_init(&app->hotbar, &app->inventory)) {
        fprintf(stderr, "Cannot load hotbar textures; run from the project root\n");
        return false;
    }
    workspace_layout_init(&app->workspace);
    app->output=workspace_output_create();
    if (!app->output) {
        fprintf(stderr,"Cannot allocate mc-output history\n");
        return false;
    }
    app->code=workspace_code_create();
    if (!app->code) {
        fprintf(stderr,"Cannot allocate code navigation state\n");
        return false;
    }
    app->workspace.gui_width = GetScreenWidth();
    app->window_gui_visible = true;
    app->window_width = GetScreenWidth();
    app->window_height = GetScreenHeight();
    app->window_text_width = WORKSPACE_TEXT_WIDTH;
    refresh_saves(app);
    return true;
}

static void shutdown_gui(GuiApp *app)
{
    close_world(app);
    for (int i = 0; i < WORKSPACE_TEXT_LIMIT; ++i) {
        workspace_editor_destroy(app->editors[i]);
        app->editors[i] = NULL;
    }
    workspace_output_destroy(app->output);
    app->output=NULL;
    workspace_code_destroy(app->code);
    app->code=NULL;
    if (app->gui_target.id) UnloadRenderTexture(app->gui_target);
    app->gui_target = (RenderTexture2D){0};
    if (app->title_background.id) UnloadTexture(app->title_background);
    app->title_background = (Texture2D){0};
    hotbar_unload(&app->hotbar);
    inventory_unload(&app->inventory);
    block_renderer_unload(&app->blocks);
    ui_font_unload();
    CloseWindow();
}

static Rectangle pane_rectangle(WorkspaceRect box)
{
    return (Rectangle){(float)box.x, (float)box.y, (float)box.width, (float)box.height};
}

static int workspace_height(const GuiApp *app)
{
    int height = GetScreenHeight();
    WorkspaceGeometry geometry;
    workspace_layout_geometry(&app->workspace, GetScreenWidth(), height, &geometry);
    if (app->session || app->screen == GUI_SAVE_ERROR || app->pane_message[0] || geometry.compact)
        height -= WORKSPACE_STATUS_HEIGHT;
    return height > 0 ? height : 1;
}

static void workspace_geometry(const GuiApp *app, WorkspaceGeometry *geometry)
{
    workspace_layout_geometry(&app->workspace, GetScreenWidth(), workspace_height(app), geometry);
}

/* Remember the split text width separately from its standalone window size.
 * Either frontend alone gets at least the preferred GUI width. */
static void sync_workspace_window(GuiApp *app)
{
    bool gui = app->workspace.gui_visible, text = app->workspace.root >= 0;
    if (gui != app->window_gui_visible || text != app->window_text_visible) {
        /* A second action can arrive before Windows acknowledges the first
         * resize. Use its requested dimensions, never the stale native size. */
        bool use_requested_size = app->window_resize_pending || IsWindowMaximized();
        int width = use_requested_size ? app->window_width : GetScreenWidth();
        int text_width = WORKSPACE_TEXT_WIDTH;
        if (app->window_text_visible) {
            text_width = app->window_text_width;
            if (app->window_gui_visible) {
                int gui_width = app->workspace.gui_width;
                int available = width - WORKSPACE_DIVIDER - WORKSPACE_MIN_WIDTH;
                if (gui_width > available) gui_width = available;
                text_width = width - gui_width - WORKSPACE_DIVIDER;
            }
            if (text_width < WORKSPACE_MIN_WIDTH) text_width = WORKSPACE_MIN_WIDTH;
        }
        int target = app->workspace.gui_width;
        if (gui && text) target += text_width + WORKSPACE_DIVIDER;
        else if (text && text_width > target) target = text_width;
        int minimum_width = gui ? 1000 : WORKSPACE_MIN_WIDTH;
        if (gui && text)
            minimum_width = app->workspace.gui_width + WORKSPACE_DIVIDER + WORKSPACE_MIN_WIDTH;
        int minimum_height = gui ? 650 : WORKSPACE_MIN_HEIGHT + WORKSPACE_STATUS_HEIGHT;
        int height = use_requested_size ? app->window_height : GetScreenHeight();
        if (height < minimum_height) height = minimum_height;

        /* Keep the preferred GUI size separate from platform resize callbacks.
         * SetWindowMinSize() also resizes a GLFW window on Windows. */
        app->window_gui_visible = gui;
        app->window_text_visible = text;
        app->window_width = target;
        app->window_height = height;
        app->window_text_width = text_width;
        app->window_resize_pending = true;
        if (IsWindowMaximized()) RestoreWindow();
        SetWindowMinSize(minimum_width, minimum_height);
        SetWindowSize(target, height);
        int monitor = GetCurrentMonitor();
        Vector2 origin = GetMonitorPosition(monitor), position = GetWindowPosition();
        int right = (int)origin.x + GetMonitorWidth(monitor) - 16;
        int x = (int)position.x;
        if (x + target > right) x = right - target;
        if (x < (int)origin.x) x = (int)origin.x;
        if (x != (int)position.x) SetWindowPosition(x, (int)position.y);
        return;
    }

    if (app->window_resize_pending) {
        if (GetScreenWidth() != app->window_width || GetScreenHeight() != app->window_height)
            SetWindowSize(app->window_width, app->window_height);
        else if (!IsWindowResized()) app->window_resize_pending = false;
        return;
    }
    /* Only a settled GUI-only window can supply a new user-selected width. */
    if (!IsWindowMaximized()) {
        if (gui && !text) app->workspace.gui_width = GetScreenWidth();
        app->window_width = GetScreenWidth();
        app->window_height = GetScreenHeight();
    }
}

static void pane_notice(GuiApp *app, const char *text)
{
    snprintf(app->pane_message, sizeof(app->pane_message), "%s", text);
}

/* The editor/output stay responsive while the child owns simulation time. */
static void sync_program_activity(GuiApp *app)
{
    bool active = workspace_output_is_program_running(app->output);
    if (active && !app->program_active) {
        app->program_gui_visible = app->workspace.gui_visible;
    } else if (!active && app->program_active) {
        app->workspace.gui_visible = app->program_gui_visible ||
                                     app->screen == GUI_SAVE_ERROR;
        for (size_t i = 0; i < world_session_manager_count(app->world_sessions); ++i)
            game_session_reset_clock(world_session_manager_at(app->world_sessions, i));
        /* The last frame may include time spent waiting for the child. */
        app->skip_world_advance = true;
    }
    app->program_active = active;
    /* An exit-time save error must remain visible and actionable. */
    if (active && app->screen != GUI_SAVE_ERROR) {
        app->workspace.gui_visible = false;
        if (!app->workspace.focus) workspace_layout_next(&app->workspace);
        release_mouse(app);
    }
}

static bool prepare_program_run(void *context, WorkspaceEditor *editor)
{
    GuiApp *app = context;
    if (app->session && app->screen == GUI_BARREL) {
        (void)world_close_barrel(game_session_world(app->session), app->barrel_pos);
        app->screen = GUI_WORLD;
    }
    GameSession *failed_session = NULL;
    GameSessionResult result = save_all_worlds(app, &failed_session);
    if (result != GAME_SESSION_OK) {
        char message[MESSAGE_CAPACITY];
        snprintf(message, sizeof(message), "Cannot run: %s save failed: %s",
                 managed_world_name(app, failed_session),
                 game_session_result_name(result));
        workspace_editor_message(editor, message, true);
        return false;
    }

    return_to_title(app);
    /* A C program may write any save, including worlds without a GUI pane. */
    world_session_manager_clear(app->world_sessions);
    if (app->gui_target.id) UnloadRenderTexture(app->gui_target);
    app->gui_target = (RenderTexture2D){0};
    app->camera = (Camera3D){0};
    app->screen_before_error = GUI_TITLE;
    app->after_save = AFTER_SAVE_NONE;
    app->world_name[0] = '\0';
    app->world_name_length = 0;
    app->workspace.gui_visible = false;
    return true;
}

static bool prepare_gui_view(GuiApp *app)
{
    gui_view_reset();
    /* Outside BeginTextureMode, these dimensions describe the window. */
    float scale_x = (float)GetRenderWidth()/GetScreenWidth();
    float scale_y = (float)GetRenderHeight()/GetScreenHeight();
    ui_font_set_scale(scale_y);
    if (!app->workspace.gui_visible) {
        world_msaa_destroy(&app->world_msaa);
        return true;
    }
    WorkspaceGeometry geometry;
    workspace_geometry(app, &geometry);
    Rectangle box = pane_rectangle(geometry.gui);
    box.y += WORKSPACE_HEADER_HEIGHT;
    box.height -= WORKSPACE_HEADER_HEIGHT;
    int width = (int)box.width, height = (int)box.height;
    if (width < 640) width = 640;
    if (height < 560) height = 560;
    int pixel_width = (int)roundf(width*scale_x);
    int pixel_height = (int)roundf(height*scale_y);
    if (!app->gui_target.id || app->gui_target.texture.width != pixel_width ||
        app->gui_target.texture.height != pixel_height) {
        RenderTexture2D target = LoadRenderTexture(pixel_width, pixel_height);
        if (!target.id || !target.texture.id || !rlFramebufferComplete(target.id)) {
            if (target.id) UnloadRenderTexture(target);
            fprintf(stderr, "Cannot create GUI pane render target\n");
            app->fatal_error = true;
            app->running = false;
            return false;
        }
        SetTextureFilter(target.texture, TEXTURE_FILTER_POINT);
        if (app->gui_target.id) UnloadRenderTexture(app->gui_target);
        app->gui_target = target;
    }
    if (app->session && app->screen != GUI_TITLE &&
        app->screen != GUI_SAVE_ERROR)
        world_msaa_prepare(&app->world_msaa, pixel_width, pixel_height);
    else world_msaa_destroy(&app->world_msaa);
    gui_view_set(box, width, height);
    return true;
}

/* A focus transition leaves GUI-only dialogs in a consistent world state. */
static void apply_pane_focus(GuiApp *app)
{
    if (app->workspace.focus != 0) {
        if (app->session && app->screen == GUI_BARREL)
            (void)world_close_barrel(game_session_world(app->session), app->barrel_pos);
        if (app->session && app->screen == GUI_COMMAND && app->plot_command.pending) {
            (void)plot_command_answer(game_session_world(app->session), &app->plot_command,
                "n", app->message, sizeof(app->message));
            add_chat_message(app, app->message, false);
        }
        if (app->session && app->screen >= GUI_COMMAND && app->screen <= GUI_DROPPER)
            app->screen = GUI_WORLD;
        release_mouse(app);
    } else if (app->session && app->screen == GUI_WORLD) capture_mouse(app);
    while (GetCharPressed() != 0) {}
}

static int split_editor_pane(GuiApp *app, bool split_x)
{
    int slot = 1;
    while (slot <= WORKSPACE_TEXT_LIMIT &&
           workspace_layout_has_pane(&app->workspace, slot)) ++slot;
    if (slot > WORKSPACE_TEXT_LIMIT) {
        pane_notice(app, "Maximum text pane count reached");
        return -1;
    }
    WorkspaceEditor *editor = workspace_editor_create();
    if (!editor) {
        pane_notice(app, "Cannot allocate an mc-nvim pane");
        return -1;
    }
    int pane = workspace_layout_split(&app->workspace, split_x,
        GetScreenWidth(), workspace_height(app));
    if (pane < 0) {
        workspace_editor_destroy(editor);
        pane_notice(app,
            "No room for a new mc-nvim pane; enlarge the window or close a pane");
        return -1;
    }
    app->editors[pane - 1] = editor;
    workspace_editor_set_output(editor,app->output,pane);
    workspace_editor_set_code(editor,app->code);
    app->pane_message[0] = '\0';
    return pane;
}

static int open_output_pane(GuiApp *app)
{
    if (app->output_pane && workspace_layout_has_pane(&app->workspace,app->output_pane)) {
        app->workspace.focus=app->output_pane;
        return app->output_pane;
    }
    int pane=workspace_layout_split(&app->workspace,false,
        GetScreenWidth(),workspace_height(app));
    if (pane<0) {
        pane_notice(app,"No room for mc-output; enlarge the window or close a pane");
        return -1;
    }
    app->output_pane=pane;
    app->pane_message[0]='\0';
    return pane;
}

static bool close_pane(GuiApp *app, bool force)
{
    int pane = app->workspace.focus;
    if (!pane && app->workspace.root < 0) {
        if (split_editor_pane(app, false) < 0) return false;
        app->workspace.focus = 0;
    }
    if (app->program_active && pane > 0 && app->workspace.root >= 0 &&
        app->workspace.nodes[app->workspace.root].first < 0) {
        pane_notice(app, "Keep a text pane open until :make/:run finishes");
        return false;
    }
    if (!force && pane > 0 && app->editors[pane - 1] &&
        workspace_editor_has_unsaved(app->editors[pane - 1])) {
        pane_notice(app, "mc-nvim has unsaved buffers; save them with :w before closing");
        workspace_editor_message(app->editors[pane - 1],
            "unsaved buffers; :e each file and :w before closing", true);
        return false;
    }
    if (!workspace_layout_close(&app->workspace)) {
        pane_notice(app, "Keep one pane open; close the host window to end the workspace");
        return false;
    }
    if (pane > 0) {
        if (pane==app->output_pane) app->output_pane=0;
        workspace_editor_destroy(app->editors[pane - 1]);
        app->editors[pane - 1] = NULL;
    } else if (app->session && app->screen == GUI_BARREL) {
        (void)world_close_barrel(game_session_world(app->session), app->barrel_pos);
        app->screen = GUI_WORLD;
    }
    app->pane_message[0] = '\0';
    apply_pane_focus(app);
    return true;
}

/* Prefix keys are consumed before either frontend receives this frame's input. */
static bool update_pane_keys(GuiApp *app)
{
    bool control = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (control && IsKeyPressed(KEY_B)) {
        app->pane_help = false;
        app->pane_prefix = true;
        app->pane_message[0] = '\0';
        release_mouse(app);
        while (GetCharPressed() != 0) {}
        return true;
    }
    if (app->pane_help) {
        if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_Q)) {
            app->pane_help = false;
            apply_pane_focus(app);
        }
        while (GetCharPressed() != 0) {}
        return true;
    }
    if (!app->pane_prefix) return false;
    if (IsKeyPressed(KEY_ESCAPE)) {
        app->pane_prefix = false;
        apply_pane_focus(app);
        return true;
    }
    WorkspaceDirection direction;
    bool arrow = true;
    if (IsKeyPressed(KEY_LEFT)) direction = WORKSPACE_LEFT;
    else if (IsKeyPressed(KEY_RIGHT)) direction = WORKSPACE_RIGHT;
    else if (IsKeyPressed(KEY_UP)) direction = WORKSPACE_UP;
    else if (IsKeyPressed(KEY_DOWN)) direction = WORKSPACE_DOWN;
    else { direction = WORKSPACE_LEFT; arrow = false; }
    bool acted = arrow;
    if (arrow) {
        if (control) {
            bool resized = workspace_layout_resize(&app->workspace, direction, 24,
                GetScreenWidth(), workspace_height(app));
            pane_notice(app, resized ? "Text divider moved" : "No movable text divider in this direction; GUI size is fixed");
        } else (void)workspace_layout_focus(&app->workspace, direction,
            GetScreenWidth(), workspace_height(app));
    }
    int character;
    while ((character = GetCharPressed()) != 0) {
        if (acted) continue;
        acted = true;
        switch (character) {
        case '%': (void)split_editor_pane(app, true); break;
        case '"': (void)split_editor_pane(app, false); break;
        case 'o': workspace_layout_next(&app->workspace); break;
        case 'c': (void)open_output_pane(app); break;
        case 'H': case 'J': case 'K': case 'L': {
            WorkspaceDirection move = character == 'H' ? WORKSPACE_LEFT :
                character == 'L' ? WORKSPACE_RIGHT : character == 'K' ? WORKSPACE_UP : WORKSPACE_DOWN;
            bool resized = workspace_layout_resize(&app->workspace, move, 24,
                GetScreenWidth(), workspace_height(app));
            pane_notice(app, resized ? "Text divider moved" : "No movable text divider; GUI size is fixed");
            break;
        }
        case 'z':
            if (!workspace_layout_zoom(&app->workspace))
                pane_notice(app, "GUI size is fixed; zoom applies to text panes");
            else pane_notice(app, "Text-pane zoom toggled");
            break;
        case 'x': (void)close_pane(app, false); break;
        case 'g':
            if (app->program_active)
                pane_notice(app, "mc-gui is paused during :make/:run; stop the task with mc-output Ctrl+C");
            else workspace_layout_show_gui(&app->workspace);
            break;
        case '?': app->pane_help = true; break;
        default: pane_notice(app, "Unknown pane key"); break;
        }
    }
    if (acted) {
        app->pane_prefix = false;
        if (!app->pane_help) apply_pane_focus(app);
    }
    return true;
}

static void update_text_pane(GuiApp *app)
{
    int pane = app->workspace.focus;
    if (pane>0 && pane==app->output_pane) {
        workspace_output_update(app->output);
        return;
    }
    if (pane <= 0 || !app->editors[pane - 1]) return;
    WorkspaceEditorAction action = workspace_editor_update(app->editors[pane - 1],
        prepare_program_run, app);
    if (action == WORKSPACE_EDITOR_QUIT ||
        action == WORKSPACE_EDITOR_QUIT_FORCE) {
        bool force = action == WORKSPACE_EDITOR_QUIT_FORCE;
        if (!close_pane(app, force)) {
            if (pane > 0 && !app->workspace.gui_visible && !app->program_active) {
                workspace_layout_show_gui(&app->workspace);
                app->workspace.focus = pane;
                (void)close_pane(app, force);
            } else {
                pane_notice(app, "Cannot close the last visible pane");
            }
        }
    }
}

static VoxelHit update_workspace_input(GuiApp *app)
{
    VoxelHit hit = {0};
    workspace_output_poll(app->output);
    sync_program_activity(app);
    workspace_code_poll(app->code);
    sync_workspace_window(app);
    if (!prepare_gui_view(app)) return hit;
    if (!IsWindowFocused() && app->workspace.focus == 0 && app->session &&
        app->screen != GUI_PAUSE &&
        app->screen != GUI_SAVE_ERROR) {
        release_mouse(app);
        app->screen = GUI_PAUSE;
        app->pane_prefix = app->pane_help = false;
    } else if (!update_pane_keys(app)) {
        /* GUI modal keys are routed only while pane 0 owns focus. */
        if (app->workspace.focus > 0) update_text_pane(app);
        else if (app->screen == GUI_TITLE) update_title(app);
        else if (app->screen == GUI_PAUSE) {
            if (IsKeyPressed(KEY_ESCAPE)) {
                app->screen = GUI_WORLD;
                apply_pane_focus(app);
            }
        }
        else if (app->screen == GUI_WORLD) {
            if (!app->mouse_captured) capture_mouse(app);
            else hit = update_world(app);
        }
        else if (app->screen == GUI_COMMAND) update_command(app);
        else if (app->screen == GUI_INVENTORY) update_inventory(app);
        else if (app->screen == GUI_BARREL) update_barrel(app);
        else if (app->screen == GUI_HOPPER || app->screen == GUI_DROPPER) update_container(app);
    }
    sync_program_activity(app);
    sync_workspace_window(app);
    return hit;
}

static void draw_pane_help(void)
{
    const char *lines[] = {
        "Workspace keys - press Ctrl+b, release, then:",
        "%                 Split mc-nvim left / right",
        "\"                 Split mc-nvim top / bottom",
        "Arrow             Focus pane in that direction",
        "o                 Focus next pane",
        "c                 Open / focus mc-output",
        "Ctrl+Arrow/HJKL   Move nearest text divider",
        "z                 Zoom / restore text area",
        "x                 Close focused pane",
        "g                 Restore and focus mc-gui",
        "?                 Show this help",
        "Esc               Cancel prefix / close help",
        "",
        "mc-nvim: v/V select, y copy, d delete, dd/yy line, D to end, p/P paste",
        "mc-nvim: i/a insert, Esc normal, :Ex, :e path, :w",
        "mc-nvim: :make builds the current C file; :run builds and runs it",
        "mc-nvim: gd definition, gD declaration, K information, Ctrl+o/Ctrl+t back",
        "mc-nvim: :q, :q!, :wq; Arrows/hjkl move; :! runs an external command",
        "mc-output: PgUp/PgDn scroll, End follow, Ctrl+C stop command",
        "mc-gui: / and // open world and simulation commands."
    };
    int width = GetScreenWidth() - 80;
    if (width > 800) width = 800;
    int height = (int)(sizeof(lines)/sizeof(lines[0]))*26 + 36;
    int x = (GetScreenWidth() - width)/2, y = (GetScreenHeight() - height)/2;
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){0, 0, 0, 170});
    DrawRectangle(x, y, width, height, (Color){22, 27, 36, 255});
    DrawRectangleLines(x, y, width, height, (Color){172, 143, 255, 255});
    BeginScissorMode(x, y, width, height);
    for (size_t i = 0; i < sizeof(lines)/sizeof(lines[0]); ++i)
        ui_text_draw(lines[i], x + 18, y + 18 + (int)i*26, 18, RAYWHITE);
    EndScissorMode();
}

static void draw_workspace(GuiApp *app, VoxelHit hit)
{
    WorkspaceGeometry geometry;
    workspace_geometry(app, &geometry);
    ClearBackground((Color){9, 12, 18, 255});
    if (app->workspace.gui_visible) {
        begin_gui_target(app->gui_target);
        if (app->screen == GUI_TITLE) {
            draw_title(app);
        } else if (app->screen == GUI_SAVE_ERROR) {
            draw_save_error(app);
        } else if (app->session) {
            draw_world(app, hit);
            if (app->screen == GUI_INVENTORY) {
                inventory_draw(&app->inventory, hotbar_selected(&app->hotbar));
                hotbar_draw(&app->hotbar, &app->inventory);
            }
            if (app->screen == GUI_BARREL) draw_container(app, BLOCK_BARREL);
            if (app->screen == GUI_HOPPER) draw_container(app, BLOCK_HOPPER);
            if (app->screen == GUI_DROPPER) draw_container(app, BLOCK_DROPPER);
            if (app->screen == GUI_WORLD || app->screen == GUI_COMMAND) draw_chat(app);
            if (app->screen == GUI_COMMAND) draw_command(app);
            if (app->screen == GUI_PAUSE) draw_pause(app);
        } else {
            ClearBackground((Color){39, 48, 49, 255});
        }
        EndTextureMode();
        Rectangle box = pane_rectangle(geometry.gui);
        box.y += WORKSPACE_HEADER_HEIGHT; box.height -= WORKSPACE_HEADER_HEIGHT;
        DrawTexturePro(app->gui_target.texture,
            (Rectangle){0, 0, (float)app->gui_target.texture.width, -(float)app->gui_target.texture.height},
            box, (Vector2){0}, 0, WHITE);
        DrawRectangle(geometry.gui.x, geometry.gui.y, geometry.gui.width, WORKSPACE_HEADER_HEIGHT,
            (Color){34, 42, 53, 255});
        Color accent = app->workspace.focus == 0 ? (Color){172, 143, 255, 255} : (Color){118, 130, 143, 255};
        ui_text_draw(app->workspace.focus == 0 ? "0: mc-gui *" : "0: mc-gui", 8, 5, 16, accent);
        DrawRectangleLinesEx(pane_rectangle(geometry.gui), app->workspace.focus == 0 ? 2 : 1, accent);
    }
    gui_view_reset();
    for (int i = 0; i < WORKSPACE_TEXT_LIMIT; ++i) {
        if (geometry.visible[i] && app->output_pane==i+1)
            workspace_output_draw(app->output,pane_rectangle(geometry.panes[i]),
                i+1,app->workspace.focus==i+1,app->workspace.zoom>0 || geometry.compact);
        if (geometry.visible[i] && app->editors[i])
            workspace_editor_draw(app->editors[i], pane_rectangle(geometry.panes[i]),
                i + 1, app->workspace.focus == i + 1,
                app->workspace.zoom > 0 || geometry.compact);
    }
    int y = GetScreenHeight() - WORKSPACE_STATUS_HEIGHT;
    const char *state = app->screen == GUI_SAVE_ERROR ? "SAVE ERROR" :
        !app->session ?
        "" :
        app->screen == GUI_PAUSE ? "GUI MENU" :
        sim_is_frozen(game_session_world(app->session)) ? "FROZEN" : "RUNNING";
    const char *message = geometry.compact ? "Text area too small: showing one pane; enlarge window to restore layout" : app->pane_message;
    bool show_status = state[0] || geometry.compact || app->pane_message[0];
    if (show_status) {
        DrawRectangle(0, y, GetScreenWidth(), WORKSPACE_STATUS_HEIGHT, (Color){37, 28, 57, 255});
        BeginScissorMode(0, y, GetScreenWidth(), WORKSPACE_STATUS_HEIGHT);
        ui_text_draw(TextFormat("%s%s%s", state,
            state[0] && message[0] ? " | " : "", message), 8, y + 5, 16, RAYWHITE);
        EndScissorMode();
    }
    if (app->pane_help) draw_pane_help();
}

int gui_run(WorldSessionManager *world_sessions)
{
    GuiApp app;
    if (!initialize(&app, world_sessions)) {
        shutdown_gui(&app);
        return EXIT_FAILURE;
    }

    while (app.running) {
        bool close_requested = WindowShouldClose();
        if (close_requested && !app.close_latched &&
            app.screen != GUI_SAVE_ERROR) {
            app.close_latched = true;
            if (app.screen == GUI_BARREL) {
                (void)world_close_barrel(game_session_world(app.session), app.barrel_pos);
                app.screen = GUI_WORLD;
            }
            if (any_world_dirty(&app))
                request_save_action(&app, AFTER_SAVE_HOST_EXIT);
            else
                app.running = false;
        }
        if (!app.running) break;

        app.skip_world_advance = false;
        gui_view_reset();
        VoxelHit hit = {0};
        hit = update_workspace_input(&app);
        if (!app.running) break;

        advance_world_sessions(&app);
        if (!app.running) break;

        if (!app.program_active && app.workspace.gui_visible &&
            app.world_renderer && !world_renderer_sync(app.world_renderer)) {
            fprintf(stderr, "Cannot update render cache\n");
            app.fatal_error = true;
            break;
        }

        BeginDrawing();
        if (prepare_gui_view(&app)) draw_workspace(&app, hit);
        EndDrawing();
    }

    if (app.fatal_error && any_world_dirty(&app))
        (void)save_all_worlds(&app, NULL);
    shutdown_gui(&app);
    return app.fatal_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
