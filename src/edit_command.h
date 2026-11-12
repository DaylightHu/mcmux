#ifndef MC_EDIT_COMMAND_H
#define MC_EDIT_COMMAND_H

#include "rs/session.h"
#include "rs/region_edit.h"

typedef enum { EDIT_INVALID, EDIT_POS1, EDIT_POS2, EDIT_COPY, EDIT_PASTE,
               EDIT_STACK, EDIT_MOVE, EDIT_UNDO, EDIT_REDO, EDIT_SCHEM_SAVE,
               EDIT_SCHEM_LOAD } EditCommandKind;
typedef struct {
    EditCommandKind kind;
    BlockPos position;
    Direction direction;
    uint32_t count;
    bool has_position, skip_air, overwrite;
    char name[68]; /* Portable structure name, optionally ending in .mcs. */
} EditCommand;
typedef struct {
    BlockPos position;
    Direction facing;
    bool has_position;
} EditCommandContext;

/* Recognized names return true even for invalid arguments (EDIT_INVALID).
 * Accepts bare names and one or two leading slashes. */
bool edit_command_parse(const char *line, EditCommand *command);
RsResult edit_command_run(GameSession *session, const EditCommand *command,
                          EditCommandContext context, char *message, size_t capacity);

#endif
