#include "rs/session.h"

#include "rs/sim.h"
#include "rs/region_edit.h"
#include "save_file.h"
#include "runtime_clock.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

struct GameSession {
    World *world;
    RegionEditor *editor;
    char save_path[GAME_SESSION_PATH_CAPACITY];
    uint64_t saved_revision;
    uint64_t saved_tick;
    double accumulator;
    bool clock_frozen;
    bool has_saved;
};

static bool ensure_directory(const char *directory);

static bool ensure_save_parent(const char *path)
{
    const char *separator = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!separator || (backslash && backslash > separator)) separator = backslash;
#endif
    if (!separator) return true;
    size_t length = (size_t)(separator - path);
    if (!length || length >= GAME_SESSION_PATH_CAPACITY) return false;
    char directory[GAME_SESSION_PATH_CAPACITY];
    memcpy(directory, path, length);
    directory[length] = '\0';
    return ensure_directory(directory);
}

static void set_result(GameSessionResult *target, GameSessionResult value)
{
    if (target) *target = value;
}

static GameSession *allocate_session(World *world, const char *save_path,
                                     bool has_saved,
                                     GameSessionResult *result)
{
    size_t length = strlen(save_path);
    if (length >= GAME_SESSION_PATH_CAPACITY) {
        world_destroy(world);
        set_result(result, GAME_SESSION_PATH_TOO_LONG);
        return NULL;
    }
    GameSession *session = calloc(1, sizeof(*session));
    if (!session) {
        world_destroy(world);
        set_result(result, GAME_SESSION_OUT_OF_MEMORY);
        return NULL;
    }
    session->world = world;
    session->editor = region_editor_create(world);
    if (!session->editor) {
        world_destroy(world); free(session);
        set_result(result, GAME_SESSION_OUT_OF_MEMORY);
        return NULL;
    }
    memcpy(session->save_path, save_path, length + 1);
    session->saved_revision = world_revision(world);
    session->saved_tick = world_tick(world);
    session->clock_frozen = sim_is_frozen(world);
    session->has_saved = has_saved;
    set_result(result, GAME_SESSION_OK);
    return session;
}

GameSession *game_session_create(const char *save_path, WorldConfig config,
                                 GameSessionResult *result)
{
    if (!save_path || !*save_path) {
        set_result(result, GAME_SESSION_INVALID_ARGUMENT);
        return NULL;
    }
    World *world = world_create(config);
    if (!world) {
        set_result(result, GAME_SESSION_OUT_OF_MEMORY);
        return NULL;
    }
    return allocate_session(world, save_path, false, result);
}

GameSession *game_session_load(const char *save_path,
                               GameSessionResult *result)
{
    if (!save_path || !*save_path) {
        set_result(result, GAME_SESSION_INVALID_ARGUMENT);
        return NULL;
    }
    GameSessionResult read_result;
    World *world = save_file_read(save_path, &read_result);
    if (!world) {
        set_result(result, read_result);
        return NULL;
    }
    return allocate_session(world, save_path, true, result);
}

void game_session_destroy(GameSession *session)
{
    if (!session) return;
    region_editor_destroy(session->editor);
    world_destroy(session->world);
    free(session);
}

World *game_session_world(GameSession *session)
{
    return session ? session->world : NULL;
}

RegionEditor *game_session_editor(GameSession *session)
{
    return session ? session->editor : NULL;
}

const World *game_session_world_const(const GameSession *session)
{
    return session ? session->world : NULL;
}

WorldConfig game_session_config(const GameSession *session)
{
    return session ? world_config(session->world) : (WorldConfig){0};
}

const char *game_session_path(const GameSession *session)
{
    return session ? session->save_path : NULL;
}

bool game_session_dirty(const GameSession *session)
{
    return session && (!session->has_saved ||
           world_revision(session->world) != session->saved_revision ||
           world_tick(session->world) != session->saved_tick);
}

