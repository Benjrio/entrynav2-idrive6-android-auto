/* Fake libvmostclient for host tests of src/vmost_listen.h, with the interface described there:
 * vmostConnect(rx, user, own_thread) starts a thread that delivers messages to rx.
 * FAKE_VMOST_COUNT messages (default 3) are sent once at least one shadow is registered. FBlock 0xEE refuses
 * registration. fake_vmost_* functions let the test inspect what happened.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct vmost_msg { uint16_t device; uint8_t fblock, inst; uint16_t function; uint16_t reserved6; uint32_t op;
                   uint16_t len; uint16_t reserved14; const uint8_t *data; uint32_t channel; };
typedef void (*rx_fn)(const struct vmost_msg *, void *);
static rx_fn rx; static void *rx_user; static pthread_t thread; static int running;
static atomic_int registered, deregistered, sent, connects, disconnects, stop_flag;

static void *deliver(void *arg) {
    (void)arg;
    while (!atomic_load(&registered) && !atomic_load(&stop_flag)) usleep(1000);
    const char *e = getenv("FAKE_VMOST_COUNT"); int count = e ? atoi(e) : 3;
    for (int i = 0; i < count && !atomic_load(&stop_flag); i++) {
        uint8_t data[40]; for (unsigned j = 0; j < sizeof data; j++) data[j] = (uint8_t)(i + j);
        struct vmost_msg m = { .device = 0x0100, .fblock = 0xE5, .inst = 0, .function = 0xC10, .op = 0xC,
                               .len = (uint16_t)(i == 1 ? 40 : 3), .data = data, .channel = 0 };
        rx(&m, rx_user); atomic_fetch_add(&sent, 1);
    }
    while (!atomic_load(&stop_flag)) usleep(1000);
    return NULL;
}
int vmostConnect(rx_fn cb, void *user, unsigned char own_thread) {
    if (!cb || !own_thread || running) return 5;
    rx = cb; rx_user = user; atomic_store(&stop_flag, 0); atomic_fetch_add(&connects, 1);
    if (pthread_create(&thread, NULL, deliver, NULL)) return 8;
    running = 1; return 0;
}
int vmostDisconnect(void) {
    if (!running) return 26;
    atomic_store(&stop_flag, 1); pthread_join(thread, NULL); running = 0; atomic_fetch_add(&disconnects, 1); return 0;
}
int vmostFblockShadowRegister(unsigned char fblock, unsigned char inst) { (void)inst; if (fblock == 0xEE) return 3; atomic_fetch_add(&registered, 1); return 0; }
int vmostFblockShadowDeregister(unsigned char fblock, unsigned char inst) { (void)fblock; (void)inst; atomic_fetch_add(&deregistered, 1); return 0; }
int fake_vmost_sent(void) { return atomic_load(&sent); }
int fake_vmost_registered(void) { return atomic_load(&registered); }
int fake_vmost_deregistered(void) { return atomic_load(&deregistered); }
int fake_vmost_disconnects(void) { return atomic_load(&disconnects); }
int fake_vmost_reset(void) { atomic_store(&registered, 0); atomic_store(&deregistered, 0); atomic_store(&sent, 0); return 0; }
