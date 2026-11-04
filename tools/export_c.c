#include "mcmux.h"

#include <limits.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s INPUT.mcs OUTPUT.c [SYMBOL]\n", argv[0]);
        return 2;
    }
    size_t length = strlen(argv[2]);
    if (length < 3 || strcmp(argv[2] + length - 2, ".c") != 0) {
        fprintf(stderr, "output must end in .c\n");
        return 2;
    }
    StructureResult status;
    Structure *structure = structure_read(argv[1], &status);
    if (!structure) {
        fprintf(stderr, "cannot read structure: %s\n", structure_result_name(status));
        return 1;
    }
    StructureInfo info = structure_info(structure);
    structure_destroy(structure);
    int64_t x = (int64_t)info.origin.x + info.width - 1;
    int64_t y = (int64_t)info.origin.y + info.height - 1;
    int64_t z = (int64_t)info.origin.z + info.length - 1;
    if (x > INT_MAX || y > INT_MAX || z > INT_MAX) return 1;
    World *world = structure_load(argv[1], &status);
    if (!world) {
        fprintf(stderr, "cannot load structure: %s\n", structure_result_name(status));
        return 1;
    }
    FILE *out = fopen(argv[2], "w");
    if (!out) { world_destroy(world); return 1; }
    RsResult result = sim_write_c(world, info.origin, (BlockPos){(int)x,(int)y,(int)z},
                                  out, argc == 4 ? argv[3] : "mcmux_circuit");
    bool ok = result == RS_OK;
    if (fclose(out) != 0) ok = false;
    world_destroy(world);
    if (!ok) {
        (void)remove(argv[2]);
        fprintf(stderr, "C export failed: %s\n",
                rs_result_name(result == RS_OK ? RS_IO_ERROR : result));
        return 1;
    }
    printf("generated %s (model: %s)\n", argv[2], argc == 4 ? argv[3] : "mcmux_circuit");
    return 0;
}
