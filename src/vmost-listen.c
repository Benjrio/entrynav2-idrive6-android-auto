/* vmost-listen: log what the head unit's virtual MOST daemon delivers for some FBlocks, to find the iDrive
 * controller messages. Passive: registers shadows only (see vmost_listen.h) and sends no MOST message.
 *   vmost-listen                      20 s, shadows CF.00 (HMIControl) and E5.00 (CentralControlSystem)
 *   vmost-listen -s 30 E5.00 24.0C    30 s, the given shadows (FBlock.Instance, hex)
 *   vmost-listen -l /usr/local/lib/libvmostclient.so.1 ...   explicit client library
 * Turn, push and tilt the controller and press its keys while it runs. Exit: 0 ok, 64 usage, 69 unavailable.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "vmost_listen.h"

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }
static uint64_t t0;
static void print_rx(const struct vmost_rx *m, void *user) { (void)user; char line[256]; vmost_rx_format(m, t0, line, sizeof line); puts(line); }

int main(int argc, char **argv) {
    unsigned seconds = 20; const char *library = NULL; uint8_t shadows[VMOST_MAX_SHADOWS][2]; unsigned n = 0; int opt;
    while ((opt = getopt(argc, argv, "s:l:h")) != -1) {
        if (opt == 's') { seconds = (unsigned)atoi(optarg); if (!seconds || seconds > 600) return 64; }
        else if (opt == 'l') library = optarg;
        else { fprintf(stderr, "usage: %s [-s seconds] [-l libvmostclient.so.1] [FB.II ...]\n", argv[0]); return 64; }
    }
    for (int i = optind; i < argc; i++) {
        if (n == VMOST_MAX_SHADOWS || !vmost_parse_shadow(argv[i], shadows[n])) { fprintf(stderr, "bad FBlock.Instance: %s (two hex digits each, e.g. E5.00)\n", argv[i]); return 64; }
        n++;
    }
    if (!n) { shadows[0][0] = 0xCF; shadows[0][1] = 0x00; shadows[1][0] = 0xE5; shadows[1][1] = 0x00; n = 2; }
    setbuf(stdout, NULL);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    printf("VMOST_LISTEN seconds=%u shadows=%u passive=1 sends_most_messages=0\n", seconds, n);
    int ok = library ? vmost_listen_open(library, (const uint8_t (*)[2])shadows, n)
                     : vmost_listen_open("libvmostclient.so.1", (const uint8_t (*)[2])shadows, n) ||
                       vmost_listen_open("/usr/local/lib/libvmostclient.so.1", (const uint8_t (*)[2])shadows, n);
    if (!ok) return 69;
    t0 = vmost_now_ns();
    uint64_t end = t0 + (uint64_t)seconds * 1000000000ULL;
    while (!stop && vmost_now_ns() < end) { vmost_listen_drain(print_rx, NULL); usleep(20000); }
    vmost_listen_drain(print_rx, NULL);
    vmost_listen_close();
    return 0;
}