GameSessionResult game_session_save(GameSession *session)
{
    if (!session || !session->world || !session->save_path[0])
        return GAME_SESSION_INVALID_ARGUMENT;
    if (sim_flush(session->world) != RS_OK)
        return GAME_SESSION_SIMULATION_ERROR;
    if (!ensure_save_parent(session->save_path))
        return GAME_SESSION_IO_ERROR;

    GameSessionResult result = save_file_write(session->world,
                                               session->save_path);
    if (result == GAME_SESSION_OK) {
        session->saved_revision = world_revision(session->world);
        session->saved_tick = world_tick(session->world);
        session->has_saved = true;
    }
    return result;
}

RsResult game_session_flush(GameSession *session)
{
    return session ? sim_flush(session->world) : RS_INVALID_ARGUMENT;
}

RsResult game_session_step(GameSession *session)
{
    return session ? sim_step(session->world) : RS_INVALID_ARGUMENT;
}

RsResult game_session_advance(GameSession *session, double elapsed)
{
    if (!session || !isfinite(elapsed) || elapsed < 0.0) return RS_INVALID_ARGUMENT;
    World *world = session->world;
    RsResult result = sim_flush(world);
    if (result != RS_OK) return result;
    bool frozen = sim_is_frozen(world);
    if (frozen != session->clock_frozen) {
        session->clock_frozen = frozen;
        session->accumulator = 0.0;
        elapsed = 0.0;
    }
    if (sim_pending_steps(world)) {
        session->accumulator = 0.0;
        double deadline = runtime_clock_now() + 0.008;
        unsigned steps = 0;
        while (sim_pending_steps(world) && steps < 1024 &&
               (steps == 0 || runtime_clock_now() < deadline)) {
            result = sim_step(world);
            if (result != RS_OK) return result;
            ++steps;
        }
        return RS_OK;
    }
    if (frozen) {
        session->accumulator = 0.0;
        return RS_UNCHANGED;
    }
    if (elapsed > 0.25) elapsed = 0.25;
    session->accumulator += elapsed;
    while (session->accumulator >= 0.05) {
        result = sim_step(world);
        if (result != RS_OK) {
            session->accumulator = 0.0;
            return result;
        }
        session->accumulator -= 0.05;
    }
    return RS_OK;
}

void game_session_reset_clock(GameSession *session)
{
    if (!session) return;
    session->accumulator = 0.0;
    session->clock_frozen = sim_is_frozen(session->world);
}

bool game_session_valid_name(const char *name)
{
    if (!name) return false;
    size_t length = strlen(name);
    if (!length || length >= GAME_SESSION_NAME_CAPACITY ||
        name[0] == ' ' || name[length - 1] == ' ') return false;
    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)name[index];
        if (character >= 128 ||
            !(isalnum(character) || character == ' ' || character == '-' ||
              character == '_')) return false;
    }
    return true;
}

GameSessionResult game_session_make_path(const char *directory,
                                         const char *name,
                                         char path[GAME_SESSION_PATH_CAPACITY])
{
    if (!directory || !*directory || !path)
        return GAME_SESSION_INVALID_ARGUMENT;
    if (!game_session_valid_name(name)) return GAME_SESSION_INVALID_NAME;
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    bool separator = directory[directory_length - 1] != '/' &&
                     directory[directory_length - 1] != '\\';
    size_t required = directory_length + (separator ? 1u : 0u) +
                      name_length + 1u + name_length + 4u + 1u;
    if (required > GAME_SESSION_PATH_CAPACITY)
        return GAME_SESSION_PATH_TOO_LONG;
    int written = snprintf(path, GAME_SESSION_PATH_CAPACITY,
                           "%s%s%s/%s.mcw",
                           directory, separator ? "/" : "", name, name);
    return written > 0 && written < GAME_SESSION_PATH_CAPACITY ?
           GAME_SESSION_OK : GAME_SESSION_PATH_TOO_LONG;
}

