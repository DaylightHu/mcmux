#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "workspace_editor.h"
#include "workspace_output.h"
#include "workspace_code.h"
#include "ui_font.h"
#include "rs/structure.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>

#define editor_getcwd _getcwd
#define editor_popen _popen
#define editor_pclose _pclose
#else
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#define editor_getcwd getcwd
#define editor_popen popen
#define editor_pclose pclose
#endif

enum {
    EDITOR_BUFFER_LIMIT = 32,
    EDITOR_PATH_CAPACITY = 4096,
    EDITOR_FILE_LIMIT = 64*1024*1024,
    EDITOR_BROWSER_LIMIT = 4096,
    EDITOR_MESSAGE_LIMIT = 32,
    EDITOR_MESSAGE_LENGTH = 768,
    EDITOR_COMMAND_CAPACITY = 520,
    EDITOR_FONT_SIZE = 20,
    EDITOR_ROW_HEIGHT = 24,
    EDITOR_HEADER_HEIGHT = 25,
    EDITOR_STATUS_HEIGHT = 25,
    EDITOR_HEADER_FONT_SIZE = 16,
    EDITOR_TAB_WIDTH = 4,
    EDITOR_DRAW_CAPACITY = 8192
};

typedef enum {
    EDITOR_NORMAL,
    EDITOR_INSERT,
    EDITOR_VISUAL,
    EDITOR_VISUAL_LINE,
    EDITOR_COMMAND,
    EDITOR_BROWSER
} EditorMode;

typedef struct {
    char *text;
    size_t length, capacity;
} EditorLine;

typedef struct {
    char path[EDITOR_PATH_CAPACITY];
    EditorLine *lines;
    size_t line_count, line_capacity;
    size_t row, column, wanted_column;
    size_t top, left;
    size_t bytes;
    bool named, dirty, readonly, synthetic, crlf;
} EditorBuffer;

typedef struct {
    char *name;
    bool directory, parent;
} BrowserEntry;

typedef struct {
    char text[EDITOR_MESSAGE_LENGTH];
    bool error;
} EditorMessage;

typedef struct {
    size_t buffer,row,column,top,left;
} EditorLocation;

struct WorkspaceEditor {
    EditorBuffer *buffers[EDITOR_BUFFER_LIMIT];
    size_t buffer_count, current;
    EditorMode mode;
    EditorMode command_previous_mode;
    char cwd[EDITOR_PATH_CAPACITY];
    char command_cwd[EDITOR_PATH_CAPACITY];
    char command[EDITOR_COMMAND_CAPACITY];
    size_t command_length, command_cursor;
    BrowserEntry entries[EDITOR_BROWSER_LIMIT];
    size_t entry_count, selected, browser_top;
    EditorMessage messages[EDITOR_MESSAGE_LIMIT];
    size_t message_first, message_count;
    bool messages_visible;
    char partial[EDITOR_MESSAGE_LENGTH];
    size_t partial_length;
    bool partial_error;
    size_t selection_row, selection_column;
    char pending_operator;
    char *yank;
    bool yank_linewise;
    WorkspaceOutput *output;
    int pane;
    bool receiving_output;
    WorkspaceCode *code;
    EditorBuffer *code_info;
    EditorLocation code_origin,jumps[64];
    size_t jump_count;
    uint64_t code_hash;
    bool code_pending;
};

static void enter_command_mode(WorkspaceEditor *editor, EditorMode return_mode);

static bool editor_key(int code)
{
    return IsKeyPressed(code) || IsKeyPressedRepeat(code);
}

static bool line_reserve(EditorLine *line, size_t needed)
{
    if (needed <= line->capacity) return true;
    size_t capacity = line->capacity ? line->capacity : 32;
    while (capacity < needed) {
        if (capacity > SIZE_MAX/2) { capacity = needed; break; }
        capacity *= 2;
    }
    char *text = realloc(line->text, capacity);
    if (!text) return false;
    line->text = text;
    line->capacity = capacity;
    return true;
}

static bool line_assign(EditorLine *line, const char *text, size_t length)
{
    if (!line_reserve(line, length + 1)) return false;
    if (length) memcpy(line->text, text, length);
    line->text[length] = '\0';
    line->length = length;
    return true;
}

static void line_destroy(EditorLine *line)
{
    free(line->text);
    *line = (EditorLine){0};
}

static bool buffer_reserve_lines(EditorBuffer *buffer, size_t needed)
{
    if (needed <= buffer->line_capacity) return true;
    size_t capacity = buffer->line_capacity ? buffer->line_capacity : 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX/2) { capacity = needed; break; }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX/sizeof(*buffer->lines)) return false;
    EditorLine *lines = realloc(buffer->lines, capacity*sizeof(*lines));
    if (!lines) return false;
    memset(lines + buffer->line_capacity, 0,
           (capacity - buffer->line_capacity)*sizeof(*lines));
    buffer->lines = lines;
    buffer->line_capacity = capacity;
    return true;
}

static EditorBuffer *buffer_create_empty(void)
{
    EditorBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer || !buffer_reserve_lines(buffer, 1) ||
        !line_assign(&buffer->lines[0], "", 0)) {
        if (buffer) {
            free(buffer->lines);
            free(buffer);
        }
        return NULL;
    }
    buffer->line_count = 1;
    return buffer;
}

static void buffer_destroy(EditorBuffer *buffer)
{
    if (!buffer) return;
    for (size_t i = 0; i < buffer->line_count; ++i)
        line_destroy(&buffer->lines[i]);
    free(buffer->lines);
    free(buffer);
}

static bool buffer_append_line(EditorBuffer *buffer, const char *text,
                               size_t length)
{
    if (!buffer_reserve_lines(buffer, buffer->line_count + 1)) return false;
    EditorLine *line = &buffer->lines[buffer->line_count];
    if (!line_assign(line, text, length)) return false;
    if (buffer->line_count) ++buffer->bytes;
    buffer->bytes += length;
    ++buffer->line_count;
    return true;
}

static EditorBuffer *buffer_from_bytes(const char *path, const char *data,
                                       size_t length)
{
    EditorBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) return NULL;
    size_t crlf = 0, lf = 0, start = 0;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != '\n') continue;
        size_t end = i;
        if (end > start && data[end - 1] == '\r') { --end; ++crlf; }
        else ++lf;
        if (!buffer_append_line(buffer, data + start, end - start)) goto fail;
        start = i + 1;
    }
    if (start < length || !buffer->line_count) {
        if (!buffer_append_line(buffer, data + start, length - start)) goto fail;
    } else if (length && data[length - 1] == '\n') {
        if (!buffer_append_line(buffer, "", 0)) goto fail;
    }
    buffer->bytes = length;
    if (crlf) buffer->bytes -= crlf;
    buffer->crlf = crlf > lf;
    buffer->named = true;
    snprintf(buffer->path, sizeof(buffer->path), "%s", path);
    return buffer;
fail:
    buffer_destroy(buffer);
    return NULL;
}

static EditorBuffer *current_buffer(WorkspaceEditor *editor)
{
    return editor && editor->current < editor->buffer_count ?
        editor->buffers[editor->current] : NULL;
}

static const EditorBuffer *current_buffer_const(const WorkspaceEditor *editor)
{
    return editor && editor->current < editor->buffer_count ?
        editor->buffers[editor->current] : NULL;
}

static void push_message(WorkspaceEditor *editor, const char *text, bool error)
{
    if (!editor || !text) return;
    size_t index;
    if (editor->message_count == EDITOR_MESSAGE_LIMIT) {
        index = editor->message_first;
        editor->message_first = (editor->message_first + 1)%EDITOR_MESSAGE_LIMIT;
    } else {
        index = (editor->message_first + editor->message_count)%EDITOR_MESSAGE_LIMIT;
        ++editor->message_count;
    }
    snprintf(editor->messages[index].text, EDITOR_MESSAGE_LENGTH, "%s", text);
    editor->messages[index].error = error;
    editor->messages_visible = true;
    if (editor->output && !editor->receiving_output)
        workspace_output_message(editor->output, text, error);
}

void workspace_editor_message(WorkspaceEditor *editor, const char *text,
                              bool error)
{
    if (!editor || !text) return;
    const char *start = text;
    while (*start) {
        const char *end = strchr(start, '\n');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length && start[length - 1] == '\r') --length;
        char line[EDITOR_MESSAGE_LENGTH];
        if (length >= sizeof(line)) length = sizeof(line) - 1;
        memcpy(line, start, length);
        line[length] = '\0';
        push_message(editor, line, error);
        if (!end) return;
        start = end + 1;
    }
}

static void clear_messages(WorkspaceEditor *editor)
{
    editor->message_first = editor->message_count = 0;
    editor->messages_visible = false;
    editor->partial_length = 0;
    editor->partial_error = false;
}

static void command_output(void *context, const char *text, bool error)
{
    WorkspaceEditor *editor = context;
    while (*text) {
        if (*text == '\n') {
            editor->partial[editor->partial_length] = '\0';
            push_message(editor, editor->partial, editor->partial_error || error);
            editor->partial_length = 0;
            editor->partial_error = false;
        } else if (*text != '\r') {
            if (editor->partial_length + 1 == sizeof(editor->partial)) {
                editor->partial[editor->partial_length] = '\0';
                push_message(editor, editor->partial,
                             editor->partial_error || error);
                editor->partial_length = 0;
                editor->partial_error = false;
            }
            editor->partial[editor->partial_length++] = *text;
            editor->partial_error |= error;
        }
        ++text;
    }
}

static void flush_command_output(WorkspaceEditor *editor)
{
    if (!editor->partial_length) return;
    editor->partial[editor->partial_length] = '\0';
    push_message(editor, editor->partial, editor->partial_error);
    editor->partial_length = 0;
    editor->partial_error = false;
}

static void slashify(char *path)
{
    for (; *path; ++path) if (*path == '\\') *path = '/';
}

static bool path_copy(char destination[EDITOR_PATH_CAPACITY], const char *source)
{
    size_t length = strlen(source);
    if (length >= EDITOR_PATH_CAPACITY) return false;
    memcpy(destination, source, length + 1);
    return true;
}

#ifndef _WIN32
static bool normalize_absolute_path(const char *input,
                                    char output[EDITOR_PATH_CAPACITY])
{
    if (input[0] != '/') return false;
    char copy[EDITOR_PATH_CAPACITY];
    if (!path_copy(copy, input)) return false;
    slashify(copy);
    size_t marks[EDITOR_PATH_CAPACITY/2];
    size_t mark_count = 0, output_length = 1;
    output[0] = '/';
    output[1] = '\0';
    char *cursor = copy;
    while (*cursor) {
        while (*cursor == '/') ++cursor;
        if (!*cursor) break;
        char *component = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        size_t length = (size_t)(cursor - component);
        if (length == 1 && component[0] == '.') continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            if (mark_count) {
                output_length = marks[--mark_count];
                output[output_length] = '\0';
            }
            continue;
        }
        size_t before = output_length;
        size_t separator = output_length > 1 ? 1 : 0;
        if (output_length + separator + length >= EDITOR_PATH_CAPACITY)
            return false;
        marks[mark_count++] = before;
        if (separator) output[output_length++] = '/';
        memcpy(output + output_length, component, length);
        output_length += length;
        output[output_length] = '\0';
    }
    return true;
}
#endif

static bool path_resolve(const char *directory, const char *input,
                         char output[EDITOR_PATH_CAPACITY])
{
    if (!directory || !*directory || !input || !*input) return false;
#ifdef _WIN32
    char joined[EDITOR_PATH_CAPACITY];
    bool absolute = (isalpha((unsigned char)input[0]) && input[1] == ':') ||
                    input[0] == '/' || input[0] == '\\';
    int count = absolute ? snprintf(joined, sizeof(joined), "%s", input) :
        snprintf(joined, sizeof(joined), "%s/%s", directory, input);
    if (count < 0 || (size_t)count >= sizeof(joined)) return false;
    char native[EDITOR_PATH_CAPACITY];
    if (!_fullpath(native, joined, sizeof(native))) return false;
    if (!path_copy(output, native)) return false;
    slashify(output);
    return true;
#else
    char joined[EDITOR_PATH_CAPACITY];
    int count = input[0] == '/' ? snprintf(joined, sizeof(joined), "%s", input) :
        snprintf(joined, sizeof(joined), "%s/%s", directory, input);
    return count >= 0 && (size_t)count < sizeof(joined) &&
           normalize_absolute_path(joined, output);
#endif
}

static bool paths_equal(const char *left, const char *right)
{
#ifdef _WIN32
    while (*left && *right) {
        char a = *left == '\\' ? '/' : (char)tolower((unsigned char)*left);
        char b = *right == '\\' ? '/' : (char)tolower((unsigned char)*right);
        if (a != b) return false;
        ++left; ++right;
    }
    return *left == *right;
#else
    return strcmp(left, right) == 0;
#endif
}

static bool path_join(const char *directory, const char *name,
                      char output[EDITOR_PATH_CAPACITY])
{
    size_t length = strlen(directory);
    const char *separator = length && directory[length - 1] == '/' ? "" : "/";
    int count = snprintf(output, EDITOR_PATH_CAPACITY, "%s%s%s",
                         directory, separator, name);
    return count >= 0 && count < EDITOR_PATH_CAPACITY;
}

