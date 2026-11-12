/* Run from the repository root after ./nob lib:
 * cc -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc tools/test_schem_load.c
 *    src/edit_command.c src/command_completion.c build/libmcmux.a -lm
 *    -o build/test_schem_load && ./build/test_schem_load
 */
#include "edit_command.h"
#include "command_completion.h"
#include "rs/sim.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char message[192];

static RsResult command(GameSession *session, const char *text)
{
    EditCommand edit;
    assert(edit_command_parse(text,&edit));
    return edit_command_run(session,&edit,(EditCommandContext){0},
                            message,sizeof(message));
}

static void stack_is(const World *world, BlockPos pos, size_t slot,
                      uint32_t item, uint8_t count, uint8_t limit)
{
    ContainerStack stack;
    assert(world_get_container_slot(world,pos,slot,&stack)==RS_OK);
    assert(stack.item==item && stack.count==count && stack.limit==limit);
}

static Structure *fixture(void)
{
    World *world=world_create((WorldConfig){0});
    assert(world);
    assert(world_expand_plot(world,DIR_EAST,1)==RS_OK);
    assert(world_expand_plot(world,DIR_SOUTH,1)==RS_OK);
    world_place(world,(BlockPos){700,5,700},BLOCK_IRON,DIR_NORTH);
    world_place(world,(BlockPos){700,5,701},BLOCK_IRON,DIR_NORTH);
    world_place(world,(BlockPos){700,6,700},BLOCK_LEVER,DIR_DOWN);
    world_set_lever_powered(world,(BlockPos){700,6,700},true);
    world_place(world,(BlockPos){700,6,701},BLOCK_REDSTONE_COMPARATOR,DIR_EAST);
    assert(world_toggle_comparator_mode(world,(BlockPos){700,6,701})==RS_OK);
    world_place(world,(BlockPos){701,5,700},BLOCK_BARREL,DIR_SOUTH);
    world_place(world,(BlockPos){702,5,700},BLOCK_DROPPER,DIR_UP);
    world_place(world,(BlockPos){701,5,701},BLOCK_HOPPER,DIR_DOWN);
    assert(world_set_container_slot(world,(BlockPos){701,5,700},0,
        (ContainerStack){ITEM_POISONOUS_POTATO,37,64})==RS_OK);
    assert(world_set_container_slot(world,(BlockPos){702,5,700},8,
        (ContainerStack){1234,9,16})==RS_OK);
    assert(world_set_container_slot(world,(BlockPos){701,5,701},4,
        (ContainerStack){4321,1,1})==RS_OK);
    StructureResult result;
    BlockPos anchor={701,6,701};
    Structure *structure=structure_capture(world,(BlockPos){700,5,700},
        (BlockPos){702,6,701},&anchor,&result);
    assert(structure && result==STRUCTURE_OK && world_result(world)==RS_OK);
    world_destroy(world);
    return structure;
}

static void pasted_fixture(const World *world)
{
    assert(world_get(world,(BlockPos){19,11,19}).id==BLOCK_IRON);
    assert(world_get(world,(BlockPos){19,12,19}).data.lever.powered);
    BlockState comparator=world_get(world,(BlockPos){19,12,20});
    assert(comparator.id==BLOCK_REDSTONE_COMPARATOR &&
           comparator.data.comparator.facing==DIR_EAST &&
           comparator.data.comparator.subtract);
    stack_is(world,(BlockPos){20,11,19},0,ITEM_POISONOUS_POTATO,37,64);
    stack_is(world,(BlockPos){21,11,19},8,1234,9,16);
    stack_is(world,(BlockPos){20,11,20},4,4321,1,1);
}

