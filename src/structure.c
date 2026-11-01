#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "rs/structure.h"
#include "sim_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

enum { HEADER_SIZE = 72, RECORD_SIZE = 8 };
static const unsigned char magic[8] = {'M','C','M','U','X','S','T','R'};
/* Indices are the permanent v1 disk type codes, independent of BlockId. Never
 * reorder this table when the in-memory registry or state encoding changes. */
static const BlockId disk_types[] = {
    BLOCK_AIR, BLOCK_IRON, BLOCK_REDSTONE_BLOCK, BLOCK_REDSTONE_WIRE,
    BLOCK_REDSTONE_TORCH, BLOCK_GLASS, BLOCK_REDSTONE_REPEATER,
    BLOCK_REDSTONE_COMPARATOR, BLOCK_POWERED_RAIL, BLOCK_ACTIVATOR_RAIL,
    BLOCK_OBSERVER, BLOCK_PISTON, BLOCK_STICKY_PISTON, BLOCK_BARREL,
    BLOCK_SLIME, BLOCK_NOTE_BLOCK, BLOCK_HONEY, BLOCK_TARGET, BLOCK_HOPPER,
    BLOCK_DROPPER, BLOCK_REDSTONE_LAMP, BLOCK_PISTON_HEAD,
    BLOCK_STICKY_PISTON_HEAD, BLOCK_LEVER
};
typedef struct { uint32_t cell; ContainerStack slots[27]; } StructureContainer;
struct Structure {
    StructureInfo info;
    BlockState *cells;
    StructureContainer *containers;
};

static void set_result(StructureResult *out, StructureResult result)
{
    if (out) *out = result;
}

/* Retain the latest read failure on this thread before cleanup changes errno. */
static _Thread_local char structure_io_detail[1024];

static void record_read_error(const char *operation, const char *path,
                              int error, const char *detail)
{
    const char *reason=detail ? detail :
        error ? strerror(error) : "system did not supply an error code";
    snprintf(structure_io_detail,sizeof(structure_io_detail),
        "structure file I/O error: %s failed: %s [path: %s]",
        operation,reason,path);
}

/* Explicit fields, not a dumped C struct or an internal palette state ID. */
static bool encode_state(BlockState s, unsigned char out[RECORD_SIZE])
{
    StateId checked;
    if (!state_encode(s, &checked)) return false;
    size_t type = 0;
    while (type < sizeof(disk_types)/sizeof(disk_types[0]) && disk_types[type] != s.id) ++type;
    if (type == sizeof(disk_types)/sizeof(disk_types[0])) return false;
    memset(out, 0, RECORD_SIZE);
    out[0] = (unsigned char)type;
    unsigned char *p = out + 2;
    switch ((BlockId)s.id) {
    case BLOCK_REDSTONE_WIRE:
        p[0]=s.data.wire.power; p[1]=s.data.wire.connections;
        p[2]=s.data.wire.up_connections; p[3]=s.data.wire.prefer_dot; break;
    case BLOCK_REDSTONE_TORCH:
        p[0]=s.data.torch.support_dir; p[1]=s.data.torch.lit; break;
    case BLOCK_REDSTONE_REPEATER:
        p[0]=s.data.repeater.facing; p[1]=s.data.repeater.delay;
        p[2]=s.data.repeater.powered; p[3]=s.data.repeater.locked; break;
    case BLOCK_REDSTONE_COMPARATOR:
        p[0]=s.data.comparator.facing; p[1]=s.data.comparator.output;
        p[2]=s.data.comparator.subtract; p[3]=s.data.comparator.powered; break;
    case BLOCK_POWERED_RAIL: case BLOCK_ACTIVATOR_RAIL:
        p[0]=s.data.rail.facing; p[1]=s.data.rail.ascending; p[2]=s.data.rail.powered; break;
    case BLOCK_OBSERVER:
        p[0]=s.data.observer.facing; p[1]=s.data.observer.powered; break;
    case BLOCK_PISTON: case BLOCK_STICKY_PISTON:
    case BLOCK_PISTON_HEAD: case BLOCK_STICKY_PISTON_HEAD:
        p[0]=s.data.piston.facing; p[1]=s.data.piston.powered; p[2]=s.data.piston.extended; break;
    case BLOCK_BARREL:
        p[0]=s.data.barrel.facing; p[1]=s.data.barrel.level; p[2]=s.data.barrel.open; break;
    case BLOCK_NOTE_BLOCK:
        p[0]=s.data.note_block.powered; p[1]=s.data.note_block.note; p[2]=s.data.note_block.instrument; break;
    case BLOCK_HOPPER:
        p[0]=s.data.hopper.facing; p[1]=s.data.hopper.level; p[2]=s.data.hopper.enabled; break;
    case BLOCK_DROPPER:
        p[0]=s.data.dropper.facing; p[1]=s.data.dropper.level; p[2]=s.data.dropper.triggered; break;
    case BLOCK_REDSTONE_LAMP: p[0]=s.data.lamp.lit; break;
    case BLOCK_LEVER:
        p[0]=s.data.lever.support_dir; p[1]=s.data.lever.facing;
        p[2]=s.data.lever.powered; break;
    default: break;
    }
    return true;
}