static bool path_parent(const char *path, char output[EDITOR_PATH_CAPACITY])
{
    if (!path_copy(output, path)) return false;
    slashify(output);
    size_t length = strlen(output);
    while (length > 1 && output[length - 1] == '/') output[--length] = '\0';
#ifdef _WIN32
    if (length == 3 && isalpha((unsigned char)output[0]) &&
        output[1] == ':' && output[2] == '/') return true;
#endif
    char *slash = strrchr(output, '/');
    if (!slash) return false;
#ifdef _WIN32
    if (slash == output + 2 && output[1] == ':') slash[1] = '\0';
    else
#endif
    if (slash == output) output[1] = '\0';
    else *slash = '\0';
    return true;
}

static const char *path_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    return slash ? slash + 1 : path;
}

typedef enum { PATH_MISSING, PATH_FILE, PATH_DIRECTORY, PATH_OTHER, PATH_ERROR } PathKind;

static PathKind path_kind(const char *path)
{
    struct stat information;
    if (stat(path, &information) != 0) {
        return errno == ENOENT || errno == ENOTDIR ? PATH_MISSING : PATH_ERROR;
    }
    if (S_ISDIR(information.st_mode)) return PATH_DIRECTORY;
    if (S_ISREG(information.st_mode)) return PATH_FILE;
    return PATH_OTHER;
}

static void browser_clear(WorkspaceEditor *editor)
{
    for (size_t i = 0; i < editor->entry_count; ++i)
        free(editor->entries[i].name);
    editor->entry_count = editor->selected = editor->browser_top = 0;
}

static bool browser_add(WorkspaceEditor *editor, const char *name,
                        bool directory, bool parent)
{
    if (editor->entry_count == EDITOR_BROWSER_LIMIT) return false;
    size_t length = strlen(name);
    char *copy = malloc(length + 1);
    if (!copy) return false;
    memcpy(copy, name, length + 1);
    editor->entries[editor->entry_count++] =
        (BrowserEntry){copy, directory, parent};
    return true;
}

static int browser_compare(const void *left, const void *right)
{
    const BrowserEntry *a = left, *b = right;
    if (a->parent != b->parent) return a->parent ? -1 : 1;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
#ifdef _WIN32
    const unsigned char *x = (const unsigned char *)a->name;
    const unsigned char *y = (const unsigned char *)b->name;
    while (*x && *y) {
        int difference = tolower(*x) - tolower(*y);
        if (difference) return difference;
        ++x; ++y;
    }
    return *x - *y;
#else
    return strcmp(a->name, b->name);
#endif
}

static bool browser_read_directory(WorkspaceEditor *editor, const char *path)
{
    char parent[EDITOR_PATH_CAPACITY];
    if (path_parent(path, parent) && !paths_equal(parent, path) &&
        !browser_add(editor, "..", true, true)) return false;
#ifdef _WIN32
    char pattern[EDITOR_PATH_CAPACITY];
    if (!path_join(path, "*", pattern)) return false;
    for (char *p = pattern; *p; ++p) if (*p == '/') *p = '\\';
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        if (strcmp(found.cFileName, ".") == 0 ||
            strcmp(found.cFileName, "..") == 0) continue;
        bool directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!browser_add(editor, found.cFileName, directory, false)) {
            ok = false;
            break;
        }
    } while (FindNextFileA(search, &found));
    DWORD error = GetLastError();
    FindClose(search);
    return ok && error == ERROR_NO_MORE_FILES;
#else
    DIR *directory = opendir(path);
    if (!directory) return false;
    bool ok = true;
    struct dirent *entry;
    while (true) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) {
            if (errno) ok = false;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        char full[EDITOR_PATH_CAPACITY];
        if (!path_join(path, entry->d_name, full)) { ok = false; break; }
        bool is_directory = path_kind(full) == PATH_DIRECTORY;
        if (!browser_add(editor, entry->d_name, is_directory, false)) {
            ok = false;
            break;
        }
    }
    if (closedir(directory) != 0) ok = false;
    return ok;
#endif
}

static bool browser_open(WorkspaceEditor *editor, const char *input)
{
    char path[EDITOR_PATH_CAPACITY];
    if (!path_resolve(editor->cwd, input && *input ? input : editor->cwd, path)) {
        workspace_editor_message(editor, "path is too long", true);
        return false;
    }
    if (path_kind(path) != PATH_DIRECTORY) {
        workspace_editor_message(editor, "not a directory", true);
        return false;
    }
    browser_clear(editor);
    if (!browser_read_directory(editor, path)) {
        browser_clear(editor);
        workspace_editor_message(editor, "cannot read directory", true);
        return false;
    }
    qsort(editor->entries, editor->entry_count, sizeof(editor->entries[0]),
          browser_compare);
    if (!path_copy(editor->cwd, path)) return false;
    editor->mode = EDITOR_BROWSER;
    editor->messages_visible = false;
    return true;
}

static size_t buffer_find(const WorkspaceEditor *editor, const char *path)
{
    for (size_t i = 0; i < editor->buffer_count; ++i) {
        const EditorBuffer *buffer = editor->buffers[i];
        if (buffer->named && paths_equal(buffer->path, path)) return i;
    }
    return SIZE_MAX;
}

static bool extension_equal(const char *path, const char *extension)
{
    size_t path_length = strlen(path), extension_length = strlen(extension);
    if (path_length < extension_length) return false;
    const char *suffix = path + path_length - extension_length;
    for (size_t i = 0; i < extension_length; ++i)
        if (tolower((unsigned char)suffix[i]) !=
            tolower((unsigned char)extension[i])) return false;
    return true;
}

static bool buffer_preview_line(EditorBuffer *buffer, const char *format, ...)
{
    char line[EDITOR_MESSAGE_LENGTH];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(line)) return false;
    return buffer_append_line(buffer, line, (size_t)length);
}

static EditorBuffer *structure_preview(const char *path,
                                       StructureResult *read_result)
{
    Structure *structure = structure_read(path, read_result);
    if (!structure) return NULL;
    EditorBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        structure_destroy(structure);
        *read_result = STRUCTURE_OUT_OF_MEMORY;
        return NULL;
    }
    StructureInfo info = structure_info(structure);
    size_t counts[BLOCK_COUNT] = {0};
    for (uint32_t y = 0; y < info.height; ++y) {
        for (uint32_t z = 0; z < info.length; ++z) {
            for (uint32_t x = 0; x < info.width; ++x) {
                BlockState state;
                if (structure_get(structure, (BlockPos){(int)x, (int)y, (int)z},
                                  &state) && state.id < BLOCK_COUNT)
                    ++counts[state.id];
            }
        }
    }
    bool ok = buffer_preview_line(buffer, "mcmux structure (read only)") &&
        buffer_preview_line(buffer, "file: %.740s", path) &&
        buffer_preview_line(buffer, "version: %d", STRUCTURE_VERSION) &&
        buffer_preview_line(buffer, "size: %" PRIu32 " x %" PRIu32 " x %" PRIu32,
                            info.width, info.height, info.length) &&
        buffer_preview_line(buffer, "volume: %zu", info.volume) &&
        buffer_preview_line(buffer, "origin: %d %d %d", info.origin.x,
                            info.origin.y, info.origin.z) &&
        buffer_preview_line(buffer, "anchor: %d %d %d", info.anchor.x,
                            info.anchor.y, info.anchor.z) &&
        buffer_preview_line(buffer, "source tick: %" PRIu64, info.source_tick) &&
        buffer_preview_line(buffer, "containers: %zu", info.container_count) &&
        buffer_preview_line(buffer, "") &&
        buffer_preview_line(buffer, "blocks:");
    for (BlockId id = BLOCK_AIR; ok && id < BLOCK_COUNT; ++id) {
        if (counts[id])
            ok = buffer_preview_line(buffer, "  %s: %zu", block_name(id), counts[id]);
    }
    structure_destroy(structure);
    if (!ok) {
        buffer_destroy(buffer);
        *read_result = STRUCTURE_OUT_OF_MEMORY;
        return NULL;
    }
    buffer->named = true;
    buffer->readonly = true;
    buffer->synthetic = true;
    snprintf(buffer->path, sizeof(buffer->path), "%s", path);
    *read_result = STRUCTURE_OK;
    return buffer;
}

static EditorBuffer *file_buffer(const char *path, const char **error)
{
    FILE *file = fopen(path, "rb");
    if (!file) { *error = "cannot open file"; return NULL; }
    bool measured = fseek(file, 0, SEEK_END) == 0;
    long measured_length = measured ? ftell(file) : -1;
    if (measured_length < 0 || measured_length > EDITOR_FILE_LIMIT ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        *error = measured_length > EDITOR_FILE_LIMIT ?
            "file is too large for mc-nvim" : "cannot measure file";
        return NULL;
    }
    size_t length = (size_t)measured_length;
    char *data = malloc(length ? length : 1);
    if (!data) { fclose(file); *error = "out of memory"; return NULL; }
    bool read_ok = fread(data, 1, length, file) == length;
    if (fclose(file) != 0) read_ok = false;
    if (!read_ok) {
        free(data);
        *error = "cannot read file";
        return NULL;
    }
    if (memchr(data, '\0', length)) {
        free(data);
        *error = "binary file; only .mcs has a built-in viewer";
        return NULL;
    }
    EditorBuffer *buffer = buffer_from_bytes(path, data, length);
    free(data);
    if (!buffer) *error = "out of memory";
    return buffer;
}

static bool install_buffer(WorkspaceEditor *editor, EditorBuffer *buffer)
{
    EditorBuffer *old = current_buffer(editor);
    if (editor->buffer_count == 1 && old && !old->named && !old->dirty) {
        buffer_destroy(old);
        editor->buffers[0] = buffer;
        editor->current = 0;
        return true;
    }
    if (editor->buffer_count == EDITOR_BUFFER_LIMIT) return false;
    editor->buffers[editor->buffer_count] = buffer;
    editor->current = editor->buffer_count++;
    return true;
}

static bool editor_open_file(WorkspaceEditor *editor, const char *input)
{
    char path[EDITOR_PATH_CAPACITY];
    if (!path_resolve(editor->command_cwd, input, path)) {
        workspace_editor_message(editor, "path is too long", true);
        return false;
    }
    PathKind kind = path_kind(path);
    if (kind == PATH_DIRECTORY) return browser_open(editor, path);
    if (kind == PATH_ERROR || kind == PATH_OTHER) {
        workspace_editor_message(editor, kind == PATH_ERROR ?
            "cannot inspect path" : "path is not a regular file", true);
        return false;
    }
    EditorBuffer *active = current_buffer(editor);
    if (active && !active->named && active->dirty) {
        if (kind == PATH_MISSING) {
            active->named = true;
            snprintf(active->path, sizeof(active->path), "%s", path);
            char message[EDITOR_MESSAGE_LENGTH];
            snprintf(message, sizeof(message), "new file: %.740s", path);
            workspace_editor_message(editor, message, false);
            editor->mode = EDITOR_NORMAL;
            return true;
        }
        workspace_editor_message(editor,
            "unnamed buffer has changes; use :w path before :e", true);
        return false;
    }
    size_t existing = buffer_find(editor, path);
    if (existing != SIZE_MAX) {
        editor->current = existing;
        editor->mode = EDITOR_NORMAL;
        editor->messages_visible = false;
        return true;
    }
    EditorBuffer *buffer = NULL;
    const char *error = NULL;
    if (kind == PATH_MISSING) {
        buffer = buffer_create_empty();
        if (buffer) {
            buffer->named = true;
            snprintf(buffer->path, sizeof(buffer->path), "%s", path);
        }
    } else if (extension_equal(path, ".mcs")) {
        StructureResult result;
        buffer = structure_preview(path, &result);
        if (!buffer) error = structure_result_name(result);
    } else {
        buffer = file_buffer(path, &error);
    }
    if (!buffer) {
        workspace_editor_message(editor, error ? error : "out of memory", true);
        return false;
    }
    if (!install_buffer(editor, buffer)) {
        buffer_destroy(buffer);
        workspace_editor_message(editor, "buffer limit reached", true);
        return false;
    }
    editor->mode = EDITOR_NORMAL;
    char message[EDITOR_MESSAGE_LENGTH];
    snprintf(message, sizeof(message), kind == PATH_MISSING ?
             "new file: %.740s" : "opened: %.740s", path);
    workspace_editor_message(editor, message, false);
    return true;
}

