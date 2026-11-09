// Linux HMI PluginApp. Loaded by the native plugin manager.
#define _GNU_SOURCE 1
#include "native_abi.hpp"
#include "window_ipc.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <spawn.h>
#include <signal.h>
#include "profile.hpp" // Build IDs and symbol anchors for the supported HMI ABI.
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

namespace {
const unsigned Screen=10, LayerModel=14, SurfaceModel=15;
class Tick : public bmw::Runnable { public: void run(); };
Tick tick;
const unsigned FocusEvent=1, VisibleEvent=11, PrimarySlots=34;
struct State {
    bmw::PluginApp* app;
    void** nativeVtable;
    void* ownVtable[PrimarySlots+2];
    bool initialized, focused, visible, failed, frameSubmitted, deactivating;
    unsigned logLines;
    int log, ipc, control;
    pid_t child;
    unsigned layer, surface;
    bool starting, running, dirty, stopRequested, controlBound, uninstalled, clearing, errorQuiesced;
    unsigned ticks;
    char socketPath[108], directory[512];
} state={};

void logEvent(const char* event, const char* format, ...) {
    // Bound disk use; no infinite logging and no stdout/pipe writes in HMI.
    if (state.log<0 || state.logLines++>=512) return;
    struct timespec now={}; clock_gettime(CLOCK_MONOTONIC,&now);
    char line[512];
    int n=snprintf(line,sizeof(line),"mono=%ld.%03ld event=%s ",
                   now.tv_sec,now.tv_nsec/1000000,event);
    if (n<0 || static_cast<size_t>(n)>=sizeof(line)-2) return;
    va_list args; va_start(args,format);
    int m=vsnprintf(line+n,sizeof(line)-n-2,format,args); va_end(args);
    if (m<0) return;
    size_t used=static_cast<size_t>(n)+static_cast<size_t>(m);
    if (used>sizeof(line)-2) used=sizeof(line)-2;
    line[used++]='\n';
    // A single bounded write to our regular /tmp file. Failure is diagnostic,
    // never a reason to retry on the HMI thread.
    (void)write(state.log,line,used);
}

#include "profile_gate.hpp"
bool knownAbi() {
    // Accept only the supported HMI/AppFramework/CoreFramework build IDs.
    // Build IDs identify the ABI profile; they do not verify a signature.
    if (!sameBuild("/proc/self/exe",HmiBuildId,sizeof(HmiBuildId))) return false;
    const char* symbols[]={AppAnchor,CoreAnchor};
    const unsigned char* ids[]={AppBuildId,CoreBuildId};
    const unsigned sizes[]={sizeof(AppBuildId),sizeof(CoreBuildId)};
    for (unsigned i=0;i<2;++i) {
        void* p=dlsym(RTLD_DEFAULT,symbols[i]); Dl_info info={};
        if (!p || !dladdr(p,&info) || !info.dli_fname ||
            !sameBuild(info.dli_fname,ids[i],sizes[i])) return false;
    }
    return true;
}

bool sendState() {
    if (state.ipc<0 || !state.running) return false;
    ow_packet p={OW_MAGIC,OW_STATE,state.layer,state.surface,{0,0,0},static_cast<unsigned>(state.focused && state.visible && !state.failed)};
    ssize_t n=send(state.ipc,&p,sizeof(p),MSG_DONTWAIT|MSG_NOSIGNAL);
    if (n==sizeof(p)) { state.dirty=false; return true; }
    // Keep only latest UI state on EAGAIN, never queue an unbounded history.
    if (n<0 && (errno==EAGAIN || errno==EINTR)) return false;
    state.failed=true; logEvent("PRODUCER_LOST","send errno=%d",errno); return false;
}
bool publishState(const char* reason) {
    if (!state.app || state.failed || !state.initialized) return false;
    state.dirty=true;
    logEvent("STATE","reason=%s focused=%u visible=%u",reason,state.focused,state.visible);
    return sendState();
}
void clearModels() {
    if (!state.initialized || !state.app || state.clearing) return;
    state.clearing=true;
    try { state.app->updateModel(SurfaceModel,0); state.app->updateModel(LayerModel,0); }
    catch (...) { logEvent("EXCEPTION","clear models"); }
    state.clearing=false;
}
void stopProducer() {
    state.focused=false; state.running=false; state.starting=false; state.dirty=false;
    state.frameSubmitted=false;
    clearModels();
    if (state.ipc>=0) {
        ow_packet p={};p.magic=OW_MAGIC;p.type=OW_STOP;
        (void)send(state.ipc,&p,sizeof(p),MSG_DONTWAIT|MSG_NOSIGNAL);
        close(state.ipc);state.ipc=-1;
    }
    // No blocking wait on the HMI thread. Tick reaps only our child.
    state.stopRequested=true;
    logEvent("STOP","producer requested; model IDs cleared");
}
void nativeDeactivate(const char* reason) {
    if (!state.app || state.deactivating) return;
    state.deactivating=true;
    logEvent("DEACTIVATE_ENTER","reason=%s",reason);
    try {
        state.app->deactivate();
        logEvent("DEACTIVATE_EXIT","reason=%s returned=1",reason);
    } catch (...) {state.failed=true;logEvent("DEACTIVATE_EXCEPTION","reason=%s",reason);}
    state.deactivating=false;
}
bool startProducer() {
    if (state.child>0 || state.ipc>=0 || state.failed) return false;
    int pair[2];if (socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,pair)) return false;
    // Reserve a source fd above 3 before spawn dup2, even if socketpair used 3.
    int source=fcntl(pair[1],F_DUPFD_CLOEXEC,4);close(pair[1]);
    if (source<0) { close(pair[0]);return false; }
    char executable[640];snprintf(executable,sizeof(executable),"%s/ownandroidauto-producer.elf",state.directory);
    char logPath[640];snprintf(logPath,sizeof(logPath),"/tmp/ownandroidauto-producer-%ld.log",static_cast<long>(getpid()));
    int log=open(logPath,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    struct stat st={};
    if (log<0 || fstat(log,&st) || !S_ISREG(st.st_mode) || st.st_size>262144) {
        if(log>=0)close(log);close(source);close(pair[0]);return false;
    }
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,source,3);
    posix_spawn_file_actions_adddup2(&actions,log,2);
    if (pair[0]!=3) posix_spawn_file_actions_addclose(&actions,pair[0]);
    posix_spawn_file_actions_addclose(&actions,source);
    if (log!=3 && log!=2) posix_spawn_file_actions_addclose(&actions,log);
    char* argv[]={executable,const_cast<char*>("--plugin-fd3"),0};

