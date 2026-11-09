#include "mcmux.h"
#include "region_edit_internal.h"
#include "runtime_clock.h"
#include "save_file.h"
#include "world_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

double mcmux_monotonic_seconds(void)
{
    return runtime_clock_now();
}

static RsResult structure_writeback_result(StructureResult result)
{
    switch (result) {
    case STRUCTURE_INVALID_ARGUMENT:
    case STRUCTURE_OUT_OF_BOUNDS:
    case STRUCTURE_TOO_LARGE:
        return RS_INVALID_ARGUMENT;
    case STRUCTURE_OUT_OF_MEMORY:
        return RS_OUT_OF_MEMORY;
    case STRUCTURE_IO_ERROR:
        return RS_IO_ERROR;
    case STRUCTURE_CORRUPT:
        return RS_CORRUPT_DATA;
    case STRUCTURE_UNSUPPORTED:
        return RS_UNSUPPORTED;
    case STRUCTURE_SIMULATION_ERROR:
        return RS_INTERNAL_ERROR;
    case STRUCTURE_OK:
    case STRUCTURE_EXISTS:
    default:
        return RS_INVALID_ARGUMENT;
    }
}

static RsResult session_world_result(GameSessionResult result)
{
    switch (result) {
    case GAME_SESSION_OK:
        return RS_OK;
    case GAME_SESSION_INVALID_ARGUMENT:
    case GAME_SESSION_INVALID_NAME:
    case GAME_SESSION_PATH_TOO_LONG:
        return RS_INVALID_ARGUMENT;
    case GAME_SESSION_OUT_OF_MEMORY:
        return RS_OUT_OF_MEMORY;
    case GAME_SESSION_IO_ERROR:
        return RS_IO_ERROR;
    case GAME_SESSION_CORRUPT_SAVE:
        return RS_CORRUPT_DATA;
    case GAME_SESSION_UNSUPPORTED_SAVE:
        return RS_UNSUPPORTED;
    case GAME_SESSION_SIMULATION_ERROR:
    case GAME_SESSION_TOO_MANY_SAVES:
    default:
        return RS_INTERNAL_ERROR;
    }
}

static void record_file_error(World *world, RsResult result, const char *operation,
                               const char *reason, const char *path)
{
    if (result == RS_OK) return;
    char detail[512];
    snprintf(detail, sizeof(detail), "%s [path: %s]", reason,
             path ? path : "(null)");
    world_record_error(world, result, operation, NULL, detail);
}

World *load_mcs(const char *path, BlockPos anchor)
{
    StructureResult result;
    World *world = structure_load_at(path, &anchor, &result);
    if (!world) {
        fprintf(stderr, "Error loading '%s': %s\n",
                path ? path : "(null)", structure_result_name(result));
        return NULL;
    }
    printf("%s loaded. \n", path);
    return world;
}

static GameSessionResult save_world(World *world, const char *path)
{
    if (!world || !path || !*path)
        return GAME_SESSION_INVALID_ARGUMENT;

    if (sim_flush(world) != RS_OK)
        return GAME_SESSION_SIMULATION_ERROR;

    return save_file_write(world, path);
}

void save_mcw(World *world, const char *path)
{
    if (!world_action_ready(world, "save_mcw", NULL)) return;
    GameSessionResult result = save_world(world, path);
    record_file_error(world, session_world_result(result), "save_mcw",
                      game_session_result_name(result), path);
}

void writeback_mcs(World *world, const char *mcs_path,
                   BlockPos anchor, const char *mcw_path)
{
    if (!world_action_ready(world, "writeback_mcs", NULL)) return;
    if (!mcs_path || !*mcs_path || !mcw_path || !*mcw_path) {
        record_file_error(world, RS_INVALID_ARGUMENT, "writeback_mcs",
                          "invalid argument", !mcs_path || !*mcs_path ? mcs_path : mcw_path);
        return;
    }

    StructureResult structure_result;
    Structure *structure = structure_read(mcs_path, &structure_result);
    if (!structure) {
        record_file_error(world, structure_writeback_result(structure_result),
                          "writeback_mcs", structure_result_name(structure_result), mcs_path);
        return;
    }
    StructureInfo info = structure_info(structure);
    structure_destroy(structure);

    GameSessionResult save_result;
    World *target = save_file_read(mcw_path, &save_result);
    if (!target) {
        record_file_error(world, session_world_result(save_result), "writeback_mcs",
                          game_session_result_name(save_result), mcw_path);
        return;
    }

    RegionEditor *source_editor = region_editor_create(world);
    RegionEditor *target_editor = region_editor_create(target);
    if (!source_editor || !target_editor) {
        region_editor_destroy(source_editor);
        region_editor_destroy(target_editor);
        world_destroy(target);
        world_record_error(world, RS_OUT_OF_MEMORY, "writeback_mcs", NULL,
                            "out of memory");
        return;
    }

    int64_t offset_x = (int64_t)anchor.x - info.anchor.x;
    int64_t offset_y = (int64_t)anchor.y - info.anchor.y;
    int64_t offset_z = (int64_t)anchor.z - info.anchor.z;
    int64_t source_x = (int64_t)info.origin.x + offset_x;
    int64_t source_y = (int64_t)info.origin.y + offset_y;
    int64_t source_z = (int64_t)info.origin.z + offset_z;
    int64_t source_max_x = source_x + info.width - 1;
    int64_t source_max_y = source_y + info.height - 1;
    int64_t source_max_z = source_z + info.length - 1;
    if (source_x < INT_MIN || source_y < INT_MIN || source_z < INT_MIN ||
        source_max_x > INT_MAX || source_max_y > INT_MAX || source_max_z > INT_MAX) {
        region_editor_destroy(source_editor);
        region_editor_destroy(target_editor);
        world_destroy(target);
        world_record_error(world, RS_INVALID_ARGUMENT, "writeback_mcs", NULL,
                            "source coordinates overflow");
        return;
    }
    BlockPos first = {(int)source_x, (int)source_y, (int)source_z};
    BlockPos second = {(int)source_max_x, (int)source_max_y, (int)source_max_z};
    BlockPos target_first = info.origin;
    RsResult result = region_editor_transfer(source_editor, target_editor,
                                             first, second, target_first, false,
                                             NULL);

    region_editor_destroy(source_editor);
    region_editor_destroy(target_editor);
    if (result != RS_OK && result != RS_UNCHANGED) {
        world_destroy(target);
        world_record_error(world, result, "writeback_mcs", NULL, rs_result_name(result));
        return;
    }

    save_result = save_world(target, mcw_path);
    world_destroy(target);
    record_file_error(world, session_world_result(save_result), "writeback_mcs",
                      game_session_result_name(save_result), mcw_path);
}
