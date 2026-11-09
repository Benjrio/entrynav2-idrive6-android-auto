#pragma once
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
bool readAt(int fd, void* p, size_t size, off_t offset) {
    return pread(fd,p,size,offset)==static_cast<ssize_t>(size);
}
bool sameBuild(const char* path, const unsigned char* expected, unsigned size) {
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if (fd<0) return false;
    Elf32_Ehdr h={}; struct stat st={}; bool match=false;
    if (fstat(fd,&st)==0 && S_ISREG(st.st_mode) && readAt(fd,&h,sizeof(h),0) &&
        memcmp(h.e_ident,ELFMAG,SELFMAG)==0 && h.e_ident[EI_CLASS]==ELFCLASS32 &&
        h.e_ident[EI_DATA]==ELFDATA2LSB && h.e_machine==EM_ARM &&
        h.e_phentsize==sizeof(Elf32_Phdr) && h.e_phnum<=32) {
        for (unsigned i=0;i<h.e_phnum && !match;++i) {
            Elf32_Phdr ph={};
            if (!readAt(fd,&ph,sizeof(ph),h.e_phoff+i*sizeof(ph))) break;
            if (ph.p_type!=PT_NOTE || ph.p_filesz>65536 ||
                static_cast<uint64_t>(ph.p_offset)+ph.p_filesz>static_cast<uint64_t>(st.st_size)) continue;
            unsigned char* b=static_cast<unsigned char*>(malloc(ph.p_filesz));
            if (!b) break;
            if (readAt(fd,b,ph.p_filesz,ph.p_offset)) {
                unsigned pos=0;
                while (pos+12<=ph.p_filesz) {
                    uint32_t words[3]; memcpy(words,b+pos,12);
                    const uint64_t names=(static_cast<uint64_t>(words[0])+3)&~3ULL;
                    const uint64_t data=(static_cast<uint64_t>(words[1])+3)&~3ULL;
                    const uint64_t next=pos+12+names+data;
                    if (next>ph.p_filesz) break;
                    if (words[2]==NT_GNU_BUILD_ID && words[0]==4 && words[1]==size &&
                        memcmp(b+pos+12,"GNU\0",4)==0 &&
                        memcmp(b+pos+12+names,expected,size)==0) { match=true; break; }
                    pos=static_cast<unsigned>(next);
                }
            }
            free(b);
        }
    }
    close(fd); return match;
}
