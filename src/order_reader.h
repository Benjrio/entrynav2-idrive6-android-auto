/* Read-only live compositor order inspection.
 * Layout is restricted to the supported 148 binaries.
 * Heap bytes are inspected in memory, never exported. Output is scene metadata.
 * Two equal samples detect some races, not an atomic compositor snapshot. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_HEAP (8u*1024u*1024u)
#define MAX_LAYERS 512u
static int memfd=-1;
static uint32_t heap_lo,heap_hi,stack_lo,stack_hi,genivi_base;
static const char *snapshot_stage;
struct snapshot {
    uint32_t context,compositor,screen,output,size,alloc,data,flip;
    uint32_t ids[MAX_LAYERS];
};
static int read_heap(uint32_t addr,void *buf,size_t n) {
    if(addr<heap_lo || addr>heap_hi || n>heap_hi-addr)return 0;
    return pread(memfd,buf,n,(off_t)addr)==(ssize_t)n;
}
static int word(uint32_t addr,uint32_t *out) {
    if(addr&3u)return 0;
    if(addr>=stack_lo&&addr<stack_hi&&stack_hi-addr>=4)
        return pread(memfd,out,4,(off_t)addr)==4;
    return read_heap(addr,out,4);
}
static int plugin_linked(uint32_t compositor,uint32_t context) {
    /* compositor_init initializes the head at +0x3c (ELF VA 0xe934).
     * load_plugin reads head.prev to append; +0x3c is not head.next. */
    uint32_t head=compositor+0x3cu,prev=head,node,links[2];
    int found=0;
    if(!word(head+4,&node)){printf("ORDER_LINK_REJECT head=%08x stage=head-read\n",head);return 0;}
    for(unsigned i=0;i<32;i++) {
        if(node==head) {
            uint32_t last;
            int ok=word(head,&last)&&last==prev&&found==1;
            if(!ok)printf("ORDER_LINK_REJECT head=%08x stage=tail found=%d\n",head,found);
            return ok;
        }
        if(!word(node,&links[0])||!word(node+4,&links[1])){printf("ORDER_LINK_REJECT head=%08x node=%08x stage=node-read\n",head,node);return 0;}
        if(links[0]!=prev){printf("ORDER_LINK_REJECT head=%08x node=%08x stage=backlink previous=%08x expected=%08x\n",head,node,links[0],prev);return 0;}
        if(node==context+8)found++;
        prev=node;node=links[1];
    }
    return 0;
}
static int snapshot(uint32_t context,struct snapshot *out) {
    uint32_t c[16],s[7],again[7];char name[7];
    memset(out,0,sizeof *out);
    snapshot_stage="context-anchor";
    if(!read_heap(context,c,sizeof c)||c[0]!=genivi_base+0x2d64u)return 0;
    snapshot_stage="plugin-name";
    if(!read_heap(c[1],name,sizeof name)||memcmp(name,"genivi",7))return 0;
    snapshot_stage="plugin-list";
    if(!plugin_linked(c[5],context))return 0;
    /* Single output only. Its sole link must point back to the list head. */
    snapshot_stage="single-screen-list";
    if(c[8]!=c[9] || c[8]<16 || c[8]==context+0x20u)return 0;
    uint32_t screen=c[8]-16;
    snapshot_stage="screen-backlinks";
    if(!read_heap(screen,s,sizeof s)||s[4]!=context+0x20u ||
       s[5]!=context+0x20u||s[6]!=context)return 0;
    snapshot_stage="array-bounds";
    if(s[1]==0 || s[1]%4 || s[1]>MAX_LAYERS*4 || s[2]<s[1] ||
       s[2]>MAX_LAYERS*4 || !read_heap(s[3],out->ids,s[1]))return 0;
    snapshot_stage="duplicate-id";
    for(unsigned i=0;i<s[1]/4;i++)for(unsigned j=0;j<i;j++)
        if(out->ids[i]==out->ids[j])return 0;
    /* Catch reallocation/size change during the read. */
    snapshot_stage="screen-header-changed";
    if(!read_heap(screen,again,sizeof again)||memcmp(s,again,sizeof s))return 0;
    out->context=context;out->compositor=c[5];out->screen=screen;
    out->output=s[0];out->size=s[1];out->alloc=s[2];out->data=s[3];
    out->flip=c[15]&255u;
    snapshot_stage="flip-range";
    return out->flip<=1;
}
static int fail(const char *why) {
    fprintf(stderr,"ORDER_READ_FAILED reason=%s errno=%d\n",why,errno);
    if(memfd>=0)close(memfd);memfd=-1;
    return 65;
}