static bool decode_state(const unsigned char in[RECORD_SIZE], BlockState *s)
{
    unsigned type = in[0] | ((unsigned)in[1] << 8);
    if (type >= sizeof(disk_types)/sizeof(disk_types[0])) return false;
    *s = block_default(disk_types[type]);
    const unsigned char *p = in + 2;
    switch ((BlockId)s->id) {
    case BLOCK_REDSTONE_WIRE:
        s->data.wire.power=p[0]; s->data.wire.connections=p[1];
        s->data.wire.up_connections=p[2]; s->data.wire.prefer_dot=p[3]!=0; break;
    case BLOCK_REDSTONE_TORCH:
        s->data.torch.support_dir=p[0]; s->data.torch.lit=p[1]!=0; break;
    case BLOCK_REDSTONE_REPEATER:
        s->data.repeater.facing=p[0]; s->data.repeater.delay=p[1];
        s->data.repeater.powered=p[2]!=0; s->data.repeater.locked=p[3]!=0; break;
    case BLOCK_REDSTONE_COMPARATOR:
        s->data.comparator.facing=p[0]; s->data.comparator.output=p[1];
        s->data.comparator.subtract=p[2]!=0; s->data.comparator.powered=p[3]!=0; break;
    case BLOCK_POWERED_RAIL: case BLOCK_ACTIVATOR_RAIL:
        s->data.rail.facing=p[0]; s->data.rail.ascending=p[1]!=0; s->data.rail.powered=p[2]!=0; break;
    case BLOCK_OBSERVER:
        s->data.observer.facing=p[0]; s->data.observer.powered=p[1]!=0; break;
    case BLOCK_PISTON: case BLOCK_STICKY_PISTON:
    case BLOCK_PISTON_HEAD: case BLOCK_STICKY_PISTON_HEAD:
        s->data.piston.facing=p[0]; s->data.piston.powered=p[1]!=0; s->data.piston.extended=p[2]!=0; break;
    case BLOCK_BARREL:
        s->data.barrel.facing=p[0]; s->data.barrel.level=p[1]; s->data.barrel.open=p[2]!=0; break;
    case BLOCK_NOTE_BLOCK:
        s->data.note_block.powered=p[0]!=0; s->data.note_block.note=p[1]; s->data.note_block.instrument=p[2]; break;
    case BLOCK_HOPPER:
        s->data.hopper.facing=p[0]; s->data.hopper.level=p[1]; s->data.hopper.enabled=p[2]!=0; break;
    case BLOCK_DROPPER:
        s->data.dropper.facing=p[0]; s->data.dropper.level=p[1]; s->data.dropper.triggered=p[2]!=0; break;
    case BLOCK_REDSTONE_LAMP: s->data.lamp.lit=p[0]!=0; break;
    case BLOCK_LEVER:
        s->data.lever.support_dir=p[0]; s->data.lever.facing=p[1];
        s->data.lever.powered=p[2]!=0; break;
    default: break;
    }
    unsigned char canonical[RECORD_SIZE];
    return encode_state(*s, canonical) && memcmp(in, canonical, RECORD_SIZE)==0;
}