static bool sync_editor_file(FILE *file)
{
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static unsigned long editor_process_id(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

static bool publish_editor_file(const char *temporary, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(temporary, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temporary, path) == 0;
#endif
}

static bool write_buffer(EditorBuffer *buffer, const char *path,
                         char error[EDITOR_MESSAGE_LENGTH])
{
    char temporary[EDITOR_PATH_CAPACITY];
    FILE *file = NULL;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int count = snprintf(temporary, sizeof(temporary), "%s.mcmux-%lu-%u.tmp",
                             path, editor_process_id(), attempt);
        if (count < 0 || (size_t)count >= sizeof(temporary)) {
            snprintf(error, EDITOR_MESSAGE_LENGTH, "path is too long");
            return false;
        }
        file = fopen(temporary, "wbx");
        if (file) break;
        if (errno != EEXIST) break;
    }
    if (!file) {
        snprintf(error, EDITOR_MESSAGE_LENGTH, "cannot create temporary file");
        return false;
    }
#ifndef _WIN32
    struct stat original;
    if (stat(path, &original) == 0)
        (void)chmod(temporary, original.st_mode & 07777);
#endif
    bool ok = true;
    const char *newline = buffer->crlf ? "\r\n" : "\n";
    size_t newline_length = buffer->crlf ? 2 : 1;
    for (size_t i = 0; ok && i < buffer->line_count; ++i) {
        EditorLine *line = &buffer->lines[i];
        ok = fwrite(line->text, 1, line->length, file) == line->length;
        if (ok && i + 1 < buffer->line_count)
            ok = fwrite(newline, 1, newline_length, file) == newline_length;
    }
    if (ok) ok = sync_editor_file(file);
    if (fclose(file) != 0) ok = false;
    if (ok) ok = publish_editor_file(temporary, path);
    if (!ok) {
        (void)remove(temporary);
        snprintf(error, EDITOR_MESSAGE_LENGTH,
                 "write failed; the previous file was kept");
        return false;
    }
    return true;
}

static bool editor_write(WorkspaceEditor *editor, const char *argument)
{
    EditorBuffer *buffer = current_buffer(editor);
    if (!buffer) return false;
    if (buffer->readonly || buffer->synthetic) {
        workspace_editor_message(editor, "buffer is read only", true);
        return false;
    }
    char path[EDITOR_PATH_CAPACITY];
    bool save_as = argument && *argument;
    if (save_as) {
        if (!path_resolve(editor->command_cwd, argument, path)) {
            workspace_editor_message(editor, "path is too long", true);
            return false;
        }
        size_t other = buffer_find(editor, path);
        if (other != SIZE_MAX && editor->buffers[other] != buffer) {
            workspace_editor_message(editor, "that file is already open", true);
            return false;
        }
        if (!buffer->named || !paths_equal(buffer->path, path)) {
            PathKind target = path_kind(path);
            if (target == PATH_FILE || target == PATH_DIRECTORY ||
                target == PATH_OTHER) {
                workspace_editor_message(editor,
                    "save-as target already exists", true);
                return false;
            }
            if (target == PATH_ERROR) {
                workspace_editor_message(editor,
                    "cannot inspect save-as target", true);
                return false;
            }
        }
    } else if (!buffer->named) {
        workspace_editor_message(editor, "no file name; use :e path first", true);
        return false;
    } else {
        snprintf(path, sizeof(path), "%s", buffer->path);
    }
    char error[EDITOR_MESSAGE_LENGTH];
    if (!write_buffer(buffer, path, error)) {
        workspace_editor_message(editor, error, true);
        return false;
    }
    if (save_as || !buffer->named) {
        buffer->named = true;
        snprintf(buffer->path, sizeof(buffer->path), "%s", path);
    }
    buffer->dirty = false;
    char message[EDITOR_MESSAGE_LENGTH];
    snprintf(message, sizeof(message), "written: %.750s", path);
    workspace_editor_message(editor, message, false);
    return true;
}

static size_t utf8_next(const EditorLine *line, size_t offset)
{
    if (offset >= line->length) return line->length;
    unsigned char lead = (unsigned char)line->text[offset];
    size_t count = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                   (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
    if (offset + count > line->length) return offset + 1;
    for (size_t i = 1; i < count; ++i)
        if (((unsigned char)line->text[offset + i] & 0xc0) != 0x80)
            return offset + 1;
    return offset + count;
}

static size_t utf8_previous(const EditorLine *line, size_t offset)
{
    if (!offset) return 0;
    size_t candidate = offset - 1;
    while (candidate &&
           ((unsigned char)line->text[candidate] & 0xc0) == 0x80) --candidate;
    return utf8_next(line, candidate) == offset ? candidate : offset - 1;
}

static size_t text_column(const EditorLine *line, size_t offset)
{
    size_t column = 0;
    for (size_t cursor = 0; cursor < offset && cursor < line->length;
         cursor = utf8_next(line, cursor)) ++column;
    return column;
}

static size_t offset_at_column(const EditorLine *line, size_t column)
{
    size_t offset = 0;
    while (column && offset < line->length) {
        offset = utf8_next(line, offset);
        --column;
    }
    return offset;
}

static size_t visual_column(const EditorLine *line, size_t offset)
{
    size_t column = 0;
    for (size_t cursor = 0; cursor < offset && cursor < line->length;) {
        unsigned char character = (unsigned char)line->text[cursor];
        if (character == '\t') column += EDITOR_TAB_WIDTH - column%EDITOR_TAB_WIDTH;
        else ++column;
        cursor = utf8_next(line, cursor);
    }
    return column;
}

static size_t normal_last_column(const EditorLine *line)
{
    return line->length ? utf8_previous(line, line->length) : 0;
}

static void clamp_cursor(EditorBuffer *buffer, bool insertion)
{
    if (!buffer->line_count) return;
    if (buffer->row >= buffer->line_count) buffer->row = buffer->line_count - 1;
    EditorLine *line = &buffer->lines[buffer->row];
    size_t maximum = insertion ? line->length : normal_last_column(line);
    if (buffer->column > maximum) buffer->column = maximum;
}

static void remember_column(EditorBuffer *buffer)
{
    buffer->wanted_column = text_column(&buffer->lines[buffer->row],
                                        buffer->column);
}

static void move_vertical(EditorBuffer *buffer, int delta, bool insertion)
{
    if (delta < 0 && buffer->row) --buffer->row;
    else if (delta > 0 && buffer->row + 1 < buffer->line_count) ++buffer->row;
    else return;
    EditorLine *line = &buffer->lines[buffer->row];
    buffer->column = offset_at_column(line, buffer->wanted_column);
    if (!insertion && buffer->column == line->length && line->length)
        buffer->column = utf8_previous(line, line->length);
}

static bool line_insert(EditorLine *line, size_t offset,
                        const char *text, size_t length)
{
    if (offset > line->length || length > SIZE_MAX - line->length - 1 ||
        !line_reserve(line, line->length + length + 1)) return false;
    memmove(line->text + offset + length, line->text + offset,
            line->length - offset + 1);
    memcpy(line->text + offset, text, length);
    line->length += length;
    return true;
}

static bool buffer_insert(EditorBuffer *buffer, const char *text, size_t length)
{
    if (length > EDITOR_FILE_LIMIT - buffer->bytes) return false;
    EditorLine *line = &buffer->lines[buffer->row];
    if (!line_insert(line, buffer->column, text, length)) return false;
    buffer->column += length;
    buffer->bytes += length;
    buffer->dirty = true;
    remember_column(buffer);
    return true;
}

static bool buffer_newline(EditorBuffer *buffer)
{
    if (buffer->bytes == EDITOR_FILE_LIMIT ||
        !buffer_reserve_lines(buffer, buffer->line_count + 1)) return false;
    EditorLine *line = &buffer->lines[buffer->row];
    EditorLine added = {0};
    size_t tail = line->length - buffer->column;
    if (!line_assign(&added, line->text + buffer->column, tail)) return false;
    memmove(buffer->lines + buffer->row + 2,
            buffer->lines + buffer->row + 1,
            (buffer->line_count - buffer->row - 1)*sizeof(*buffer->lines));
    buffer->lines[buffer->row + 1] = added;
    line = &buffer->lines[buffer->row];
    line->length = buffer->column;
    line->text[line->length] = '\0';
    ++buffer->line_count;
    ++buffer->row;
    buffer->column = 0;
    ++buffer->bytes;
    buffer->dirty = true;
    remember_column(buffer);
    return true;
}

static size_t line_indent_length(const EditorLine *line)
{
    size_t length = 0;
    while (length < line->length &&
           (line->text[length] == ' ' || line->text[length] == '\t')) ++length;
    return length;
}

typedef struct { bool tabs; size_t width; } EditorIndent;

static EditorIndent buffer_indent_style(const EditorBuffer *buffer)
{
    size_t tabs = 0, spaces = 0, width = 0, block_width = 0;
    size_t previous_indent = 0;
    bool previous_open = false;
    for (size_t row = 0; row < buffer->line_count; ++row) {
        const EditorLine *line = &buffer->lines[row];
        size_t length = line_indent_length(line);
        if (length == line->length || line->text[length] == '#' ||
            line->text[length] == '*' ||
            (line->text[length] == '/' && length + 1 < line->length &&
             (line->text[length + 1] == '/' || line->text[length + 1] == '*')))
            continue;
        if (length && memchr(line->text, '\t', length)) ++tabs;
        else if (length) {
            ++spaces;
            if (length <= 2*EDITOR_TAB_WIDTH && (!width || length < width))
                width = length;
            if (previous_open && length > previous_indent) {
                size_t step = length - previous_indent;
                if (step <= 2*EDITOR_TAB_WIDTH && (!block_width || step < block_width))
                    block_width = step;
            }
        }
        size_t end = line->length;
        while (end && (line->text[end - 1] == ' ' || line->text[end - 1] == '\t')) --end;
        previous_open = end && line->text[end - 1] == '{';
        previous_indent = length;
    }
    if (block_width) width = block_width;
    return (EditorIndent){tabs > spaces, width ? width : EDITOR_TAB_WIDTH};
}

static bool buffer_is_c(const EditorBuffer *buffer)
{
    return buffer->named && (extension_equal(buffer->path, ".c") ||
                             extension_equal(buffer->path, ".h"));
}

typedef struct {
    size_t depth, brace_row;
    char last;
    bool code;
} EditorCIndent;

/* Inspect only text before the cursor. Braces in literals, comments and
 * preprocessor lines do not participate in C block indentation. */
static EditorCIndent c_indent_context(const EditorBuffer *buffer, size_t row,
                                      size_t column, size_t find_depth)
{
    enum { C_CODE, C_STRING, C_CHAR, C_COMMENT, C_LINE_COMMENT } state = C_CODE;
    EditorCIndent result = {.brace_row = SIZE_MAX};
    bool escaped = false, directive = false;
    for (size_t r = 0; r <= row; ++r) {
        const EditorLine *line = &buffer->lines[r];
        size_t limit = r == row ? column : line->length;
        result.last = 0;
        for (size_t i = 0; i < limit; ++i) {
            char ch = line->text[i];
            char next = i + 1 < limit ? line->text[i + 1] : '\0';
            if (state == C_LINE_COMMENT) continue;
            if (state == C_COMMENT) {
                if (ch == '*' && next == '/') { state = C_CODE; ++i; }
                continue;
            }
            if (state == C_STRING || state == C_CHAR) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == (state == C_STRING ? '"' : '\'')) state = C_CODE;
                continue;
            }
            if (ch == '/' && next == '/') { state = C_LINE_COMMENT; ++i; continue; }
            if (ch == '/' && next == '*') { state = C_COMMENT; ++i; continue; }
            if (ch == ' ' || ch == '\t' || ch == '\r') continue;
            if (ch == '#' && !result.last) directive = true;
            if (ch == '"' || ch == '\'') state = ch == '"' ? C_STRING : C_CHAR;
            if (directive) continue;
            result.last = ch;
            if (ch == '{') {
                if (++result.depth == find_depth) result.brace_row = r;
            } else if (ch == '}' && result.depth) --result.depth;
        }
        if (r != row) {
            bool continued = line->length && line->text[line->length - 1] == '\\';
            if (state != C_COMMENT && !continued) state = C_CODE;
            directive = directive && continued;
            escaped = false;
        }
    }
    result.code = state == C_CODE && !directive;
    return result;
}

static size_t c_open_brace_row(const EditorBuffer *buffer,
                                EditorCIndent context)
{
    if (!context.depth || !context.code) return SIZE_MAX;
    return c_indent_context(buffer, buffer->row, buffer->column,
                            context.depth).brace_row;
}

static bool line_with_indent(EditorLine *line, const EditorLine *prefix,
                              size_t indent, char extra, size_t extra_length,
                              const char *tail, size_t tail_length)
{
    size_t length = indent + extra_length + tail_length;
    if (!line_reserve(line, length + 1)) return false;
    if (indent) memcpy(line->text, prefix->text, indent);
    if (extra_length) memset(line->text + indent, extra, extra_length);
    if (tail_length) memcpy(line->text + indent + extra_length, tail, tail_length);
    line->text[length] = '\0';
    line->length = length;
    return true;
}

static bool buffer_indented_newline(EditorBuffer *buffer)
{
    const EditorLine *line = &buffer->lines[buffer->row];
    size_t indent = line_indent_length(line);
    if (indent > buffer->column) indent = buffer->column;
    const EditorLine *prefix = line;
    size_t tail_start = buffer->column, extra = 0, count = 1;
    size_t closing_indent = 0;
    const EditorLine *closing_prefix = line;
    EditorIndent style = buffer_indent_style(buffer);
    if (buffer_is_c(buffer)) {
        EditorCIndent context = c_indent_context(buffer, buffer->row,
                                                buffer->column, SIZE_MAX);
        if (context.last == '{') extra = style.tabs ? 1 : style.width;
        if (context.code && buffer->column >= line_indent_length(line)) {
            while (tail_start < line->length &&
                (line->text[tail_start] == ' ' || line->text[tail_start] == '\t'))
                ++tail_start;
            if (tail_start < line->length && line->text[tail_start] == '}') {
                size_t row = c_open_brace_row(buffer, context);
                if (row != SIZE_MAX) {
                    closing_prefix = &buffer->lines[row];
                    closing_indent = line_indent_length(closing_prefix);
                    if (extra) count = 2;
                    else { prefix = closing_prefix; indent = closing_indent; }
                }
            }
        }
    }
    size_t removed = tail_start - buffer->column;
    size_t added_bytes = count + indent + extra + (count == 2 ? closing_indent : 0);
    if (added_bytes > EDITOR_FILE_LIMIT - (buffer->bytes - removed)) return false;
    EditorLine added[2] = {{0}, {0}};
    const char *tail = line->text + tail_start;
    size_t tail_length = line->length - tail_start;
    if (!line_with_indent(&added[0], prefix, indent, style.tabs ? '\t' : ' ', extra,
                          tail, count == 2 ? 0 : tail_length) ||
        (count == 2 && !line_with_indent(&added[1], closing_prefix, closing_indent,
                                         ' ', 0, tail, tail_length)) ||
        !buffer_reserve_lines(buffer, buffer->line_count + count)) {
        line_destroy(&added[0]); line_destroy(&added[1]);
        return false;
    }
    memmove(buffer->lines + buffer->row + count + 1,
            buffer->lines + buffer->row + 1,
            (buffer->line_count - buffer->row - 1)*sizeof(*buffer->lines));
    for (size_t i = 0; i < count; ++i) buffer->lines[buffer->row + i + 1] = added[i];
    EditorLine *original = &buffer->lines[buffer->row];
    original->length = buffer->column;
    original->text[original->length] = '\0';
    buffer->line_count += count;
    buffer->bytes = buffer->bytes - removed + added_bytes;
    ++buffer->row;
    buffer->column = indent + extra;
    buffer->dirty = true;
    remember_column(buffer);
    return true;
}

