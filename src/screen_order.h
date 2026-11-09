/* Live screen-order adapter. No atomic CAS exists in the vendor API.
 * Read just before one commit; validate afterwards; never restore a snapshot.
 * The only process-memory descriptor is read-only. */
#pragma once
#include <dirent.h>
#include "order_reader.h"
static struct snapshot aa_order_before,aa_order_expected;
static int aa_order_opened;
static int aa_file_equal(const char *a,const char *b){
    FILE *x=fopen(a,"rb"),*y=fopen(b,"rb");int ok=0;
    if(x&&y){unsigned char u[4096],v[4096];ok=1;
        for(;;){size_t m=fread(u,1,sizeof u,x),n=fread(v,1,sizeof v,y);
            if(m!=n||memcmp(u,v,m)||ferror(x)||ferror(y)){ok=0;break;}
            if(!m)break;
        }
    }
    if(x)fclose(x);if(y)fclose(y);return ok;
}
static int aa_order_init(void){
    char exe[PATH_MAX],path[PATH_MAX];ssize_t n=readlink("/proc/self/exe",exe,sizeof exe-1);
    if(n<=0||(size_t)n>=sizeof exe-1)return 0;
    exe[n]=0;char *slash=strrchr(exe,'/');if(!slash)return 0;*slash=0;
    const char *files[]={"/usr/bin/winston","/usr/lib/libwlxs-genivi.so"};
    const char *names[]={"winston","libwlxs-genivi.so"};
    for(unsigned i=0;i<2;i++){
        if(snprintf(path,sizeof path,"%s/aa/scene/%s",exe,names[i])>=(int)sizeof path||
           !aa_file_equal(files[i],path))return 0;
    }
    DIR *dir=opendir("/proc");if(!dir)return 0;struct dirent *ent;unsigned long pid=0;unsigned count=0;
    while((ent=readdir(dir))){char *end;unsigned long v=strtoul(ent->d_name,&end,10);
        if(*end||v<2||v>4194304)continue;
        snprintf(path,sizeof path,"/proc/%lu/comm",v);FILE *f=fopen(path,"r");if(!f)continue;
        char name[32]={0};if(fgets(name,sizeof name,f)&&!strcmp(name,"winston\n")){pid=v;count++;}fclose(f);
    }
    closedir(dir);if(count!=1||aa_mem_open(pid))return 0;
    aa_order_opened=1;return 1;
}
static int aa_order_read(struct snapshot *out){
    if(!aa_order_opened&&!aa_order_init())return 0;
    return aa_mem_sample(out)==0;
}
static int aa_order_equal(const struct snapshot*a,const struct snapshot*b){
    return a->context==b->context&&a->screen==b->screen&&a->output==b->output&&
        a->flip==b->flip&&a->size==b->size&&!memcmp(a->ids,b->ids,a->size);
}
static int aa_order_verify(void){
    struct snapshot current;
    int ok=aa_order_opened&&snapshot(aa_order_expected.context,&current)&&
        aa_order_equal(&aa_order_expected,&current);
    fprintf(stderr,"AA_ORDER_VERIFY equal=%d own_first=%u\n",ok,aa_order_expected.ids[0]);
    return ok;
}
static void aa_order_close(void){if(memfd>=0)close(memfd);memfd=-1;aa_order_opened=0;}
