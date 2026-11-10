#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "workspace_output.h"
#include "ui_font.h"
#include "app_config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOUSER
#define NOGDI
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

enum { OUTPUT_LINES=2048, OUTPUT_LINE_LENGTH=1024, OUTPUT_READ_BUDGET=65536,
       OUTPUT_HEADER_HEIGHT=25 };
#define OUTPUT_FONT_SIZE (app_config_current()->output_font_size)
#define OUTPUT_ROW_HEIGHT (OUTPUT_FONT_SIZE + 4)
typedef struct { char text[OUTPUT_LINE_LENGTH]; bool error; } OutputLine;
struct WorkspaceOutput {
    OutputLine lines[OUTPUT_LINES];
    size_t first,count,scroll,left,partial_length,visible_rows;
    char partial[OUTPUT_LINE_LENGTH];
    bool running,exited,eof,cancelling,program;
    int status;
    double cancel_time;
    Vector2 wheel_remainder;
    void *source;
    WorkspaceOutputReceiver receiver;
#ifdef _WIN32
    HANDLE pipe,process,job;
#else
    int pipe;
    pid_t process;
#endif
};

/* Keep a full page visible instead of scrolling past the oldest page. */
static size_t scroll_limit(const WorkspaceOutput *output)
{
    size_t available=output->count+(output->partial_length ? 1 : 0);
    size_t rows=output->visible_rows ? output->visible_rows : 1;
    return available>rows ? available-rows : 0;
}

static void clamp_scroll(WorkspaceOutput *output)
{
    size_t limit=scroll_limit(output);
    if (output->scroll>limit) output->scroll=limit;
}

static void push_line(WorkspaceOutput *output,const char *text,bool error)
{
    size_t index;
    if (output->count==OUTPUT_LINES) {
        index=output->first;
        output->first=(output->first+1)%OUTPUT_LINES;
    } else index=(output->first+output->count++)%OUTPUT_LINES;
    snprintf(output->lines[index].text,OUTPUT_LINE_LENGTH,"%s",text);
    output->lines[index].error=error;
    if (output->scroll && output->scroll<output->count-1) ++output->scroll;
}

void workspace_output_message(WorkspaceOutput *output,const char *text,bool error)
{
    if (!output || !text) return;
    while (*text) {
        char line[OUTPUT_LINE_LENGTH];
        const char *end=strchr(text,'\n');
        size_t length=end ? (size_t)(end-text) : strlen(text);
        if (length && text[length-1]=='\r') --length;
        if (!length) push_line(output,"",error);
        while (length) {
            size_t amount=length<sizeof(line)-1 ? length : sizeof(line)-1;
            memcpy(line,text,amount); line[amount]='\0';
            push_line(output,line,error);
            text+=amount; length-=amount;
        }
        if (!end) break;
        text=end+1;
    }
}

static void receive_text(WorkspaceOutput *output,const char *text)
{
    for (const char *p=text; *p; ++p) {
        if (*p=='\r') continue;
        if (*p=='\n') {
            output->partial[output->partial_length]='\0';
            push_line(output,output->partial,false);
            output->partial_length=0;
            continue;
        }
        if (output->partial_length==OUTPUT_LINE_LENGTH-1) {
            size_t complete=output->partial_length;
            if (((unsigned char)*p&0xc0)==0x80) {
                while (complete && ((unsigned char)output->partial[complete-1]&0xc0)==0x80) --complete;
                if (complete && ((unsigned char)output->partial[complete-1]&0x80)) --complete;
                if (!complete) complete=output->partial_length;
            }
            char saved=output->partial[complete];
            output->partial[complete]='\0'; push_line(output,output->partial,false);
            output->partial[complete]=saved;
            output->partial_length-=complete;
            memmove(output->partial,output->partial+complete,output->partial_length);
        }
        output->partial[output->partial_length++]=*p;
    }
    output->partial[output->partial_length]='\0';
    if (output->source && output->receiver)
        output->receiver(output->source,text,false,false);
}

WorkspaceOutput *workspace_output_create(void)
{
    WorkspaceOutput *output=calloc(1,sizeof(*output));
#ifndef _WIN32
    if (output) output->pipe=-1;
#endif
    return output;
}