    int rc=posix_spawn(&state.child,executable,&actions,0,argv,environ);
    posix_spawn_file_actions_destroy(&actions);close(source);close(log);
    if (rc) { close(pair[0]);state.child=0;logEvent("SPAWN_ERROR","code=%d",rc);return false; }
    state.ipc=pair[0];state.starting=true;state.ticks=0;state.stopRequested=false;state.frameSubmitted=false;
    logEvent("SPAWN","pid=%ld",static_cast<long>(state.child));return true;
}
void requestOpen() {
    if (!state.initialized || state.failed || state.stopRequested) return;
    // Native focus event targets only the application's own screen; HMI keeps
    // authority to accept/reject navigation. No controller/global focus calls.
    if (!state.child && !startProducer()) { state.failed=true;return; }
    try {
        if (!state.app->activate()) { logEvent("OPEN_REJECTED","activate");return; }
        if (!state.app->triggerEvent(80)) logEvent("OPEN_REJECTED","event=80");
    }
    catch (...) { state.failed=true;logEvent("EXCEPTION","open"); }
}
void Tick::run() {
    if (!state.initialized || !state.app) return;
    ++state.ticks;
    if (state.failed && !state.errorQuiesced) {
        state.errorQuiesced=true;
        stopProducer();
        logEvent("ERROR_QUIESCED","native deactivate not called; owner MENU required");
    }
    if (state.child>0) {
        int status=0;pid_t p=waitpid(state.child,&status,WNOHANG);
        if (p==state.child || (p<0 && errno==ECHILD)) {
            // A host SIGCHLD reaper may consume the status. Do not signal a
            // stale PID or claim successful cleanup without an exit status.
            if (p<0) status=-1;
            state.child=0;state.running=state.starting=false;
            if (state.ipc>=0) { close(state.ipc);state.ipc=-1; }
            clearModels();logEvent("CHILD_EXIT","status=%d",status);
            if (status!=0) state.failed=true;
            if (!state.stopRequested) {
                state.focused=false;state.failed=true;
                logEvent("CHILD_ENDED","native deactivate not called; owner MENU required");
            }
        }
    }
    if (state.ipc>=0) {
        ow_packet p={};ssize_t n=recv(state.ipc,&p,sizeof(p),MSG_DONTWAIT);
        if (n==sizeof(p) && p.magic==OW_MAGIC && p.type==OW_READY && state.starting &&
            p.layer>0 && p.surface>0 && p.layer==(p.surface&~15u) && p.surface<0x80000000u) {
            state.layer=p.layer;state.surface=p.surface;state.running=true;state.starting=false;state.dirty=true;
            try {
                bool a=state.app->updateModel(LayerModel,static_cast<int>(p.layer));
                bool b=state.app->updateModel(SurfaceModel,static_cast<int>(p.surface));
                if (!a || !b) { state.failed=true;stopProducer(); }
            } catch (...) { state.failed=true;stopProducer(); }
            logEvent("SURFACE_BOUND","layer=%u surface=%u",p.layer,p.surface);
        } else if (n==sizeof(p) && p.magic==OW_MAGIC && p.type==OW_FRAME && state.running &&
                   p.layer==state.layer && p.surface==state.surface) {
            state.frameSubmitted=true;
            logEvent("FRAME_SUBMITTED","layer=%u surface=%u swap_accepted=1 LCD_not_verified=1",p.layer,p.surface);
        } else if ((n>0) || n==0 || (n<0 && errno!=EAGAIN && errno!=EINTR)) {
            logEvent("PRODUCER_STATUS","bytes=%d type=%u",static_cast<int>(n),p.type);
            stopProducer();
            logEvent("PRODUCER_QUIESCED","native deactivate not called; owner MENU required");
        }
        if (state.starting && state.ticks>150) { logEvent("START_TIMEOUT","");stopProducer(); }
    }
    // At most four local control commands per tick; no disk polling/threads.
    for (unsigned i=0;i<4 && state.control>=0;++i) {
        char command[16]={};sockaddr_un peer={};socklen_t peerSize=sizeof(peer);
        ssize_t n=recvfrom(state.control,command,sizeof(command)-1,MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&peer),&peerSize);
        if (n<0) break;
        if (n==4 && !memcmp(command,"OPEN",4) && !state.uninstalled) {
            state.stopRequested=false;
            if (!state.child) {state.failed=false;state.errorQuiesced=false;}
            requestOpen();
        } else if (n==4 && !memcmp(command,"QUIE",4)) {
            stopProducer();
            logEvent("QUIESCE","producer only; no native deactivate; owner MENU required");
        } else if (n==4 && (!memcmp(command,"STOP",4) || !memcmp(command,"UNIN",4))) {
            if (!memcmp(command,"UNIN",4)) state.uninstalled=true;
            stopProducer();
            nativeDeactivate(!memcmp(command,"UNIN",4)?"UNIN":"STOP");
        }
        if (n==4 && (!memcmp(command,"OPEN",4) || !memcmp(command,"STOP",4) || !memcmp(command,"STAT",4) || !memcmp(command,"UNIN",4) || !memcmp(command,"QUIE",4))) {
            char reply[160];int len=snprintf(reply,sizeof(reply),"STATE %u %u %u %u %u %u %ld %u %u %u",state.initialized,
                state.focused,state.visible,state.failed,state.starting,state.running,static_cast<long>(state.child),state.stopRequested,
                state.frameSubmitted,OW_BUILD_REVISION);
            (void)sendto(state.control,reply,len,MSG_DONTWAIT|MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&peer),peerSize);
        }
    }
    if (state.dirty && state.running) sendState();
    try { state.app->post(&tick,100); } catch (...) { state.failed=true;stopProducer(); }
}
bool focus(void*,const bmw::Parcel& value) {
    bool focused=false;
    try { value.readBool(focused); } catch (...) { state.failed=true; }
    state.focused=focused && !state.failed;
    if (state.failed) stopProducer();
    logEvent("FOCUS","value=%u",state.focused);
    if (state.focused && !state.child && !state.stopRequested && !startProducer()) state.failed=true;
    publishState("focus");
    // Focus loss never sends a request to reclaim focus or restore screen order.
    return true;
}
bool visible(void*,const bmw::Parcel& value) {
    bool shown=false;
    try { value.readBool(shown); } catch (...) { state.failed=true; }
    state.visible=shown && !state.failed;
    if (state.failed) stopProducer();
    logEvent("VISIBLE","value=%u",state.visible); publishState("visible");return true;
}
bool initialize(bmw::PluginApp* self) {
    try {
        bmw::ActionDelegate f={self,focus}, v={self,visible};
        self->addHandler(Screen,FocusEvent,f); self->addHandler(Screen,VisibleEvent,v);
        state.initialized=self->onInit();
        logEvent("INIT","base_ok=%u screen=10",state.initialized);
        if (state.initialized) { clearModels();self->post(&tick,100); }
        return state.initialized && !state.failed;
    } catch (...) { state.failed=true; logEvent("EXCEPTION","init"); return false; }
}
void finalize(bmw::PluginApp* self) {
    stopProducer();
    state.initialized=state.focused=state.visible=false;
    logEvent("FINALIZE","owner stopped");
    try { self->onFinalize(); } catch (...) { logEvent("EXCEPTION","finalize"); }
}
void onError(bmw::PluginApp*,unsigned code,const bmw::BasicString<char>& text) {
    state.failed=true;stopProducer();
    logEvent("FRAMEWORK_ERROR","code=%u text=%.250s",code,text.data()?text.data():"");
}
bool copyVtable(bmw::PluginApp* app) {
    void** table=*reinterpret_cast<void***>(app);
    void** expected=static_cast<void**>(dlsym(RTLD_DEFAULT,"_ZTVN3bmw9PluginAppE"));
    if (!expected || table!=expected+2) return false;
    const unsigned slots[]={21,22,23};
    const char* names[]={"_ZN3bmw9PluginApp7onErrorEjRKNS_11BasicStringIcEE",
                         "_ZN3bmw9PluginApp6onInitEv","_ZN3bmw9PluginApp10onFinalizeEv"};
    for (unsigned i=0;i<3;++i) if (table[slots[i]]!=dlsym(RTLD_DEFAULT,names[i])) return false;
    state.nativeVtable=table;
    memcpy(state.ownVtable,table-2,sizeof(state.ownVtable));
    state.ownVtable[2+21]=reinterpret_cast<void*>(onError);
    state.ownVtable[2+22]=reinterpret_cast<void*>(initialize);
    state.ownVtable[2+23]=reinterpret_cast<void*>(finalize);
    *reinterpret_cast<void***>(app)=state.ownVtable+2;
    return true;
}
bmw::PluginApp* create(bmw::IDispatcher& dispatcher,const char* name,
                       const bmw::Bundle& parameters,const bmw::Bundle& config) {
    if (state.app) return 0;
    memset(&state,0,sizeof(state)); state.log=state.ipc=state.control=-1;
    char path[96]; snprintf(path,sizeof(path),"/tmp/ownandroidauto-native-%ld.log",static_cast<long>(getpid()));
    state.log=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if (state.log>=0) {
        struct stat st={};
        if (fstat(state.log,&st) || !S_ISREG(st.st_mode) || st.st_size>262144) { close(state.log); state.log=-1; }
    }
    if (!knownAbi()) {
        logEvent("ABI_REJECTED","requires exact installed-HU HMI/AppFramework/CoreFramework build IDs");
        if (state.log>=0) close(state.log); state.log=-1; return 0;
    }
    Dl_info own={};char resolved[512];
    if (!dladdr(reinterpret_cast<void*>(create),&own) || !own.dli_fname ||
        !realpath(own.dli_fname,resolved)) return 0;
    char* slash=strrchr(resolved,'/');if (!slash) return 0;*slash=0;
    snprintf(state.directory,sizeof(state.directory),"%s",resolved);
    snprintf(state.socketPath,sizeof(state.socketPath),"/tmp/ownandroidauto-control-%ld.sock",static_cast<long>(getpid()));
    state.control=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,state.socketPath);
    // Never unlink a pre-existing endpoint belonging to a prior/foreign object.
    if (state.control>=0 && !bind(state.control,reinterpret_cast<sockaddr*>(&address),sizeof(address))) state.controlBound=true;
    if (!state.controlBound || chmod(state.socketPath,0600)) {
        if (state.control>=0) close(state.control);state.control=-1;
        if (state.controlBound) {unlink(state.socketPath);state.controlBound=false;}
        logEvent("CONTROL_ERROR","errno=%d",errno);
        if (state.log>=0) close(state.log);state.log=-1;return 0;
    }
    void* memory=calloc(1,sizeof(bmw::PluginApp));
    if (!memory) {
        close(state.control);state.control=-1;unlink(state.socketPath);state.controlBound=false;
        if (state.log>=0) close(state.log);state.log=-1;return 0;
    }
    bmw::PluginApp* app=0;
    try {
        app=new(memory) bmw::PluginApp(dispatcher,bmw::BasicString<char>(name?name:"OwnAndroidAuto"),
            1,0,bmw::RHMI_INTERFACE_VERSION_1,parameters,config);
        if (!copyVtable(app)) { app->~PluginApp(); app=0; }
    } catch (...) { logEvent("EXCEPTION","create"); app=0; }
    if (!app) {
        free(memory);close(state.control);state.control=-1;
        unlink(state.socketPath);state.controlBound=false;
        if (state.log>=0) close(state.log);state.log=-1;return 0;
    }
    state.app=app; logEvent("CREATE","abi=installed-HU separate_surface=true"); return app;
}
void destroy(void* pointer) {
    if (!pointer || pointer!=state.app) return;
    stopProducer();
    state.initialized=state.focused=state.visible=false;
    if (state.control>=0) {close(state.control);state.control=-1;unlink(state.socketPath);}
    bmw::PluginApp* app=state.app;
    *reinterpret_cast<void***>(app)=state.nativeVtable;
    try { app->~PluginApp(); } catch (...) { logEvent("EXCEPTION","destroy"); }
    free(app); state.app=0;
    logEvent("DESTROY","own object released");
    if (state.log>=0) close(state.log); state.log=-1;
}
}
extern "C" {
__attribute__((visibility("default"))) PluginAppInfo pluginAppInfo={
    1,0,"Android Auto","Wired Android Auto",1,create,destroy
};
}
