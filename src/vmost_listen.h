/* Passive listener for the head unit's virtual MOST daemon (vmostd), through the head unit's client library
 * libvmostclient.so.1, loaded with dlopen at runtime: nothing links against it, and without it the listener just
 * reports "unavailable". It registers FBlock *shadows* (MOST's normal client role; vmostd drops them when we
 * disconnect) and copies every message vmostd delivers into a ring that the caller's thread drains.
 * It sends no MOST message: no Set, no Notification, no inject.
 *
 * Client library interface used here:
 *   int vmostConnect(void (*rx)(const struct vmost_msg *msg, void *user), void *user, unsigned char own_thread);
 *       0 = ok. own_thread != 0: the library runs its own event loop in its own thread and calls rx there.
 *       msg and msg->data are only valid during the call.
 *   int vmostDisconnect(void);
 *   int vmostFblockShadowRegister(unsigned char fblock, unsigned char inst);     0 = ok
 *   int vmostFblockShadowDeregister(unsigned char fblock, unsigned char inst);   0 = ok
 *   struct vmost_msg, 24 bytes on 32-bit ARM:
 *       u16 device @0, u8 fblock @2, u8 inst @3, u16 function @4, u32 op @8, u16 len @12, const u8 *data @16,
 *       u32 channel @20 (0..2).
 */
#ifndef VMOST_LISTEN_H
#define VMOST_LISTEN_H
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct vmost_msg {
    uint16_t device;
    uint8_t fblock, inst;
    uint16_t function;
    uint16_t reserved6;
    uint32_t op;
    uint16_t len;
    uint16_t reserved14;
    const uint8_t *data;
    uint32_t channel;
};
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(struct vmost_msg) == 24, "vmost message is 24 bytes on 32-bit ARM");
#endif

#define VMOST_RING 128u
#define VMOST_RX_DATA 32u
#define VMOST_MAX_SHADOWS 8u
struct vmost_rx {
    uint64_t t_ns;
    uint16_t device, function, len;
    uint8_t fblock, inst, channel, op;
    uint8_t data[VMOST_RX_DATA];
};
static struct vmost_rx vmost_ring[VMOST_RING];
static atomic_uint vmost_ring_w, vmost_ring_r, vmost_ring_lost, vmost_rx_total;

typedef void (*vmost_rx_fn)(const struct vmost_msg *, void *);
static struct {
    void *lib;
    int (*connect)(vmost_rx_fn, void *, unsigned char);
    int (*disconnect)(void);
    int (*shadow_reg)(unsigned char, unsigned char);
    int (*shadow_dereg)(unsigned char, unsigned char);
    int connected;
    unsigned nshadows;
    uint8_t shadows[VMOST_MAX_SHADOWS][2];
} vmost_l;

static uint64_t vmost_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec; }

/* Runs on the library's thread: copy and return. Single producer (that thread), single consumer (the drainer). */
static void vmost_on_rx(const struct vmost_msg *m, void *user) {
    (void)user;
    atomic_fetch_add_explicit(&vmost_rx_total, 1, memory_order_relaxed);
    if (!m) return;
    unsigned w = atomic_load_explicit(&vmost_ring_w, memory_order_relaxed);
    if (w - atomic_load_explicit(&vmost_ring_r, memory_order_acquire) >= VMOST_RING) {
        atomic_fetch_add_explicit(&vmost_ring_lost, 1, memory_order_relaxed); return;
    }
    struct vmost_rx *r = &vmost_ring[w % VMOST_RING];
    r->t_ns = vmost_now_ns(); r->device = m->device; r->fblock = m->fblock; r->inst = m->inst; r->function = m->function;
    r->op = (uint8_t)m->op; r->channel = (uint8_t)m->channel; r->len = m->len;
    unsigned n = m->data && m->len ? (m->len < VMOST_RX_DATA ? m->len : VMOST_RX_DATA) : 0;
    if (n) memcpy(r->data, m->data, n);
    atomic_store_explicit(&vmost_ring_w, w + 1, memory_order_release);
}