static int aa_mem_open(unsigned long pid){
    char path[80],line[1024],comm[32];
    snprintf(path,sizeof path,"/proc/%lu/comm",pid);
    FILE *f=fopen(path,"r");
    if(!f)return fail("comm-open");
    int ok=fgets(comm,sizeof comm,f)!=NULL;fclose(f);
    if(!ok||strcmp(comm,"winston\n"))return fail("not-winston");
    snprintf(path,sizeof path,"/proc/%lu/maps",pid);
    f=fopen(path,"r");if(!f)return fail("maps-open");
    unsigned heaps=0,bases=0,stacks=0;
    while(fgets(line,sizeof line,f)) {
        unsigned lo,hi,offset;char perms[5],file[700]={0};
        int fields=sscanf(line,"%x-%x %4s %x %*s %*s %699s",&lo,&hi,perms,&offset,file);
        if(fields!=5)continue;
        if(!strcmp(file,"[heap]")&&!strcmp(perms,"rw-p")) {
            heaps++;heap_lo=lo;heap_hi=hi;
        }
        /* main owns the compositor on its stack (winston ELF VA 0xca3c,
         * 0xcb34). Only plugin-list head words are read here, no stack scan. */
        if(!strcmp(file,"[stack]")&&!strcmp(perms,"rw-p")) {
            stacks++;stack_lo=lo;stack_hi=hi;
        }
        if(!strcmp(file,"/usr/lib/libwlxs-genivi.so")&&!strcmp(perms,"r-xp")&&offset==0) {
            bases++;genivi_base=lo;
        }
    }
    fclose(f);
    if(heaps!=1||bases!=1||stacks!=1||stack_hi<=stack_lo||
       heap_hi<=heap_lo||heap_hi-heap_lo>MAX_HEAP)
        return fail("mapping-bounds");
    snprintf(path,sizeof path,"/proc/%lu/mem",pid);
    memfd=open(path,O_RDONLY|O_CLOEXEC);
    if(memfd<0)return fail("mem-open");
    /* Fingerprint the context-init instructions, independently of the shell
     * file hash gate. Never execute a discovered target function. */
    const uint32_t signature[]={0xe5845014u,0xe08fc00cu,0xe584c000u};
    uint32_t got[3];
    if(pread(memfd,got,sizeof got,(off_t)genivi_base+0x3738)!=(ssize_t)sizeof got ||
       memcmp(got,signature,sizeof got))return fail("code-signature");
    return 0;
}
static int aa_mem_sample(struct snapshot *out){
    uint32_t *heap=malloc(heap_hi-heap_lo);
    if(!heap)return fail("malloc");
    if(!read_heap(heap_lo,heap,heap_hi-heap_lo)){free(heap);return fail("heap-read");}
    unsigned count=0,anchors=0;struct snapshot selected={0},candidate={0};
    for(uint32_t off=0;off+64<=heap_hi-heap_lo;off+=4) {
        if(heap[off/4]==genivi_base+0x2d64u) {
            if(++anchors>16){free(heap);return fail("too-many-anchors");}
            if(snapshot(heap_lo+off,&candidate)){selected=candidate;count++;}
            else printf("ORDER_CANDIDATE context=%08x rejected=%s\n",heap_lo+off,snapshot_stage);
        }
    }
    free(heap);
    if(count!=1){printf("ORDER_CANDIDATES anchors=%u valid=%u\n",anchors,count);return fail("unique-active-context");}
    usleep(50000);
    if(!snapshot(selected.context,&candidate)||memcmp(&selected,&candidate,sizeof selected))
        return fail("scene-changed-repeat-collection");
    *out=selected;return 0;
}