static bool buffer_tab(EditorBuffer *buffer)
{
    EditorIndent style = buffer_indent_style(buffer);
    if (style.tabs) return buffer_insert(buffer, "\t", 1);
    size_t column = visual_column(&buffer->lines[buffer->row], buffer->column);
    size_t spaces = style.width - column%style.width;
    return buffer_insert(buffer, "        ", spaces);
}

static bool buffer_typed_text(EditorBuffer *buffer, const char *text, size_t length)
{
    EditorLine *line = &buffer->lines[buffer->row];
    if (length != 1 || *text != '}' || !buffer_is_c(buffer) ||
        buffer->column != line_indent_length(line))
        return buffer_insert(buffer, text, length);
    EditorCIndent context = c_indent_context(buffer, buffer->row,
                                            buffer->column, SIZE_MAX);
    size_t row = c_open_brace_row(buffer, context);
    if (row == SIZE_MAX) return buffer_insert(buffer, text, length);
    const EditorLine *prefix = &buffer->lines[row];
    size_t indent = line_indent_length(prefix), removed = buffer->column;
    size_t tail = line->length - buffer->column;
    if (indent + 1 > EDITOR_FILE_LIMIT - (buffer->bytes - removed) ||
        !line_reserve(line, indent + 1 + tail + 1)) return false;
    memmove(line->text + indent + 1, line->text + removed, tail + 1);
    if (indent) memcpy(line->text, prefix->text, indent);
    line->text[indent] = '}';
    line->length = indent + 1 + tail;
    buffer->bytes = buffer->bytes - removed + indent + 1;
    buffer->column = indent + 1;
    buffer->dirty = true;
    remember_column(buffer);
    return true;
}

static bool buffer_backspace(EditorBuffer *buffer)
{
    EditorLine *line = &buffer->lines[buffer->row];
    if (buffer->column) {
        size_t previous = utf8_previous(line, buffer->column);
        size_t removed = buffer->column - previous;
        memmove(line->text + previous, line->text + buffer->column,
                line->length - buffer->column + 1);
        line->length -= removed;
        buffer->column = previous;
        buffer->bytes -= removed;
    } else if (buffer->row) {
        EditorLine *above = &buffer->lines[buffer->row - 1];
        size_t join = above->length;
        if (!line_reserve(above, above->length + line->length + 1)) return false;
        memcpy(above->text + above->length, line->text, line->length + 1);
        above->length += line->length;
        line_destroy(line);
        memmove(buffer->lines + buffer->row,
                buffer->lines + buffer->row + 1,
                (buffer->line_count - buffer->row - 1)*sizeof(*buffer->lines));
        --buffer->line_count;
        memset(&buffer->lines[buffer->line_count], 0, sizeof(*buffer->lines));
        --buffer->row;
        buffer->column = join;
        --buffer->bytes;
    } else return true;
    buffer->dirty = true;
    remember_column(buffer);
    return true;
}

static bool buffer_delete(EditorBuffer *buffer)
{
    EditorLine *line = &buffer->lines[buffer->row];
    if (buffer->column < line->length) {
        size_t next = utf8_next(line, buffer->column);
        size_t removed = next - buffer->column;
        memmove(line->text + buffer->column, line->text + next,
                line->length - next + 1);
        line->length -= removed;
        buffer->bytes -= removed;
    } else if (buffer->row + 1 < buffer->line_count) {
        EditorLine *below = &buffer->lines[buffer->row + 1];
        if (!line_reserve(line, line->length + below->length + 1)) return false;
        memcpy(line->text + line->length, below->text, below->length + 1);
        line->length += below->length;
        line_destroy(below);
        memmove(buffer->lines + buffer->row + 1,
                buffer->lines + buffer->row + 2,
                (buffer->line_count - buffer->row - 2)*sizeof(*buffer->lines));
        --buffer->line_count;
        memset(&buffer->lines[buffer->line_count], 0, sizeof(*buffer->lines));
        --buffer->bytes;
    } else return true;
    buffer->dirty = true;
    remember_column(buffer);
    return true;
}

static bool insert_text(EditorBuffer *buffer, const char *text)
{
    while (*text) {
        if (*text == '\r' && text[1] == '\n') { ++text; continue; }
        if (*text == '\n' || *text == '\r') {
            if (!buffer_newline(buffer)) return false;
            ++text;
            continue;
        }
        size_t length = 1;
        unsigned char lead = (unsigned char)*text;
        if ((lead & 0xe0) == 0xc0) length = 2;
        else if ((lead & 0xf0) == 0xe0) length = 3;
        else if ((lead & 0xf8) == 0xf0) length = 4;
        for (size_t i = 1; i < length; ++i) {
            if (!text[i] || ((unsigned char)text[i] & 0xc0) != 0x80) {
                length = 1;
                break;
            }
        }
        if (!buffer_insert(buffer, text, length)) return false;
        text += length;
    }
    return true;
}

typedef struct { size_t row, column; } EditorPosition;
typedef struct { EditorPosition first, end; bool linewise; } EditorRange;

static bool visual_mode(const WorkspaceEditor *editor)
{
    return editor->mode==EDITOR_VISUAL || editor->mode==EDITOR_VISUAL_LINE;
}

static EditorRange selected_range(const WorkspaceEditor *editor,
                                   const EditorBuffer *buffer)
{
    EditorPosition first={editor->selection_row,editor->selection_column};
    EditorPosition last={buffer->row,buffer->column};
    if (first.row>last.row || (first.row==last.row && first.column>last.column)) {
        EditorPosition swap=first; first=last; last=swap;
    }
    bool linewise=editor->mode==EDITOR_VISUAL_LINE;
    if (linewise) { first.column=0; last=(EditorPosition){last.row+1,0}; }
    else {
        const EditorLine *line=&buffer->lines[last.row];
        last.column=utf8_next(line,last.column);
        if (!line->length && last.row+1<buffer->line_count)
            last=(EditorPosition){last.row+1,0};
    }
    return (EditorRange){first,last,linewise};
}

static char *range_text(const EditorBuffer *buffer, EditorRange range)
{
    size_t bytes=0;
    for (size_t row=range.first.row; row<buffer->line_count && row<=range.end.row; ++row) {
        if (range.linewise && row==range.end.row) break;
        size_t first=row==range.first.row ? range.first.column : 0;
        size_t end=row==range.end.row ? range.end.column : buffer->lines[row].length;
        bytes+=end-first;
        if (range.linewise || row<range.end.row) ++bytes;
    }
    char *text=malloc(bytes+1);
    if (!text) return NULL;
    size_t offset=0;
    for (size_t row=range.first.row; row<buffer->line_count && row<=range.end.row; ++row) {
        if (range.linewise && row==range.end.row) break;
        size_t first=row==range.first.row ? range.first.column : 0;
        size_t end=row==range.end.row ? range.end.column : buffer->lines[row].length;
        memcpy(text+offset,buffer->lines[row].text+first,end-first);
        offset+=end-first;
        if (range.linewise || row<range.end.row) text[offset++]='\n';
    }
    text[offset]='\0';
    return text;
}

/* Prepare replacement lines before changing the buffer. Positions are bytes,
 * and both ends are at UTF-8 character boundaries. The end is exclusive. */
static bool replace_range(EditorBuffer *buffer, EditorPosition first,
                           EditorPosition end, const char *text)
{
    size_t removed=end.column-first.column;
    if (first.row!=end.row) {
        removed=buffer->lines[first.row].length-first.column+end.column;
        for (size_t row=first.row+1; row<end.row; ++row)
            removed+=buffer->lines[row].length;
        removed+=end.row-first.row;
    }
    size_t length=strlen(text), added_count=1;
    if (length>EDITOR_FILE_LIMIT-(buffer->bytes-removed)) return false;
    for (size_t i=0; i<length; ++i) if (text[i]=='\n') ++added_count;
    EditorLine *added=calloc(added_count,sizeof(*added));
    if (!added) return false;
    size_t start=0,index=0;
    bool ok=false;
    for (size_t i=0; i<=length; ++i) {
        if (i!=length && text[i]!='\n') continue;
        size_t prefix=index==0 ? first.column : 0;
        size_t suffix=i==length ? buffer->lines[end.row].length-end.column : 0;
        size_t size=prefix+i-start+suffix;
        if (!line_reserve(&added[index],size+1)) goto done;
        if (prefix) memcpy(added[index].text,buffer->lines[first.row].text,prefix);
        memcpy(added[index].text+prefix,text+start,i-start);
        if (suffix) memcpy(added[index].text+prefix+i-start,
            buffer->lines[end.row].text+end.column,suffix);
        added[index].text[size]='\0'; added[index].length=size;
        ++index; start=i+1;
    }
    size_t old_count=end.row-first.row+1;
    size_t count=buffer->line_count-old_count+added_count;
    if (!buffer_reserve_lines(buffer,count)) goto done;
    for (size_t row=first.row; row<=end.row; ++row) line_destroy(&buffer->lines[row]);
    memmove(buffer->lines+first.row+added_count,buffer->lines+end.row+1,
        (buffer->line_count-end.row-1)*sizeof(*buffer->lines));
    memcpy(buffer->lines+first.row,added,added_count*sizeof(*added));
    if (count<buffer->line_count)
        memset(buffer->lines+count,0,(buffer->line_count-count)*sizeof(*buffer->lines));
    buffer->line_count=count;
    buffer->bytes=buffer->bytes-removed+length;
    buffer->row=first.row; buffer->column=first.column;
    buffer->dirty=true;
    clamp_cursor(buffer,false); remember_column(buffer);
    ok=true;
done:
    if (!ok) for (size_t i=0; i<added_count; ++i) line_destroy(&added[i]);
    free(added);
    return ok;
}

static bool operate_range(WorkspaceEditor *editor, EditorRange range, bool erase)
{
    EditorBuffer *buffer=current_buffer(editor);
    size_t previous_column=buffer->column;
    if (erase && buffer->readonly) {
        workspace_editor_message(editor,"buffer is read only",true);
        return false;
    }
    char *text=range_text(buffer,range);
    if (!text) goto fail;
    EditorPosition first=range.first,end=range.end;
    if (erase && range.linewise) {
        if (end.row==buffer->line_count) {
            end=(EditorPosition){buffer->line_count-1,
                buffer->lines[buffer->line_count-1].length};
            if (first.row) first=(EditorPosition){first.row-1,buffer->lines[first.row-1].length};
        }
    }
    if (erase && !replace_range(buffer,first,end,"")) { free(text); goto fail; }
    free(editor->yank); editor->yank=text; editor->yank_linewise=range.linewise;
    SetClipboardText(text);
    buffer->row=range.first.row<buffer->line_count ? range.first.row : buffer->line_count-1;
    buffer->column=range.linewise ?
        erase ? line_indent_length(&buffer->lines[buffer->row]) : previous_column :
        range.first.column;
    clamp_cursor(buffer,false); remember_column(buffer);
    editor->mode=EDITOR_NORMAL; editor->pending_operator=0;
    editor->messages_visible=false;
    return true;
fail:
    workspace_editor_message(editor,"cannot copy or edit buffer",true);
    return false;
}

static bool paste_register(WorkspaceEditor *editor, bool before, bool system_clipboard)
{
    EditorBuffer *buffer=current_buffer(editor);
    if (buffer->readonly) {
        workspace_editor_message(editor,"buffer is read only",true);
        return false;
    }
    const char *clipboard=!system_clipboard && editor->yank ? editor->yank : GetClipboardText();
    if (!clipboard || !*clipboard) return true;
    size_t length=strlen(clipboard);
    if (length>EDITOR_FILE_LIMIT) goto fail;
    char *text=malloc(length+2);
    if (!text) goto fail;
    size_t used=0;
    for (size_t i=0; i<length; ++i) {
        if (clipboard[i]=='\r' && clipboard[i+1]=='\n') continue;
        text[used++]=clipboard[i]=='\r' ? '\n' : clipboard[i];
    }
    text[used]='\0';
    bool linewise=editor->yank && !strcmp(clipboard,editor->yank) ?
        editor->yank_linewise : used && text[used-1]=='\n';
    EditorPosition position={buffer->row,buffer->column};
    size_t cursor_row=position.row,cursor_column=position.column;
    if (linewise) {
        bool empty=buffer->line_count==1 && !buffer->lines[0].length;
        if (empty) text[--used]='\0';
        else if (before) position.column=0;
        else if (position.row+1<buffer->line_count) { ++position.row; position.column=0; }
        else {
            position.column=buffer->lines[position.row].length;
            memmove(text+1,text,used);
            text[0]='\n';
            text[used]='\0'; /* Replace the register's final newline with a leading one. */
        }
        cursor_row=empty ? 0 : before ? buffer->row : buffer->row+1;
        cursor_column=0;
    } else {
        if (!before && buffer->lines[position.row].length)
            position.column=utf8_next(&buffer->lines[position.row],position.column);
        cursor_row=position.row; cursor_column=position.column;
        for (size_t i=0; i<used; ++i) {
            if (text[i]=='\n') { ++cursor_row; cursor_column=0; }
            else ++cursor_column;
        }
    }
    bool ok=replace_range(buffer,position,position,text);
    free(text);
    if (!ok) goto fail;
    buffer->row=cursor_row;
    if (linewise) buffer->column=line_indent_length(&buffer->lines[buffer->row]);
    else buffer->column=cursor_column ? utf8_previous(&buffer->lines[buffer->row],cursor_column) : 0;
    clamp_cursor(buffer,false); remember_column(buffer);
    editor->messages_visible=false;
    return true;
fail:
    workspace_editor_message(editor,"cannot paste clipboard",true);
    return false;
}

