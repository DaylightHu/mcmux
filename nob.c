#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200909L
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#define NOB_UNSTRIP_PREFIX
#define NOB_IMPLEMENTATION
#include "nob.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAYLIB_DIR "third_party/raylib"
#define RAYLIB_SRC RAYLIB_DIR "/src"
#define RAYLIB_CONFIG "raylib_config.h"

static const char *common_sources[] = {
    "src/mcmux.c",
    "src/world.c",
    "src/worldgen.c",
    "src/storage.c",
    "src/chunk.c",
    "src/posmap.c",
    "src/queue.c",
    "src/scheduler.c",
    "src/sim.c",
    "src/sim_executor.c",
    "src/redstone_plan.c",
    "src/redstone_queue.c",
    "src/redstone_eval.c",
    "src/piston_motion.c",
    "src/block_updates.c",
    "src/wire_solver.c",
    "src/rail_solver.c",
    "src/power.c",
    "src/blocks/signal.c",
    "src/session.c",
    "src/runtime_clock.c",
    "src/region_edit.c",
    "src/structure.c",
    "src/item.c",
    "src/container.c",
    "src/save_file.c",
    "src/blocks/registry.c",
    "src/blocks/iron.c",
    "src/blocks/glass.c", 
    "src/blocks/redstone_block.c",
    "src/blocks/wire.c",
    "src/blocks/torch.c",
    "src/blocks/repeater.c",
    "src/blocks/comparator.c",
    "src/blocks/rail.c",
    "src/blocks/observer.c",
    "src/blocks/piston.c",
    "src/blocks/barrel.c",
    "src/blocks/slime.c",
    "src/blocks/note_block.c",
    "src/blocks/honey.c",
    "src/blocks/target.c",
    "src/blocks/hopper.c",
    "src/blocks/dropper.c",
    "src/blocks/lamp.c",
    "src/blocks/lever.c",
};

static const char *gui_sources[] = {
    "src/gui_main.c",
    "src/command_completion.c",
    "src/edit_command.c",
    "src/plot_command.c",
    "src/tick_command.c",
    "src/frontend/gui.c",
    "src/frontend/gui_view.c",
    "src/frontend/ui_font.c",
    "src/frontend/workspace_layout.c",
    "src/frontend/workspace_editor.c",
    "src/frontend/workspace_code.c",
    "src/frontend/workspace_output.c",
    "src/frontend/world_session_manager.c",
    "src/frontend/block_renderer.c",
    "src/frontend/world_renderer.c",
    "src/frontend/voxel_pick.c",
    "src/frontend/flexible_placement.c",
    "src/frontend/block_shape.c",
    "src/frontend/inventory.c",
    "src/frontend/hotbar.c",
};

static const char *raylib_sources[] = {
    RAYLIB_SRC "/rcore.c",
    RAYLIB_SRC "/rshapes.c",
    RAYLIB_SRC "/rtextures.c",
    RAYLIB_SRC "/rtext.c",
    RAYLIB_SRC "/rmodels.c",
    RAYLIB_SRC "/rglfw.c",
};

typedef Nob_File_Paths Files;

static char *string_dup_n(const char *text, size_t count)
{
    char *result = malloc(count + 1);
    if (!result) {
        nob_log(NOB_ERROR, "out of memory");
        exit(1);
    }
    memcpy(result, text, count);
    result[count] = '\0';
    return result;
}

static char *string_dup(const char *text)
{
    return string_dup_n(text, strlen(text));
}

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool ends_with(const char *text, const char *suffix)
{
    size_t text_count = strlen(text);
    size_t suffix_count = strlen(suffix);
    return text_count >= suffix_count &&
           strcmp(text + text_count - suffix_count, suffix) == 0;
}

static bool files_contains(const Files *files, const char *path)
{
    for (size_t index = 0; index < files->count; ++index) {
        if (strcmp(files->items[index], path) == 0) return true;
    }
    return false;
}

static void files_add(Files *files, const char *path)
{
    if (!files_contains(files, path)) nob_da_append(files, string_dup(path));
}

static void files_append(Files *destination, const Files *source)
{
    for (size_t index = 0; index < source->count; ++index) {
        files_add(destination, source->items[index]);
    }
}

