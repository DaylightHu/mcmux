#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "app_config.h"
#include "raylib.h"
#include "../../third_party/tomlc17/tomlc17.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define config_mkdir(p) _mkdir(p)
#else
#define config_mkdir(p) mkdir(p,0700)
#endif

enum { CTRL=1, SHIFT=2, ALT=4, SUPER=8, KEY_COUNT=512,
       BINDING_LIMIT=512, TEXT_LIMIT=1024, PATH_LIMIT=4096 };
#define S(x) (1u << APP_##x)
#define EDIT (S(NORMAL)|S(VISUAL)|S(INSERT)|S(COMMAND)|S(BROWSER)|S(INFO))
#define NAV (EDIT|S(OUTPUT))
typedef struct {
    const char *name;
    unsigned scopes;
    int key, mods;
    const char *characters, *command, *description;
} Action;
#define KEY_ACTION(n,sc,k,m) {n,sc,k,m,NULL,NULL,n}
#define NORMAL_ACTION(n,c) {n,S(NORMAL),0,0,c,NULL,n}
#define COMMAND_ACTION(n,c) {n,EDIT,0,0,NULL,c,n}
#define WORKSPACE_ACTION(n,d) {n,S(WORKSPACE),0,0,NULL,NULL,d}
#define WORLD_ACTION(n) {n,S(WORLD),0,0,NULL,NULL,n}
static const Action actions[] = {
    KEY_ACTION("left",NAV&~S(BROWSER),KEY_LEFT,0), KEY_ACTION("right",NAV&~S(BROWSER),KEY_RIGHT,0),
    KEY_ACTION("up",NAV&~S(COMMAND),KEY_UP,0), KEY_ACTION("down",NAV&~S(COMMAND),KEY_DOWN,0),
    KEY_ACTION("home",NAV,KEY_HOME,0), KEY_ACTION("end",NAV,KEY_END,0),
    KEY_ACTION("page_up",NAV&~(S(INSERT)|S(COMMAND)),KEY_PAGE_UP,0), KEY_ACTION("page_down",NAV&~(S(INSERT)|S(COMMAND)),KEY_PAGE_DOWN,0),
    KEY_ACTION("escape",EDIT,KEY_ESCAPE,0), KEY_ACTION("enter",S(INSERT)|S(COMMAND)|S(BROWSER),KEY_ENTER,0),
    KEY_ACTION("backspace",S(INSERT)|S(COMMAND),KEY_BACKSPACE,0), KEY_ACTION("delete",S(NORMAL)|S(VISUAL)|S(INSERT)|S(COMMAND),KEY_DELETE,0),
    KEY_ACTION("tab",S(INSERT),KEY_TAB,0), KEY_ACTION("paste",S(NORMAL)|S(INSERT)|S(COMMAND),KEY_V,CTRL),
    KEY_ACTION("copy",S(VISUAL),KEY_C,CTRL), KEY_ACTION("cut",S(VISUAL),KEY_X,CTRL),
    KEY_ACTION("back",S(NORMAL),KEY_O,CTRL), KEY_ACTION("definition_key",S(NORMAL),KEY_RIGHT_BRACKET,CTRL),
    {"parent",S(BROWSER),KEY_H,0,NULL,NULL,"Parent directory"},
    NORMAL_ACTION("insert","i"), NORMAL_ACTION("append","a"),
    NORMAL_ACTION("insert_start","I"), NORMAL_ACTION("append_end","A"),
    {"visual",S(NORMAL)|S(VISUAL),0,0,"v",NULL,"visual"}, {"visual_line",S(NORMAL)|S(VISUAL),0,0,"V",NULL,"visual_line"},
    {"delete_operator",S(NORMAL)|S(VISUAL),0,0,"d",NULL,"delete_operator"}, {"copy_operator",S(NORMAL)|S(VISUAL),0,0,"y",NULL,"copy_operator"},
    NORMAL_ACTION("goto_operator","g"), {"delete_to_end",S(NORMAL)|S(VISUAL),0,0,"D",NULL,"delete_to_end"},
    NORMAL_ACTION("paste_after","p"), NORMAL_ACTION("paste_before","P"),
    NORMAL_ACTION("delete_line","dd"), NORMAL_ACTION("copy_line","yy"),
    {"command",S(NORMAL)|S(BROWSER),0,0,":",NULL,"enter command mode"},
    COMMAND_ACTION("save","w"), COMMAND_ACTION("build","make"),
    COMMAND_ACTION("run","run"), COMMAND_ACTION("model","model"),
    COMMAND_ACTION("definition","definition"), COMMAND_ACTION("declaration","declaration"),
    COMMAND_ACTION("hover","hover"), COMMAND_ACTION("explore","Ex"),
    {"config",EDIT|S(WORKSPACE),0,0,NULL,"config","Open configuration"},
    {"reload_config",EDIT|S(WORKSPACE),0,0,NULL,"reloadconfig","Reload configuration"},
    WORKSPACE_ACTION("split_horizontal","Split mc-nvim left / right"),
    WORKSPACE_ACTION("split_vertical","Split mc-nvim top / bottom"),
    WORKSPACE_ACTION("focus_left","Focus left pane"), WORKSPACE_ACTION("focus_right","Focus right pane"),
    WORKSPACE_ACTION("focus_up","Focus upper pane"), WORKSPACE_ACTION("focus_down","Focus lower pane"),
    WORKSPACE_ACTION("resize_left","Move divider left"), WORKSPACE_ACTION("resize_right","Move divider right"),
    WORKSPACE_ACTION("resize_up","Move divider up"), WORKSPACE_ACTION("resize_down","Move divider down"),
    WORKSPACE_ACTION("next_pane","Focus next pane"), WORKSPACE_ACTION("output","Open / focus mc-output"),
    WORKSPACE_ACTION("zoom","Zoom / restore text panes"), WORKSPACE_ACTION("close_pane","Close focused pane"),
    WORKSPACE_ACTION("world","Restore and focus mc-gui"), WORKSPACE_ACTION("help","Show key bindings"),
    {"cancel",S(OUTPUT),KEY_C,CTRL,NULL,NULL,"Stop command"},
    WORLD_ACTION("forward"), WORLD_ACTION("backward"), WORLD_ACTION("left"), WORLD_ACTION("right"),
    WORLD_ACTION("up"), WORLD_ACTION("down"), WORLD_ACTION("inventory"), WORLD_ACTION("pause"),
    WORLD_ACTION("command"), WORLD_ACTION("look_left"), WORLD_ACTION("look_right"),
    WORLD_ACTION("look_up"), WORLD_ACTION("look_down"), WORLD_ACTION("roll_left"),
    WORLD_ACTION("slot_1"), WORLD_ACTION("slot_2"), WORLD_ACTION("slot_3"),
    WORLD_ACTION("slot_4"), WORLD_ACTION("slot_5"), WORLD_ACTION("slot_6"),
    WORLD_ACTION("slot_7"), WORLD_ACTION("slot_8"), WORLD_ACTION("slot_9")
};
static const char *scope_names[] = {
    "workspace","normal","visual","insert","command","browser","info","output","world"
};
typedef struct { int key, character, mods; bool leader; char text[64]; } Trigger;
typedef struct { AppKeyScope scope; size_t action; Trigger trigger; int line, column; } Binding;
typedef struct { AppSettings settings; Trigger leader; Binding bindings[BINDING_LIMIT]; size_t count; } Config;
static Config current;
static char config_path[PATH_LIMIT], config_error[768];
static bool ready;
static unsigned revision;
static bool reset_operator;
typedef struct { bool pressed[KEY_COUNT], repeat[KEY_COUNT], down[KEY_COUNT]; int chars[TEXT_LIMIT], count, next, mods; const char *command; } Input;
static Input physical, logical;

