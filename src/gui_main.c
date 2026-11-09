#if defined(__APPLE__)
#define _XOPEN_SOURCE 700
#endif

#include "frontend/gui.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include "raylib.h"
#include <limits.h>
#include <string.h>

static char asset_path[PATH_MAX];
static size_t asset_prefix;

static bool configure_mac_bundle(void)
{
    const char *directory = GetApplicationDirectory();
    const char suffix[] = "/Contents/MacOS/";
    size_t length = strlen(directory), suffix_length = sizeof(suffix) - 1;
    if (length < suffix_length || strcmp(directory + length - suffix_length, suffix) != 0)
        return true;

    /* Repository launches keep the editable resources, saves and workspace. */
    if (FileExists("nob.c") && DirectoryExists("res")) return true;

    char resources[PATH_MAX];
    int length_written = snprintf(resources, sizeof(resources), "%s../Resources", directory);
    char resolved[PATH_MAX];
    if (length_written <= 0 || (size_t)length_written >= sizeof(resources) ||
        !realpath(resources, resolved)) return false;
    int prefix = snprintf(asset_path, sizeof(asset_path), "%s/", resolved);
    if (prefix <= 0 || (size_t)prefix >= sizeof(asset_path)) return false;
    asset_prefix = (size_t)prefix;
    if (!DirectoryExists(gui_asset_path("res"))) return false;

    const char *user_home = getenv("HOME");
    if (!user_home || !*user_home) return false;
    char data_directory[PATH_MAX];
    int size = snprintf(data_directory, sizeof(data_directory),
                        "%s/Library/Application Support/mcmux", user_home);
    return size > 0 && (size_t)size < sizeof(data_directory) &&
           MakeDirectory(data_directory) == 0 && ChangeDirectory(data_directory);
}
#endif

const char *gui_asset_path(const char *relative)
{
#if defined(__APPLE__)
    if (asset_prefix) {
        size_t size = strlen(relative) + 1;
        if (size <= sizeof(asset_path) - asset_prefix) {
            memcpy(asset_path + asset_prefix, relative, size);
            return asset_path;
        }
    }
#endif
    return relative;
}

int main(void)
{
#if defined(__APPLE__)
    if (!configure_mac_bundle()) {
        fputs("Cannot open the app resources or application data directory\n", stderr);
        return EXIT_FAILURE;
    }
#endif
    WorldSessionManager *world_sessions = world_session_manager_create();
    if (!world_sessions) {
        fputs("Cannot create world session manager\n", stderr);
        return EXIT_FAILURE;
    }
    int result = gui_run(world_sessions);
    world_session_manager_destroy(world_sessions);
    return result;
}