static void close_process(WorkspaceOutput *output)
{
#ifdef _WIN32
    if (output->pipe) CloseHandle(output->pipe);
    if (output->process) CloseHandle(output->process);
    if (output->job) CloseHandle(output->job);
    output->pipe=output->process=output->job=NULL;
#else
    if (output->pipe>=0) close(output->pipe);
    output->pipe=-1; output->process=0;
#endif
}

void workspace_output_destroy(WorkspaceOutput *output)
{
    if (!output) return;
#ifdef _WIN32
    if (output->running) TerminateJobObject(output->job,130);
#else
    if (output->running && output->process>0) {
        kill(-output->process,SIGKILL);
        if (!output->exited) while (waitpid(output->process,NULL,0)<0 && errno==EINTR) {}
    }
#endif
    close_process(output);
    free(output);
}

void workspace_output_detach(WorkspaceOutput *output,void *source)
{
    if (output && output->source==source) {
        output->source=NULL; output->receiver=NULL;
    }
}

bool workspace_output_is_running(const WorkspaceOutput *output)
{
    return output && output->running;
}

bool workspace_output_is_program_running(const WorkspaceOutput *output)
{
    return output && output->running && output->program;
}

#ifdef _WIN32
static char *program_command_line(const char *const *arguments)
{
    size_t capacity=1;
    for (size_t i=0; arguments[i]; ++i) {
        size_t length=strlen(arguments[i]);
        if (capacity>SIZE_MAX-3 || length>(SIZE_MAX-capacity-3)/2) return NULL;
        capacity+=2*length+3;
    }
    char *line=malloc(capacity),*cursor=line;
    if (!line) return NULL;
    for (size_t i=0; arguments[i]; ++i) {
        if (i) *cursor++=' ';
        *cursor++='"';
        size_t slashes=0;
        for (const char *p=arguments[i]; ; ++p) {
            if (*p=='\\') { ++slashes; continue; }
            size_t count=(*p=='"' || !*p) ? 2*slashes : slashes;
            while (count--) *cursor++='\\';
            slashes=0;
            if (!*p) break;
            if (*p=='"') *cursor++='\\';
            *cursor++=*p;
        }
        *cursor++='"';
    }
    *cursor='\0';
    return line;
}
#endif

static bool launch_process(WorkspaceOutput *output,const char *command,
    const char *const *arguments,const char *directory,char *error,size_t capacity)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
    HANDLE reader=NULL,writer=NULL,input=INVALID_HANDLE_VALUE,job=NULL;
    PROCESS_INFORMATION process={0};
    char *line=NULL;
    DWORD failure=0;
    if (!CreatePipe(&reader,&writer,&security,0) ||
        !SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0)) goto fail;
    input=CreateFileA("NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        &security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (input==INVALID_HANDLE_VALUE) goto fail;
    job=CreateJobObjectA(NULL,NULL);
    if (!job) goto fail;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) goto fail;
    size_t length=command ? strlen(command) : 0;
    line=arguments ? program_command_line(arguments) : malloc(length+32);
    if (!line) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto fail; }
    if (!arguments) snprintf(line,length+32,"cmd.exe /d /s /c \"%s\"",command);
    STARTUPINFOA startup={0};
    startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=input; startup.hStdOutput=startup.hStdError=writer;
    if (!CreateProcessA(NULL,line,NULL,NULL,TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED,NULL,directory,&startup,&process)) goto fail;
    if (!AssignProcessToJobObject(job,process.hProcess) ||
        ResumeThread(process.hThread)==(DWORD)-1) goto fail;
    free(line); CloseHandle(input); CloseHandle(writer); CloseHandle(process.hThread);
    output->pipe=reader; output->process=process.hProcess; output->job=job;
    return true;
fail:
    failure=GetLastError();
    if (process.hProcess) { TerminateProcess(process.hProcess,1); CloseHandle(process.hProcess); }
    if (process.hThread) CloseHandle(process.hThread);
    if (reader) CloseHandle(reader);
    if (writer) CloseHandle(writer);
    if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);
    if (job) CloseHandle(job);
    free(line);
    snprintf(error,capacity,"cannot start external command (Windows error %lu)",(unsigned long)failure);
    return false;
