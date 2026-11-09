/* Host tests for usb_selection.h (config parser) and usb_discovery.h (sysfs-only filter).
 * Build and run on Linux/WSL:  sh tests/run.sh
 * Each case builds a fake /sys/bus/usb/devices tree in a temporary folder; nothing touches real USB devices.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/usb_selection.h"
#include "../src/usb_discovery.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[256];
static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(1); }
    fputs(text, f); fclose(f);
}
static void attr(const char *dev, const char *name, const char *value) {
    char p[512]; snprintf(p, sizeof p, "%s/%s/%s", root, dev, name);
    char v[128]; snprintf(v, sizeof v, "%s\n", value); write_file(p, v);
}
static void mkdev(const char *name, const char *vid, const char *pid, const char *cls, unsigned bus, unsigned dev, const char *cfg) {
    char p[512], n[16]; snprintf(p, sizeof p, "%s/%s", root, name); mkdir(p, 0755);
    attr(name, "idVendor", vid); attr(name, "idProduct", pid); attr(name, "bDeviceClass", cls);
    snprintf(n, sizeof n, "%u", bus); attr(name, "busnum", n); snprintf(n, sizeof n, "%u", dev); attr(name, "devnum", n);
    attr(name, "bConfigurationValue", cfg);
}
static void mkif(const char *dev, const char *ifn, const char *cls, const char *sub, const char *proto, const char *driver, const char *iname) {
    char name[128], p[512]; snprintf(name, sizeof name, "%s:%s", dev, ifn);
    snprintf(p, sizeof p, "%s/%s", root, name); mkdir(p, 0755);
    attr(name, "bInterfaceClass", cls); attr(name, "bInterfaceSubClass", sub); attr(name, "bInterfaceProtocol", proto);
    if (iname) attr(name, "interface", iname);
    if (driver) { char t[256]; snprintf(p, sizeof p, "%s/%s/driver", root, name); snprintf(t, sizeof t, "../../../bus/usb/drivers/%s", driver); symlink(t, p); }
}
static void fresh_tree(void) {
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", root, root);
    if (system(cmd)) exit(1);
    memset(usb_seen, 0, sizeof usb_seen); memset(usb_logged, 0, sizeof usb_logged); usb_not_aoa_count = 0;
}
static int conf(const char *text) { write_file("usb-device.conf", text); return usb_phone_config(); }

static const struct usb_cand *find(const struct usb_cand *l, unsigned n, const char *port) {
    for (unsigned i = 0; i < n; i++) if (!strcmp(l[i].port, port)) return &l[i];
    return NULL;
}
#define T0 1000000000000ULL
#define LATER (T0 + 2000000000ULL)

static void test_parser(void) {
    CHECK(!conf("USB_PORT VID PID\n"), "placeholder must fail");
    CHECK(conf("1-1 18d1 4ee7\n") && usb_policy == USB_POLICY_FIXED && usb_expected_vid == 0x18d1 && !strcmp(usb_configured_port, "1-1"), "legacy line");
    CHECK(conf("1-1.2 18D1 4EE7") && usb_expected_pid == 0x4ee7, "legacy hub port, upper hex, no newline");
    CHECK(!conf("1-1 0000 4ee7\n"), "VID 0 rejected");
    CHECK(!conf("1-1 0x18d1 4ee7\n"), "0x rejected");
    CHECK(!conf("11 18d1 4ee7\n"), "port without dash rejected");
    CHECK(conf("auto\n") && usb_policy == USB_POLICY_AUTO, "auto");
    CHECK(conf("any\n") && usb_policy == USB_POLICY_ANY && !*usb_configured_port, "any");
    CHECK(conf("# comment\n\n  any   # trailing comment\n") && usb_policy == USB_POLICY_ANY, "comments and blank lines");
    CHECK(conf("port 1-1\n") && usb_policy == USB_POLICY_PORTS && usb_policy_nports == 1 && usb_policy_has_port("1-1"), "one port");
    CHECK(conf("port 1-1 2-1 1-1.4\n") && usb_policy_nports == 3 && usb_policy_has_port("1-1.4") && !usb_policy_has_port("1-2"), "three ports");
    CHECK(!conf("port\n"), "port without a path");
    CHECK(!conf("port 1-1 1-1\n"), "duplicate port");
    CHECK(!conf("port 1-1 2-1 3-1 4-1 5-1\n"), "too many ports");
    CHECK(!conf("port 1-\n") && !conf("port -1\n") && !conf("port 1-1.\n") && !conf("port 1-1..2\n") && !conf("port usb1\n"), "bad port names");
    CHECK(!conf("any\nauto\n"), "two directives");
    CHECK(!conf("any extra\n"), "extra token");
    CHECK(!conf("\n# only comments\n"), "no directive");
    char big[600]; memset(big, '#', sizeof big - 2); big[sizeof big - 2] = '\n'; big[sizeof big - 1] = 0;
    CHECK(!conf(big), "over 512 bytes");
    char boundary[514]; memset(boundary, '#', sizeof boundary);
    memcpy(boundary, "any\n#", 5); boundary[512] = 0;
    CHECK(conf(boundary) && usb_policy == USB_POLICY_ANY, "exactly 512 bytes including comments is accepted");
    boundary[512] = '#'; boundary[513] = 0;
    CHECK(!conf(boundary), "513 bytes including comments is rejected");
}

static void test_shipped_config(const char *path) {
    char b[1024]; FILE *f = fopen(path, "r");
    CHECK(f != NULL, "shipped configuration is readable");
    if (!f) return;
    size_t n = fread(b, 1, sizeof b - 1, f); int ok = !ferror(f) && feof(f); fclose(f); b[n] = 0;
    CHECK(ok && n <= 512 && conf(b) && usb_policy == USB_POLICY_ANY,
          "shipped configuration fits the parser and selects any (%zu bytes)", n);
}

static void test_filter(void) {
    struct usb_cand l[USB_DISC_MAX]; unsigned n, picked, settling; const struct usb_cand *c;
    fresh_tree(); conf("any\n");
    mkdev("usb1", "1d6b", "0002", "09", 1, 1, "1");                                    /* root hub */
    mkdev("1-1", "18d1", "4ee1", "00", 1, 5, "1"); mkif("1-1", "1.0", "ff", "ff", "00", NULL, "MTP"); /* Pixel, MTP */
    mkdev("1-2", "0781", "5567", "00", 1, 6, "1"); mkif("1-2", "1.0", "08", "06", "50", "usb-storage", NULL);
    mkdev("1-3", "046d", "c31c", "00", 1, 7, "1"); mkif("1-3", "1.0", "03", "01", "01", "usbhid", NULL);
    mkdev("1-4", "05e3", "0608", "09", 1, 8, "1"); mkif("1-4", "1.0", "09", "00", "00", "hub", NULL);
    mkdev("1-5", "05ac", "12a8", "00", 1, 9, "1"); mkif("1-5", "1.0", "06", "01", "01", NULL, NULL);
    mkdev("2-1", "0a12", "0001", "e0", 2, 2, "1"); mkif("2-1", "1.0", "e0", "01", "01", "btusb", NULL);
    mkdev("2-2", "1199", "9071", "00", 2, 3, "1"); mkif("2-2", "1.0", "ff", "ff", "ff", "option", NULL);
    mkdev("2-3", "04e8", "6860", "00", 2, 4, "1"); mkif("2-3", "1.0", "ff", "42", "01", NULL, "ADB Interface");  /* Samsung, ADB only */
    mkdev("2-4", "2717", "ff48", "00", 2, 5, "1"); mkif("2-4", "1.0", "ff", "ff", "00", "usbfs", "MTP");        /* claimed MTP */
    mkdev("2-5", "18d1", "2d01", "00", 2, 6, "1"); mkif("2-5", "1.0", "ff", "ff", "00", NULL, NULL);          /* accessory */
    mkdev("2-6", "18d1", "4ee7", "00", 2, 7, "");                                                            /* unconfigured */
    mkdev("2-7", "1234", "5678", "00", 2, 8, "1"); mkif("2-7", "1.0", "01", "01", "00", NULL, NULL);           /* audio only */
    mkdev("3-1", "22b8", "2e82", "00", 3, 2, "1"); mkif("3-1", "1.0", "e0", "01", "03", "rndis_host", NULL); mkif("3-1", "1.1", "0a", "00", "00", "rndis_host", NULL); /* tethering */

    n = usb_disc_scan(l, USB_DISC_MAX, T0, &picked, &settling);
    CHECK(!find(l, n, "usb1"), "root hub is skipped");
    c = find(l, n, "1-1"); CHECK(c && c->settling && !strcmp(c->reason, "settling:new"), "new device settles first: %s", c ? c->reason : "?");
    c = find(l, n, "2-5"); CHECK(c && c->accessory, "accessory accepted at once");

    n = usb_disc_scan(l, USB_DISC_MAX, LATER, &picked, &settling);
    struct { const char *port, *reason; } want[] = {
        {"1-1", "eligible"}, {"1-2", "storage"}, {"1-3", "hid"}, {"1-4", "hub"}, {"1-5", "apple"}, {"2-1", "bluetooth"},
        {"2-2", "driver:option"}, {"2-3", "eligible"}, {"2-4", "claimed-by-userspace"}, {"2-5", "accessory"},
        {"2-6", "settling:unconfigured"}, {"2-7", "no-android-signal"}, {"3-1", "eligible"},
    };
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
        c = find(l, n, want[i].port);
        CHECK(c && !strcmp(c->reason, want[i].reason), "%s: want %s, got %s", want[i].port, want[i].reason, c ? c->reason : "(missing)");
    }
    CHECK(picked == 4, "1-1, 2-3, 2-5 and 3-1 selectable: %u", picked);
    CHECK(settling == 1, "only 2-6 still settling: %u", settling);

    /* R8: an enumeration that refused AOA is not offered again; a replug (new devnum) clears it. */
    usb_disc_mark_not_aoa("1-1", 1, 5);
    CHECK(!usb_disc_eval("1-1", &l[0], LATER) && !strcmp(l[0].reason, "not-aoa-remembered"), "not-aoa remembered: %s", l[0].reason);
    attr("1-1", "devnum", "15");
    CHECK(!usb_disc_eval("1-1", &l[0], LATER) && !strcmp(l[0].reason, "settling:new"), "replug settles again: %s", l[0].reason);
    CHECK(usb_disc_eval("1-1", &l[0], LATER + 2000000000ULL) && l[0].eligible, "replugged phone eligible again: %s", l[0].reason);
}