/* "CF.00" -> fblock 0xCF, inst 0x00. Two hex digits each, nothing else. */
static int vmost_parse_shadow(const char *s, uint8_t out[2]) {
    unsigned v[2]; char extra;
    if (strlen(s) != 5 || s[2] != '.' || sscanf(s, "%2x.%2x%c", &v[0], &v[1], &extra) != 2) return 0;
    for (int i = 0; i < 5; i++) if (i != 2 && !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    out[0] = (uint8_t)v[0]; out[1] = (uint8_t)v[1]; return 1;
}

static void vmost_listen_close(void) {
    if (vmost_l.connected) {
        for (unsigned i = 0; i < vmost_l.nshadows; i++) {
            int rc = vmost_l.shadow_dereg ? vmost_l.shadow_dereg(vmost_l.shadows[i][0], vmost_l.shadows[i][1]) : -1;
            printf("VMOST_SHADOW_DEREGISTER fblock=0x%02x inst=0x%02x rc=%d\n", vmost_l.shadows[i][0], vmost_l.shadows[i][1], rc);
        }
        int rc = vmost_l.disconnect();
        printf("VMOST_DISCONNECT rc=%d received=%u lost=%u\n", rc, atomic_load(&vmost_rx_total), atomic_load(&vmost_ring_lost));
    }
    vmost_l.connected = 0; vmost_l.nshadows = 0;
    /* The library's own thread may still exist after vmostDisconnect, so it is never dlclose()d. */
}

/* Returns 1 when connected with every shadow registered; on failure everything is undone and 0 is returned. */
static int vmost_listen_open(const char *library, const uint8_t (*shadows)[2], unsigned nshadows) {
    if (nshadows > VMOST_MAX_SHADOWS) { printf("VMOST_UNAVAILABLE reason=too-many-shadows\n"); return 0; }
    if (!vmost_l.lib) {
        vmost_l.lib = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        if (!vmost_l.lib) { const char *e = dlerror(); printf("VMOST_UNAVAILABLE reason=dlopen library=%s detail=%s\n", library, e ? e : "?"); return 0; }
        *(void **)&vmost_l.connect = dlsym(vmost_l.lib, "vmostConnect");
        *(void **)&vmost_l.disconnect = dlsym(vmost_l.lib, "vmostDisconnect");
        *(void **)&vmost_l.shadow_reg = dlsym(vmost_l.lib, "vmostFblockShadowRegister");
        *(void **)&vmost_l.shadow_dereg = dlsym(vmost_l.lib, "vmostFblockShadowDeregister");
    }
    if (!vmost_l.connect || !vmost_l.disconnect || !vmost_l.shadow_reg) { printf("VMOST_UNAVAILABLE reason=missing-symbols\n"); return 0; }
    int rc = vmost_l.connect(vmost_on_rx, NULL, 1);
    printf("VMOST_CONNECT rc=%d own_thread=1\n", rc);
    if (rc != 0) return 0;
    vmost_l.connected = 1;
    for (unsigned i = 0; i < nshadows; i++) {
        rc = vmost_l.shadow_reg(shadows[i][0], shadows[i][1]);
        printf("VMOST_SHADOW_REGISTER fblock=0x%02x inst=0x%02x rc=%d\n", shadows[i][0], shadows[i][1], rc);
        if (rc != 0) { vmost_listen_close(); return 0; }
        vmost_l.shadows[vmost_l.nshadows][0] = shadows[i][0]; vmost_l.shadows[vmost_l.nshadows][1] = shadows[i][1]; vmost_l.nshadows++;
    }
    return 1;
}

/* Drain on the caller's thread. Returns how many messages were handed to fn. */
static unsigned vmost_listen_drain(void (*fn)(const struct vmost_rx *, void *), void *user) {
    unsigned n = 0, r = atomic_load_explicit(&vmost_ring_r, memory_order_relaxed);
    while (r != atomic_load_explicit(&vmost_ring_w, memory_order_acquire)) {
        fn(&vmost_ring[r % VMOST_RING], user); n++;
        atomic_store_explicit(&vmost_ring_r, ++r, memory_order_release);
    }
    return n;
}

static void vmost_rx_format(const struct vmost_rx *m, uint64_t t0, char *out, size_t size) {
    int w = snprintf(out, size, "VMOST_RX t=%.3f dev=0x%04x fblock=0x%02x inst=0x%02x fkt=0x%03x op=0x%x ch=%u len=%u data=",
                     (double)(m->t_ns - t0) / 1e9, m->device, m->fblock, m->inst, m->function, m->op, m->channel, m->len);
    unsigned n = m->len < VMOST_RX_DATA ? m->len : VMOST_RX_DATA;
    for (unsigned i = 0; i < n && w > 0 && (size_t)w + 3 < size; i++) w += snprintf(out + w, size - (size_t)w, "%02x", m->data[i]);
    if (m->len > VMOST_RX_DATA && w > 0 && (size_t)w + 4 < size) snprintf(out + w, size - (size_t)w, "...");
    if (!n && w > 0 && (size_t)w + 2 < size) snprintf(out + w, size - (size_t)w, "-");
}
#endif