#else
    char **program_arguments=NULL;
    if (arguments) {
        size_t count=0;
        while (arguments[count]) ++count;
        program_arguments=calloc(count+6,sizeof(*program_arguments));
        if (!program_arguments) goto fail;
        program_arguments[0]="sh"; program_arguments[1]="-c";
        program_arguments[2]="cd \"$1\" && shift && exec \"$@\"";
        program_arguments[3]="mc-nvim";
        program_arguments[4]=(char *)directory;
        for (size_t i=0; i<count; ++i) program_arguments[i+5]=(char *)arguments[i];
    }
    int descriptors[2];
    if (pipe(descriptors)<0) goto fail;
    /* GUI launches may have closed standard descriptors. Keep the pipe away
     * from 0..2 so spawn's dup/close actions cannot close a redirected stream. */
    for (int i=0;i<2;++i) if (descriptors[i]<3) {
        int moved=fcntl(descriptors[i],F_DUPFD,3);
        if (moved<0) {
            int saved=errno; close(descriptors[0]); close(descriptors[1]); errno=saved; goto fail;
        }
        close(descriptors[i]); descriptors[i]=moved;
    }
    int flags=fcntl(descriptors[0],F_GETFL);
    if (flags<0 || fcntl(descriptors[0],F_SETFL,flags|O_NONBLOCK)<0 ||
        fcntl(descriptors[0],F_SETFD,FD_CLOEXEC)<0 ||
        fcntl(descriptors[1],F_SETFD,FD_CLOEXEC)<0) {
        int saved=errno; close(descriptors[0]); close(descriptors[1]); errno=saved; goto fail;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int result=posix_spawn_file_actions_init(&actions);
    if (result) { close(descriptors[0]); close(descriptors[1]); errno=result; goto fail; }
    result=posix_spawnattr_init(&attributes);
    if (result) { posix_spawn_file_actions_destroy(&actions); close(descriptors[0]); close(descriptors[1]); errno=result; goto fail; }
    result=posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    if (!result) result=posix_spawn_file_actions_adddup2(&actions,descriptors[1],STDOUT_FILENO);
    if (!result) result=posix_spawn_file_actions_adddup2(&actions,descriptors[1],STDERR_FILENO);
    if (!result) result=posix_spawn_file_actions_addclose(&actions,descriptors[0]);
    if (!result) result=posix_spawn_file_actions_addclose(&actions,descriptors[1]);
    if (!result) result=posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP);
    if (!result) result=posix_spawnattr_setpgroup(&attributes,0);
    char *shell_arguments[]={"sh","-c",(char *)command,NULL};
    pid_t child=0;
    if (!result) result=posix_spawn(&child,"/bin/sh",&actions,&attributes,
        program_arguments ? program_arguments : shell_arguments,environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    close(descriptors[1]);
    free(program_arguments); program_arguments=NULL;
    if (result) { close(descriptors[0]); errno=result; goto fail; }
    output->pipe=descriptors[0]; output->process=child;
    return true;
fail: {
    int failure=errno;
    free(program_arguments);
    snprintf(error,capacity,"cannot start external command: %s",strerror(failure));
    return false;
}
#endif
}

static bool start_process(WorkspaceOutput *output,const char *command,
    const char *const *arguments,const char *directory,const char *label,
    void *source,WorkspaceOutputReceiver receiver,char *error,size_t capacity)
{
    if (!output || output->running) {
        snprintf(error,capacity,"an external command is already running; stop the task in mc-output");
        return false;
    }
    if (!launch_process(output,command,arguments,directory,error,capacity)) return false;
    output->running=true; output->exited=output->eof=output->cancelling=false;
    output->program=arguments!=NULL;
    output->source=source; output->receiver=receiver; output->partial_length=0;
    output->partial[0]='\0'; output->status=0;
    output->scroll=0;
    workspace_output_message(output,label,false);
    return true;
}

bool workspace_output_start(WorkspaceOutput *output,const char *command,
    const char *label,void *source,WorkspaceOutputReceiver receiver,
    char *error,size_t capacity)
{
    return start_process(output,command,NULL,NULL,label,source,receiver,error,capacity);
}

