#include "edit_command.h"
#include "rs/structure.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool integer(const char *text, int *value)
{
    if (!*text) return false;
    char *end; errno=0;
    intmax_t n=strtoimax(text,&end,10);
    if (errno || *end || n<INT_MIN || n>INT_MAX) return false;
    *value=(int)n; return true;
}

static bool coordinates(char **args, size_t count, BlockPos *pos)
{
    if (count==3) return integer(args[0],&pos->x) && integer(args[1],&pos->y) && integer(args[2],&pos->z);
    if (count!=1) return false;
    char *a=args[0], *b=strchr(a,',');
    if (!b) return false;
    *b++='\0';
    char *c=strchr(b,',');
    if (!c) return false;
    *c++='\0';
    return integer(a,&pos->x) && integer(b,&pos->y) && integer(c,&pos->z);
}

static bool count_value(const char *text, uint32_t *value)
{
    uint32_t n=0;
    if (!*text) return false;
    for (;*text;++text) {
        if (*text<'0' || *text>'9') return false;
        unsigned digit=(unsigned)(*text-'0');
        if (n>(UINT32_MAX-digit)/10u) return false;
        n=n*10u+digit;
    }
    *value=n; return n!=0;
}

static Direction direction_value(const char *text)
{
    static const char *const names[]={"east","west","up","down","south","north"};
    for (unsigned i=0;i<DIR_COUNT;++i) if (!strcmp(text,names[i])) return (Direction)i;
    return DIR_COUNT;
}

bool edit_command_parse(const char *line, EditCommand *command)
{
    if (!line || !command) return false;
    *command=(EditCommand){.direction=DIR_COUNT,.count=1};
    line+=strspn(line," \t\r\n");
    unsigned slashes=0;
    while (*line=='/' && slashes<3) { ++slashes; ++line; }
    if (slashes>2) return false;
    size_t name_length=strcspn(line," \t\r\n");
    static const char *const names[]={"","pos1","pos2","copy","paste","stack","move","undo","redo","schem"};
    EditCommandKind kind=EDIT_INVALID;
    for (unsigned i=1;i<sizeof(names)/sizeof(names[0]);++i)
        if (strlen(names[i])==name_length && !strncmp(line,names[i],name_length)) kind=(EditCommandKind)i;
    if (kind==EDIT_INVALID) return false;
    line+=name_length;
    if (strlen(line)>=512) return true;
    char copy[512]; strcpy(copy,line);
    char *args[6]; size_t count=0;
    char *cursor=copy;
    while (*cursor) {
        cursor+=strspn(cursor," \t\r\n");
        if (!*cursor) break;
        if (count==sizeof(args)/sizeof(args[0])) return true;
        args[count++]=cursor;
        cursor+=strcspn(cursor," \t\r\n");
        if (*cursor) *cursor++='\0';
    }
    if (kind==EDIT_SCHEM_SAVE) {
        if (count==2 && !strcmp(args[0],"load")) {
            if (strlen(args[1])>=sizeof(command->name)) return true;
            strcpy(command->name,args[1]);
            command->kind=EDIT_SCHEM_LOAD;
            return true;
        }
        if (count<2 || strcmp(args[0],"save")) return true;
        size_t output=1;
        for (size_t i=1;i<count;++i) {
            if (!strcmp(args[i],"-f")) {
                if (command->overwrite) return true;
                command->overwrite=true;
            } else args[output++]=args[i];
        }
        count=output;
        if (count<2 || strlen(args[1])>=sizeof(command->name)) return true;
        strcpy(command->name,args[1]);
        if (count>2) {
            if (!coordinates(args+2,count-2,&command->position)) return true;
            command->has_position=true;
        }
        command->kind=kind; return true;
    }
    if (kind==EDIT_PASTE) {
        size_t output=0;
        for (size_t i=0;i<count;++i) {
            if (!strcmp(args[i],"-a")) {
                if (command->skip_air) return true;
                command->skip_air=true;
            } else args[output++]=args[i];
        }
        count=output;
    }
    if (kind==EDIT_UNDO || kind==EDIT_REDO) {
        if (count) return true;
    } else if (kind==EDIT_POS1 || kind==EDIT_POS2 || kind==EDIT_COPY || kind==EDIT_PASTE) {
        if (count) {
            if (!coordinates(args,count,&command->position)) return true;
            command->has_position=true;
        }
    } else {
        if (count>2) return true;
        if (count==1 && !count_value(args[0],&command->count)) {
            command->direction=direction_value(args[0]);
            if (command->direction==DIR_COUNT) return true;
        } else if (count>=1) {
            if (!count_value(args[0],&command->count)) return true;
            if (count==2) {
                command->direction=direction_value(args[1]);
                if (command->direction==DIR_COUNT) return true;
            }
        }
    }
    command->kind=kind; return true;
}