static void test_port_policy(void) {
    struct usb_cand l[USB_DISC_MAX]; unsigned picked, settling;
    fresh_tree(); conf("port 1-1.2\n");
    mkdev("1-1", "05e3", "0608", "09", 1, 2, "1"); mkif("1-1", "1.0", "09", "00", "00", "hub", NULL);
    mkdev("1-1.2", "18d1", "4ee1", "00", 1, 3, "1"); mkif("1-1.2", "1.0", "ff", "ff", "00", NULL, "MTP");
    mkdev("1-1.3", "04e8", "6860", "00", 1, 4, "1"); mkif("1-1.3", "1.0", "ff", "42", "01", NULL, NULL);
    usb_disc_scan(l, USB_DISC_MAX, T0, &picked, &settling);
    unsigned n = usb_disc_scan(l, USB_DISC_MAX, LATER, &picked, &settling);
    CHECK(picked == 1, "only the phone on 1-1.2: %u", picked);
    const struct usb_cand *c = find(l, n, "1-1.3"); CHECK(c && !strcmp(c->reason, "port-not-selected"), "other port ignored");
    c = find(l, n, "1-1"); CHECK(c && !strcmp(c->reason, "port-not-selected"), "hub port ignored");
}

static void test_ambiguous(void) {
    struct usb_cand l[USB_DISC_MAX]; unsigned picked, settling;
    fresh_tree(); conf("any\n");
    mkdev("1-1", "18d1", "4ee1", "00", 1, 3, "1"); mkif("1-1", "1.0", "ff", "ff", "00", NULL, "MTP");
    mkdev("1-2", "04e8", "6860", "00", 1, 4, "1"); mkif("1-2", "1.0", "06", "01", "01", NULL, NULL);
    usb_disc_scan(l, USB_DISC_MAX, T0, &picked, &settling);
    usb_disc_scan(l, USB_DISC_MAX, LATER, &picked, &settling);
    CHECK(picked == 2, "two phones are both candidates (the caller then refuses): %u", picked);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s /absolute/path/to/runtime/aa/usb-device.conf\n", argv[0]); return 64; }
    snprintf(root, sizeof root, "/tmp/usb-discovery-test-%d/sys", (int)getpid());
    char work[300]; snprintf(work, sizeof work, "/tmp/usb-discovery-test-%d", (int)getpid());
    mkdir(work, 0700); if (chdir(work)) return 1;
    usb_sysfs_root = root;
    test_parser(); test_shipped_config(argv[1]); test_filter(); test_port_policy(); test_ambiguous();
    char cmd[350]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", work); if (system(cmd)) {}
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