static char *skip_space(char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

static void trim_end(char *text)
{
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t'))
        text[--length] = '\0';
}

static bool append_text(char **cursor, size_t *remaining, const char *text)
{
    size_t length = strlen(text);
    if (length >= *remaining) return false;
    memcpy(*cursor, text, length);
    *cursor += length;
    *remaining -= length;
    **cursor = '\0';
    return true;
}

#ifndef _WIN32
static bool append_shell_path(char **cursor, size_t *remaining,
                              const char *path)
{
    if (!append_text(cursor, remaining, "'")) return false;
    for (; *path; ++path) {
        if (*path == '\'') {
            if (!append_text(cursor, remaining, "'\\''")) return false;
            continue;
        }
        char character[2] = {*path, '\0'};
        if (!append_text(cursor, remaining, character)) return false;
    }
    return append_text(cursor, remaining, "'");
}
#endif

static char *external_command_line(const WorkspaceEditor *editor,
                                   const char *command)
{
    size_t command_length = strlen(command);
    size_t cwd_length = strlen(editor->command_cwd);
    if (command_length > SIZE_MAX - 3*cwd_length - 32) return NULL;
    size_t capacity = command_length + 3*cwd_length + 32;
    char *line = malloc(capacity);
    if (!line) return NULL;
    line[0] = '\0';
    char *cursor = line;
    size_t remaining = capacity;
#ifdef _WIN32
    char prefix[EDITOR_PATH_CAPACITY + 32];
    int prefix_length = snprintf(prefix, sizeof(prefix), "( cd /d \"%s\" && ",
                                 editor->command_cwd);
    bool ok = prefix_length >= 0 && (size_t)prefix_length < sizeof(prefix) &&
              append_text(&cursor, &remaining, prefix);
#else
    bool ok = append_text(&cursor, &remaining, "(\ncd ") &&
              append_shell_path(&cursor, &remaining, editor->command_cwd) &&
              append_text(&cursor, &remaining, " &&\n");
#endif
    ok = ok && append_text(&cursor, &remaining, command);
#ifdef _WIN32
    ok = ok && append_text(&cursor, &remaining, " ) 2>&1");
#else
    ok = ok && append_text(&cursor, &remaining, "\n) 2>&1");
#endif
    if (!ok) {
        free(line);
        return NULL;
    }
    return line;
}

static int external_command_status(int status)
{
#ifdef _WIN32
    return status;
#else
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
#endif
}

static void receive_process_output(void *source,const char *text,
                                    bool error,bool finished)
{
    WorkspaceEditor *editor=source;
    editor->receiving_output=true;
    if (finished) {
        flush_command_output(editor);
        workspace_editor_message(editor,text,error);
    } else command_output(editor,text,error);
    editor->receiving_output=false;
}

static void run_external_command(WorkspaceEditor *editor, const char *command)
{
    if (!*command) {
        workspace_editor_message(editor, "usage: :! command", true);
        return;
    }
    char *line = external_command_line(editor, command);
    if (!line) {
        workspace_editor_message(editor, "external command is too long", true);
        return;
    }
    if (editor->output) {
        char error[EDITOR_MESSAGE_LENGTH],label[EDITOR_COMMAND_CAPACITY+64];
        snprintf(label,sizeof(label),"[mc-nvim %d] $ %s",editor->pane,command);
        if (workspace_output_start(editor->output,line,label,editor,
                receive_process_output,error,sizeof(error)))
            clear_messages(editor);
        else workspace_editor_message(editor,error,true);
        free(line);
        return;
    }
    FILE *pipe = editor_popen(line, "r");
    free(line);
    if (!pipe) {
        workspace_editor_message(editor, "cannot start external command", true);
        return;
    }
    clear_messages(editor);
    char output[1024];
    while (fgets(output, sizeof(output), pipe))
        command_output(editor, output, false);
    int status = external_command_status(editor_pclose(pipe));
    flush_command_output(editor);
    if (status == 0) {
        workspace_editor_message(editor, "external command finished: 0", false);
    } else if (status < 0) {
        workspace_editor_message(editor, "external command did not return a status", true);
    } else {
        char message[EDITOR_MESSAGE_LENGTH];
        snprintf(message, sizeof(message), "external command failed: %d", status);
        workspace_editor_message(editor, message, true);
    }
}

static bool find_project_root(const char *directory,char root[EDITOR_PATH_CAPACITY])
{
    if (!path_copy(root,directory)) return false;
    for (;;) {
        char path[EDITOR_PATH_CAPACITY],parent[EDITOR_PATH_CAPACITY];
        if (path_join(root,"nob.c",path) && path_kind(path)==PATH_FILE &&
            path_join(root,"nob.h",path) && path_kind(path)==PATH_FILE &&
            path_join(root,"include/rs/structure.h",path) && path_kind(path)==PATH_FILE)
            return true;
        if (!path_parent(root,parent) || paths_equal(root,parent)) return false;
        if (!path_copy(root,parent)) return false;
    }
}

static bool project_task_tool(WorkspaceEditor *editor,const EditorBuffer *buffer,
    char root[EDITOR_PATH_CAPACITY],char tool[EDITOR_PATH_CAPACITY])
{
    if (!editor->output) {
        workspace_editor_message(editor,"build output is unavailable",true);
        return false;
    }
    if (workspace_output_is_running(editor->output)) {
        workspace_editor_message(editor,
            "a command is already running; use mc-output Ctrl+C to stop it",true);
        return false;
    }
    char directory[EDITOR_PATH_CAPACITY];
    bool found=path_parent(buffer->path,directory) && find_project_root(directory,root);
    if (!found) found=find_project_root(editor->command_cwd,root);
    if (!found) {
        workspace_editor_message(editor,"cannot find the mcmux project directory",true);
        return false;
    }
#ifdef _WIN32
    const char *tool_name="nob.exe";
#else
    const char *tool_name="nob";
#endif
    if (!path_join(root,tool_name,tool) || path_kind(tool)!=PATH_FILE) {
        workspace_editor_message(editor,"project build tool not found; compile nob.c first",true);
        return false;
    }
    return true;
}

static void build_current_program(WorkspaceEditor *editor,bool run,
    WorkspaceEditorPrepareRun prepare_run,void *context)
{
    EditorBuffer *buffer=current_buffer(editor);
    if (!buffer || !buffer->named || !extension_equal(buffer->path,".c")) {
        workspace_editor_message(editor,"open a named .c file before :make or :run",true);
        return;
    }
    char root[EDITOR_PATH_CAPACITY],tool[EDITOR_PATH_CAPACITY];
    if (!project_task_tool(editor,buffer,root,tool)) return;
    if (!editor_write(editor,"")) return;
    if (run && prepare_run && !prepare_run(context,editor)) return;
    const char *arguments[]={tool,run ? "run-c" : "build-c",buffer->path,NULL};
    char error[EDITOR_MESSAGE_LENGTH],label[EDITOR_MESSAGE_LENGTH];
    snprintf(label,sizeof(label),"[mc-nvim %d] :%s %.680s",editor->pane,
        run ? "run" : "make",path_name(buffer->path));
    if (workspace_output_start_program(editor->output,arguments,root,label,editor,
            receive_process_output,error,sizeof(error)))
        clear_messages(editor);
    else workspace_editor_message(editor,error,true);
}

/* Whole arguments may be quoted. Backslashes remain literal for Windows paths;
 * no shell expansion or C-source parsing is involved. */
static size_t model_words(const char *text,char words[3][EDITOR_PATH_CAPACITY])
{
    size_t count=0;
    while (*text) {
        while (isspace((unsigned char)*text)) ++text;
        if (!*text) break;
        if (count==3) return 0;
        char quote=(*text=='\'' || *text=='"') ? *text++ : '\0';
        size_t length=0;
        while (*text && (quote ? *text!=quote : !isspace((unsigned char)*text))) {
            if (length+1==EDITOR_PATH_CAPACITY) return 0;
            words[count][length++]=*text++;
        }
        if (!length) return 0;
        words[count++][length]='\0';
        if (quote) {
            if (*text++!=quote || (*text && !isspace((unsigned char)*text))) return 0;
        }
    }
    return count;
}

static void export_current_model(WorkspaceEditor *editor,const char *argument)
{
    EditorBuffer *buffer=current_buffer(editor);
    if (!buffer || !buffer->named || !extension_equal(buffer->path,".c")) {
        workspace_editor_message(editor,"open a named .c file before :model",true);
        return;
    }
    const char *specification=*argument ? argument : NULL;
    if (!specification) {
        static const char prefix[]="// mcmux-model:";
        for (size_t row=0;row<buffer->line_count;++row) {
            const char *line=buffer->lines[row].text;
            while (*line==' ' || *line=='\t') ++line;
            if (strncmp(line,prefix,sizeof(prefix)-1)) continue;
            if (specification) {
                workspace_editor_message(editor,"keep only one // mcmux-model: line",true);
                return;
            }
            specification=line+sizeof(prefix)-1;
        }
    }
    if (!specification) {
        workspace_editor_message(editor,
            "add // mcmux-model: INPUT.mcs OUTPUT.c SYMBOL, or use :model INPUT.mcs OUTPUT.c [SYMBOL]",true);
        return;
    }
    char words[3][EDITOR_PATH_CAPACITY];
    size_t count=model_words(specification,words);
    if (count<2 || !extension_equal(words[0],".mcs") ||
        !extension_equal(words[1],".c")) {
        workspace_editor_message(editor,"usage: :model INPUT.mcs OUTPUT.c [SYMBOL] (quote paths with spaces)",true);
        return;
    }
    char root[EDITOR_PATH_CAPACITY],tool[EDITOR_PATH_CAPACITY];
    if (!project_task_tool(editor,buffer,root,tool)) return;
    char output[EDITOR_PATH_CAPACITY];
    if (!path_resolve(root,words[1],output)) {
        workspace_editor_message(editor,"model output path is too long",true);
        return;
    }
    if (paths_equal(output,buffer->path)) {
        workspace_editor_message(editor,"model output must differ from the current C program",true);
        return;
    }
    size_t open=buffer_find(editor,output);
    if (open!=SIZE_MAX && editor->buffers[open]->dirty) {
        workspace_editor_message(editor,"model output has unsaved changes in an open buffer",true);
        return;
    }
    if (!editor_write(editor,"")) return;
    const char *arguments[]={tool,"export-c",words[0],words[1],
        count==3 ? words[2] : "mcmux_circuit",NULL};
    char error[EDITOR_MESSAGE_LENGTH],label[EDITOR_MESSAGE_LENGTH];
    snprintf(label,sizeof(label),"[mc-nvim %d] :model %.680s",editor->pane,path_name(words[0]));
    if (workspace_output_start_program(editor->output,arguments,root,label,editor,
            receive_process_output,error,sizeof(error)))
        clear_messages(editor);
    else workspace_editor_message(editor,error,true);
}

static EditorLocation editor_location(const WorkspaceEditor *editor)
{
    const EditorBuffer *buffer=current_buffer_const(editor);
    return (EditorLocation){editor->current,buffer->row,buffer->column,buffer->top,buffer->left};
}

static uint64_t buffer_hash(const EditorBuffer *buffer)
{
    uint64_t hash=UINT64_C(14695981039346656037);
    for (size_t row=0;row<buffer->line_count;++row) {
        for (size_t i=0;i<buffer->lines[row].length;++i)
            hash=(hash^(unsigned char)buffer->lines[row].text[i])*UINT64_C(1099511628211);
        hash=(hash^'\n')*UINT64_C(1099511628211);
    }
    return hash;
}

static void close_code_info(WorkspaceEditor *editor)
{
    buffer_destroy(editor->code_info); editor->code_info=NULL;
}

static void jump_back(WorkspaceEditor *editor)
{
    if (editor->code_pending) {
        workspace_code_detach(editor->code,editor); editor->code_pending=false;
    }
    close_code_info(editor);
    if (!editor->jump_count) {
        workspace_editor_message(editor,"No earlier code jump",true); return;
    }
    EditorLocation location=editor->jumps[--editor->jump_count];
    editor->current=location.buffer;
    EditorBuffer *buffer=current_buffer(editor);
    buffer->row=location.row; buffer->column=location.column;
    buffer->top=location.top; buffer->left=location.left;
    clamp_cursor(buffer,false); remember_column(buffer);
    editor->mode=EDITOR_NORMAL; editor->pending_operator=0; editor->messages_visible=false;
}

static void receive_code(void *source,const WorkspaceCodeReply *reply)
{
    WorkspaceEditor *editor=source;
    editor->code_pending=false;
    EditorBuffer *buffer=current_buffer(editor);
    EditorLocation origin=editor->code_origin;
    if (!buffer || editor->current!=origin.buffer || buffer->row!=origin.row ||
        buffer->column!=origin.column || editor->mode!=EDITOR_NORMAL ||
        buffer_hash(buffer)!=editor->code_hash) return;
    if (reply->error) { workspace_editor_message(editor,reply->text,true); return; }
    close_code_info(editor);
    if (!reply->path[0]) {
        editor->code_info=buffer_from_bytes("[Symbol information]",reply->text,strlen(reply->text));
        if (!editor->code_info) workspace_editor_message(editor,"Cannot show symbol information: out of memory",true);
        else editor->messages_visible=false;
        return;
    }
    if (!editor_open_file(editor,reply->path)) return;
    if (editor->jump_count==sizeof(editor->jumps)/sizeof(editor->jumps[0])) {
        memmove(editor->jumps,editor->jumps+1,(editor->jump_count-1)*sizeof(editor->jumps[0]));
        --editor->jump_count;
    }
    editor->jumps[editor->jump_count++]=origin;
    buffer=current_buffer(editor);
    buffer->row=reply->line<buffer->line_count ? reply->line : buffer->line_count-1;
    buffer->column=reply->column;
    buffer->top=buffer->row>5 ? buffer->row-5 : 0; buffer->left=0;
    clamp_cursor(buffer,false); remember_column(buffer);
    editor->messages_visible=false;
}

static void query_code(WorkspaceEditor *editor,WorkspaceCodeKind kind)
{
    editor->pending_operator=0;
    EditorBuffer *buffer=current_buffer(editor);
    if (!buffer || !buffer->named || buffer->synthetic || !buffer_is_c(buffer)) {
        workspace_editor_message(editor,"Open a C source or header before looking up a symbol",true); return;
    }
    char root[EDITOR_PATH_CAPACITY],directory[EDITOR_PATH_CAPACITY];
    bool found=path_parent(buffer->path,directory) && find_project_root(directory,root);
    if (!found) found=find_project_root(editor->command_cwd,root);
    if (!found || !editor->code) {
        workspace_editor_message(editor,"Code navigation needs an open mcmux project",true); return;
    }
    WorkspaceCodeDocument documents[EDITOR_BUFFER_LIMIT];
    char *texts[EDITOR_BUFFER_LIMIT]={0};
    size_t count=0,active=0;
    size_t total=0;
    char error[EDITOR_MESSAGE_LENGTH]="Cannot prepare code lookup: out of memory";
    bool ok=true;
    for (size_t i=0;i<editor->buffer_count;++i) {
        const EditorBuffer *b=editor->buffers[i];
        if (!b->named || b->synthetic || !buffer_is_c(b)) continue;
        if (b->bytes>16*1024*1024-total) {
            snprintf(error,sizeof(error),"Open C buffers exceed the code navigation size limit"); ok=false; break;
        }
        total+=b->bytes;
        char *value=malloc(b->bytes+1);
        if (!value) { ok=false; break; }
        size_t length=0;
        for (size_t row=0;row<b->line_count;++row) {
            if (row) value[length++]='\n';
            memcpy(value+length,b->lines[row].text,b->lines[row].length); length+=b->lines[row].length;
        }
        value[length]='\0'; texts[count]=value;
        documents[count]=(WorkspaceCodeDocument){b->path,value};
        if (i==editor->current) active=count;
        ++count;
    }
    if (ok) {
        EditorLocation origin=editor_location(editor);
        ok=workspace_code_query(editor->code,root,documents,count,active,buffer->row,
            buffer->column,kind,editor,
            receive_code,error,sizeof(error));
        if (ok) {
            editor->code_origin=origin; editor->code_hash=buffer_hash(buffer);
            editor->code_pending=true;
            workspace_editor_message(editor,kind==CODE_HOVER ? "Reading symbol information..." : "Finding symbol location...",false);
        }
    }
    for (size_t i=0;i<count;++i) free(texts[i]);
    if (!ok) workspace_editor_message(editor,error,true);
}

static WorkspaceEditorAction execute_editor_command(WorkspaceEditor *editor,
    WorkspaceEditorPrepareRun prepare_run, void *context)
{
    WorkspaceEditorAction action = WORKSPACE_EDITOR_CONTINUE;
    char line[EDITOR_COMMAND_CAPACITY];
    memcpy(line, editor->command, editor->command_length + 1);
    trim_end(line);
    char *command_start = skip_space(line);
    /* Vim's shell escape does not require a space after '!'. Dispatch it
     * before splitting editor command words, keeping shell quoting intact. */
    if (*command_start == '!') {
        run_external_command(editor, skip_space(command_start + 1));
        return action;
    }
    char *command = command_start;
    char *argument = command;
    while (*argument && *argument != ' ' && *argument != '\t') ++argument;
    if (*argument) *argument++ = '\0';
    argument = skip_space(argument);
    if (!*command) return action;
    if (strcmp(command, "Ex") == 0 || strcmp(command, "Explore") == 0) {
        (void)browser_open(editor, argument);
    } else if (strcmp(command, "e") == 0) {
        if (!*argument) workspace_editor_message(editor, "usage: :e path", true);
        else (void)editor_open_file(editor, argument);
    } else if (strcmp(command, "w") == 0) {
        (void)editor_write(editor, argument);
    } else if (strcmp(command,"make")==0 || strcmp(command,"run")==0) {
        if (*argument) workspace_editor_message(editor,"usage: :make or :run",true);
        else build_current_program(editor,strcmp(command,"run")==0,
                                   prepare_run,context);
    } else if (strcmp(command,"model")==0) {
        export_current_model(editor,argument);
    } else if (strcmp(command,"definition")==0 || strcmp(command,"declaration")==0 || strcmp(command,"hover")==0) {
        if (*argument) workspace_editor_message(editor,"usage: :definition, :declaration or :hover",true);
        else query_code(editor,!strcmp(command,"hover") ? CODE_HOVER :
            !strcmp(command,"definition") ? CODE_DEFINITION : CODE_DECLARATION);
    } else if (strcmp(command,"back")==0) {
        if (*argument) workspace_editor_message(editor,"usage: :back",true);
        else jump_back(editor);
    } else if (strcmp(command, "q") == 0) {
        if (*argument) {
            workspace_editor_message(editor, "usage: :q", true);
        } else if (workspace_editor_has_unsaved(editor)) {
            workspace_editor_message(editor,
                "unsaved changes; use :q! to discard them", true);
        } else {
            action = WORKSPACE_EDITOR_QUIT;
        }
    } else if (strcmp(command, "q!") == 0) {
        if (*argument)
            workspace_editor_message(editor, "usage: :q!", true);
        else
            action = WORKSPACE_EDITOR_QUIT_FORCE;
    } else if (strcmp(command, "wq") == 0) {
        if (editor_write(editor, argument)) {
            if (workspace_editor_has_unsaved(editor))
                workspace_editor_message(editor,
                    "other buffers have unsaved changes; use :q! to discard them",
                    true);
            else
                action = WORKSPACE_EDITOR_QUIT;
        }
    } else {
        workspace_editor_message(editor, "Unknown editor command", true);
    }
    return action;
}

static void update_normal(WorkspaceEditor *editor)
{
    EditorBuffer *buffer = current_buffer(editor);
    if (!buffer) return;
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (editor->code_pending) {
            workspace_code_detach(editor->code,editor); editor->code_pending=false;
            editor->messages_visible=false;
        }
        editor->mode=EDITOR_NORMAL; editor->pending_operator=0;
        while (GetCharPressed()!=0) {}
        return;
    }
    bool control=IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
        IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    if (control) {
        if (!visual_mode(editor) && (IsKeyPressed(KEY_O) || IsKeyPressed(KEY_T)))
            jump_back(editor);
        else if (!visual_mode(editor) && IsKeyPressed(KEY_RIGHT_BRACKET))
            query_code(editor,CODE_DEFINITION);
        else if (visual_mode(editor) && (IsKeyPressed(KEY_C) || IsKeyPressed(KEY_X)))
            (void)operate_range(editor,selected_range(editor,buffer),IsKeyPressed(KEY_X));
        else if (IsKeyPressed(KEY_V) && !visual_mode(editor))
            (void)paste_register(editor,false,true);
        while (GetCharPressed()!=0) {}
        return;
    }
    if (IsKeyPressed(KEY_DELETE)) {
        if (visual_mode(editor)) (void)operate_range(editor,selected_range(editor,buffer),true);
        else if (buffer->lines[buffer->row].length) {
            EditorRange range={{buffer->row,buffer->column},
                {buffer->row,utf8_next(&buffer->lines[buffer->row],buffer->column)},false};
            (void)operate_range(editor,range,true);
        }
        while (GetCharPressed()!=0) {}
        return;
    }
    bool moved = false, interacted = false;
    bool shift=IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (editor_key(KEY_LEFT) || (!shift && editor_key(KEY_H))) {
        EditorLine *line = &buffer->lines[buffer->row];
        if (buffer->column) buffer->column = utf8_previous(line, buffer->column);
        moved = interacted = true;
    } else if (editor_key(KEY_RIGHT) || (!shift && editor_key(KEY_L))) {
        EditorLine *line = &buffer->lines[buffer->row];
        size_t next = utf8_next(line, buffer->column);
        if (next < line->length) buffer->column = next;
        moved = interacted = true;
    } else if (editor_key(KEY_UP) || (!shift && editor_key(KEY_K))) {
        move_vertical(buffer, -1, false);
        interacted = true;
    } else if (editor_key(KEY_DOWN) || (!shift && editor_key(KEY_J))) {
        move_vertical(buffer, 1, false);
        interacted = true;
    } else if (IsKeyPressed(KEY_HOME)) {
        buffer->column = 0;
        moved = interacted = true;
    } else if (IsKeyPressed(KEY_END)) {
        buffer->column = normal_last_column(&buffer->lines[buffer->row]);
        moved = interacted = true;
    } else if (IsKeyPressed(KEY_PAGE_UP)) {
        for (int i = 0; i < 10; ++i) move_vertical(buffer, -1, false);
        interacted = true;
    } else if (IsKeyPressed(KEY_PAGE_DOWN)) {
        for (int i = 0; i < 10; ++i) move_vertical(buffer, 1, false);
        interacted = true;
    }
    if (moved) remember_column(buffer);
    if (interacted) editor->pending_operator=0;
    int character;
    while ((character = GetCharPressed()) != 0) {
        if (character=='v' || character=='V') {
            EditorMode mode=character=='V' ? EDITOR_VISUAL_LINE : EDITOR_VISUAL;
            if (!visual_mode(editor)) {
                editor->selection_row=buffer->row;
                editor->selection_column=buffer->column;
            }
            editor->mode=editor->mode==mode ? EDITOR_NORMAL : mode;
            editor->pending_operator=0;
            editor->messages_visible=false;
            continue;
        }
        if (visual_mode(editor)) {
            if (character=='D') {
                size_t first=editor->selection_row<buffer->row ? editor->selection_row : buffer->row;
                size_t last=editor->selection_row>buffer->row ? editor->selection_row : buffer->row;
                (void)operate_range(editor,(EditorRange){{first,0},{last+1,0},true},true);
            } else if (character=='y' || character=='d' || character=='x')
                (void)operate_range(editor,selected_range(editor,buffer),character!='y');
            continue;
        }
        if (editor->pending_operator=='g') {
            editor->pending_operator=0;
            if (character=='d' || character=='D') {
                query_code(editor,character=='d' ? CODE_DEFINITION : CODE_DECLARATION);
                continue;
            }
        }
        if (character=='g') { editor->pending_operator='g'; continue; }
        if (character=='K') { query_code(editor,CODE_HOVER); continue; }
        EditorLine *line=&buffer->lines[buffer->row];
        if (character=='d' || character=='y') {
            if (editor->pending_operator==character) {
                EditorRange range={{buffer->row,0},{buffer->row+1,0},true};
                (void)operate_range(editor,range,character=='d');
            } else editor->pending_operator=(char)character;
            continue;
        }
        editor->pending_operator=0;
        if (character=='D' || character=='x') {
            if (line->length) {
                EditorRange range={{buffer->row,buffer->column},
                    {buffer->row,character=='D' ? line->length : utf8_next(line,buffer->column)},false};
                (void)operate_range(editor,range,true);
            }
        } else if (character=='p' || character=='P') {
            (void)paste_register(editor,character=='P',false);
        } else if (character=='i' || character=='a' || character=='I' || character=='A') {
            if (buffer->readonly) {
                workspace_editor_message(editor,"buffer is read only",true);
                continue;
            }
            if (character=='a' && line->length) buffer->column=utf8_next(line,buffer->column);
            else if (character=='I') buffer->column=line_indent_length(line);
            else if (character=='A') buffer->column=line->length;
            editor->mode=EDITOR_INSERT;
            editor->messages_visible=false;
            remember_column(buffer);
            while (GetCharPressed()!=0) {}
            return;
        }
        if (character == ':') {
            enter_command_mode(editor, EDITOR_NORMAL);
            break;
        }
    }
    if (interacted) editor->messages_visible = false;
}

