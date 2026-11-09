/* usb-scan: read-only view of the phone selection hu-aa.elf would make with this usb-device.conf.
 * Reads ./usb-device.conf and sysfs attribute files only. Never opens a /dev/bus/usb node, never sends a USB request.
 *   usb-scan            two passes 1.6 s apart (so devices that just appeared can settle), then the verdict
 *   usb-scan -w 30      repeat every second for 30 s and print changes (plug/unplug phones meanwhile)
 * Exit: 0 one device would be selected, 1 none, 2 ambiguous, 64 bad usage or configuration.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "usb_selection.h"
#include "usb_discovery.h"

static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec; }

static int legacy_match(const struct usb_cand *c) {
    if (usb_policy == USB_POLICY_FIXED) return strcmp(c->port, usb_configured_port) ? 0 : usb_phone_matches(c->vid, c->pid) || c->accessory;
    return c->accessory; /* auto */
}

int main(int argc, char **argv) {
    unsigned watch = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) { watch = (unsigned)atoi(argv[++i]); if (!watch || watch > 600) return 64; }
        else { fprintf(stderr, "usage: %s [-w seconds]\n", argv[0]); return 64; }
    }
    setbuf(stdout, NULL);
    if (!usb_phone_config()) { puts("USB_CONFIG_ERROR expected_one_of=port_vid_pid,auto,port,any (see usb-device.conf)"); return 64; }
    printf("USB_POLICY mode=%s", usb_policy_name());
    for (unsigned i = 0; i < usb_policy_nports; i++) printf("%s%s", i ? "," : " ports=", usb_policy_ports[i]);
    if (usb_policy == USB_POLICY_FIXED) printf(" port=%s vid=%04x pid=%04x", usb_configured_port, usb_expected_vid, usb_expected_pid);
    printf("\n");

    static struct usb_cand list[USB_DISC_MAX];
    unsigned passes = watch ? watch + 1 : 2, picked = 0, settling = 0, n = 0;
    for (unsigned pass = 0; pass < passes; pass++) {
        if (pass) usleep(watch ? 1000000 : 1600000);
        n = usb_disc_scan(list, USB_DISC_MAX, now(), &picked, &settling);
        if (pass + 1 == passes || watch) for (unsigned i = 0; i < n; i++) usb_disc_log("usb-scan", &list[i]);
    }
    if (!usb_policy_discovers()) {
        unsigned m = 0; const char *port = NULL;
        for (unsigned i = 0; i < n; i++) if (legacy_match(&list[i])) { m++; port = list[i].port; }
        printf("RESULT policy=%s matches=%u%s%s\n", usb_policy_name(), m, port ? " port=" : "", port ? port : "");
        printf("NOTE: the verdicts above are the port/any filter, shown for information; this policy selects by %s.\n",
               usb_policy == USB_POLICY_FIXED ? "port + VID/PID" : "accessory IDs only");
        return m == 1 ? 0 : m ? 2 : 1;
    }
    const char *port = NULL;
    for (unsigned i = 0; i < n; i++) if (list[i].eligible || list[i].accessory) port = list[i].port;
    if(picked && settling){puts("RESULT waiting_for_complete_selection (no AOA request sent)");return 1;}
    if (picked == 1) printf("RESULT would_select=%s (no AOA request sent by this tool)\n", port);
    else if (picked > 1) printf("RESULT ambiguous candidates=%u: hu-aa.elf sends nothing; unplug one or use 'port <path>'\n", picked);
    else printf("RESULT none%s\n", settling ? " (some devices still settling; run again)" : "");
    return picked == 1 ? 0 : picked ? 2 : 1;
}