static bool fail(const char *format, ...)
{
    va_list args; va_start(args,format);
    vsnprintf(config_error,sizeof(config_error),format,args); va_end(args);
    return false;
}
static int action_index(AppKeyScope scope,const char *name)
{
    for (size_t i=0;i<sizeof(actions)/sizeof(actions[0]);++i)
        if ((actions[i].scopes&(1u<<scope)) && !strcmp(actions[i].name,name)) return (int)i;
    return -1;
}
static bool equal_ci(const char *a,const char *b)
{
    for (;*a && *b;++a,++b) if (tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return false;
    return *a==*b;
}
static int named_key(const char *name)
{
    static const struct { const char *name; int key; } keys[] = {
        {"Left",KEY_LEFT},{"Right",KEY_RIGHT},{"Up",KEY_UP},{"Down",KEY_DOWN},
        {"Home",KEY_HOME},{"End",KEY_END},{"PageUp",KEY_PAGE_UP},{"PageDown",KEY_PAGE_DOWN},
        {"Escape",KEY_ESCAPE},{"Esc",KEY_ESCAPE},{"Enter",KEY_ENTER},{"Tab",KEY_TAB},
        {"Backspace",KEY_BACKSPACE},{"Delete",KEY_DELETE},{"Space",KEY_SPACE},
        {"LeftShift",KEY_LEFT_SHIFT},{"RightShift",KEY_RIGHT_SHIFT}
    };
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if (equal_ci(name,keys[i].name)) return keys[i].key;
    if ((name[0]=='F'||name[0]=='f') && isdigit((unsigned char)name[1])) {
        char *end; long n=strtol(name+1,&end,10);
        if (!*end && n>=1 && n<=12) return KEY_F1+(int)n-1;
    }
    return 0;
}
static bool parse_trigger(const char *text,Trigger *out)
{
    *out=(Trigger){0};
    if (!*text || strlen(text)>=sizeof(out->text)) return false;
    snprintf(out->text,sizeof(out->text),"%s",text);
    char copy[64]; snprintf(copy,sizeof(copy),"%s",text);
    char *name=copy,*plus;
    while ((plus=strchr(name,'+')) && plus[1]) {
        *plus='\0'; int flag=0;
        if (equal_ci(name,"Ctrl") || equal_ci(name,"Control")) flag=CTRL;
        else if (equal_ci(name,"Shift")) flag=SHIFT;
        else if (equal_ci(name,"Alt") || equal_ci(name,"Opt")) flag=ALT;
        else if (equal_ci(name,"Super") || equal_ci(name,"Cmd")) flag=SUPER;
        else if (equal_ci(name,"Leader") && !out->leader && name==copy) out->leader=true;
        else return false;
        if (flag && (out->mods&flag)) return false;
        out->mods|=flag; name=plus+1;
    }
    out->key=named_key(name);
    if (out->key) return true;
    if (name[0] && !name[1] && (unsigned char)name[0]>=33 && (unsigned char)name[0]<=126) {
        int c=(unsigned char)name[0];
        if (out->mods) {
            if (isalnum(c) || strchr("[];',./`-=\\",c)) {
                out->key=isalpha(c) ? toupper(c) : c;
                return true;
            }
            return false;
        }
        out->character=c; return true;
    }
    return false;
}
static bool add_binding(Config *config,AppKeyScope scope,const char *name,const char *text)
{
    int action=action_index(scope,name);
    if (action<0) return fail("keys.%s: unknown action '%s'",scope_names[scope],name);
    if (config->count==BINDING_LIMIT) return fail("too many key bindings (maximum %d)",BINDING_LIMIT);
    Trigger trigger;
    if (!parse_trigger(text,&trigger) || (trigger.leader && scope!=APP_WORKSPACE))
        return fail("keys.%s.%s: invalid binding '%s'",scope_names[scope],name,text);
    bool continuous=scope==APP_WORLD &&
        (!strcmp(name,"forward") || !strcmp(name,"backward") || !strcmp(name,"left") || !strcmp(name,"right") ||
         !strcmp(name,"up") || !strcmp(name,"down") || !strncmp(name,"look_",5) || !strcmp(name,"roll_left"));
    if (continuous && trigger.character && !isalnum(trigger.character) && !strchr("[];',./`-=\\",trigger.character))
        return fail("keys.world.%s: held movement needs a letter, digit, unshifted key or named key",name);
    config->bindings[config->count++]=(Binding){.scope=scope,.action=(size_t)action,.trigger=trigger};
    return true;
}
static void defaults(Config *config)
{
    *config=(Config){.settings={.vsync=true,.fps_limit=60,.msaa_samples=4,.fov=65,
        .editor_font_size=20,.output_font_size=20,.tab_width=4,.stream_radius=5,
        .leader_timeout_ms=0,.mouse_sensitivity=0.003f,.move_speed=5.4f,.scroll_lines=3}};
    (void)parse_trigger("Ctrl+B",&config->leader);
    static const struct { const char *action,*trigger; } nav[] = {
        {"left","Left"},{"right","Right"},{"up","Up"},{"down","Down"},
        {"home","Home"},{"end","End"},{"page_up","PageUp"},{"page_down","PageDown"}
    };
    for (int s=APP_NORMAL;s<=APP_OUTPUT;++s) {
        for (size_t i=0;i<sizeof(nav)/sizeof(nav[0]);++i)
            if (action_index((AppKeyScope)s,nav[i].action)>=0) (void)add_binding(config,(AppKeyScope)s,nav[i].action,nav[i].trigger);
        if (s!=APP_OUTPUT) (void)add_binding(config,(AppKeyScope)s,"escape","Escape");
    }
    for (int s=APP_NORMAL;s<=APP_VISUAL;++s) {
        static const struct { const char *action,*trigger; } normal[] = {
            {"left","h"},{"right","l"},{"up","k"},{"down","j"},
            {"insert","i"},{"append","a"},{"insert_start","I"},{"append_end","A"},
            {"visual","v"},{"visual_line","V"},{"delete_operator","d"},{"copy_operator","y"},
            {"goto_operator","g"},{"delete_to_end","D"},{"delete","x"},{"delete","Delete"},
            {"paste_after","p"},{"paste_before","P"},{"command",":"},
            {"back","Ctrl+O"},{"back","Ctrl+T"},{"definition_key","Ctrl+]"},
            {"back","Super+O"},{"back","Super+T"},{"definition_key","Super+]"},
            {"paste","Ctrl+V"},{"paste","Super+V"},{"copy","Ctrl+C"},{"copy","Super+C"},
            {"cut","Ctrl+X"},{"cut","Super+X"},{"hover","K"}
        };
        for (size_t i=0;i<sizeof(normal)/sizeof(normal[0]);++i)
            if (action_index((AppKeyScope)s,normal[i].action)>=0) (void)add_binding(config,(AppKeyScope)s,normal[i].action,normal[i].trigger);
    }
    for (int s=APP_INSERT;s<=APP_COMMAND;++s) {
        (void)add_binding(config,(AppKeyScope)s,"enter","Enter");
        (void)add_binding(config,(AppKeyScope)s,"backspace","Backspace");
        (void)add_binding(config,(AppKeyScope)s,"delete","Delete");
        (void)add_binding(config,(AppKeyScope)s,"paste","Ctrl+V");
        (void)add_binding(config,(AppKeyScope)s,"paste","Super+V");
    }
    (void)add_binding(config,APP_INSERT,"tab","Tab");
    (void)add_binding(config,APP_BROWSER,"up","k"); (void)add_binding(config,APP_BROWSER,"down","j");
    (void)add_binding(config,APP_BROWSER,"enter","Enter"); (void)add_binding(config,APP_BROWSER,"enter","l");
    (void)add_binding(config,APP_BROWSER,"parent","h");
    (void)add_binding(config,APP_BROWSER,"command",":");
    (void)add_binding(config,APP_INFO,"up","k"); (void)add_binding(config,APP_INFO,"down","j");
    (void)add_binding(config,APP_OUTPUT,"up","k"); (void)add_binding(config,APP_OUTPUT,"down","j");
    (void)add_binding(config,APP_OUTPUT,"cancel","Ctrl+C");
    static const struct { const char *action,*trigger; } workspace[] = {
        {"split_horizontal","Leader+%"},{"split_vertical","Leader+\""},
        {"focus_left","Leader+Left"},{"focus_right","Leader+Right"},
        {"focus_up","Leader+Up"},{"focus_down","Leader+Down"},
        {"resize_left","Leader+Ctrl+Left"},{"resize_left","Leader+H"},
        {"resize_right","Leader+Ctrl+Right"},{"resize_right","Leader+L"},
        {"resize_up","Leader+Ctrl+Up"},{"resize_up","Leader+K"},
        {"resize_down","Leader+Ctrl+Down"},{"resize_down","Leader+J"},
        {"next_pane","Leader+o"},{"output","Leader+c"},{"zoom","Leader+z"},
        {"close_pane","Leader+x"},{"world","Leader+g"},{"help","Leader+?"},
        {"config","Leader+,"},{"reload_config","Leader+r"}
    };
    for (size_t i=0;i<sizeof(workspace)/sizeof(workspace[0]);++i) (void)add_binding(config,APP_WORKSPACE,workspace[i].action,workspace[i].trigger);
    static const struct { const char *action,*trigger; } world[] = {
        {"forward","w"},{"backward","s"},{"left","a"},{"right","d"},
        {"up","Space"},{"down","LeftShift"},{"down","RightShift"},
        {"inventory","e"},{"pause","Escape"},{"command","/"},
        {"look_left","Left"},{"look_right","Right"},{"look_up","Up"},
        {"look_down","Down"},{"roll_left","q"},
        {"slot_1","1"},{"slot_2","2"},{"slot_3","3"},{"slot_4","4"},{"slot_5","5"},
        {"slot_6","6"},{"slot_7","7"},{"slot_8","8"},{"slot_9","9"}
    };
    for (size_t i=0;i<sizeof(world)/sizeof(world[0]);++i) (void)add_binding(config,APP_WORLD,world[i].action,world[i].trigger);
}

const AppSettings *app_config_current(void)
{
    if (!ready) { defaults(&current); ready=true; }
    return &current.settings;
}
const char *app_config_path(void) { return config_path; }
const char *app_config_error(void) { return config_error; }
unsigned app_config_revision(void) { return revision; }

static bool resolve_path(void)
{
    const char *override=getenv("MCMUX_CONFIG");
    if (override && *override) {
        if (strlen(override)>=sizeof(config_path)) return fail("MCMUX_CONFIG path is too long");
        bool absolute=override[0]=='/';
#ifdef _WIN32
        absolute=absolute || (override[0]=='\\' && override[1]=='\\') ||
            (isalpha((unsigned char)override[0]) && override[1]==':' && (override[2]=='/' || override[2]=='\\'));
#endif
        if (!absolute)
            return fail("MCMUX_CONFIG must be an absolute path");
        memcpy(config_path,override,strlen(override)+1); return true;
    }
    const char *base=getenv("XDG_CONFIG_HOME"),*suffix="/mcmux/config.toml";
    if (!base || !*base) {
#ifdef _WIN32
        base=getenv("APPDATA");
#else
        base=getenv("HOME"); suffix="/.config/mcmux/config.toml";
#endif
    }
    if (!base || !*base) return fail("cannot find user configuration directory; set MCMUX_CONFIG");
    int n=snprintf(config_path,sizeof(config_path),"%s%s",base,suffix);
    return (n>=0 && n<(int)sizeof(config_path)) || fail("configuration path is too long");
}
static bool config_value_error(const char *name,toml_datum_t value,const char *reason)
{
    return fail("%s:%d:%d: %s: %s",config_path,value.lineno,value.colno,name,reason);
}
static bool binding_string(toml_datum_t value)
{
    return value.type==TOML_STRING && (size_t)value.u.str.len==strlen(value.u.s);
}
static bool allowed_table(toml_datum_t table,const char *name,const char *allowed)
{
    if (table.type==TOML_UNKNOWN) return true;
    if (table.type!=TOML_TABLE) return config_value_error(name,table,"expected a table");
    for (int i=0;i<table.u.tab.size;++i) {
        char key[128]; snprintf(key,sizeof(key)," %s ",table.u.tab.key[i]);
        if ((size_t)table.u.tab.len[i]!=strlen(table.u.tab.key[i]) ||
            strpbrk(table.u.tab.key[i]," \t\r\n") || !strstr(allowed,key))
            return config_value_error(table.u.tab.key[i],table.u.tab.value[i],"unknown setting");
    }
    return true;
}
static bool number(toml_datum_t root,const char *name,double low,double high,double *out,bool integer)
{
    toml_datum_t v=toml_seek(root,name);
    if (v.type==TOML_UNKNOWN) return true;
    if (v.type!=TOML_INT64 && (integer || v.type!=TOML_FP64)) return config_value_error(name,v,"wrong numeric type");
    double n=v.type==TOML_INT64 ? (double)v.u.int64 : v.u.fp64;
    if (!isfinite(n) || n<low || n>high) return config_value_error(name,v,"value out of range");
    *out=n; return true;
}
static bool read_settings(Config *next,toml_datum_t root)
{
    if (!allowed_table(root,"config"," window  editor  output  input  world  workspace  keys ")) return false;
    static const struct { const char *table,*allowed; } fields[] = {
        {"window"," vsync  fps_limit  msaa_samples  fov "},
        {"editor"," font_size  tab_width "},{"output"," font_size "},
        {"input"," mouse_sensitivity  move_speed  scroll_lines "},
        {"world"," stream_radius "},{"workspace"," leader  leader_timeout_ms "}
    };
    for (size_t i=0;i<sizeof(fields)/sizeof(fields[0]);++i)
        if (!allowed_table(toml_seek(root,fields[i].table),fields[i].table,fields[i].allowed)) return false;
    AppSettings *s=&next->settings; double n;
#define INT_FIELD(path,field,low,high) n=s->field; if (!number(root,path,low,high,&n,true)) return false; s->field=(int)n
#define FLOAT_FIELD(path,field,low,high) n=s->field; if (!number(root,path,low,high,&n,false)) return false; s->field=(float)n
    INT_FIELD("window.fps_limit",fps_limit,0,1000);
    INT_FIELD("window.msaa_samples",msaa_samples,0,8);
    if (s->msaa_samples!=0 && s->msaa_samples!=2 && s->msaa_samples!=4 && s->msaa_samples!=8)
        return config_value_error("window.msaa_samples",toml_seek(root,"window.msaa_samples"),"use 0, 2, 4 or 8");
    FLOAT_FIELD("window.fov",fov,30,120);
    INT_FIELD("editor.font_size",editor_font_size,10,48);
    INT_FIELD("editor.tab_width",tab_width,1,16);
    INT_FIELD("output.font_size",output_font_size,10,48);
    INT_FIELD("world.stream_radius",stream_radius,1,16);
    INT_FIELD("workspace.leader_timeout_ms",leader_timeout_ms,0,10000);
    FLOAT_FIELD("input.mouse_sensitivity",mouse_sensitivity,0.0001,0.1);
    FLOAT_FIELD("input.move_speed",move_speed,0.1,100);
    FLOAT_FIELD("input.scroll_lines",scroll_lines,0.1,20);
#undef INT_FIELD
#undef FLOAT_FIELD
    toml_datum_t v=toml_seek(root,"window.vsync");
    if (v.type!=TOML_UNKNOWN) {
        if (v.type!=TOML_BOOLEAN) return config_value_error("window.vsync",v,"expected true or false");
        s->vsync=v.u.boolean;
    }
    v=toml_seek(root,"workspace.leader");
    if (v.type!=TOML_UNKNOWN && (!binding_string(v) || !parse_trigger(v.u.s,&next->leader) || next->leader.leader))
        return config_value_error("workspace.leader",v,"expected a single key or key chord");
    return true;
}
static bool read_bindings(Config *next,toml_datum_t root)
{
    toml_datum_t keys=toml_seek(root,"keys");
    if (!allowed_table(keys,"keys"," workspace  normal  visual  insert  command  browser  info  output  world ")) return false;
    for (int scope=0;scope<APP_SCOPE_COUNT;++scope) {
        char path[64]; snprintf(path,sizeof(path),"keys.%s",scope_names[scope]);
        toml_datum_t table=toml_seek(root,path);
        if (table.type==TOML_UNKNOWN) continue;
        if (table.type!=TOML_TABLE) return config_value_error(path,table,"expected a table");
        for (int i=0;i<table.u.tab.size;++i) {
            const char *name=table.u.tab.key[i]; toml_datum_t value=table.u.tab.value[i];
            if ((size_t)table.u.tab.len[i]!=strlen(name)) return config_value_error(path,value,"action must not contain NUL characters");
            int action=action_index((AppKeyScope)scope,name);
            if (action<0) return config_value_error(name,value,"unknown action in this scope");
            for (size_t j=0;j<next->count;)
                if ((int)next->bindings[j].scope==scope && (int)next->bindings[j].action==action)
                    next->bindings[j]=next->bindings[--next->count];
                else ++j;
            if (value.type==TOML_STRING) {
                if (!binding_string(value)) return config_value_error(name,value,"binding must not contain NUL characters");
                if (!add_binding(next,(AppKeyScope)scope,name,value.u.s)) {
                    char detail[768]; snprintf(detail,sizeof(detail),"%s",config_error);
                    return config_value_error(path,value,detail);
                }
                next->bindings[next->count-1].line=value.lineno;
                next->bindings[next->count-1].column=value.colno;
            } else if (value.type==TOML_ARRAY) {
                for (int j=0;j<value.u.arr.size;++j) {
                    toml_datum_t item=value.u.arr.elem[j];
                    if (!binding_string(item)) return config_value_error(name,item,"bindings must be strings without NUL characters");
                    if (!add_binding(next,(AppKeyScope)scope,name,item.u.s)) {
                        char detail[768]; snprintf(detail,sizeof(detail),"%s",config_error);
                        return config_value_error(path,item,detail);
                    }
                    next->bindings[next->count-1].line=item.lineno;
                    next->bindings[next->count-1].column=item.colno;
                }
            } else return config_value_error(name,value,"expected a string or array; [] disables the action");
        }
    }
    for (size_t i=0;i<next->count;++i) {
        Trigger *t=&next->bindings[i].trigger;
        if (!t->leader && t->key==next->leader.key && t->character==next->leader.character && t->mods==next->leader.mods)
            return fail("%s:%d:%d: keys.%s.%s: '%s' conflicts with workspace.leader",config_path,
                        next->bindings[i].line,next->bindings[i].column,scope_names[next->bindings[i].scope],
                        actions[next->bindings[i].action].name,t->text);
    }
    for (size_t i=0;i<next->count;++i) for (size_t j=0;j<i;++j) {
        Binding *a=&next->bindings[i],*b=&next->bindings[j];
        if (a->scope==b->scope && a->trigger.key==b->trigger.key &&
            a->trigger.character==b->trigger.character && a->trigger.mods==b->trigger.mods && a->trigger.leader==b->trigger.leader)
            return fail("%s:%d:%d: keys.%s: '%s' is bound to both %s and %s",config_path,
                        a->line ? a->line : b->line,a->line ? a->column : b->column,scope_names[a->scope],a->trigger.text,
                        actions[a->action].name,actions[b->action].name);
    }
    return true;
}
bool app_config_reload(void)
{
    (void)app_config_current(); config_error[0]='\0';
    if (!config_path[0] && !resolve_path()) return false;
    FILE *file=fopen(config_path,"rb");
    Config next; defaults(&next);
    if (!file) {
        if (errno==ENOENT) { current=next; ++revision; return true; }
        return fail("cannot open %s: %s",config_path,strerror(errno));
    }
    char *text=malloc(1024*1024+1);
    if (!text) { fclose(file); return fail("cannot allocate configuration buffer"); }
    size_t n=fread(text,1,1024*1024+1,file); bool bad=ferror(file)!=0;
    if (fclose(file)!=0) bad=true;
    if (bad || n>1024*1024 || memchr(text,0,n)) {
        free(text); return fail("%s: cannot read configuration (limit 1 MiB, no NUL bytes)",config_path);
    }
    toml_result_t result=toml_parse_named(text,(int)n,config_path); free(text);
    if (!result.ok) {
        bool ok=fail("%s: %s",config_path,result.errmsg); toml_free(result); return ok;
    }
    bool ok=read_settings(&next,result.toptab) && read_bindings(&next,result.toptab);
    toml_free(result);
    if (ok) { current=next; ++revision; }
    return ok;
}
bool app_config_init(void)
{
    defaults(&current); ready=true; config_path[0]='\0'; config_error[0]='\0';
    return resolve_path() && app_config_reload();
}
bool app_config_ensure_file(void)
{
    if (!config_path[0] && !resolve_path()) return false;
    FILE *existing=fopen(config_path,"rb");
    if (existing) { fclose(existing); return true; }
    if (errno!=ENOENT) return fail("cannot open %s: %s",config_path,strerror(errno));
    char path[PATH_LIMIT]; snprintf(path,sizeof(path),"%s",config_path);
    for (char *p=path+1;*p;++p) if (*p=='/' || *p=='\\') {
        if (p==path+2 && path[1]==':') continue;
        char c=*p;*p=0;
        if (config_mkdir(path)!=0 && errno!=EEXIST) return fail("cannot create %s: %s",path,strerror(errno));
        *p=c;
    }
    FILE *file=fopen(config_path,"wx");
    if (!file) return errno==EEXIST || fail("cannot create %s: %s",config_path,strerror(errno));
    Config base; defaults(&base);
    fputs("# mcmux configuration. Missing settings keep built-in defaults.\n"
        "# Save with :w, then use :reloadconfig. Errors keep the previous configuration.\n"
        "# MCMUX_CONFIG may override this file with an absolute path.\n"
        "# Key values are strings or arrays; [] disables an action.\n"
        "# Each action replaces its default bindings only in the named scope.\n"
        "# Leader+X means press the workspace leader, release it, then X.\n"
        "# Modifiers: Ctrl, Shift, Alt/Opt, Super/Cmd. Named keys: Escape, Enter,\n"
        "# Tab, Space, arrows, Home, End, PageUp, PageDown, Backspace, Delete, F1..F12.\n"
        "# A single printable character uses the typed character (keyboard-layout aware).\n"
        "# Example: [keys.normal] save = [\"Ctrl+S\"] ; run = [\"F5\"] (on separate lines).\n\n"
        "[window]\nvsync = true\nfps_limit = 60 # 0..1000; 0 removes the frame cap\n"
        "msaa_samples = 4 # 0, 2, 4, 8; unsupported hardware uses single-sample rendering\n"
        "fov = 65.0 # 30..120 degrees\n\n"
        "[editor]\nfont_size = 20 # 10..48\ntab_width = 4 # 1..16\n\n"
        "[output]\nfont_size = 20 # 10..48\n\n"
        "[input]\nmouse_sensitivity = 0.003 # 0.0001..0.1 radians per pixel\n"
        "move_speed = 5.4 # 0.1..100 blocks per second\nscroll_lines = 3.0 # 0.1..20\n\n"
        "[world]\nstream_radius = 5 # 1..16 chunks; affects the loaded view, not circuit semantics\n\n"
        "[workspace]\nleader = \"Ctrl+B\"\nleader_timeout_ms = 0 # 0 disables timeout\n",file);
    for (int scope=0;scope<APP_SCOPE_COUNT;++scope) {
        fprintf(file,"\n[keys.%s]\n",scope_names[scope]);
        for (size_t action=0;action<sizeof(actions)/sizeof(actions[0]);++action) {
            if (!(actions[action].scopes&(1u<<scope))) continue;
            fprintf(file,"%s = [",actions[action].name); bool first=true;
            for (size_t i=0;i<base.count;++i) {
                Binding *b=&base.bindings[i];
                if ((int)b->scope!=scope || b->action!=action) continue;
                fputs(first ? "\"" : ", \"",file); first=false;
                for (const char *p=b->trigger.text;*p;++p) {
                    if (*p=='\"' || *p=='\\') fputc('\\',file);
                    fputc(*p,file);
                }
                fputc('"',file);
            }
            fputs("]\n",file);
        }
    }
    bool ok=!ferror(file);
    if (fclose(file)!=0) ok=false;
    return ok || fail("cannot write %s",config_path);
}

size_t app_config_action_count(AppKeyScope scope)
{
    size_t count=0; for (size_t i=0;i<sizeof(actions)/sizeof(actions[0]);++i) if (actions[i].scopes&(1u<<scope)) ++count;
    return count;
}
const char *app_config_action_name(AppKeyScope scope,size_t index)
{
    for (size_t i=0;i<sizeof(actions)/sizeof(actions[0]);++i) if (actions[i].scopes&(1u<<scope)) {
        if (!index--) return actions[i].name;
    }
    return "";
}
void app_config_describe(AppKeyScope scope,const char *name,char *text,size_t capacity)
{
    if (!capacity) return;
    text[0]=0;
    for (size_t i=0;i<current.count;++i) {
        Binding *b=&current.bindings[i];
        if (b->scope!=scope || strcmp(actions[b->action].name,name)) continue;
        size_t used=strlen(text);
        const char *trigger=b->trigger.text;
        char expanded[140];
        if (b->trigger.leader) {
            snprintf(expanded,sizeof(expanded),"%s then %s",current.leader.text,trigger+7); trigger=expanded;
        }
        if (used<capacity) snprintf(text+used,capacity-used,"%s%s",used ? " / " : "",trigger);
    }
    if (!text[0]) snprintf(text,capacity,"unbound");
}

void app_input_frame(void)
{
    physical=(Input){0};
    for (int i=0;i<KEY_COUNT;++i) {
        physical.pressed[i]=IsKeyPressed(i); physical.repeat[i]=IsKeyPressedRepeat(i); physical.down[i]=IsKeyDown(i);
    }
    if (physical.down[KEY_LEFT_CONTROL]||physical.down[KEY_RIGHT_CONTROL]) physical.mods|=CTRL;
    if (physical.down[KEY_LEFT_SHIFT]||physical.down[KEY_RIGHT_SHIFT]) physical.mods|=SHIFT;
    if (physical.down[KEY_LEFT_ALT]||physical.down[KEY_RIGHT_ALT]) physical.mods|=ALT;
    if (physical.down[KEY_LEFT_SUPER]||physical.down[KEY_RIGHT_SUPER]) physical.mods|=SUPER;
    int c; while ((c=GetCharPressed())!=0) if (physical.count<TEXT_LIMIT) physical.chars[physical.count++]=c;
    logical=physical;
    reset_operator=false;
}
static bool match(const Trigger *t,bool held,bool repeat)
{
    if (held) {
        int key=t->key;
        if (!key && (isalnum(t->character) || strchr("[];',./`-=\\",t->character))) key=toupper(t->character);
        if (key<=0 || key>=KEY_COUNT || !physical.down[key]) return false;
        return (physical.mods&t->mods)==t->mods;
    }
    if (t->key) return physical.mods==t->mods &&
        (physical.pressed[t->key] || (repeat && physical.repeat[t->key]));
    if ((physical.mods&~SHIFT)!=t->mods) return false;
    for (int i=0;i<physical.count;++i) if (physical.chars[i]==t->character) return true;
    return false;
}
bool app_input_leader(void) { return match(&current.leader,false,false); }
const char *app_input_workspace(bool after_leader)
{
    for (size_t i=0;i<current.count;++i) {
        Binding *b=&current.bindings[i];
        if (b->scope==APP_WORKSPACE && b->trigger.leader==after_leader && match(&b->trigger,false,false))
            return actions[b->action].name;
    }
    return NULL;
}
bool app_input_world(const char *name,bool held)
{
    for (size_t i=0;i<current.count;++i) {
        Binding *b=&current.bindings[i];
        if (b->scope==APP_WORLD && !strcmp(actions[b->action].name,name) && match(&b->trigger,held,false)) return true;
    }
    return false;
}
static void emit_action(const Action *action,bool pressed,bool repeated,bool down)
{
    if (pressed && action->characters && strlen(action->characters)>1) reset_operator=true;
    if (action->key) {
        logical.pressed[action->key]|=pressed; logical.repeat[action->key]|=repeated; logical.down[action->key]|=down;
        if (action->mods&CTRL) logical.down[KEY_LEFT_CONTROL]=true;
        if (action->mods&SHIFT) logical.down[KEY_LEFT_SHIFT]=true;
        if (action->mods&ALT) logical.down[KEY_LEFT_ALT]=true;
        if (action->mods&SUPER) logical.down[KEY_LEFT_SUPER]=true;
    }
    if (pressed && action->command) logical.command=action->command;
    if (pressed && action->characters) for (const char *p=action->characters;*p;++p)
        if (logical.count<TEXT_LIMIT) logical.chars[logical.count++]=(unsigned char)*p;
}
void app_input_scope(AppKeyScope scope)
{
    logical=(Input){0};
    bool key_mapped=false;
    for (size_t i=0;i<current.count;++i) {
        Binding *b=&current.bindings[i];
        if (b->scope!=scope || !b->trigger.key || !match(&b->trigger,false,true)) continue;
        emit_action(&actions[b->action],physical.pressed[b->trigger.key],physical.repeat[b->trigger.key],physical.down[b->trigger.key]);
        key_mapped=true;
    }
    /* A named key/chord owns its text too (for example Space remapped to Enter). */
    if (key_mapped) return;
    for (int i=0;i<physical.count;++i) {
        int c=physical.chars[i]; bool mapped=false;
        for (size_t j=0;j<current.count;++j) {
            Binding *b=&current.bindings[j];
            if (b->scope!=scope || b->trigger.key || b->trigger.character!=c || (physical.mods&~SHIFT)!=b->trigger.mods) continue;
            emit_action(&actions[b->action],true,false,true); mapped=true; break;
        }
        if (!mapped && (scope==APP_INSERT || scope==APP_COMMAND) && !(physical.mods&(CTRL|SUPER)) && logical.count<TEXT_LIMIT)
            logical.chars[logical.count++]=c;
    }
}
const char *app_input_command(void) { return logical.command; }
bool app_input_resets_operator(void) { return reset_operator; }
int app_input_char(void) { return logical.next<logical.count ? logical.chars[logical.next++] : 0; }
bool app_input_pressed(int key) { return key>=0 && key<KEY_COUNT && logical.pressed[key]; }
bool app_input_repeat(int key) { return key>=0 && key<KEY_COUNT && logical.repeat[key]; }
bool app_input_down(int key) { return key>=0 && key<KEY_COUNT && logical.down[key]; }
void app_input_discard(void) { logical.next=logical.count; }