bool workspace_output_start_program(WorkspaceOutput *output,
    const char *const *arguments,const char *directory,const char *label,
    void *source,WorkspaceOutputReceiver receiver,char *error,size_t capacity)
{
    if (!arguments || !arguments[0] || !*arguments[0] || !directory || !*directory) {
        snprintf(error,capacity,"a program and working directory are required");
        return false;
    }
    return start_process(output,NULL,arguments,directory,label,source,receiver,error,capacity);
}

void workspace_output_cancel(WorkspaceOutput *output)
{
    if (!output || !output->running || output->cancelling) return;
    output->cancelling=true; output->cancel_time=GetTime();
#ifdef _WIN32
    TerminateJobObject(output->job,130);
#else
    kill(-output->process,SIGTERM);
#endif
}

void workspace_output_poll(WorkspaceOutput *output)
{
    if (!output || !output->running) return;
    size_t budget=OUTPUT_READ_BUDGET;
    while (!output->eof && budget) {
        char text[4097];
        size_t size=sizeof(text)-1;
        if (size>budget) size=budget;
#ifdef _WIN32
        DWORD available=0,received=0;
        if (!PeekNamedPipe(output->pipe,NULL,0,NULL,&available,NULL)) { output->eof=true; break; }
        if (!available) break;
        if (size>available) size=available;
        if (!ReadFile(output->pipe,text,(DWORD)size,&received,NULL) || !received) { output->eof=true; break; }
        size=received;
#else
        ssize_t received=read(output->pipe,text,size);
        if (received<0) {
            if (errno==EINTR) continue;
            if (errno!=EAGAIN && errno!=EWOULDBLOCK) output->eof=true;
            break;
        }
        if (!received) { output->eof=true; break; }
        size=(size_t)received;
#endif
        for (size_t i=0;i<size;++i) if (!text[i]) text[i]='?';
        text[size]='\0'; receive_text(output,text); budget-=size;
    }
#ifdef _WIN32
    if (!output->exited && WaitForSingleObject(output->process,0)==WAIT_OBJECT_0) {
        DWORD status=1;
        GetExitCodeProcess(output->process,&status);
        output->status=(int)status; output->exited=true;
    }
#else
    if (output->cancelling && GetTime()-output->cancel_time>0.25)
        kill(-output->process,SIGKILL);
    if (!output->exited) {
        int status;
        pid_t waited=waitpid(output->process,&status,WNOHANG);
        if (waited==output->process) {
            output->status=WIFEXITED(status) ? WEXITSTATUS(status) :
                WIFSIGNALED(status) ? 128+WTERMSIG(status) : 1;
            output->exited=true;
        } else if (waited<0 && errno!=EINTR) { output->status=1; output->exited=true; }
    }
#endif
    if (!output->exited || !output->eof) return;
    if (output->partial_length) {
        push_line(output,output->partial,false); output->partial_length=0; output->partial[0]='\0';
    }
    char message[128];
    snprintf(message,sizeof(message),"external command %s: %d",
        output->status ? "failed" : "finished",output->status);
    push_line(output,message,output->status!=0);
    if (output->source && output->receiver)
        output->receiver(output->source,message,output->status!=0,true);
    close_process(output); output->running=false; output->source=NULL; output->receiver=NULL;
}

void workspace_output_update(WorkspaceOutput *output)
{
    if (!output) return;
    app_input_scope(APP_OUTPUT);
    bool control=app_input_down(KEY_LEFT_CONTROL) || app_input_down(KEY_RIGHT_CONTROL);
    if (control && app_input_pressed(KEY_C)) workspace_output_cancel(output);
    if (app_input_pressed(KEY_PAGE_UP)) output->scroll+=10;
    if (app_input_pressed(KEY_PAGE_DOWN)) output->scroll=output->scroll>10 ? output->scroll-10 : 0;
    if (app_input_pressed(KEY_UP) || app_input_repeat(KEY_UP) || app_input_pressed(KEY_K)) ++output->scroll;
    if (app_input_pressed(KEY_DOWN) || app_input_repeat(KEY_DOWN) || app_input_pressed(KEY_J))
        output->scroll=output->scroll ? output->scroll-1 : 0;
    if (app_input_pressed(KEY_HOME)) output->scroll=scroll_limit(output);
    if (app_input_pressed(KEY_END)) output->scroll=0;
    if (app_input_pressed(KEY_LEFT) || app_input_repeat(KEY_LEFT)) output->left=output->left>4 ? output->left-4 : 0;
    if (app_input_pressed(KEY_RIGHT) || app_input_repeat(KEY_RIGHT)) output->left+=4;
    if (output->left>OUTPUT_LINE_LENGTH) output->left=OUTPUT_LINE_LENGTH;
    clamp_scroll(output);
    while (app_input_char()!=0) {}
}