static int compare_paths(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static void files_sort(Files *files)
{
    qsort(files->items, files->count, sizeof(*files->items), compare_paths);
}

static void files_free(Files *files)
{
    for (size_t index = 0; index < files->count; ++index) {
        free((void *)files->items[index]);
    }
    free(files->items);
    *files = (Files){0};
}

static char *path_join(const char *left, const char *right)
{
    size_t left_count = strlen(left);
    size_t right_count = strlen(right);
    char *result = malloc(left_count + right_count + 2);
    if (!result) {
        nob_log(NOB_ERROR, "out of memory");
        exit(1);
    }
    memcpy(result, left, left_count);
    result[left_count] = '/';
    memcpy(result + left_count + 1, right, right_count + 1);
    return result;
}

static bool remove_tree_impl(const char *path)
{
    if (!nob_file_exists(path)) return true;

    Nob_File_Type type = nob_get_file_type(path);
    if (type != NOB_FILE_DIRECTORY) return nob_delete_file(path);

    Nob_Dir_Entry directory = {0};
    Files children = {0};
    if (!nob_dir_entry_open(path, &directory)) return false;

    while (nob_dir_entry_next(&directory)) {
        if (strcmp(directory.name, ".") == 0 ||
            strcmp(directory.name, "..") == 0) continue;
        char *child = path_join(path, directory.name);
        nob_da_append(&children, child);
    }
    bool read_ok = !directory.error;
    nob_dir_entry_close(directory);
    if (!read_ok) {
        files_free(&children);
        return false;
    }

    for (size_t index = 0; index < children.count; ++index) {
        if (!remove_tree_impl(children.items[index])) {
            files_free(&children);
            return false;
        }
    }
    files_free(&children);
    return nob_delete_file(path);
}

static bool remove_tree(const char *path)
{
    Nob_Log_Level previous_level = nob_minimal_log_level;
    nob_minimal_log_level = NOB_WARNING;
    bool result = remove_tree_impl(path);
    nob_minimal_log_level = previous_level;
    return result;
}

static const char *compiler(void)
{
    const char *configured = getenv("CC");
    if (configured && configured[0]) return configured;
#if defined(_WIN32)
    return "clang";
#else
    return "cc";
#endif
}

static bool supported_platform(void)
{
#if defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    nob_log(NOB_ERROR, "Machinecraft currently supports macOS and Windows builds");
    return false;
#endif
}

static const char *platform_name(void)
{
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "unsupported";
#endif
}

static const char *platform_build_dir(void)
{
    return nob_temp_sprintf("build/%s", platform_name());
}

static const char *executable_suffix(void)
{
#if defined(_WIN32)
    return ".exe";
#else
    return "";
#endif
}

static bool ensure_build_directories(void)
{
    const char *platform = platform_build_dir();
    const char *objects = nob_temp_sprintf("%s/obj", platform);
    return nob_mkdir_if_not_exists("build") &&
           nob_mkdir_if_not_exists(platform) &&
           nob_mkdir_if_not_exists(objects) &&
           nob_mkdir_if_not_exists("build/vendor");
}

static char *normalize_path(const char *path)
{
    char *copy = string_dup(path);
    size_t count = strlen(copy);
    for (size_t index = 0; index < count; ++index) {
        if (copy[index] == '\\') copy[index] = '/';
    }

    char **segments = calloc(count + 1, sizeof(*segments));
    if (!segments) {
        nob_log(NOB_ERROR, "out of memory");
        exit(1);
    }

    size_t segment_count = 0;
    char *cursor = copy;
    while (*cursor) {
        while (*cursor == '/') ++cursor;
        if (!*cursor) break;
        char *segment = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        if (*cursor) *cursor++ = '\0';

        if (strcmp(segment, ".") == 0) continue;
        if (strcmp(segment, "..") == 0) {
            if (segment_count > 0) --segment_count;
            continue;
        }
        segments[segment_count++] = segment;
    }

    char *result = malloc(count + 1);
    if (!result) {
        nob_log(NOB_ERROR, "out of memory");
        exit(1);
    }
    size_t output_count = 0;
    for (size_t index = 0; index < segment_count; ++index) {
        if (index > 0) result[output_count++] = '/';
        size_t segment_size = strlen(segments[index]);
        memcpy(result + output_count, segments[index], segment_size);
        output_count += segment_size;
    }
    result[output_count] = '\0';

    free(segments);
    free(copy);
    return result;
}

static bool parse_dependency_file(const char *path, Files *files)
{
    Nob_String_Builder contents = {0};
    if (!nob_read_entire_file(path, &contents)) return false;

    size_t index = 0;
    while (index < contents.count) {
        while (index < contents.count && isspace((unsigned char)contents.items[index])) ++index;
        size_t start = index;
        while (index < contents.count && !isspace((unsigned char)contents.items[index])) ++index;
        if (start == index) continue;

        char *token = string_dup_n(contents.items + start, index - start);
        if (strcmp(token, "\\") != 0 && token[strlen(token) - 1] != ':') {
            char *normalized = normalize_path(token);
            if (starts_with(normalized, RAYLIB_DIR "/")) files_add(files, normalized);
            free(normalized);
        }
        free(token);
    }

    free(contents.items);
    return true;
}

static void append_dependency_scan_flags(Nob_Cmd *command, bool macos)
{
    nob_cmd_append(command,
        "-MM", "-MG", "-nostdinc", "-x", "c",
        "-U__linux__", "-U__FreeBSD__", "-U__OpenBSD__",
        "-U__NetBSD__", "-U__DragonFly__");

    if (macos) {
        nob_cmd_append(command,
            "-U_WIN32", "-UWIN32", "-U__WIN32__",
            "-U__MINGW32__", "-U__MINGW64__", "-U__CYGWIN__",
            "-D__APPLE__=1", "-D__MACH__=1");
    } else {
        nob_cmd_append(command,
            "-U__APPLE__", "-U__MACH__", "-U__CYGWIN__", "-D_WIN32=1");
    }

    nob_cmd_append(command,
        "-DPLATFORM_DESKTOP_GLFW",
        "-DGRAPHICS_API_OPENGL_33",
        "-include", RAYLIB_CONFIG,
        "-I" RAYLIB_SRC,
        "-I" RAYLIB_SRC "/external/glfw/include");
}

static bool verify_raylib_version(void)
{
    Nob_String_Builder header = {0};
    if (!nob_read_entire_file(RAYLIB_SRC "/raylib.h", &header)) return false;
    nob_sb_append_null(&header);

    bool valid = strstr(header.items, "#define RAYLIB_VERSION_MAJOR 6") &&
                 strstr(header.items, "#define RAYLIB_VERSION_MINOR 0") &&
                 strstr(header.items, "#define RAYLIB_VERSION_PATCH 0");
    free(header.items);

    if (!valid) {
        nob_log(NOB_ERROR, "expected the vendored raylib version to be exactly 6.0.0");
    }
    return valid;
}

static bool write_vendor_manifest(const Files *files)
{
    Nob_String_Builder manifest = {0};
    nob_sb_append_cstr(&manifest, "# Generated by ./nob vendor-list; do not commit this build file.\n");
    for (size_t index = 0; index < files->count; ++index) {
        nob_sb_append_cstr(&manifest, files->items[index]);
        nob_sb_append_cstr(&manifest, "\n");
    }
    bool result = nob_write_entire_file("build/vendor/required.txt",
                                        manifest.items, manifest.count);
    free(manifest.items);
    return result;
}

static bool collect_vendor_files(Files *files)
{
    if (!nob_mkdir_if_not_exists("build") ||
        !nob_mkdir_if_not_exists("build/vendor")) return false;
    if (nob_file_exists(RAYLIB_DIR "/.git")) {
        nob_log(NOB_ERROR,
            RAYLIB_DIR "/.git makes raylib a nested repository; move or remove that metadata first");
        return false;
    }
    if (!verify_raylib_version()) return false;

    static const struct {
        bool macos;
        const char *output;
    } scans[] = {
        {true,  "build/vendor/deps-macos.d"},
        {false, "build/vendor/deps-windows.d"},
    };

    Nob_Cmd command = {0};
    for (size_t scan = 0; scan < NOB_ARRAY_LEN(scans); ++scan) {
        nob_cmd_append(&command, compiler());
        append_dependency_scan_flags(&command, scans[scan].macos);
        for (size_t index = 0; index < NOB_ARRAY_LEN(raylib_sources); ++index) {
            nob_cmd_append(&command, raylib_sources[index]);
        }
        if (!nob_cmd_run(&command, .stdout_path = scans[scan].output)) return false;
        if (!parse_dependency_file(scans[scan].output, files)) return false;
    }

    files_add(files, RAYLIB_DIR "/LICENSE");
    files_add(files, RAYLIB_SRC "/external/glfw/LICENSE.md");
    files_sort(files);

    for (size_t index = 0; index < files->count; ++index) {
        if (!nob_file_exists(files->items[index])) {
            nob_log(NOB_ERROR, "required raylib file is missing: %s", files->items[index]);
            return false;
        }
    }

    return write_vendor_manifest(files);
}

static bool collect_header(Nob_Walk_Entry entry)
{
    Files *headers = entry.data;
    if (entry.type == NOB_FILE_REGULAR && ends_with(entry.path, ".h")) {
        files_add(headers, entry.path);
    }
    return true;
}

static bool collect_project_headers(Files *headers)
{
    if (!nob_walk_dir("include", collect_header, .data = headers)) return false;
    if (!nob_walk_dir("src", collect_header, .data = headers)) return false;
    files_add(headers, "nob.c");
    files_add(headers, RAYLIB_CONFIG);
    files_add(headers, RAYLIB_SRC "/raylib.h");
    files_add(headers, RAYLIB_SRC "/raymath.h");
    files_add(headers, RAYLIB_SRC "/rlgl.h");
    return true;
}

static const char *object_path(const char *source)
{
    const char *name = strrchr(source, '/');
    name = name ? name + 1 : source;
    size_t name_count = strlen(name);
    if (name_count >= 2 && strcmp(name + name_count - 2, ".c") == 0) name_count -= 2;
    return nob_temp_sprintf("%s/obj/%.*s.o", platform_build_dir(),
                            (int)name_count, name);
}

static bool needs_rebuild(const char *output, const char *source,
                          const Files *dependencies, bool *needed)
{
    Files inputs = {0};
    nob_da_append(&inputs, source);
    for (size_t index = 0; index < dependencies->count; ++index) {
        nob_da_append(&inputs, dependencies->items[index]);
    }
    int state = nob_needs_rebuild(output, inputs.items, inputs.count);
    free(inputs.items);
    if (state < 0) return false;
    *needed = state != 0;
    return true;
}

static bool compile_project_source(const char *source, const Files *headers,
                                   Files *objects)
{
    const char *output = object_path(source);
    bool rebuild = false;
    if (!needs_rebuild(output, source, headers, &rebuild)) return false;

    if (rebuild) {
        Nob_Cmd command = {0};
        nob_cmd_append(&command,
            compiler(), "-std=c11", "-O2", "-Wall", "-Wextra",
            "-Iinclude", "-Isrc", "-I" RAYLIB_SRC,
            "-c", source, "-o", output);
        if (!nob_cmd_run(&command)) return false;
    }

    files_add(objects, output);
    return true;
}

static bool compile_raylib_source(const char *source, const Files *dependencies,
                                  Files *objects)
{
    const char *output = object_path(source);
    bool rebuild = false;
    if (!needs_rebuild(output, source, dependencies, &rebuild)) return false;

    if (rebuild) {
        Nob_Cmd command = {0};
        nob_cmd_append(&command,
            compiler(), "-std=c99", "-O2", "-fno-strict-aliasing",
            "-DPLATFORM_DESKTOP_GLFW", "-DGRAPHICS_API_OPENGL_33",
            "-include", RAYLIB_CONFIG,
            "-I" RAYLIB_SRC,
            "-I" RAYLIB_SRC "/external/glfw/include");
#if defined(_WIN32)
        nob_cmd_append(&command, "-DUNICODE");
#endif
#if defined(__APPLE__)
        if (ends_with(source, "/rglfw.c")) nob_cmd_append(&command, "-x", "objective-c");
#endif
        nob_cmd_append(&command, "-c", source, "-o", output);
        if (!nob_cmd_run(&command)) return false;
    }

    files_add(objects, output);
    return true;
}

static bool link_target(const char *output, const Files *objects)
{
    int state = nob_needs_rebuild(output, objects->items, objects->count);
    if (state < 0) return false;
    if (state == 0) return true;

    Nob_Cmd command = {0};
    nob_cmd_append(&command, compiler(), "-o", output);
    for (size_t index = 0; index < objects->count; ++index) {
        nob_cmd_append(&command, objects->items[index]);
    }

#if defined(__APPLE__)
    nob_cmd_append(&command,
        "-framework", "OpenGL",
        "-framework", "Cocoa",
        "-framework", "IOKit",
        "-framework", "CoreAudio",
        "-framework", "CoreVideo");
#elif defined(_WIN32)
    nob_cmd_append(&command,
        "-lopengl32", "-lgdi32", "-lwinmm", "-lshell32");
#endif

    return nob_cmd_run(&command);
}

static bool compile_sources(const char **sources, size_t source_count,
                            const Files *headers, Files *objects)
{
    for (size_t index = 0; index < source_count; ++index) {
        if (!compile_project_source(sources[index], headers, objects)) return false;
    }
    return true;
}

static bool build_machinecraft(void)
{
    if (!supported_platform() || !ensure_build_directories()) return false;

    bool result = false;
    Files headers = {0};
    Files common_objects = {0};
    Files gui_objects = {0};
    Files raylib_objects = {0};
    Files vendor_files = {0};
    Files link_objects = {0};

    if (!collect_project_headers(&headers)) goto cleanup;
    if (!compile_sources(common_sources, NOB_ARRAY_LEN(common_sources),
                         &headers, &common_objects)) goto cleanup;

    if (!collect_vendor_files(&vendor_files)) goto cleanup;
    files_add(&vendor_files, RAYLIB_CONFIG);
    if (!compile_sources(gui_sources, NOB_ARRAY_LEN(gui_sources),
                         &headers, &gui_objects)) goto cleanup;
    for (size_t index = 0; index < NOB_ARRAY_LEN(raylib_sources); ++index) {
        if (!compile_raylib_source(raylib_sources[index], &vendor_files,
                                   &raylib_objects)) goto cleanup;
    }
    files_append(&link_objects, &common_objects);
    files_append(&link_objects, &gui_objects);
    files_append(&link_objects, &raylib_objects);
    const char *output = nob_temp_sprintf("%s/machinecraft_gui%s",
                                          platform_build_dir(), executable_suffix());
    if (!link_target(output, &link_objects)) goto cleanup;

    result = true;

cleanup:
    files_free(&link_objects);
    files_free(&vendor_files);
    files_free(&raylib_objects);
    files_free(&gui_objects);
    files_free(&common_objects);
    files_free(&headers);
    return result;
}

static bool parse_nul_paths(const char *path, Files *files)
{
    Nob_String_Builder contents = {0};
    if (!nob_read_entire_file(path, &contents)) return false;

    size_t start = 0;
    for (size_t index = 0; index <= contents.count; ++index) {
        if (index == contents.count || contents.items[index] == '\0') {
            if (index > start) {
                char *item = string_dup_n(contents.items + start, index - start);
                files_add(files, item);
                free(item);
            }
            start = index + 1;
        }
    }
    free(contents.items);
    files_sort(files);
    return true;
}

static bool verify_tracked_vendor(const Files *required)
{
    Nob_Cmd command = {0};
    nob_cmd_append(&command, "git", "ls-files", "-z", "--", RAYLIB_DIR);
    if (!nob_cmd_run(&command, .stdout_path = "build/vendor/tracked.z")) return false;

    Files tracked = {0};
    if (!parse_nul_paths("build/vendor/tracked.z", &tracked)) return false;

    bool valid = true;
    for (size_t index = 0; index < required->count; ++index) {
        if (!files_contains(&tracked, required->items[index])) {
            nob_log(NOB_ERROR, "required raylib file is not tracked: %s",
                    required->items[index]);
            valid = false;
        }
    }
    for (size_t index = 0; index < tracked.count; ++index) {
        if (!files_contains(required, tracked.items[index])) {
            nob_log(NOB_ERROR, "unneeded raylib file is tracked: %s",
                    tracked.items[index]);
            valid = false;
        }
    }

    files_free(&tracked);
    return valid;
}

static bool sync_vendor_index(const Files *required)
{
    Nob_Cmd command = {0};
    nob_cmd_append(&command,
        "git", "rm", "-r", "--cached", "-f", "--ignore-unmatch", "--", RAYLIB_DIR);
    if (!nob_cmd_run(&command, .stdout_path = "build/vendor/git-rm.log")) return false;

    nob_cmd_append(&command, "git", "add", "-f", "--");
    for (size_t index = 0; index < required->count; ++index) {
        nob_cmd_append(&command, required->items[index]);
    }
    if (!nob_cmd_run(&command)) return false;

    return verify_tracked_vendor(required);
}

static bool verify_recipe_is_tracked(void)
{
    Nob_Cmd command = {0};
    nob_cmd_append(&command,
        "git", "ls-files", "--error-unmatch", "--",
        "nob.c", "nob.h", RAYLIB_CONFIG, ".gitignore");
    if (!nob_cmd_run(&command)) {
        nob_log(NOB_ERROR,
            "stage nob.c, nob.h, %s and .gitignore before running precommit",
            RAYLIB_CONFIG);
        return false;
    }
    return true;
}

static bool verify_index_build(void)
{
    const char *export_dir = "build/precommit-tree";
    if (!remove_tree(export_dir)) return false;
    if (!nob_mkdir_if_not_exists(export_dir)) return false;

    Nob_Cmd command = {0};
    nob_cmd_append(&command,
        "git", "checkout-index", "--all", "--force",
        "--prefix=build/precommit-tree/");
    if (!nob_cmd_run(&command)) return false;

    char *original_directory = string_dup(nob_get_current_dir_temp());
    if (!nob_set_current_dir(export_dir)) {
        free(original_directory);
        return false;
    }

    const char *check_binary = nob_temp_sprintf("nob-check%s", executable_suffix());
    nob_cmd_append(&command,
        compiler(), "-std=c11", "-O2", "nob.c", "-o", check_binary);
    bool compile_ok = nob_cmd_run(&command);
    if (!compile_ok) {
        bool restore_ok = nob_set_current_dir(original_directory);
        free(original_directory);
        if (!restore_ok) nob_log(NOB_ERROR, "could not restore repository directory");
        return false;
    }

#if defined(_WIN32)
    nob_cmd_append(&command, ".\\nob-check.exe", "build");
#else
    nob_cmd_append(&command, "./nob-check", "build");
#endif
    bool build_ok = nob_cmd_run(&command);
    bool restore_ok = nob_set_current_dir(original_directory);
    free(original_directory);
    if (!build_ok || !restore_ok) {
        nob_log(NOB_ERROR, "index-only build left at %s for inspection", export_dir);
        return false;
    }

    return remove_tree(export_dir);
}

static bool precommit(void)
{
    Files required = {0};
    bool result = false;

    if (!collect_vendor_files(&required)) goto cleanup;
    if (!verify_recipe_is_tracked()) goto cleanup;
    if (!sync_vendor_index(&required)) goto cleanup;

    if (!verify_index_build()) goto cleanup;

    nob_log(NOB_INFO,
        "precommit passed: %zu raylib 6.0 files are staged and the index-only tree builds",
        required.count);
    nob_log(NOB_INFO, "review git status, then create your commit");
    result = true;

cleanup:
    files_free(&required);
    return result;
}

/* These tools need only the simulation core, including on non-GUI platforms. */
static bool build_core_library(void)
{
    if (!nob_mkdir_if_not_exists("build") ||
        !nob_mkdir_if_not_exists("build/lib") ||
        !nob_mkdir_if_not_exists("build/lib/obj")) return false;
    bool ok=false;
    Files headers={0},objects={0};
    if (!nob_walk_dir("include",collect_header,.data=&headers) ||
        !nob_walk_dir("src",collect_header,.data=&headers)) goto done;
    files_add(&headers,"nob.c");
    for (size_t i=0;i<NOB_ARRAY_LEN(common_sources);++i) {
        const char *source=common_sources[i];
        char *name=string_dup(source);
        for (char *p=name;*p;++p) if (*p=='/' || *p=='\\') *p='_';
        const char *object=nob_temp_sprintf("build/lib/obj/%.*s.o",
            (int)strlen(name)-2,name);
        free(name);
        bool rebuild;
        if (!needs_rebuild(object,source,&headers,&rebuild)) goto done;
        if (rebuild) {
            Nob_Cmd cc={0};
            nob_cmd_append(&cc,compiler(),"-std=c11","-O2","-Wall","-Wextra",
                "-Iinclude","-Isrc","-c",source,"-o",object);
            if (!nob_cmd_run(&cc)) goto done;
        }
        files_add(&objects,object);
    }
    int rebuild=nob_needs_rebuild("build/libmcmux.a",objects.items,objects.count);
    if (rebuild<0) goto done;
    if (rebuild) {
        const char *archive="build/libmcmux.a.tmp";
        if (nob_file_exists(archive) && !nob_delete_file(archive)) goto done;
        const char *ar=getenv("AR");
#ifdef _WIN32
        if (!ar || !*ar) ar="llvm-ar";
#else
        if (!ar || !*ar) ar="ar";
#endif
        Nob_Cmd command={0};
        nob_cmd_append(&command,ar,"rcs",archive);
        for (size_t i=0;i<objects.count;++i) nob_cmd_append(&command,objects.items[i]);
        if (!nob_cmd_run(&command)) goto done;
        if (!nob_rename(archive,"build/libmcmux.a")) goto done;
    }
    ok=true;
done:
    files_free(&headers); files_free(&objects);
    return ok;
}

static char *user_source_path(const char *source)
{
#ifdef _WIN32
    char *absolute=_fullpath(NULL,source,0);
#else
    char *absolute=realpath(source,NULL);
#endif
    if (!absolute) {
        nob_log(NOB_ERROR,"cannot resolve source %s: %s",source,strerror(errno));
        return NULL;
    }
#ifdef _WIN32
    for (char *p=absolute; *p; ++p) if (*p=='\\') *p='/';
#endif
    size_t length=strlen(absolute);
    if (length<2 || absolute[length-2]!='.' ||
        tolower((unsigned char)absolute[length-1])!='c' ||
        nob_get_file_type(absolute)!=NOB_FILE_REGULAR) {
        nob_log(NOB_ERROR,"expected a C source file: %s",source);
        free(absolute);
        return NULL;
    }
    return absolute;
}

static bool make_parent_directories(const char *path)
{
    char *directory=string_dup(path);
    bool ok=true;
    for (char *p=directory; *p; ++p) if (*p=='/') {
        *p='\0';
        if (!nob_mkdir_if_not_exists(directory)) { ok=false; break; }
        *p='/';
    }
    free(directory);
    return ok;
}

static bool build_user_program(const char *source,bool run)
{
    char *absolute=user_source_path(source);
    if (!absolute) return false;
    bool ok=false;
    const char *root=nob_get_current_dir_temp();
    if (!root) goto done;
    size_t root_length=strlen(root);
    if (strlen(absolute)<root_length) goto outside_project;
    for (size_t i=0; i<root_length; ++i) {
        unsigned char a=(unsigned char)root[i],b=(unsigned char)absolute[i];
#ifdef _WIN32
        if (a=='\\') a='/';
        a=(unsigned char)tolower(a); b=(unsigned char)tolower(b);
#endif
        if (a!=b) goto outside_project;
    }
    const char *relative=absolute+root_length;
    bool root_separator=root_length && root[root_length-1]=='/';
#ifdef _WIN32
    root_separator=root_separator || (root_length && root[root_length-1]=='\\');
#endif
    if (!root_separator) {
        if (*relative!='/') goto outside_project;
        ++relative;
    }
    if (!*relative) goto outside_project;
    const char *stem=nob_temp_sprintf("build/programs/%.*s",(int)strlen(relative)-2,relative);
    const char *output=nob_temp_sprintf("%s%s",stem,executable_suffix());
    const char *temporary=nob_temp_sprintf("build/program-tmp/%.*s%s",
        (int)strlen(relative)-2,relative,executable_suffix());
    if (!make_parent_directories(output) || !make_parent_directories(temporary) ||
        !build_core_library()) goto done;
    Nob_Cmd command={0};
    nob_cmd_append(&command,compiler(),"-std=c11","-O2","-Wall","-Wextra",
        "-Iinclude","-x","c",absolute,"-x","none","build/libmcmux.a","-o",temporary);
#ifndef _WIN32
    nob_cmd_append(&command,"-lm");
#endif
    if (!nob_cmd_run(&command)) {
        if (nob_file_exists(temporary)) (void)nob_delete_file(temporary);
        goto done;
    }
    if (!nob_rename(temporary,output)) {
        if (nob_file_exists(temporary)) (void)nob_delete_file(temporary);
        goto done;
    }
    nob_log(NOB_INFO,"built %s",output);
    if (run) {
        nob_cmd_append(&command,output);
        if (!nob_cmd_run(&command)) goto done;
    }
    ok=true;
    goto done;
outside_project:
    nob_log(NOB_ERROR,"source must be inside the project directory: %s",source);
done:
    free(absolute);
    return ok;
}

static bool build_headless_tool(const char *source, const char *name, bool run)
{
    if (!nob_mkdir_if_not_exists("build")) return false;
    const char *output=nob_temp_sprintf("build/%s%s",name,executable_suffix());
    Nob_Cmd command={0};
    nob_cmd_append(&command,compiler(),"-std=c11","-O2","-Wall","-Wextra","-Iinclude","-Isrc");
    for (size_t i=0;i<NOB_ARRAY_LEN(common_sources);++i) nob_cmd_append(&command,common_sources[i]);
    nob_cmd_append(&command,source,"-o",output);
#ifndef _WIN32
    nob_cmd_append(&command,"-lm");
#endif
    if (!nob_cmd_run(&command)) return false;
    if (run) {
        nob_cmd_append(&command,output);
        if (!nob_cmd_run(&command)) return false;
    }
    return true;
}

static bool run_redstone_benchmark(int argc, char **argv)
{
    if (!build_headless_tool("benchmarks/redstone_scaling.c",
                             "redstone_scaling", false)) return false;
    Nob_Cmd command = {0};
    nob_cmd_append(&command,
        nob_temp_sprintf("build/redstone_scaling%s", executable_suffix()));
    for (int index = 0; index < argc; ++index)
        nob_cmd_append(&command, argv[index]);
    return nob_cmd_run(&command);
}

static void print_help(const char *program)
{
    printf("Usage: %s [command]\n\n", program);
    printf("Commands:\n");
    printf("  build        Build the GUI (default)\n");
    printf("  gui          Build only the GUI\n");
    printf("  run          Build and run the GUI from the repository root\n");
    printf("  vendor-list  Print the raylib files required by macOS and Windows\n");
    printf("  precommit    Stage the exact raylib set and verify an index-only build\n");
    printf("  test         Build and run the structure persistence tests\n");
    printf("  reader       Build build/inspect_structure, a standalone .mcs reader\n");
    printf("  benchmark    Run the deterministic redstone scaling benchmark\n");
    printf("  lib          Build build/libmcmux.a (no GUI dependencies)\n");
    printf("  build-c FILE Build a user C program linked with the simulation core\n");
    printf("  run-c FILE   Build and run a user C program from the repository root\n");
    printf("  clean        Remove the build directory\n");
    printf("  help         Show this help\n");
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF_PLUS(argc, argv, "nob.h");

    const char *program = nob_shift(argv, argc);
    const char *command = argc > 0 ? nob_shift(argv, argc) : "build";

    if (strcmp(command,"test")==0)
        return build_headless_tool("tests/structure_test.c","structure_test",true) ? 0 : 1;
    if (strcmp(command,"reader")==0)
        return build_headless_tool("tools/inspect_structure.c","inspect_structure",false) ? 0 : 1;
    if (strcmp(command,"benchmark")==0)
        return run_redstone_benchmark(argc, argv) ? 0 : 1;
    if (strcmp(command,"lib")==0) return build_core_library() ? 0 : 1;
    if (strcmp(command,"build-c")==0 || strcmp(command,"run-c")==0) {
        if (argc!=1) {
            nob_log(NOB_ERROR,"usage: %s %s path/to/program.c",program,command);
            return 1;
        }
        return build_user_program(argv[0],strcmp(command,"run-c")==0) ? 0 : 1;
    }
    if (strcmp(command, "build") == 0) {
        return build_machinecraft() ? 0 : 1;
    }
    if (strcmp(command, "gui") == 0) {
        return build_machinecraft() ? 0 : 1;
    }
    if (strcmp(command, "run") == 0) {
        if (!build_machinecraft()) return 1;
        Nob_Cmd run = {0};
        const char *binary = nob_temp_sprintf("%s/machinecraft_gui%s",
                                              platform_build_dir(), executable_suffix());
        nob_cmd_append(&run, binary);
        return nob_cmd_run(&run) ? 0 : 1;
    }
    if (strcmp(command, "vendor-list") == 0) {
        Files required = {0};
        if (!collect_vendor_files(&required)) return 1;
        for (size_t index = 0; index < required.count; ++index) {
            puts(required.items[index]);
        }
        nob_log(NOB_INFO, "%zu files are required", required.count);
        files_free(&required);
        return 0;
    }
    if (strcmp(command, "precommit") == 0) {
        return precommit() ? 0 : 1;
    }
    if (strcmp(command, "clean") == 0) {
        return remove_tree("build") ? 0 : 1;
    }
    if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0 ||
        strcmp(command, "-h") == 0) {
        print_help(program);
        return 0;
    }

    nob_log(NOB_ERROR, "unknown command: %s", command);
    print_help(program);
    return 1;
}
