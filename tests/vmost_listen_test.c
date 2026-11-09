/* Host tests for src/vmost_listen.h against tests/fake_vmostclient.c (built as a shared library by run.sh).
 * Usage: vmost_listen_test <path to fake libvmostclient.so>
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include "../src/vmost_listen.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct vmost_rx got[VMOST_RING]; static unsigned ngot;
static void collect(const struct vmost_rx *m, void *user) { (void)user; if (ngot < VMOST_RING) got[ngot++] = *m; }
static int (*fake_int)(void);
static int call(const char *name) { *(void **)&fake_int = dlsym(vmost_l.lib, name); return fake_int ? fake_int() : -1; }
static void wait_sent(int n) { for (int i = 0; i < 2000 && call("fake_vmost_sent") < n; i++) usleep(1000); }
static void reset_ring(void) { atomic_store(&vmost_ring_w, 0); atomic_store(&vmost_ring_r, 0); atomic_store(&vmost_ring_lost, 0); atomic_store(&vmost_rx_total, 0); ngot = 0; }

int main(int argc, char **argv) {
    if (argc != 2) return 64;
    uint8_t s[2];
    CHECK(vmost_parse_shadow("E5.00", s) && s[0] == 0xE5 && s[1] == 0, "E5.00");
    CHECK(vmost_parse_shadow("cf.0c", s) && s[0] == 0xCF && s[1] == 0x0C, "lower case");
    CHECK(!vmost_parse_shadow("E5", s) && !vmost_parse_shadow("E5.0", s) && !vmost_parse_shadow("E5.000", s) &&
          !vmost_parse_shadow("G5.00", s) && !vmost_parse_shadow("E5-00", s) && !vmost_parse_shadow(" 5.00", s), "bad shadow specs");

    const uint8_t two[2][2] = { { 0xCF, 0x00 }, { 0xE5, 0x00 } };
    CHECK(!vmost_listen_open("/nonexistent/libvmostclient.so.1", two, 2) && !vmost_l.connected, "missing library: unavailable, not connected");

    setenv("FAKE_VMOST_COUNT", "3", 1); reset_ring();
    CHECK(vmost_listen_open(argv[1], two, 2) && vmost_l.connected && vmost_l.nshadows == 2, "connect and two shadows");
    CHECK(call("fake_vmost_registered") == 2, "library saw two registrations");
    wait_sent(3);
    CHECK(vmost_listen_drain(collect, NULL) == 3 && ngot == 3, "three messages drained (%u)", ngot);
    CHECK(got[0].fblock == 0xE5 && got[0].function == 0xC10 && got[0].op == 0xC && got[0].device == 0x0100 && got[0].len == 3 &&
          got[0].data[0] == 0 && got[0].data[2] == 2, "fields and data copied");
    CHECK(got[1].len == 40 && got[1].data[VMOST_RX_DATA - 1] == (uint8_t)(1 + VMOST_RX_DATA - 1), "long payload: length kept, first 32 bytes copied");
    char line[256]; vmost_rx_format(&got[1], got[0].t_ns, line, sizeof line);
    CHECK(strstr(line, "fblock=0xe5") && strstr(line, "fkt=0xc10") && strstr(line, "len=40") && strstr(line, "..."), "format: %s", line);
    vmost_rx_format(&got[0], got[0].t_ns, line, sizeof line);
    CHECK(strstr(line, "data=000102") && strstr(line, "t=0.000"), "format: %s", line);
    vmost_listen_close();
    CHECK(!vmost_l.connected && call("fake_vmost_deregistered") == 2 && call("fake_vmost_disconnects") == 1, "close deregisters both and disconnects");

    call("fake_vmost_reset"); setenv("FAKE_VMOST_COUNT", "300", 1); reset_ring();
    CHECK(vmost_listen_open(argv[1], two, 1), "reconnect");
    wait_sent(300);
    unsigned drained = vmost_listen_drain(collect, NULL);
    CHECK(drained == VMOST_RING && atomic_load(&vmost_ring_lost) == 300 - VMOST_RING && atomic_load(&vmost_rx_total) == 300,
          "full ring drops and counts (drained %u, lost %u)", drained, atomic_load(&vmost_ring_lost));
    vmost_listen_close();

    call("fake_vmost_reset"); setenv("FAKE_VMOST_COUNT", "0", 1); reset_ring();
    const uint8_t bad[2][2] = { { 0xCF, 0x00 }, { 0xEE, 0x00 } };
    CHECK(!vmost_listen_open(argv[1], bad, 2) && !vmost_l.connected, "a refused shadow fails the open");
    CHECK(call("fake_vmost_deregistered") == 1 && call("fake_vmost_disconnects") == 3, "and undoes the one that worked");

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
