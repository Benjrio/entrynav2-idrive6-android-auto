/* Private inherited fd4, never a public socket or persistent video recording.
 * Single producer/single consumer; acquire/release ownership prevents tearing.
 */
#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#define OW_AA_MAGIC 0x41415731u
#define OW_AA_MAX_WIDTH 1280u
#define OW_AA_MAX_HEIGHT 720u
struct ow_aa_frame {
    uint32_t magic, version;
    atomic_uint slot; /* 0 free, 1 writer, 2 ready, 3 reader */
    atomic_uint visible, stop, submitted, epoch;
    uint32_t width, height, frame_epoch;
    uint8_t rgb[OW_AA_MAX_WIDTH * OW_AA_MAX_HEIGHT * 3];
};
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "shared atomics must be lock free");
static struct ow_aa_frame *ow_aa_map(int fd) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != sizeof(struct ow_aa_frame)) return NULL;
    void *p = mmap(NULL, sizeof(struct ow_aa_frame), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return NULL;
    struct ow_aa_frame *f = p;
    if (f->magic != OW_AA_MAGIC || f->version != 1) { munmap(p, sizeof(*f)); return NULL; }
    return f;
}
