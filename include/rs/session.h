#ifndef RS_SESSION_H
#define RS_SESSION_H

#include "world.h"

#include <stdbool.h>
#include <stddef.h>

enum {
    GAME_SESSION_NAME_CAPACITY = 64,
    GAME_SESSION_PATH_CAPACITY = 512,
    GAME_SESSION_MAX_SAVES = 128
};

typedef enum {
    GAME_SESSION_OK = 0,
    GAME_SESSION_INVALID_ARGUMENT,
    GAME_SESSION_INVALID_NAME,
    GAME_SESSION_PATH_TOO_LONG,
    GAME_SESSION_OUT_OF_MEMORY,
    GAME_SESSION_IO_ERROR,
    GAME_SESSION_CORRUPT_SAVE,
    GAME_SESSION_UNSUPPORTED_SAVE,
    GAME_SESSION_SIMULATION_ERROR,
    GAME_SESSION_TOO_MANY_SAVES
} GameSessionResult;

typedef struct GameSession GameSession;
typedef struct RegionEditor RegionEditor;

typedef struct {
    size_t count;
    char names[GAME_SESSION_MAX_SAVES][GAME_SESSION_NAME_CAPACITY];
} GameSaveList;

/* A session owns one world and its save path. Creation does not write the
 * file; call game_session_save() after the caller has accepted the name. */
GameSession *game_session_create(const char *save_path, WorldConfig config,
                                 GameSessionResult *result);
GameSession *game_session_load(const char *save_path,
                               GameSessionResult *result);
void game_session_destroy(GameSession *session);

World *game_session_world(GameSession *session);
RegionEditor *game_session_editor(GameSession *session);
const World *game_session_world_const(const GameSession *session);
WorldConfig game_session_config(const GameSession *session);
const char *game_session_path(const GameSession *session);
bool game_session_dirty(const GameSession *session);

GameSessionResult game_session_save(GameSession *session);
RsResult game_session_flush(GameSession *session);
RsResult game_session_step(GameSession *session);
/* Session time driver. Automatic time runs at 20 TPS; frozen manual steps
 * run in bounded batches without real-time pacing. elapsed must be finite and
 * nonnegative. Freeze transitions discard automatic-clock carryover. */
RsResult game_session_advance(GameSession *session, double elapsed_seconds);
/* Discard real-time carryover when a host pauses its input/render loop. */
void game_session_reset_clock(GameSession *session);

/* World names are deliberately portable: ASCII letters, digits, spaces,
 * '-' and '_'. Leading/trailing spaces and path separators are rejected.
 * A world named NAME in directory DIR is stored at DIR/NAME/NAME.mcw; the
 * world directory may also contain its programs and structures. */
bool game_session_valid_name(const char *name);
GameSessionResult game_session_make_path(const char *directory,
                                         const char *name,
                                         char path[GAME_SESSION_PATH_CAPACITY]);
GameSessionResult game_session_list_saves(const char *directory,
                                          GameSaveList *list);
const char *game_session_result_name(GameSessionResult result);

#endif
