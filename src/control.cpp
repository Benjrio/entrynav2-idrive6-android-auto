/* Offline-built owner launcher helper. Read-only HMI inspection or private IPC.
 * No ptrace, memory writes, function calls in a foreign process or global input.
 */
#define _GNU_SOURCE 1
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/statvfs.h>
#include "window_ipc.h"
#include "profile_gate.hpp"
#include "profile.hpp"

static int memory=-1;
static bool rd(uint32_t address,void* data,unsigned n){
    return address>=4096 && address<=0xffff0000u-n && pread(memory,data,n,address)==static_cast<ssize_t>(n);
}
static bool stringAt(uint32_t address,char* out,unsigned size){
    for(unsigned i=0;i<size;i++){
        if(!rd(address+i,out+i,1))return false;
        if(!out[i])return true;
        // Paths are emitted to a shell consumer. Reject control/shell characters.
        char c=out[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c=='_'||c=='-'||c=='.'))return false;
    }
    return false;
}
static uint32_t visited[128];static unsigned nodes;
static bool roots(uint32_t node,uint32_t parent){
    if(!node)return true;
    if(nodes>=128)return false;
    for(unsigned i=0;i<nodes;i++)if(visited[i]==node)return false;
    visited[nodes++]=node;
    uint32_t w[6];if(!rd(node,w,sizeof(w))||w[1]!=parent||w[0]>1)return false;
    if(!roots(w[2],node))return false;
    char text[512];if(!stringAt(w[4],text,sizeof(text))||text[0]!='/')return false;
    printf("ROOT|%u|%s\n",w[5]&255u,text);
    return roots(w[3],node);
}
static int inspect(pid_t pid){
    char path[80];snprintf(path,sizeof(path),"/proc/%ld/exe",static_cast<long>(pid));
    if(!sameBuild(path,HmiBuildId,sizeof(HmiBuildId))){fprintf(stderr,"Unsupported HMI build ID.\n");return 66;}
    snprintf(path,sizeof(path),"/proc/%ld/mem",static_cast<long>(pid));memory=open(path,O_RDONLY|O_CLOEXEC);
    if(memory<0){perror("Read HMI app-manager metadata");return 67;}
    uint32_t manager=0,w[12]={};
    // Exact installed-HU mInstance ELF VA, not an offset into BIN or PT_LOAD.
    bool ok=rd(0x041e3198,&manager,4)&&rd(manager,w,sizeof(w))&&w[0]==0x040cb328;
    if(!ok){fprintf(stderr,"App-manager identity unavailable; no installation.\n");close(memory);return 68;}
    // std::map at +0x18, header at +0x1c, root +0x20, leftmost +0x24.
    ok=roots(w[8],manager+0x1c);
    // Existing ManagedPluginApp records: CAppManager vector +4, count +8.
    if(w[2]>1024)ok=false;
    for(unsigned i=0;ok&&i<w[2];i++){
        uint32_t address=0,app[36]={};
        ok=rd(w[1]+i*4,&address,4)&&rd(address,app,sizeof(app))&&app[0]==0x040cb400;
        if(ok)printf("APP|%u|%u|%u\n",app[7],app[1],app[25]!=0);
    }
    close(memory);
    if(!ok){fprintf(stderr,"Unstable/unsupported manager layout; no installation.\n");return 68;}
    puts("INSPECT_COMPLETE");return 0;
}
static int control(pid_t pid,const char* command){
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return 69;
    sockaddr_un local={},remote={};local.sun_family=remote.sun_family=AF_UNIX;
    snprintf(local.sun_path,sizeof(local.sun_path),"/tmp/ownandroidauto-client-%ld.sock",static_cast<long>(getpid()));
    snprintf(remote.sun_path,sizeof(remote.sun_path),"/tmp/ownandroidauto-control-%ld.sock",static_cast<long>(pid));
    if(bind(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))||chmod(local.sun_path,0600)){
        perror("Private client endpoint");close(fd);return 69;
    }
    int result=70;unsigned init=0,focus=0,visible=0,failed=0,starting=0,running=0,stopped=0,frame=0,revision=0;long child=0;
    bool verified=false,dispatched=false;
    // OPEN waits for the USB phone session and first video.
    // Bound elapsed time, not just the number of fast private-IPC replies.
    struct timespec begun={};clock_gettime(CLOCK_MONOTONIC,&begun);
    const bool phoneOpen=!strcmp(command,"OPEN");
    for(unsigned i=0;i<(phoneOpen?1800u:50u);i++){
        struct timespec current={};clock_gettime(CLOCK_MONOTONIC,&current);
        if(phoneOpen&&(current.tv_sec-begun.tv_sec)*1000+(current.tv_nsec-begun.tv_nsec)/1000000>=180000){
            puts("OPEN_TIMEOUT: no submitted phone frame within 180 seconds");break;
        }
        const char* request=!verified||dispatched?"STAT":command;
        if(!strcmp(command,"PROBE"))request="STAT";
        if(sendto(fd,request,4,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))!=4){perror("Plugin control");break;}
        if(verified&&strcmp(request,"STAT"))dispatched=true;
        pollfd p={fd,POLLIN,0};if(poll(&p,1,300)<=0)continue;
        char reply[160]={};sockaddr_un sender={};socklen_t size=sizeof(sender);
        ssize_t n=recvfrom(fd,reply,sizeof(reply)-1,0,reinterpret_cast<sockaddr*>(&sender),&size);
        if(n<=0||strcmp(sender.sun_path,remote.sun_path))continue;
        if(sscanf(reply,"STATE %u %u %u %u %u %u %ld %u %u %u",&init,&focus,&visible,&failed,&starting,&running,&child,&stopped,&frame,&revision)!=10||revision!=OW_BUILD_REVISION){
            puts("OLD_PLUGIN_LOADED: generation 24 required; no mutating command sent");result=76;break;
        }
        if(!verified){
            verified=true;
            if(strcmp(command,"STAT")&&strcmp(command,"PROBE"))continue;
        }
        // PROBE checks loaded generation/readiness. OPEN owns the existing
        // error reset, but only after the previous producer has exited.
        bool probeReady=init&&(!failed||(!child&&!running&&!starting&&stopped));
        if(!strcmp(command,"STAT") || (!strcmp(command,"PROBE")&&probeReady) ||
            ((!strcmp(command,"STOP")||!strcmp(command,"UNIN"))&&dispatched&&stopped&&!child&&!focus&&!running) ||
            (!strcmp(command,"QUIE")&&dispatched&&stopped&&!child&&!running) ||
            (!strcmp(command,"OPEN")&&dispatched&&focus&&visible&&running&&frame&&!failed)){printf("%s\n",reply);result=0;break;}
        if(failed){printf("%s\n",reply);break;}
        poll(NULL,0,100);
    }
    unlink(local.sun_path);close(fd);return result;
}
int main(int argc,char** argv){
    if(argc!=3)return 64;
    if(!strcmp(argv[1],"storage")){
        if(strcmp(argv[2],"/opt/app/cisu"))return 64;
        struct statvfs fs={};struct stat st={};
        if(lstat(argv[2],&st)||!S_ISDIR(st.st_mode)||statvfs(argv[2],&fs)){perror("Persistent executable storage");return 75;}
        unsigned long long available=static_cast<unsigned long long>(fs.f_bavail)*fs.f_frsize;
        printf("STORAGE|%s|%llu|flags=%lu\n",argv[2],available,static_cast<unsigned long>(fs.f_flag));
        if((fs.f_flag&(ST_RDONLY|ST_NOEXEC))||available<8u*1024u*1024u)return 75;
        return 0;
    }
    char* end=NULL;errno=0;long number=strtol(argv[2],&end,10);
    if(errno||!end||*end||number<=1||number>4194304)return 64;
    pid_t pid=static_cast<pid_t>(number);
    if(!strcmp(argv[1],"inspect"))return inspect(pid);
    if(strcmp(argv[1],"OPEN")&&strcmp(argv[1],"STOP")&&strcmp(argv[1],"STAT")&&strcmp(argv[1],"UNIN")&&strcmp(argv[1],"QUIE")&&strcmp(argv[1],"PROBE"))return 64;
    char exe[80];snprintf(exe,sizeof(exe),"/proc/%ld/exe",number);
    if(!sameBuild(exe,HmiBuildId,sizeof(HmiBuildId)))return 66;
    return control(pid,argv[1]);
}