int main(void)
{
    EditCommand edit;
    const char *valid[]={"//schem load fixture", "/schem load fixture.mcs",
                          "schem load fixture"};
    for (size_t i=0;i<sizeof(valid)/sizeof(valid[0]);++i) {
        assert(edit_command_parse(valid[i],&edit));
        assert(edit.kind==EDIT_SCHEM_LOAD && !edit.has_position);
    }
    const char *invalid[]={"//schem", "//schem load", "//schem load a b",
        "//schem load -f fixture", "//schem load fixture 1 2 3",
        "//schem unknown fixture"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        assert(edit_command_parse(invalid[i],&edit));
        assert(edit.kind==EDIT_INVALID);
    }
    assert(!edit_command_parse("///schem load fixture",&edit));
    CommandCompletion completion={0};
    char line[128]="//schem lo";
    assert(command_complete(&completion,line,sizeof(line)));
    assert(!strcmp(line,"//schem load"));
    completion=(CommandCompletion){0};
    strcpy(line,"//schem ");
    assert(command_complete(&completion,line,sizeof(line)));
    assert(!strcmp(line,"//schem save"));
    assert(command_complete(&completion,line,sizeof(line)));
    assert(!strcmp(line,"//schem load"));

    GameSessionResult created;
    GameSession *session=game_session_create("build/test-schem-load/world.mcw",
        (WorldConfig){0},&created);
    assert(session && created==GAME_SESSION_OK);
    World *world=game_session_world(session);
    RegionEditor *editor=game_session_editor(session);
    assert(sim_set_frozen(world,true)==RS_OK);
    assert(sim_set_updates_enabled(world,false)==RS_OK);
    world_place(world,(BlockPos){5,5,5},BLOCK_REDSTONE_BLOCK,DIR_NORTH);
    world_place(world,(BlockPos){19,11,19},BLOCK_GLASS,DIR_NORTH);
    world_place(world,(BlockPos){20,11,19},BLOCK_BARREL,DIR_NORTH);
    assert(world_set_container_slot(world,(BlockPos){20,11,19},1,
        (ContainerStack){99,1,1})==RS_OK);
    world_place(world,(BlockPos){21,11,20},BLOCK_REDSTONE_BLOCK,DIR_NORTH);
    assert(command(session,"//pos1 5 5 5")==RS_OK);
    assert(command(session,"//pos2 5 5 5")==RS_OK);
    assert(command(session,"//copy")==RS_OK);
    assert(game_session_save(session)==GAME_SESSION_OK);
    Structure *structure=fixture();
    assert(structure_write(structure,"build/test-schem-load/fixture.mcs",true)==STRUCTURE_OK);
    structure_destroy(structure);

    assert(command(session,"//schem load missing")==RS_IO_ERROR);
    assert(strstr(message,"build/test-schem-load/missing.mcs"));
    assert(world_result(world)==RS_OK);
    assert(command(session,"//paste 6 5 5")==RS_OK);
    assert(world_get(world,(BlockPos){6,5,5}).id==BLOCK_REDSTONE_BLOCK);
    assert(command(session,"//undo")==RS_OK);
    assert(game_session_save(session)==GAME_SESSION_OK);

    uint64_t revision=world_revision(world),tick=world_tick(world);
    BlockPos first,second;
    assert(command(session,"//schem load fixture")==RS_OK);
    assert(strstr(message,"anchor (701, 6, 701)"));
    assert(world_revision(world)==revision && world_tick(world)==tick);
    assert(!game_session_dirty(session));
    assert(sim_is_frozen(world) && !sim_updates_enabled(world));
    assert(region_editor_selection(editor,&first,&second)==3);
    assert(first.x==5 && first.y==5 && first.z==5 &&
           second.x==5 && second.y==5 && second.z==5);
    /* Loading preserves the existing redo entry, independent of the clipboard. */
    assert(command(session,"//redo")==RS_OK);
    assert(world_get(world,(BlockPos){6,5,5}).id==BLOCK_REDSTONE_BLOCK);
    assert(command(session,"//undo")==RS_OK);
    assert(command(session,"//schem load fixture.mcs")==RS_OK);

    FILE *bad=fopen("build/test-schem-load/corrupt.mcs","wb");
    assert(bad && fwrite("invalid",1,7,bad)==7 && fclose(bad)==0);
    assert(command(session,"//schem load corrupt")==RS_CORRUPT_DATA);
    assert(command(session,"//schem load ../fixture")==RS_INVALID_ARGUMENT);
    assert(command(session,"//schem load sub/fixture")==RS_INVALID_ARGUMENT);
    assert(command(session,"//schem load sub\\fixture")==RS_INVALID_ARGUMENT);
    assert(region_editor_load_structure(editor,NULL)==RS_INVALID_ARGUMENT);
    assert(world_result(world)==RS_OK);
    /* No file or borrowed Structure is needed once the clipboard owns its copy. */
    assert(remove("build/test-schem-load/fixture.mcs")==0);
    assert(command(session,"//schem load fixture")==RS_IO_ERROR);
    assert(command(session,"//paste 20 -64 20")==RS_OUT_OF_BOUNDS);
    assert(command(session,"//paste 20 12 20")==RS_OK);
    pasted_fixture(world);
    assert(world_get(world,(BlockPos){21,11,20}).id==BLOCK_AIR);
    assert(command(session,"//undo")==RS_OK);
    assert(world_get(world,(BlockPos){19,11,19}).id==BLOCK_GLASS);
    assert(world_get(world,(BlockPos){21,11,20}).id==BLOCK_REDSTONE_BLOCK);
    stack_is(world,(BlockPos){20,11,19},1,99,1,1);
    assert(command(session,"//redo")==RS_OK);
    pasted_fixture(world);
    assert(command(session,"//undo")==RS_OK);
    assert(command(session,"//paste -a 20 12 20")==RS_OK);
    pasted_fixture(world);
    assert(world_get(world,(BlockPos){21,11,20}).id==BLOCK_REDSTONE_BLOCK);

    /* Existing save grammar and selected-region semantics still work. */
    assert(command(session,"//schem save -f saved 4,4,4")==RS_OK);
    StructureResult read;
    structure=structure_read("build/test-schem-load/saved.mcs",&read);
    assert(structure && read==STRUCTURE_OK);
    StructureInfo info=structure_info(structure);
    assert(info.volume==1 && info.anchor.x==4 && info.anchor.y==4 && info.anchor.z==4);
    structure_destroy(structure);
    assert(command(session,"//schem save saved")==RS_INVALID_ARGUMENT);
    /* The loaded clipboard also works with normal simulation updates. */
    assert(sim_set_updates_enabled(world,true)==RS_OK);
    assert(command(session,"//paste 20 12 20")==RS_OK);
    pasted_fixture(world);
    assert(world_tick(world)==tick && world_result(world)==RS_OK);
    game_session_destroy(session);
    assert(remove("build/test-schem-load/world.mcw")==0);
    assert(remove("build/test-schem-load/corrupt.mcs")==0);
    assert(remove("build/test-schem-load/saved.mcs")==0);
    puts("schem load: parsing, completion, anchor, containers, failure preservation, air, undo/redo and save regression passed");
}