static void update_insert(WorkspaceEditor *editor)
{
    EditorBuffer *buffer = current_buffer(editor);
    if (!buffer) return;
    if (IsKeyPressed(KEY_ESCAPE)) {
        EditorLine *line = &buffer->lines[buffer->row];
        if (buffer->column) buffer->column = utf8_previous(line, buffer->column);
        editor->mode = EDITOR_NORMAL;
        clamp_cursor(buffer, false);
        remember_column(buffer);
        while (GetCharPressed() != 0) {}
        return;
    }
    bool control = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool super = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    if ((control || super) && IsKeyPressed(KEY_V)) {
        const char *clipboard = GetClipboardText();
        if (clipboard && !insert_text(buffer, clipboard))
            workspace_editor_message(editor, "cannot insert clipboard", true);
    }
    EditorLine *line = &buffer->lines[buffer->row];
    if (editor_key(KEY_LEFT)) {
        if (buffer->column) buffer->column = utf8_previous(line, buffer->column);
        else if (buffer->row) {
            --buffer->row;
            buffer->column = buffer->lines[buffer->row].length;
        }
        remember_column(buffer);
    } else if (editor_key(KEY_RIGHT)) {
        if (buffer->column < line->length)
            buffer->column = utf8_next(line, buffer->column);
        else if (buffer->row + 1 < buffer->line_count) {
            ++buffer->row;
            buffer->column = 0;
        }
        remember_column(buffer);
    } else if (editor_key(KEY_UP)) {
        move_vertical(buffer, -1, true);
    } else if (editor_key(KEY_DOWN)) {
        move_vertical(buffer, 1, true);
    }
    if (IsKeyPressed(KEY_HOME)) { buffer->column = 0; remember_column(buffer); }
    if (IsKeyPressed(KEY_END)) {
        buffer->column = buffer->lines[buffer->row].length;
        remember_column(buffer);
    }
    if (editor_key(KEY_BACKSPACE) && !buffer_backspace(buffer))
        workspace_editor_message(editor, "cannot edit buffer", true);
    if (editor_key(KEY_DELETE) && !buffer_delete(buffer))
        workspace_editor_message(editor, "cannot edit buffer", true);
    if (IsKeyPressed(KEY_ENTER) && !buffer_indented_newline(buffer))
        workspace_editor_message(editor, "cannot add line", true);
    if (IsKeyPressed(KEY_TAB) && !buffer_tab(buffer))
        workspace_editor_message(editor, "cannot insert tab", true);
    int character;
    while ((character = GetCharPressed()) != 0) {
        if (control || super || character < 32 || character == 127) continue;
        int count = 0;
        const char *encoded = CodepointToUTF8(character, &count);
        if (count > 0 && !buffer_typed_text(buffer, encoded, (size_t)count)) {
            workspace_editor_message(editor, "cannot insert text", true);
            break;
        }
    }
}