static bool dimensions(StructureInfo *info)
{
    uint64_t volume = info->width;
    if (!volume || !info->height || !info->length || volume > STRUCTURE_BLOCK_LIMIT) return false;
    volume *= info->height;
    if (volume > STRUCTURE_BLOCK_LIMIT) return false;
    volume *= info->length;
    if (volume > STRUCTURE_BLOCK_LIMIT) return false;
    if ((int64_t)info->origin.x + info->width - 1 > INT32_MAX ||
        (int64_t)info->origin.y + info->height - 1 > INT32_MAX ||
        (int64_t)info->origin.z + info->length - 1 > INT32_MAX) return false;
    info->volume = (size_t)volume;
    return true;
}

static BlockPos local_pos(const StructureInfo *info, size_t index)
{
    return (BlockPos){(int)(index % info->width),
        (int)(index / ((size_t)info->width * info->length)),
        (int)((index / info->width) % info->length)};
}

static bool cell_index(const Structure *s, BlockPos p, size_t *index)
{
    if (!s || p.x<0 || p.y<0 || p.z<0 || (uint32_t)p.x>=s->info.width ||
        (uint32_t)p.y>=s->info.height || (uint32_t)p.z>=s->info.length) return false;
    *index = ((size_t)p.y*s->info.length+(unsigned)p.z)*s->info.width+(unsigned)p.x;
    return true;
}

void structure_destroy(Structure *s)
{
    if (s) { free(s->cells); free(s->containers); free(s); }
}

StructureInfo structure_info(const Structure *s) { return s ? s->info : (StructureInfo){0}; }

bool structure_get(const Structure *s, BlockPos local, BlockState *state)
{
    size_t index;
    if (!state || !cell_index(s,local,&index)) return false;
    *state=s->cells[index]; return true;
}

size_t structure_container_size(const Structure *s, BlockPos local)
{
    size_t index;
    return cell_index(s,local,&index) ? container_slot_count((BlockId)s->cells[index].id) : 0;
}

StructureResult structure_get_container_slot(const Structure *s, BlockPos local,
    size_t slot, ContainerStack *stack)
{
    size_t index;
    if (!stack || !cell_index(s,local,&index) || slot>=container_slot_count((BlockId)s->cells[index].id))
        return STRUCTURE_INVALID_ARGUMENT;
    size_t lo=0, hi=s->info.container_count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if (s->containers[mid].cell<index) lo=mid+1; else hi=mid;
    }
    if (lo==s->info.container_count || s->containers[lo].cell!=index) return STRUCTURE_CORRUPT;
    *stack=s->containers[lo].slots[slot]; return STRUCTURE_OK;
}

