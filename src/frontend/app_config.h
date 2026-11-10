#ifndef MC_APP_CONFIG_H
#define MC_APP_CONFIG_H
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    APP_WORKSPACE, APP_NORMAL, APP_VISUAL, APP_INSERT, APP_COMMAND,
    APP_BROWSER, APP_INFO, APP_OUTPUT, APP_WORLD, APP_SCOPE_COUNT
} AppKeyScope;
typedef struct {
    bool vsync;
    int fps_limit, msaa_samples, editor_font_size, output_font_size, tab_width;
    int stream_radius, leader_timeout_ms;
    float fov, mouse_sensitivity, move_speed, scroll_lines;
} AppSettings;

/* A failed reload preserves the complete previous configuration. */
bool app_config_init(void);
bool app_config_reload(void);
bool app_config_ensure_file(void);
const AppSettings *app_config_current(void);
const char *app_config_path(void);
const char *app_config_error(void);
unsigned app_config_revision(void);
size_t app_config_action_count(AppKeyScope scope);
const char *app_config_action_name(AppKeyScope scope, size_t index);
void app_config_describe(AppKeyScope scope, const char *action, char *text, size_t capacity);

/* Snapshot physical input once per GUI frame. Scoped views map actions to the
 * editor's existing key/character semantics; text and IME input stay literal. */
void app_input_frame(void);
void app_input_scope(AppKeyScope scope);
bool app_input_leader(void);
const char *app_input_workspace(bool after_leader);
bool app_input_world(const char *action, bool held);
const char *app_input_command(void);
bool app_input_resets_operator(void);
int app_input_char(void);
bool app_input_pressed(int key);
bool app_input_repeat(int key);
bool app_input_down(int key);
void app_input_discard(void);
#endif
