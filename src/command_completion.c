#include "command_completion.h"

#include <string.h>

static const char *const shared_commands[] = {"tick", "plot"};
static const char *const edit_commands[] = {"pos1", "pos2", "copy", "paste", "stack", "move", "undo", "redo", "schem"};
static const char *const schem_commands[] = {"save", "load"};
static const char *const schem_flags[] = {"-f"};
static const char *const air_flags[] = {"-a"};
static const char *const tick_commands[] = {"freeze", "step", "noupdate"};
static const char *const plot_commands[] = {"info", "expand", "shrink"};
static const char *const plot_directions[] = {"east", "west", "south", "north"};
static const char *const directions[] = {"east", "west", "up", "down", "south", "north"};

#define COUNT(array) (sizeof(array)/sizeof((array)[0]))

bool command_complete(CommandCompletion *state, char *line, size_t capacity)
{
    if (!state || !line || !capacity) return false;
    size_t length = 0;
    while (length < capacity && line[length]) ++length;
    if (length == capacity || length >= COMMAND_COMPLETION_CAPACITY) return false;
    if (!state->active || strcmp(line, state->completed) != 0) {
        memcpy(state->original, line, length + 1);
        state->next = 0;
    }
    state->active = false;
    char copy[COMMAND_COMPLETION_CAPACITY];
    strcpy(copy, state->original);
    char *tokens[8];
    size_t count = 0;
    char *cursor = copy;
    for (;;) {
        cursor += strspn(cursor, " \t");
        if (count == COUNT(tokens)) return false;
        tokens[count++] = cursor;
        cursor += strcspn(cursor, " \t");
        if (!*cursor) break;
        *cursor++ = '\0';
    }
    char *word = tokens[count - 1];
    size_t slashes = strspn(tokens[0], "/");
    if (slashes > 2) return false;
    const char *name = tokens[0] + slashes;
    if (slashes == 2 && count > 1) {
        bool editing = false;
        for (size_t i = 0; i < COUNT(edit_commands); ++i)
            if (!strcmp(name, edit_commands[i])) editing = true;
        if (!editing) return false;
    }
    if (count == 1) word += slashes;
    size_t offset = (size_t)(word - copy);
    size_t prefix = strlen(word);
    const char *choices[COUNT(shared_commands) + COUNT(edit_commands) + COUNT(directions)];
    size_t choices_count = 0;
    bool directional_edit = strcmp(name, "stack") == 0 || strcmp(name, "move") == 0;
#define ADD(array) do { \
    for (size_t i = 0; i < COUNT(array); ++i) choices[choices_count++] = (array)[i]; \
} while (0)
    if (count == 1) {
        if (slashes < 2) ADD(shared_commands);
        ADD(edit_commands);
    } else if (directional_edit || strcmp(name, "copy") == 0 || strcmp(name, "paste") == 0) {
        size_t arguments = 0;
        bool has_air_flag = false;
        for (size_t i = 1; i + 1 < count; ++i) {
            if (strcmp(tokens[i], "-a") == 0) has_air_flag = true;
            else ++arguments;
        }
        if (directional_edit && arguments < 2) ADD(directions);
        if (!has_air_flag && arguments <= (directional_edit ? 2u : 3u)) ADD(air_flags);
    } else if (count == 2 && strcmp(name, "schem") == 0) {
        ADD(schem_commands);
    } else if (count == 3 && strcmp(name, "schem") == 0 && strcmp(tokens[1], "save") == 0) {
        ADD(schem_flags);
    } else if (count == 2 && strcmp(name, "tick") == 0) {
        ADD(tick_commands);
    } else if (count == 2 && strcmp(name, "plot") == 0) {
        ADD(plot_commands);
    } else if (count == 3 && strcmp(name, "plot") == 0 &&
               (strcmp(tokens[1], "expand") == 0 || strcmp(tokens[1], "shrink") == 0)) {
        ADD(plot_directions);
    }
#undef ADD
    size_t matches = 0;
    for (size_t i = 0; i < choices_count; ++i) {
        if (strncmp(choices[i], word, prefix) == 0 &&
            strlen(choices[i]) < capacity - offset)
            choices[matches++] = choices[i];
    }
    if (!matches) return false;
    const char *choice = choices[state->next % matches];
    state->next = (state->next % matches + 1) % matches;
    memcpy(state->completed, state->original, offset);
    strcpy(state->completed + offset, choice);
    bool changed = strcmp(line, state->completed) != 0;
    strcpy(line, state->completed);
    state->active = true;
    return changed;
}
