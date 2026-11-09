#pragma once
#include <sys/wait.h>
#include <limits.h>
#include "frame_bridge.h"
static struct ow_aa_frame *phone_frame;
static pid_t phone_pid;
static int phone_running(void) {
    if(phone_pid<=0)return 0;
    int status=0;pid_t r=waitpid(phone_pid,&status,WNOHANG);
    if(r==0||(r<0&&errno==EINTR))return 1;
    fprintf(stderr,"AA_CHILD_EXIT pid=%ld status=%d wait_rc=%ld\n",(long)phone_pid,status,(long)r);
    phone_pid=0;return 0;
}
static int phone_start(void) {
    char path[PATH_MAX],dir[PATH_MAX],libs[PATH_MAX],run[]="/tmp/ownandroidauto-aa-XXXXXX";
    ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);if(n<=0||(size_t)n>=sizeof(path)-1)return 0;
    path[n]=0;char *slash=strrchr(path,'/');if(!slash)return 0;*slash=0;
    if(snprintf(dir,sizeof(dir),"%s/aa",path)>=(int)sizeof(dir)||snprintf(libs,sizeof(libs),"%s/lib",dir)>=(int)sizeof(libs))return 0;
    if(!mkdtemp(run))return 0;
    char file[PATH_MAX];snprintf(file,sizeof(file),"%s/frames",run);
    int fd=open(file,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);if(fd<0)return 0;
    unlink(file);if(ftruncate(fd,sizeof(struct ow_aa_frame))){close(fd);return 0;}
    phone_frame=mmap(NULL,sizeof(*phone_frame),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(phone_frame==MAP_FAILED){phone_frame=NULL;close(fd);return 0;}
    memset(phone_frame,0,sizeof(*phone_frame));phone_frame->magic=OW_AA_MAGIC;phone_frame->version=1;
    atomic_init(&phone_frame->slot,0);atomic_init(&phone_frame->visible,0);atomic_init(&phone_frame->stop,0);
    atomic_init(&phone_frame->submitted,0);atomic_init(&phone_frame->epoch,0);
    snprintf(file,sizeof(file),"%s/android-auto.log",run);
    int log=open(file,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);if(log<0){close(fd);return 0;}
    pid_t owner=getpid();long maxfd=sysconf(_SC_OPEN_MAX);
    phone_pid=fork();
    if(phone_pid==0){
        if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=owner)_exit(65);
        if(dup2(log,1)<0||dup2(log,2)<0||dup2(fd,4)<0||fcntl(4,F_SETFD,0))_exit(65);
        close(0);if(open("/dev/null",O_RDONLY)!=0)_exit(65);
        for(int i=3;i<maxfd;i++)if(i!=4)close(i);
        if(chdir(dir)||setenv("LD_LIBRARY_PATH",libs,1))_exit(65);
        execl("./hu-aa.elf","hu-aa.elf","--usb-takeover-usbfs","--single-attempt","--out",run,(char*)NULL);
        _exit(127);
    }
    close(fd);close(log);
    fprintf(stderr,"AA_CHILD pid=%ld results=%s\n",(long)phone_pid,run);
    return phone_pid>0;
}
static int phone_stop(void) {
    int ok=1;
    if(phone_frame)atomic_store(&phone_frame->stop,1);
    if(phone_pid>0){
        /* Unreaped direct child cannot be replaced by a reused PID. */
        kill(phone_pid,SIGTERM);int status=0,done=0;
        for(unsigned i=0;i<100;i++){
            pid_t r=waitpid(phone_pid,&status,WNOHANG);
            if(r==phone_pid){done=1;break;}if(r<0&&errno!=EINTR){ok=0;done=1;break;}usleep(10000);
        }
        if(!done){
            /* Only our unreaped direct USB child. Kernel close releases its
             * interface even if synchronous legacy I/O ignored TERM. */
            kill(phone_pid,SIGKILL);
            pid_t r;do{r=waitpid(phone_pid,&status,0);}while(r<0&&errno==EINTR);
            fprintf(stderr,"AA_USB_CHILD_FORCED_CLOSE wait_rc=%ld\n",(long)r);
            ok=0;
        }
        fprintf(stderr,"AA_CHILD_END status=%d clean=%d\n",status,ok&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
        ok=ok&&WIFEXITED(status)&&WEXITSTATUS(status)==0;phone_pid=0;
    }
    if(phone_frame){munmap(phone_frame,sizeof(*phone_frame));phone_frame=NULL;}return ok;
}