Structure *structure_capture(const World *w, BlockPos first, BlockPos second,
                             const BlockPos *anchor, StructureResult *out)
{
    StructureResult result=STRUCTURE_INVALID_ARGUMENT;
    Structure *s=NULL;
    if (!w || w->sim->handling_tick || w->sim->block_entities.ticking) goto fail;
    if (w->sim->error!=RS_OK) { result=STRUCTURE_SIMULATION_ERROR; goto fail; }
    if (!world_in_bounds(w,first) || !world_in_bounds(w,second)) {
        result=STRUCTURE_OUT_OF_BOUNDS; goto fail;
    }
    s=calloc(1,sizeof(*s));
    if (!s) { result=STRUCTURE_OUT_OF_MEMORY; goto fail; }
#define LOW(a) (first.a<second.a ? first.a : second.a)
#define HIGH(a) (first.a>second.a ? first.a : second.a)
    s->info.origin=(BlockPos){LOW(x),LOW(y),LOW(z)};
    s->info.width=(uint32_t)((int64_t)HIGH(x)-LOW(x)+1);
    s->info.height=(uint32_t)((int64_t)HIGH(y)-LOW(y)+1);
    s->info.length=(uint32_t)((int64_t)HIGH(z)-LOW(z)+1);
#undef LOW
#undef HIGH
    if (!dimensions(&s->info)) { result=STRUCTURE_TOO_LARGE; goto fail; }
    s->info.anchor=anchor ? *anchor : s->info.origin;
    s->info.source_tick=world_tick(w);
    s->cells=malloc(s->info.volume*sizeof(*s->cells));
    if (!s->cells) { result=STRUCTURE_OUT_OF_MEMORY; goto fail; }
    unsigned char record[RECORD_SIZE];
    for (size_t i=0;i<s->info.volume;++i) {
        BlockPos p=local_pos(&s->info,i);
        p.x+=s->info.origin.x; p.y+=s->info.origin.y; p.z+=s->info.origin.z;
        BlockState state=s->cells[i]=world_get(w,p);
        if (w->sim->error!=RS_OK) { result=STRUCTURE_SIMULATION_ERROR; goto fail; }
        if (!encode_state(state,record)) { result=STRUCTURE_UNSUPPORTED; goto fail; }
        if (container_is_block((BlockId)state.id)) ++s->info.container_count;
    }
    if (s->info.container_count) {
        s->containers=calloc(s->info.container_count,sizeof(*s->containers));
        if (!s->containers) { result=STRUCTURE_OUT_OF_MEMORY; goto fail; }
    }
    size_t c=0;
    for (size_t i=0;i<s->info.volume;++i) {
        size_t slots=container_slot_count((BlockId)s->cells[i].id);
        if (!slots) continue;
        StructureContainer *stored=&s->containers[c++]; stored->cell=(uint32_t)i;
        BlockPos p=local_pos(&s->info,i);
        p.x+=s->info.origin.x; p.y+=s->info.origin.y; p.z+=s->info.origin.z;
        for (size_t j=0;j<slots;++j) {
            if (world_get_container_slot(w,p,j,&stored->slots[j])!=RS_OK) {
                result=STRUCTURE_SIMULATION_ERROR; goto fail;
            }
        }
    }
    set_result(out,STRUCTURE_OK); return s;
fail:
    structure_destroy(s); set_result(out,result); return NULL;
}