static void command_insert(WorkspaceEditor *editor, const char *text,
                           size_t length)
{
    if (length > sizeof(editor->command) - editor->command_length - 1) return;
    memmove(editor->command + editor->command_cursor + length,
            editor->command + editor->command_cursor,
            editor->command_length - editor->command_cursor + 1);
    memcpy(editor->command + editor->command_cursor, text, length);
    editor->command_cursor += length;
    editor->command_length += length;
}

static void enter_command_mode(WorkspaceEditor *editor, EditorMode return_mode)
{
    editor->messages_visible = false;
    editor->command_previous_mode = return_mode;
    editor->mode = EDITOR_COMMAND;
    editor->command[0] = '\0';
    editor->command_length = editor->command_cursor = 0;
}

static WorkspaceEditorAction update_command_mode(WorkspaceEditor *editor,
    WorkspaceEditorPrepareRun prepare_run, void *context)
{
    WorkspaceEditorAction action = WORKSPACE_EDITOR_CONTINUE;
    if (IsKeyPressed(KEY_ESCAPE)) {
        editor->mode = editor->command_previous_mode;
        editor->command[0] = '\0';
        editor->command_length = editor->command_cursor = 0;
        while (GetCharPressed() != 0) {}
        return action;
    }
    bool control = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool super = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    if ((control || super) && IsKeyPressed(KEY_V)) {
        const char *clipboard = GetClipboardText();
        if (clipboard) {
            for (; *clipboard; ++clipboard) {
                char character = *clipboard == '\n' || *clipboard == '\r' ?
                    ' ' : *clipboard;
                command_insert(editor, &character, 1);
            }
        }
    }
    if (editor_key(KEY_BACKSPACE) && editor->command_cursor) {
        size_t previous = editor->command_cursor - 1;
        while (previous &&
               ((unsigned char)editor->command[previous] & 0xc0) == 0x80)
            --previous;
        memmove(editor->command + previous,
                editor->command + editor->command_cursor,
                editor->command_length - editor->command_cursor + 1);
        editor->command_length -= editor->command_cursor - previous;
        editor->command_cursor = previous;
    }
    if (editor_key(KEY_DELETE) && editor->command_cursor < editor->command_length) {
        size_t next = editor->command_cursor + 1;
        while (next < editor->command_length &&
               ((unsigned char)editor->command[next] & 0xc0) == 0x80) ++next;
        memmove(editor->command + editor->command_cursor,
                editor->command + next,
                editor->command_length - next + 1);
        editor->command_length -= next - editor->command_cursor;
    }
    if (editor_key(KEY_LEFT) && editor->command_cursor) {
        --editor->command_cursor;
        while (editor->command_cursor &&
               ((unsigned char)editor->command[editor->command_cursor] & 0xc0) == 0x80)
            --editor->command_cursor;
    }
    if (editor_key(KEY_RIGHT) && editor->command_cursor < editor->command_length) {
        ++editor->command_cursor;
        while (editor->command_cursor < editor->command_length &&
               ((unsigned char)editor->command[editor->command_cursor] & 0xc0) == 0x80)
            ++editor->command_cursor;
    }
    if (IsKeyPressed(KEY_HOME)) editor->command_cursor = 0;
    if (IsKeyPressed(KEY_END)) editor->command_cursor = editor->command_length;
    int character;
    while ((character = GetCharPressed()) != 0) {
        if (control || super || character < 32 || character == 127) continue;
        int count = 0;
        const char *encoded = CodepointToUTF8(character, &count);
        if (count > 0) command_insert(editor, encoded, (size_t)count);
    }
    if (IsKeyPressed(KEY_ENTER)) {
        editor->mode = editor->command_previous_mode;
        action = execute_editor_command(editor, prepare_run, context);
        editor->command[0] = '\0';
        editor->command_length = editor->command_cursor = 0;
    }
    return action;
}

static void browser_enter(WorkspaceEditor *editor)
{
    if (editor->selected >= editor->entry_count) return;
    BrowserEntry *entry = &editor->entries[editor->selected];
    char path[EDITOR_PATH_CAPACITY];
    if (entry->parent) {
        if (!path_parent(editor->cwd, path)) return;
    } else if (!path_join(editor->cwd, entry->name, path)) {
        workspace_editor_message(editor, "path is too long", true);
        return;
    }
    if (entry->directory) (void)browser_open(editor, path);
    else (void)editor_open_file(editor, path);
}

static void update_browser(WorkspaceEditor *editor)
{
    if (IsKeyPressed(KEY_ESCAPE)) {
        editor->mode = EDITOR_NORMAL;
        return;
    }
    if (editor_key(KEY_UP) || editor_key(KEY_K)) {
        if (editor->selected) --editor->selected;
    } else if (editor_key(KEY_DOWN) || editor_key(KEY_J)) {
        if (editor->selected + 1 < editor->entry_count) ++editor->selected;
    } else if (IsKeyPressed(KEY_HOME)) {
        editor->selected = 0;
    } else if (IsKeyPressed(KEY_END) && editor->entry_count) {
        editor->selected = editor->entry_count - 1;
    } else if (IsKeyPressed(KEY_PAGE_UP)) {
        editor->selected = editor->selected > 10 ? editor->selected - 10 : 0;
    } else if (IsKeyPressed(KEY_PAGE_DOWN) && editor->entry_count) {
        editor->selected += 10;
        if (editor->selected >= editor->entry_count)
            editor->selected = editor->entry_count - 1;
    }
    float wheel = GetMouseWheelMove();
    if (wheel > 0 && editor->selected) --editor->selected;
    if (wheel < 0 && editor->selected + 1 < editor->entry_count) ++editor->selected;
    if (IsKeyPressed(KEY_H)) {
        char parent[EDITOR_PATH_CAPACITY];
        if (path_parent(editor->cwd, parent)) (void)browser_open(editor, parent);
    } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_L)) {
        browser_enter(editor);
    }
    int character;
    while ((character = GetCharPressed()) != 0) {
        if (character == ':') {
            enter_command_mode(editor, EDITOR_BROWSER);
            break;
        }
    }
}

WorkspaceEditor *workspace_editor_create(void)
{
    WorkspaceEditor *editor = calloc(1, sizeof(*editor));
    if (!editor) return NULL;
    if (!editor_getcwd(editor->cwd, sizeof(editor->cwd))) {
        free(editor);
        return NULL;
    }
    slashify(editor->cwd);
    /* Keep the initial directory for commands while browsing other folders. */
    memcpy(editor->command_cwd, editor->cwd, strlen(editor->cwd) + 1);
    EditorBuffer *buffer = buffer_create_empty();
    if (!buffer) { free(editor); return NULL; }
    editor->buffers[0] = buffer;
    editor->buffer_count = 1;
    editor->mode = EDITOR_NORMAL;
    return editor;
}

void workspace_editor_set_output(WorkspaceEditor *editor,WorkspaceOutput *output,
                                  int pane)
{
    if (!editor) return;
    workspace_output_detach(editor->output,editor);
    editor->output=output; editor->pane=pane;
}

void workspace_editor_set_code(WorkspaceEditor *editor,WorkspaceCode *code)
{
    if (!editor) return;
    workspace_code_detach(editor->code,editor); editor->code=code; editor->code_pending=false;
}

void workspace_editor_destroy(WorkspaceEditor *editor)
{
    if (!editor) return;
    workspace_output_detach(editor->output,editor);
    workspace_code_detach(editor->code,editor);
    close_code_info(editor);
    free(editor->yank);
    browser_clear(editor);
    for (size_t i = 0; i < editor->buffer_count; ++i)
        buffer_destroy(editor->buffers[i]);
    free(editor);
}

bool workspace_editor_has_unsaved(const WorkspaceEditor *editor)
{
    if (!editor) return false;
    for (size_t i = 0; i < editor->buffer_count; ++i)
        if (editor->buffers[i]->dirty) return true;
    return false;
}

WorkspaceEditorAction workspace_editor_update(WorkspaceEditor *editor,
    WorkspaceEditorPrepareRun prepare_run, void *context)
{
    WorkspaceEditorAction action = WORKSPACE_EDITOR_CONTINUE;
    if (!editor) return action;
    if (editor->code_info) {
        EditorBuffer *info=editor->code_info;
        if (IsKeyPressed(KEY_ESCAPE)) close_code_info(editor);
        else {
            if (editor_key(KEY_UP) || editor_key(KEY_K)) { if (info->top) --info->top; }
            if (editor_key(KEY_DOWN) || editor_key(KEY_J)) ++info->top;
            if (IsKeyPressed(KEY_PAGE_UP)) info->top=info->top>10 ? info->top-10 : 0;
            if (IsKeyPressed(KEY_PAGE_DOWN)) info->top+=10;
            if (IsKeyPressed(KEY_HOME)) info->top=0;
            if (IsKeyPressed(KEY_END)) info->top=info->line_count-1;
            if (editor_key(KEY_LEFT)) info->left=info->left>4 ? info->left-4 : 0;
            if (editor_key(KEY_RIGHT)) info->left+=4;
            if (info->top>=info->line_count) info->top=info->line_count-1;
            if (info->left>EDITOR_DRAW_CAPACITY) info->left=EDITOR_DRAW_CAPACITY;
        }
        while (GetCharPressed()!=0) {}
        return action;
    }
    if (editor->mode == EDITOR_INSERT) update_insert(editor);
    else if (editor->mode == EDITOR_COMMAND)
        action = update_command_mode(editor, prepare_run, context);
    else if (editor->mode == EDITOR_BROWSER) update_browser(editor);
    else update_normal(editor);
    return action;
}

static size_t decimal_digits(size_t value)
{
    size_t digits = 1;
    while (value >= 10) { value /= 10; ++digits; }
    return digits;
}

static size_t display_slice(const EditorLine *line, size_t first_column,
                            size_t columns, char output[EDITOR_DRAW_CAPACITY])
{
    size_t output_length = 0, display = 0;
    for (size_t offset = 0; offset < line->length;) {
        size_t next = utf8_next(line, offset);
        unsigned char character = (unsigned char)line->text[offset];
        size_t width = character == '\t' ?
            EDITOR_TAB_WIDTH - display%EDITOR_TAB_WIDTH : 1;
        if (display + width > first_column && display < first_column + columns) {
            if (character == '\t') {
                size_t visible_start = display > first_column ? display : first_column;
                size_t visible_end = display + width < first_column + columns ?
                    display + width : first_column + columns;
                for (size_t i = visible_start; i < visible_end &&
                     output_length + 1 < EDITOR_DRAW_CAPACITY; ++i)
                    output[output_length++] = ' ';
            } else if (character < 32 || character == 127) {
                if (output_length + 1 < EDITOR_DRAW_CAPACITY)
                    output[output_length++] = '?';
            } else if (output_length + next - offset < EDITOR_DRAW_CAPACITY) {
                memcpy(output + output_length, line->text + offset, next - offset);
                output_length += next - offset;
            }
        }
        display += width;
        if (display >= first_column + columns) break;
        offset = next;
    }
    output[output_length] = '\0';
    return output_length;
}