static bool add_save(GameSaveList *list, const char *name)
{
    if (!game_session_valid_name(name)) return true;
    if (list->count == GAME_SESSION_MAX_SAVES) return false;
    size_t length = strlen(name);
    memcpy(list->names[list->count++], name, length + 1);
    return true;
}

static int compare_names(const void *left, const void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

static bool ensure_directory(const char *directory)
{
#ifdef _WIN32
    if (_mkdir(directory) == 0) return true;
#else
    if (mkdir(directory, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

GameSessionResult game_session_list_saves(const char *directory,
                                          GameSaveList *list)
{
    if (!directory || !*directory || !list)
        return GAME_SESSION_INVALID_ARGUMENT;
    *list = (GameSaveList){0};
    if (!ensure_directory(directory)) return GAME_SESSION_IO_ERROR;
    bool complete = true;

#ifdef _WIN32
    char pattern[GAME_SESSION_PATH_CAPACITY];
    size_t length = strlen(directory);
    bool separator = directory[length - 1] != '/' && directory[length - 1] != '\\';
    int written = snprintf(pattern, sizeof(pattern), "%s%s*",
                           directory, separator ? "/" : "");
    if (written <= 0 || written >= (int)sizeof(pattern))
        return GAME_SESSION_PATH_TOO_LONG;
    WIN32_FIND_DATAA data;
    HANDLE search = FindFirstFileA(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ?
               GAME_SESSION_OK : GAME_SESSION_IO_ERROR;
    }
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !game_session_valid_name(data.cFileName)) continue;
        char path[GAME_SESSION_PATH_CAPACITY];
        if (game_session_make_path(directory, data.cFileName, path) !=
            GAME_SESSION_OK) continue;
        DWORD attributes = GetFileAttributesA(path);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!add_save(list, data.cFileName)) complete = false;
    } while (FindNextFileA(search, &data));
    DWORD error = GetLastError();
    FindClose(search);
    if (error != ERROR_NO_MORE_FILES) return GAME_SESSION_IO_ERROR;
#else
    DIR *stream = opendir(directory);
    if (!stream) return GAME_SESSION_IO_ERROR;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(stream);
        if (!entry) break;
        if (!game_session_valid_name(entry->d_name)) continue;
        char directory_path[GAME_SESSION_PATH_CAPACITY];
        size_t directory_length = strlen(directory);
        bool separator = directory[directory_length - 1] != '/';
        int written = snprintf(directory_path, sizeof(directory_path),
                               "%s%s%s", directory,
                               separator ? "/" : "", entry->d_name);
        if (written <= 0 || written >= (int)sizeof(directory_path)) continue;
        struct stat status;
        if (stat(directory_path, &status) != 0 || !S_ISDIR(status.st_mode))
            continue;
        char save_path[GAME_SESSION_PATH_CAPACITY];
        if (game_session_make_path(directory, entry->d_name, save_path) !=
            GAME_SESSION_OK) continue;
        if (stat(save_path, &status) != 0 || !S_ISREG(status.st_mode)) continue;
        if (!add_save(list, entry->d_name)) complete = false;
    }
    int read_error = errno;
    if (closedir(stream) != 0 || read_error) return GAME_SESSION_IO_ERROR;
#endif

    qsort(list->names, list->count, sizeof(list->names[0]), compare_names);
    return complete ? GAME_SESSION_OK : GAME_SESSION_TOO_MANY_SAVES;
}

const char *game_session_result_name(GameSessionResult result)
{
    static const char *const names[] = {
        "ok", "invalid argument", "invalid world name", "path too long",
        "out of memory", "I/O error", "corrupt save", "unsupported save version",
        "simulation error", "too many saves"
    };
    return (unsigned)result < sizeof(names) / sizeof(names[0]) ?
           names[result] : "unknown session error";
}