static RsResult structure_path(GameSession *session, const EditCommand *c,
                               char *path, size_t path_capacity,
                               char *message, size_t capacity)
{
    size_t length=strlen(c->name);
    if (length>4 && !strcmp(c->name+length-4,".mcs")) length-=4;
    if (!length || length>=GAME_SESSION_NAME_CAPACITY) {
        snprintf(message,capacity,"Use a structure name of 1..63 letters, digits, '-' or '_'");
        return RS_INVALID_ARGUMENT;
    }
    for (size_t i=0;i<length;++i) {
        unsigned char ch=(unsigned char)c->name[i];
        if (!((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') ||
              (ch>='0' && ch<='9') || ch=='-' || ch=='_')) {
            snprintf(message,capacity,"Use a structure name of 1..63 letters, digits, '-' or '_'");
            return RS_INVALID_ARGUMENT;
        }
    }
    const char *save_path=game_session_path(session);
    if (!save_path || !*save_path) {
        snprintf(message,capacity,"Current world has no save path");
        return RS_INVALID_ARGUMENT;
    }
    const char *separator=strrchr(save_path,'/');
#ifdef _WIN32
    const char *backslash=strrchr(save_path,'\\');
    if (!separator || (backslash && backslash>separator)) separator=backslash;
#endif
    size_t directory_length=separator ? (size_t)(separator-save_path)+1 : 0;
    int written=snprintf(path,path_capacity,"%.*s%.*s.mcs",
        (int)directory_length,save_path,(int)length,c->name);
    if (written<=0 || (size_t)written>=path_capacity) {
        snprintf(message,capacity,"Structure path is too long");
        return RS_INVALID_ARGUMENT;
    }
    return RS_OK;
}

static RsResult save_structure(GameSession *session, const EditCommand *c,
                               char *message, size_t capacity)
{
    char path[GAME_SESSION_PATH_CAPACITY];
    RsResult result=structure_path(session,c,path,sizeof(path),message,capacity);
    if (result!=RS_OK) return result;
    RegionBounds bounds;
    if (!region_editor_bounds(game_session_editor(session),&bounds)) {
        snprintf(message,capacity,"Set both pos1 and pos2 first");
        return RS_INVALID_ARGUMENT;
    }
    StructureResult saved;
    Structure *structure=structure_capture(game_session_world(session),bounds.min,bounds.max,
        c->has_position ? &c->position : NULL,&saved);
    if (structure) {
        saved=structure_write(structure,path,c->overwrite);
        if (saved==STRUCTURE_OK) {
            StructureInfo info=structure_info(structure);
            snprintf(message,capacity,"Saved %zu cells to %s; anchor (%d, %d, %d)",
                info.volume,path,info.anchor.x,info.anchor.y,info.anchor.z);
        }
        structure_destroy(structure);
    }
    if (saved!=STRUCTURE_OK) snprintf(message,capacity,"%s",structure_result_name(saved));
    return saved==STRUCTURE_OK ? RS_OK : saved==STRUCTURE_OUT_OF_MEMORY ? RS_OUT_OF_MEMORY : RS_INVALID_ARGUMENT;
}

static RsResult load_structure(GameSession *session, const EditCommand *c,
                               char *message, size_t capacity)
{
    char path[GAME_SESSION_PATH_CAPACITY];
    RsResult result=structure_path(session,c,path,sizeof(path),message,capacity);
    if (result!=RS_OK) return result;
    StructureResult loaded;
    Structure *structure=structure_read(path,&loaded);
    if (!structure) {
        snprintf(message,capacity,"%s",structure_result_name(loaded));
        switch (loaded) {
        case STRUCTURE_OUT_OF_MEMORY: return RS_OUT_OF_MEMORY;
        case STRUCTURE_IO_ERROR: return RS_IO_ERROR;
        case STRUCTURE_CORRUPT: return RS_CORRUPT_DATA;
        case STRUCTURE_UNSUPPORTED: return RS_UNSUPPORTED;
        default: return RS_INVALID_ARGUMENT;
        }
    }
    RegionEditor *editor=game_session_editor(session);
    result=region_editor_load_structure(editor,structure);
    if (result==RS_OK) {
        StructureInfo info=structure_info(structure);
        snprintf(message,capacity,
            "Loaded %zu cells from %s; anchor (%d, %d, %d). Use //paste X Y Z",
            info.volume,c->name,info.anchor.x,info.anchor.y,info.anchor.z);
    } else snprintf(message,capacity,"%s",region_editor_error(editor));
    structure_destroy(structure);
    return result;
}

RsResult edit_command_run(GameSession *session, const EditCommand *c,
                          EditCommandContext context, char *message, size_t capacity)
{
    if (!session || !c || !message || !capacity) return RS_INVALID_ARGUMENT;
    message[0]='\0';
    RegionEditor *e=game_session_editor(session);
    BlockPos pos=c->has_position ? c->position : context.position;
    bool has_position=c->has_position || context.has_position;
    size_t changed=0;
    RsResult result;
    switch (c->kind) {
    case EDIT_SCHEM_SAVE:
        return save_structure(session,c,message,capacity);
    case EDIT_SCHEM_LOAD:
        return load_structure(session,c,message,capacity);
    case EDIT_POS1: case EDIT_POS2:
        if (!has_position) {
            snprintf(message,capacity,"Use pos1 X Y Z or pos2 X Y Z");
            return RS_INVALID_ARGUMENT;
        }
        result=region_editor_select(e,c->kind==EDIT_POS1 ? 1 : 2,pos);
        if (result==RS_OK) snprintf(message,capacity,"pos%u: (%d, %d, %d)",
            c->kind==EDIT_POS1 ? 1u : 2u,pos.x,pos.y,pos.z);
        break;
    case EDIT_COPY:
        result=region_editor_copy(e,has_position ? &pos : NULL);
        if (result==RS_OK) {
            RegionBounds b; (void)region_editor_bounds(e,&b);
            snprintf(message,capacity,"Copied %zu blocks; anchor (%d, %d, %d)",b.volume,
                has_position ? pos.x : b.min.x,has_position ? pos.y : b.min.y,has_position ? pos.z : b.min.z);
        }
        break;
    case EDIT_PASTE:
        if (!has_position) {
            snprintf(message,capacity,"Use paste [-a] X Y Z");
            return RS_INVALID_ARGUMENT;
        }
        result=region_editor_paste(e,pos,c->skip_air,&changed);
        if (result==RS_OK) snprintf(message,capacity,"Pasted: %zu blocks changed",changed);
        break;
    case EDIT_STACK: case EDIT_MOVE: {
        Direction direction=c->direction==DIR_COUNT ? context.facing : c->direction;
        if ((unsigned)direction>=DIR_COUNT) {
            snprintf(message,capacity,"Specify east, west, up, down, south or north");
            return RS_INVALID_ARGUMENT;
        }
        result=c->kind==EDIT_STACK ? region_editor_stack(e,c->count,direction,&changed) :
            region_editor_move(e,c->count,direction,&changed);
        if (result==RS_OK) snprintf(message,capacity,"%s: %zu blocks changed",
            c->kind==EDIT_STACK ? "Stacked" : "Moved",changed);
        break;
    }
    case EDIT_UNDO: case EDIT_REDO:
        result=c->kind==EDIT_UNDO ? region_editor_undo(e,&changed) : region_editor_redo(e,&changed);
        if (result==RS_OK) snprintf(message,capacity,"%s: %zu blocks changed",
            c->kind==EDIT_UNDO ? "Undid" : "Redid",changed);
        break;
    default:
        snprintf(message,capacity,"Use pos1|pos2|copy|paste|stack|move|undo|redo; //schem save [-f] NAME [X Y Z] or //schem load NAME");
        return RS_INVALID_ARGUMENT;
    }
    if (result!=RS_OK) snprintf(message,capacity,"%s",region_editor_error(e)[0] ? region_editor_error(e) : rs_result_name(result));
    return result;
}