static int visible_message_rows(const WorkspaceEditor *editor, int available)
{
    if (!editor->messages_visible || !editor->message_count || available < 3)
        return 0;
    int rows = (int)editor->message_count;
    if (rows > 5) rows = 5;
    if (rows > available/3) rows = available/3;
    return rows;
}

static void draw_messages(const WorkspaceEditor *editor, Rectangle box,
                          int bottom, int rows)
{
    if (!rows) return;
    int top = bottom - rows*EDITOR_ROW_HEIGHT;
    DrawRectangle((int)box.x + 1, top, (int)box.width - 2,
                  rows*EDITOR_ROW_HEIGHT, (Color){22, 27, 36, 248});
    size_t start = editor->message_count > (size_t)rows ?
        editor->message_count - (size_t)rows : 0;
    for (int row = 0; row < rows; ++row) {
        size_t logical = start + (size_t)row;
        size_t index = (editor->message_first + logical)%EDITOR_MESSAGE_LIMIT;
        const EditorMessage *message = &editor->messages[index];
        ui_text_draw(message->text, (int)box.x + 8,
                     top + row*EDITOR_ROW_HEIGHT + 2, EDITOR_FONT_SIZE,
                     message->error ? (Color){255, 128, 128, 255} :
                                      (Color){211, 222, 231, 255});
    }
}

static void draw_browser(WorkspaceEditor *editor, Rectangle box,
                         int body_top, int body_bottom, int cell_width)
{
    int rows = (body_bottom - body_top)/EDITOR_ROW_HEIGHT;
    if (rows < 1) return;
    if (editor->selected < editor->browser_top)
        editor->browser_top = editor->selected;
    if (editor->selected >= editor->browser_top + (size_t)rows)
        editor->browser_top = editor->selected - (size_t)rows + 1;
    int columns = ((int)box.width - 24)/cell_width;
    if (columns < 1) columns = 1;
    for (int row = 0; row < rows; ++row) {
        size_t index = editor->browser_top + (size_t)row;
        if (index >= editor->entry_count) break;
        const BrowserEntry *entry = &editor->entries[index];
        int y = body_top + row*EDITOR_ROW_HEIGHT;
        if (index == editor->selected)
            DrawRectangle((int)box.x + 3, y, (int)box.width - 6,
                          EDITOR_ROW_HEIGHT, (Color){59, 53, 83, 255});
        char label[EDITOR_PATH_CAPACITY + 8];
        snprintf(label, sizeof(label), "%s%s%s",
                 entry->directory ? "[D] " : "    ", entry->name,
                 entry->directory && !entry->parent ? "/" : "");
        size_t limit = (size_t)columns < sizeof(label) ?
            (size_t)columns : sizeof(label) - 1;
        label[limit] = '\0';
        ui_text_draw(label, (int)box.x + 8, y + 2, EDITOR_FONT_SIZE,
                     entry->directory ? (Color){172, 143, 255, 255} :
                                        (Color){211, 222, 231, 255});
    }
}

static void draw_buffer(WorkspaceEditor *editor, Rectangle box,
                        int body_top, int body_bottom, int cell_width,
                        bool focused)
{
    EditorBuffer *buffer = current_buffer(editor);
    if (!buffer) return;
    int rows = (body_bottom - body_top)/EDITOR_ROW_HEIGHT;
    if (rows < 1) return;
    size_t digits = decimal_digits(buffer->line_count);
    int gutter_columns = (int)digits + 2;
    int text_x = (int)box.x + 6 + gutter_columns*cell_width;
    int columns = ((int)(box.x + box.width) - text_x - 6)/cell_width;
    if (columns < 1) columns = 1;
    if (buffer->row < buffer->top) buffer->top = buffer->row;
    if (buffer->row >= buffer->top + (size_t)rows)
        buffer->top = buffer->row - (size_t)rows + 1;
    EditorLine *cursor_line = &buffer->lines[buffer->row];
    size_t cursor_visual = visual_column(cursor_line, buffer->column);
    if (cursor_visual < buffer->left) buffer->left = cursor_visual;
    if (cursor_visual >= buffer->left + (size_t)columns)
        buffer->left = cursor_visual - (size_t)columns + 1;
    for (int row = 0; row < rows; ++row) {
        size_t index = buffer->top + (size_t)row;
        if (index >= buffer->line_count) break;
        int y = body_top + row*EDITOR_ROW_HEIGHT;
        if (visual_mode(editor)) {
            EditorRange range=selected_range(editor,buffer);
            bool selected=index>=range.first.row &&
                (range.linewise ? index<range.end.row : index<=range.end.row);
            if (selected) {
                size_t first=index==range.first.row ? range.first.column : 0;
                size_t end=index==range.end.row ? range.end.column : buffer->lines[index].length;
                size_t a=range.linewise ? buffer->left : visual_column(&buffer->lines[index],first);
                size_t b=range.linewise ? buffer->left+(size_t)columns :
                    visual_column(&buffer->lines[index],end)+(index<range.end.row ? 1 : 0);
                if (a<buffer->left) a=buffer->left;
                if (b>buffer->left+(size_t)columns) b=buffer->left+(size_t)columns;
                if (b>a) DrawRectangle(text_x+(int)(a-buffer->left)*cell_width,y,
                    (int)(b-a)*cell_width,EDITOR_ROW_HEIGHT,(Color){75,59,110,255});
            }
        }
        char number[32];
        snprintf(number, sizeof(number), "%*zu", (int)digits, index + 1);
        ui_text_draw(number, (int)box.x + 6, y + 2, EDITOR_FONT_SIZE,
                     index == buffer->row ? (Color){172, 143, 255, 255} :
                                            (Color){91, 105, 119, 255});
        char visible[EDITOR_DRAW_CAPACITY];
        (void)display_slice(&buffer->lines[index], buffer->left,
                            (size_t)columns, visible);
        ui_text_draw(visible, text_x, y + 2, EDITOR_FONT_SIZE,
                     (Color){211, 222, 231, 255});
    }
    if (focused && (editor->mode == EDITOR_NORMAL || editor->mode == EDITOR_INSERT || visual_mode(editor))) {
        int screen_row = (int)(buffer->row - buffer->top);
        int cursor_x = text_x + (int)(cursor_visual - buffer->left)*cell_width;
        int cursor_y = body_top + screen_row*EDITOR_ROW_HEIGHT + 2;
        if (editor->mode != EDITOR_INSERT) {
            DrawRectangle(cursor_x, cursor_y, cell_width,
                          EDITOR_FONT_SIZE + 2, (Color){172, 143, 255, 95});
            DrawRectangleLines(cursor_x, cursor_y, cell_width,
                               EDITOR_FONT_SIZE + 2,
                               (Color){205, 188, 255, 255});
        } else if (((int)(GetTime()*2.0))%2 == 0) {
            DrawRectangle(cursor_x, cursor_y, 2, EDITOR_FONT_SIZE + 2,
                          (Color){205, 188, 255, 255});
        }
    }
}

static void draw_code_info(WorkspaceEditor *editor,Rectangle box,int cell_width)
{
    EditorBuffer *info=editor->code_info;
    if (!info || box.width<100 || box.height<100) return;
    int rows=((int)box.height-2*EDITOR_HEADER_HEIGHT-24)/EDITOR_ROW_HEIGHT-2;
    if (rows>20) rows=20;
    if (rows>(int)info->line_count) rows=(int)info->line_count;
    if (rows<1) return;
    if (info->top+(size_t)rows>info->line_count)
        info->top=info->line_count>(size_t)rows ? info->line_count-(size_t)rows : 0;
    int width=(int)box.width-24;
    if (width>880) width=880;
    int height=(rows+2)*EDITOR_ROW_HEIGHT+12;
    int x=(int)box.x+((int)box.width-width)/2;
    int y=(int)box.y+EDITOR_HEADER_HEIGHT+10;
    DrawRectangle(x,y,width,height,(Color){29,33,44,255});
    DrawRectangleLines(x,y,width,height,(Color){172,143,255,255});
    ui_text_draw("Symbol information",x+8,y+5,EDITOR_FONT_SIZE,(Color){205,188,255,255});
    int columns=(width-16)/cell_width;
    if (columns<1) columns=1;
    for (int row=0;row<rows;++row) {
        char visible[EDITOR_DRAW_CAPACITY];
        display_slice(&info->lines[info->top+(size_t)row],info->left,(size_t)columns,visible);
        ui_text_draw(visible,x+8,y+(row+1)*EDITOR_ROW_HEIGHT+5,EDITOR_FONT_SIZE,(Color){211,222,231,255});
    }
    ui_text_draw("Esc close | Arrows scroll",x+8,y+(rows+1)*EDITOR_ROW_HEIGHT+5,16,(Color){172,143,255,255});
}

static const char *mode_name(EditorMode mode)
{
    switch (mode) {
    case EDITOR_VISUAL: return "-- VISUAL --";
    case EDITOR_VISUAL_LINE: return "-- VISUAL LINE --";
    case EDITOR_INSERT: return "-- INSERT --";
    case EDITOR_COMMAND: return "COMMAND";
    case EDITOR_BROWSER: return "EXPLORER";
    default: return "NORMAL";
    }
}

void workspace_editor_draw(WorkspaceEditor *editor, Rectangle box, int pane,
                           bool focused, bool zoomed)
{
    if (!editor || box.width < 2 || box.height < 2) return;
    int cell_width = ui_text_width("M", EDITOR_FONT_SIZE);
    if (cell_width < 1) cell_width = 1;
    Color accent = focused ? (Color){172, 143, 255, 255} :
                             (Color){79, 89, 99, 255};
    DrawRectangleRec(box, (Color){30, 30, 30, 255});
    BeginScissorMode((int)box.x, (int)box.y, (int)box.width, (int)box.height);
    DrawRectangle((int)box.x, (int)box.y, (int)box.width,
                  EDITOR_HEADER_HEIGHT, (Color){34, 42, 53, 255});
    const EditorBuffer *buffer = current_buffer_const(editor);
    const char *name = editor->mode == EDITOR_BROWSER ? editor->cwd :
        buffer && buffer->named ? path_name(buffer->path) : "[No Name]";
    char title[EDITOR_PATH_CAPACITY + 96];
    snprintf(title, sizeof(title), "%d: mc-nvim %s%s%s%s%s", pane, name,
             buffer && buffer->dirty ? " [+]" : "",
             buffer && buffer->readonly ? " [RO]" : "",
             zoomed ? " [zoom]" : "", focused ? " *" : "");
    ui_text_draw(title, (int)box.x + 8, (int)box.y + 5,
                 EDITOR_HEADER_FONT_SIZE, accent);
    int status_y = (int)(box.y + box.height) - EDITOR_STATUS_HEIGHT;
    int available_rows = (status_y - (int)box.y - EDITOR_HEADER_HEIGHT)/
                         EDITOR_ROW_HEIGHT;
    int message_rows = visible_message_rows(editor, available_rows);
    int message_top = status_y - message_rows*EDITOR_ROW_HEIGHT;
    int body_top = (int)box.y + EDITOR_HEADER_HEIGHT + 3;
    int body_bottom = message_top - 2;
    if (editor->mode == EDITOR_BROWSER)
        draw_browser(editor, box, body_top, body_bottom, cell_width);
    else
        draw_buffer(editor, box, body_top, body_bottom, cell_width, focused);
    draw_messages(editor, box, status_y, message_rows);
    DrawRectangle((int)box.x, status_y, (int)box.width,
                  EDITOR_STATUS_HEIGHT, (Color){44, 38, 63, 255});
    if (editor->mode == EDITOR_COMMAND) {
        char command[EDITOR_COMMAND_CAPACITY + 2];
        snprintf(command, sizeof(command), ":%s", editor->command);
        char prefix[EDITOR_COMMAND_CAPACITY + 2];
        size_t count = editor->command_cursor;
        if (count > sizeof(prefix) - 2) count = sizeof(prefix) - 2;
        prefix[0] = ':';
        memcpy(prefix + 1, editor->command, count);
        prefix[count + 1] = '\0';
        int command_x = (int)box.x + 7;
        int cursor_width = ui_text_width(prefix, EDITOR_FONT_SIZE);
        int right = (int)(box.x + box.width) - 7;
        if (command_x + cursor_width > right)
            command_x -= command_x + cursor_width - right;
        ui_text_draw(command, command_x, status_y + 3,
                     EDITOR_FONT_SIZE, RAYWHITE);
        if (focused && ((int)(GetTime()*2.0))%2 == 0) {
            int x = command_x + cursor_width;
            DrawRectangle(x, status_y + 3, 2, EDITOR_FONT_SIZE + 1, RAYWHITE);
        }
    } else {
        ui_text_draw(mode_name(editor->mode), (int)box.x + 7,
                     status_y + 3, EDITOR_FONT_SIZE, RAYWHITE);
        if (buffer && editor->mode != EDITOR_BROWSER) {
            char location[64];
            snprintf(location, sizeof(location), "%zu,%zu",
                     buffer->row + 1,
                     text_column(&buffer->lines[buffer->row], buffer->column) + 1);
            int width = ui_text_width(location, EDITOR_FONT_SIZE);
            ui_text_draw(location, (int)(box.x + box.width) - width - 7,
                         status_y + 3, EDITOR_FONT_SIZE, RAYWHITE);
        }
    }
    draw_code_info(editor,box,cell_width);
    EndScissorMode();
    DrawRectangleLinesEx(box, focused ? 2 : 1, accent);
}