void workspace_output_scroll(WorkspaceOutput *output, Vector2 wheel)
{
    if (!output) return;
    output->wheel_remainder.x += wheel.x*app_config_current()->scroll_lines;
    output->wheel_remainder.y += wheel.y*app_config_current()->scroll_lines;
    int x=(int)output->wheel_remainder.x, y=(int)output->wheel_remainder.y;
    output->wheel_remainder.x-=x; output->wheel_remainder.y-=y;
    if (y>0) output->scroll+=(size_t)y;
    else if (y<0) output->scroll=output->scroll>(size_t)-y ? output->scroll-(size_t)-y : 0;
    if (x<0) output->left+=(size_t)-x;
    else if (x>0) output->left=output->left>(size_t)x ? output->left-(size_t)x : 0;
    if (output->left>OUTPUT_LINE_LENGTH) output->left=OUTPUT_LINE_LENGTH;
    clamp_scroll(output);
}

void workspace_output_draw(WorkspaceOutput *output,Rectangle box,int pane,bool focused,bool zoomed)
{
    if (!output || box.width<2 || box.height<2) return;
    Color accent=focused ? (Color){172,143,255,255} : (Color){118,130,143,255};
    DrawRectangleRec(box,(Color){22,27,36,255});
    BeginScissorMode((int)box.x,(int)box.y,(int)box.width,(int)box.height);
    DrawRectangle((int)box.x,(int)box.y,(int)box.width,OUTPUT_HEADER_HEIGHT,(Color){34,42,53,255});
    char title[96];
    snprintf(title,sizeof(title),"%d: mc-output%s%s%s",pane,output->running ? " [running]" : "",zoomed ? " [zoom]" : "",focused ? " *" : "");
    ui_text_draw(title,(int)box.x+8,(int)box.y+5,16,accent);
    int bottom=(int)(box.y+box.height)-OUTPUT_HEADER_HEIGHT;
    int rows=(bottom-(int)box.y-OUTPUT_HEADER_HEIGHT-4)/OUTPUT_ROW_HEIGHT;
    if (rows<0) rows=0;
    output->visible_rows=(size_t)rows;
    clamp_scroll(output);
    size_t available=output->count+(output->partial_length ? 1 : 0);
    size_t end=available>output->scroll ? available-output->scroll : 0;
    size_t start=end>(size_t)rows ? end-(size_t)rows : 0;
    for (size_t row=start; row<end; ++row) {
        const OutputLine *line=row<output->count ? &output->lines[(output->first+row)%OUTPUT_LINES] : NULL;
        const char *text=line ? line->text : output->partial;
        size_t offset=0,column=0;
        while (text[offset] && column<output->left) {
            ++offset;
            while (((unsigned char)text[offset]&0xc0)==0x80) ++offset;
            ++column;
        }
        ui_text_draw(text+offset,(int)box.x+8,(int)box.y+OUTPUT_HEADER_HEIGHT+3+(int)(row-start)*OUTPUT_ROW_HEIGHT,
            OUTPUT_FONT_SIZE,line && line->error ? (Color){255,128,128,255} : (Color){211,222,231,255});
    }
    DrawRectangle((int)box.x,bottom,(int)box.width,OUTPUT_HEADER_HEIGHT,(Color){44,38,63,255});
    char stop[128],follow[128],footer[320];
    app_config_describe(APP_OUTPUT,"cancel",stop,sizeof(stop));
    app_config_describe(APP_OUTPUT,"end",follow,sizeof(follow));
    snprintf(footer,sizeof(footer),"%s follow | %s stop",follow,stop);
    ui_text_draw(footer,(int)box.x+7,bottom+5,14,accent);
    EndScissorMode();
    DrawRectangleLinesEx(box,focused ? 2 : 1,accent);
}