static uint32_t checksum(const unsigned char *data, size_t size)
{
    uint32_t crc=UINT32_MAX;
    for (size_t i=0;i<size;++i) {
        crc^=data[i];
        for (unsigned j=0;j<8;++j) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}

static bool sync_file(FILE *file)
{
    if (fflush(file)!=0) return false;
#ifdef _WIN32
    return _commit(_fileno(file))==0;
#else
    return fsync(fileno(file))==0;
#endif
}

static StructureResult publish(const char *temporary, const char *path, bool overwrite)
{
#ifdef _WIN32
    DWORD flags=MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (MoveFileExA(temporary,path,flags)) return STRUCTURE_OK;
    DWORD error=GetLastError();
    return error==ERROR_ALREADY_EXISTS || error==ERROR_FILE_EXISTS ? STRUCTURE_EXISTS : STRUCTURE_IO_ERROR;
#else
    if (overwrite) return rename(temporary,path)==0 ? STRUCTURE_OK : STRUCTURE_IO_ERROR;
    if (link(temporary,path)!=0) return errno==EEXIST ? STRUCTURE_EXISTS : STRUCTURE_IO_ERROR;
    (void)unlink(temporary); return STRUCTURE_OK;
#endif
}

StructureResult structure_write(const Structure *s, const char *path, bool overwrite)
{
    structure_io_detail[0]='\0';
    if (!s || !path || !*path) return STRUCTURE_INVALID_ARGUMENT;
    ByteBuffer palette, b; bytes_init(&palette); bytes_init(&b);
    StructureResult result=STRUCTURE_OUT_OF_MEMORY;
    uint16_t *lookup=calloc(STATE_ID_LIMIT,sizeof(*lookup));
    uint16_t *indices=malloc(s->info.volume*sizeof(*indices));
    char *temporary=NULL;
    if (!lookup || !indices) goto done;
    uint32_t count=0;
    for (size_t i=0;i<s->info.volume;++i) {
        StateId id; unsigned char record[RECORD_SIZE];
        if (!state_encode(s->cells[i],&id) || id>=STATE_ID_LIMIT || !encode_state(s->cells[i],record)) {
            result=STRUCTURE_UNSUPPORTED; goto done;
        }
        if (!lookup[id]) {
            lookup[id]=(uint16_t)(++count);
            bytes_write(&palette,record,sizeof(record));
        }
        indices[i]=(uint16_t)(lookup[id]-1);
    }
    bytes_write(&b,magic,sizeof(magic)); bytes_u16(&b,STRUCTURE_VERSION); bytes_u16(&b,HEADER_SIZE);
    bytes_u32(&b,0); bytes_u32(&b,s->info.width); bytes_u32(&b,s->info.height); bytes_u32(&b,s->info.length);
    bytes_pos(&b,s->info.origin); bytes_pos(&b,s->info.anchor); bytes_u64(&b,s->info.source_tick);
    bytes_u32(&b,count); bytes_u32(&b,(uint32_t)s->info.container_count); bytes_u32(&b,(uint32_t)s->info.volume);
    if (!palette.ok) goto done;
    bytes_write(&b,palette.data,palette.size);
    for (size_t i=0;i<s->info.volume;++i) bytes_u16(&b,indices[i]);
    for (size_t i=0;i<s->info.container_count;++i) {
        const StructureContainer *c=&s->containers[i];
        size_t slots=container_slot_count((BlockId)s->cells[c->cell].id);
        bytes_u32(&b,c->cell); bytes_u8(&b,(uint8_t)slots); bytes_u8(&b,0); bytes_u16(&b,0);
        for (size_t j=0;j<slots;++j) {
            bytes_u32(&b,c->slots[j].item); bytes_u8(&b,c->slots[j].count); bytes_u8(&b,c->slots[j].limit);
        }
    }
    if (!b.ok) goto done;
    bytes_u32(&b,checksum(b.data,b.size));
    if (!b.ok) goto done;
    size_t length=strlen(path);
    if (length>SIZE_MAX-5) { result=STRUCTURE_INVALID_ARGUMENT; goto done; }
    temporary=malloc(length+5);
    if (!temporary) goto done;
    memcpy(temporary,path,length); memcpy(temporary+length,".tmp",5);
    FILE *file=fopen(temporary,"wbx");
    if (!file) { result=STRUCTURE_IO_ERROR; goto done; }
    bool written=fwrite(b.data,1,b.size,file)==b.size && sync_file(file);
    if (fclose(file)!=0) written=false;
    result=written ? publish(temporary,path,overwrite) : STRUCTURE_IO_ERROR;
    if (result!=STRUCTURE_OK) (void)remove(temporary);
done:
    free(temporary); free(indices); free(lookup); bytes_destroy(&palette); bytes_destroy(&b);
    return result;
}

Structure *structure_read(const char *path, StructureResult *out)
{
    structure_io_detail[0]='\0';
    StructureResult result=STRUCTURE_INVALID_ARGUMENT;
    Structure *s=NULL; BlockState *palette=NULL; bool *seen=NULL;
    ByteBuffer b; bytes_init(&b);
    if (!path || !*path) goto done;
    errno=0;
    FILE *file=fopen(path,"rb");
    if (!file) {
        record_read_error("fopen",path,errno,NULL);
        result=STRUCTURE_IO_ERROR;
        goto done;
    }
    result=STRUCTURE_IO_ERROR;
    errno=0;
    if (fseek(file,0,SEEK_END)!=0) {
        record_read_error("fseek(SEEK_END)",path,errno,NULL);
        fclose(file);
        goto done;
    }
    errno=0;
    long length=ftell(file);
    if (length<0) {
        record_read_error("ftell",path,errno,NULL);
        fclose(file);
        goto done;
    }
    errno=0;
    if (fseek(file,0,SEEK_SET)!=0) {
        record_read_error("fseek(SEEK_SET)",path,errno,NULL);
        fclose(file);
        goto done;
    }
    /* The largest v1 body is all barrels, plus the maximum palette. */
    uint64_t limit=HEADER_SIZE+(uint64_t)STATE_ID_LIMIT*RECORD_SIZE+
        (uint64_t)STRUCTURE_BLOCK_LIMIT*(2+8+27*6)+4;
    if ((uint64_t)length>limit) { fclose(file); result=STRUCTURE_TOO_LARGE; goto done; }
    b.size=b.capacity=(size_t)length;
    b.data=malloc(b.size ? b.size : 1);
    if (!b.data) { fclose(file); result=STRUCTURE_OUT_OF_MEMORY; goto done; }
    errno=0;
    size_t read_size=fread(b.data,1,b.size,file);
    if (read_size!=b.size) {
        int error=errno;
        record_read_error("fread",path,error,
            ferror(file) ? NULL : "file ended before its measured length");
        fclose(file);
        goto done;
    }
    errno=0;
    if (fclose(file)!=0) {
        record_read_error("fclose",path,errno,NULL);
        goto done;
    }
    result=STRUCTURE_CORRUPT;
    if (b.size<HEADER_SIZE+4 || memcmp(b.data,magic,sizeof(magic))) goto done;
    b.cursor=8;
    if (bytes_read16(&b)!=STRUCTURE_VERSION) { result=STRUCTURE_UNSUPPORTED; goto done; }
    if (bytes_read16(&b)!=HEADER_SIZE) goto done;
    if (bytes_read32(&b)!=0) { result=STRUCTURE_UNSUPPORTED; goto done; }
    size_t file_size=b.size; b.cursor=file_size-4;
    uint32_t crc=bytes_read32(&b);
    if (checksum(b.data,file_size-4)!=crc) goto done;
    b.size-=4; b.cursor=16;
    s=calloc(1,sizeof(*s));
    if (!s) { result=STRUCTURE_OUT_OF_MEMORY; goto done; }
    s->info.width=bytes_read32(&b); s->info.height=bytes_read32(&b); s->info.length=bytes_read32(&b);
    s->info.origin=bytes_read_pos(&b); s->info.anchor=bytes_read_pos(&b); s->info.source_tick=bytes_read64(&b);
    uint32_t palette_count=bytes_read32(&b), container_count=bytes_read32(&b), volume=bytes_read32(&b);
    if (!dimensions(&s->info) || volume!=s->info.volume || !palette_count ||
        palette_count>STATE_ID_LIMIT || palette_count>volume || container_count>volume ||
        (uint64_t)palette_count*RECORD_SIZE+(uint64_t)volume*2+(uint64_t)container_count*(8+5*6)>b.size-b.cursor)
        goto done;
    palette=malloc((size_t)palette_count*sizeof(*palette));
    seen=calloc(STATE_ID_LIMIT,sizeof(*seen));
    s->cells=malloc(s->info.volume*sizeof(*s->cells));
    if (!palette || !seen || !s->cells) {
        result=STRUCTURE_OUT_OF_MEMORY; goto done;
    }
    for (uint32_t i=0;i<palette_count;++i) {
        const unsigned char *record=b.data+b.cursor;
        unsigned type=record[0] | ((unsigned)record[1]<<8);
        if (type>=sizeof(disk_types)/sizeof(disk_types[0])) { result=STRUCTURE_UNSUPPORTED; goto done; }
        if (!decode_state(record,&palette[i])) goto done;
        /* State IDs are used only for bounded duplicate detection in memory. */
        StateId id;
        if (!state_encode(palette[i],&id) || id>=STATE_ID_LIMIT || seen[id]) goto done;
        seen[id]=true;
        b.cursor+=RECORD_SIZE;
    }
    size_t expected_containers=0; uint64_t inventory_bytes=0;
    for (size_t i=0;i<s->info.volume;++i) {
        unsigned index=bytes_read16(&b);
        if (index>=palette_count) goto done;
        s->cells[i]=palette[index];
        size_t slots=container_slot_count((BlockId)s->cells[i].id);
        if (slots) { ++expected_containers; inventory_bytes+=8+slots*6; }
    }
    if (expected_containers!=container_count || inventory_bytes!=b.size-b.cursor) goto done;
    if (container_count) {
        s->containers=calloc(container_count,sizeof(*s->containers));
        if (!s->containers) { result=STRUCTURE_OUT_OF_MEMORY; goto done; }
    }
    s->info.container_count=container_count;
    for (size_t i=0;i<container_count;++i) {
        StructureContainer *c=&s->containers[i];
        c->cell=bytes_read32(&b); unsigned slots=bytes_read8(&b);
        unsigned reserved=bytes_read8(&b); reserved|=bytes_read16(&b);
        if (!b.ok || reserved || c->cell>=volume || (i && c->cell<=s->containers[i-1].cell) ||
            !slots || slots!=container_slot_count((BlockId)s->cells[c->cell].id) ||
            (size_t)slots*6>b.size-b.cursor) goto done;
        for (size_t j=0;j<slots;++j) {
            c->slots[j].item=bytes_read32(&b); c->slots[j].count=bytes_read8(&b); c->slots[j].limit=bytes_read8(&b);
            if (!item_stack_valid(c->slots[j])) goto done;
        }
        BlockState state=s->cells[c->cell];
        ContainerRuntime runtime={.id=state.id,.slots=c->slots};
        uint8_t level=state.id==BLOCK_BARREL ? state.data.barrel.level :
            state.id==BLOCK_HOPPER ? state.data.hopper.level : state.data.dropper.level;
        if (container_level(&runtime)!=level) goto done;
    }
    if (!b.ok || b.cursor!=b.size) goto done;
    result=STRUCTURE_OK;
done:
    free(seen); free(palette); bytes_destroy(&b);
    if (result!=STRUCTURE_OK) { structure_destroy(s); s=NULL; }
    set_result(out,result); return s;
}

static int64_t structure_plot_minimum(int coordinate)
{
    int64_t n=coordinate, q=n/WORLD_PLOT_SIZE;
    if (n%WORLD_PLOT_SIZE<0) --q;
    return q*WORLD_PLOT_SIZE;
}

World *structure_load_at(const char *path, const BlockPos *anchor,
                         StructureResult *out)
{
    StructureResult result;
    Structure *s=structure_read(path,&result);
    World *w=NULL;
    if (!s) goto fail;

    int64_t offset_x = anchor ? (int64_t)anchor->x - s->info.anchor.x : 0;
    int64_t offset_y = anchor ? (int64_t)anchor->y - s->info.anchor.y : 0;
    int64_t offset_z = anchor ? (int64_t)anchor->z - s->info.anchor.z : 0;
    int64_t low_x=(int64_t)s->info.origin.x+offset_x;
    int64_t low_y=(int64_t)s->info.origin.y+offset_y;
    int64_t low_z=(int64_t)s->info.origin.z+offset_z;
    int64_t high_x=low_x+s->info.width-1;
    int64_t high_y=low_y+s->info.height-1;
    int64_t high_z=low_z+s->info.length-1;
    result=STRUCTURE_OUT_OF_BOUNDS;
    if (low_x<INT_MIN || low_y<INT_MIN || low_z<INT_MIN ||
        high_x>INT_MAX || high_y>INT_MAX || high_z>INT_MAX ||
        low_y<WORLD_MIN_Y || high_y>WORLD_MAX_Y) goto fail;
    BlockPos low={(int)low_x,(int)low_y,(int)low_z};
    /* Include the default plot; allocate only the chunks actually accessed. */
    int64_t x0=structure_plot_minimum(low.x<0 ? low.x : 0);
    int64_t z0=structure_plot_minimum(low.z<0 ? low.z : 0);
    int64_t x1=structure_plot_minimum(high_x>=WORLD_PLOT_SIZE ? (int)high_x : WORLD_PLOT_SIZE-1)
        +WORLD_PLOT_SIZE-1;
    int64_t z1=structure_plot_minimum(high_z>=WORLD_PLOT_SIZE ? (int)high_z : WORLD_PLOT_SIZE-1)
        +WORLD_PLOT_SIZE-1;
    if (x0<INT_MIN || z0<INT_MIN || x1>INT_MAX || z1>INT_MAX) goto fail;
    WorldPlotBounds plot={(int)x0,(int)z0,(int)x1,(int)z1};
    if (!world_plot_bounds_valid(plot)) goto fail;

    result=STRUCTURE_OUT_OF_MEMORY;
    w=world_create(world_config_default());
    if (!w) goto fail;
    w->plot=plot;
    for (size_t i=0;i<s->info.volume;++i) {
        BlockPos p=local_pos(&s->info,i);
        p=(BlockPos){low.x+p.x,low.y+p.y,low.z+p.z};
        BlockState state=s->cells[i];
        if (!world_write_visual(w,p,state)) {
            result=w->sim->error==RS_OUT_OF_MEMORY ? STRUCTURE_OUT_OF_MEMORY :
                STRUCTURE_SIMULATION_ERROR;
            goto fail;
        }
        if (state.id==BLOCK_REDSTONE_TORCH && !sim_attach_torch(w,p)) goto fail;
    }
    /* Attach runtime objects only after every saved cell has been installed. */
    for (size_t i=0;i<s->info.container_count;++i) {
        const StructureContainer *c=&s->containers[i];
        BlockPos p=local_pos(&s->info,c->cell);
        p=(BlockPos){low.x+p.x,low.y+p.y,low.z+p.z};
        BlockState state=s->cells[c->cell];
        if (!container_attach(w,p,state)) goto fail;
        ContainerRuntime *runtime=container_runtime(w,p);
        memcpy(runtime->slots,c->slots,container_slot_count((BlockId)state.id)*sizeof(*c->slots));
        if (state.id==BLOCK_BARREL) runtime->openers=state.data.barrel.open ? 1u : 0u;
    }
    if (sim_set_frozen(w,true)!=RS_OK) {
        result=STRUCTURE_SIMULATION_ERROR;
        goto fail;
    }
    structure_destroy(s);
    set_result(out,STRUCTURE_OK);
    return w;
fail:
    world_destroy(w);
    structure_destroy(s);
    set_result(out,result);
    return NULL;
}

World *structure_load(const char *path, StructureResult *out)
{
    return structure_load_at(path, NULL, out);
}

const char *structure_result_name(StructureResult result)
{
    if (result==STRUCTURE_IO_ERROR && structure_io_detail[0])
        return structure_io_detail;
    static const char *const names[]={"ok","invalid argument","out of bounds",
        "structure exceeds the block limit","out of memory","structure file I/O error",
        "structure already exists; use -f to replace it","corrupt structure file",
        "unsupported structure version, flags or block (finish piston motion before saving)",
        "world simulation error"};
    return (unsigned)result<sizeof(names)/sizeof(names[0]) ? names[result] : "unknown structure error";
}
