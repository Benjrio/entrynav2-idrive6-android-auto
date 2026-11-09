/* Candidate filter for the "port" and "any" policies (usb_selection.h). Sysfs attribute reads only: this file never
 * opens a /dev/bus/usb node and never sends a USB request. Rules:
 *   R1 device names only (no root hubs/interfaces)   R2 port policy        R3 no hubs        R4 no Apple
 *   R5 settled: configured, interfaces present, devnum unchanged for USB_SETTLE_NS
 *   R6 no HID/printer/smart-card/video/Bluetooth; storage requires a separate positive Android interface
 *   R7 an Android signal: MTP/PTP, vendor-specific (ADB, MTP), RNDIS/ECM/NCM tethering
 *   R8 not an enumeration that already failed AOA GET_PROTOCOL
 * Ordinary usbfs/storage bindings permit control-only negotiation, never detach/claim before AOA.
 * A device already in AOA accessory mode (18d1:2d00/2d01) on an allowed port is accepted as is.
 */
#pragma once
#include <dirent.h>
#ifndef USB_SYSFS
#define USB_SYSFS "/sys/bus/usb/devices"
#endif
#define USB_SETTLE_NS 1500000000ULL
#define USB_DISC_MAX 64
#define USB_DISC_MAX_IFACES 32
static const char *usb_sysfs_root = USB_SYSFS;

struct usb_cand {
    char port[80];
    unsigned vid, pid, bus, dev, ifaces;
    int accessory, eligible, settling;
    char reason[64];
    char summary[160]; /* interface list for logs: cls/sub/proto:driver */
};

static int usb_aoa_ids(unsigned v, unsigned p) { return v == 0x18d1 && (p == 0x2d00 || p == 0x2d01); }

static int usb_sysfs_text(const char *dev, const char *attr, char *out, size_t size) {
    char path[256]; out[0] = 0;
    if (snprintf(path, sizeof path, "%s/%s/%s", usb_sysfs_root, dev, attr) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int ok = fgets(out, (int)size, f) != NULL; fclose(f);
    if (!ok) { out[0] = 0; return 0; }
    out[strcspn(out, "\r\n")] = 0;
    return 1;
}
static unsigned usb_sysfs_num(const char *dev, const char *attr, int base) {
    char t[40]; return usb_sysfs_text(dev, attr, t, sizeof t) && *t ? (unsigned)strtoul(t, NULL, base) : 0;
}
static void usb_sysfs_driver(const char *dev, char *out, size_t size) {
    char path[256], link[256]; out[0] = 0;
    if (snprintf(path, sizeof path, "%s/%s/driver", usb_sysfs_root, dev) >= (int)sizeof path) return;
    ssize_t n = readlink(path, link, sizeof link - 1); if (n <= 0) return;
    link[n] = 0; const char *base = strrchr(link, '/');
    snprintf(out, size, "%s", base ? base + 1 : link);
}

/* R5 bookkeeping: when each port's current enumeration was first seen. R8: enumerations that refused AOA. */
static struct { char port[80]; unsigned bus, dev; uint64_t since; } usb_seen[USB_DISC_MAX];
static struct { char port[80]; unsigned bus, dev; } usb_not_aoa[16];
static unsigned usb_not_aoa_count;

static uint64_t usb_seen_since(const char *port, unsigned bus, unsigned dev, uint64_t t) {
    unsigned free_slot = USB_DISC_MAX;
    for (unsigned i = 0; i < USB_DISC_MAX; i++) {
        if (!usb_seen[i].port[0]) { if (free_slot == USB_DISC_MAX) free_slot = i; continue; }
        if (strcmp(usb_seen[i].port, port)) continue;
        if (usb_seen[i].bus != bus || usb_seen[i].dev != dev) { usb_seen[i].bus = bus; usb_seen[i].dev = dev; usb_seen[i].since = t; }
        return usb_seen[i].since;
    }
    if (free_slot == USB_DISC_MAX) return t; /* table full: treat as just seen, stays "settling" */
    snprintf(usb_seen[free_slot].port, sizeof usb_seen[free_slot].port, "%s", port);
    usb_seen[free_slot].bus = bus; usb_seen[free_slot].dev = dev; usb_seen[free_slot].since = t;
    return t;
}
static void usb_disc_mark_not_aoa(const char *port, unsigned bus, unsigned dev) {
    unsigned i = usb_not_aoa_count < 16 ? usb_not_aoa_count++ : 15; /* keep the newest when full */
    snprintf(usb_not_aoa[i].port, sizeof usb_not_aoa[i].port, "%s", port); usb_not_aoa[i].bus = bus; usb_not_aoa[i].dev = dev;
}
static int usb_disc_is_not_aoa(const char *port, unsigned bus, unsigned dev) {
    for (unsigned i = 0; i < usb_not_aoa_count; i++)
        if (!strcmp(usb_not_aoa[i].port, port) && usb_not_aoa[i].bus == bus && usb_not_aoa[i].dev == dev) return 1;
    return 0;
}

static int usb_disc_reject(struct usb_cand *c, const char *why) { snprintf(c->reason, sizeof c->reason, "%s", why); return 0; }

/* Fills *c for one sysfs device name. Returns 1 when the device may be selected (eligible or accessory). */
static int usb_disc_eval(const char *name, struct usb_cand *c, uint64_t t) {
    memset(c, 0, sizeof *c);
    snprintf(c->port, sizeof c->port, "%s", name);
    if (!usb_port_name_valid(name)) return usb_disc_reject(c, "not-a-device");                       /* R1 */
    if (usb_policy == USB_POLICY_PORTS && !usb_policy_has_port(name)) return usb_disc_reject(c, "port-not-selected"); /* R2 */
    c->vid = usb_sysfs_num(name, "idVendor", 16); c->pid = usb_sysfs_num(name, "idProduct", 16);
    c->bus = usb_sysfs_num(name, "busnum", 10); c->dev = usb_sysfs_num(name, "devnum", 10);
    if (!c->vid || !c->bus || !c->dev) return usb_disc_reject(c, "absent");
    if (usb_sysfs_num(name, "bDeviceClass", 16) == 9) return usb_disc_reject(c, "hub");                  /* R3 */
    if (usb_aoa_ids(c->vid, c->pid)) { c->accessory = 1; snprintf(c->reason, sizeof c->reason, "accessory"); return 1; }
    if (c->vid == 0x05ac) return usb_disc_reject(c, "apple");                                            /* R4 */

    char cfg[16]; usb_sysfs_text(name, "bConfigurationValue", cfg, sizeof cfg);
    uint64_t since = usb_seen_since(name, c->bus, c->dev, t);
    if (!*cfg || strspn(cfg, "0123456789") != strlen(cfg) || !strtoul(cfg,NULL,10)) {
        c->settling = 1; return usb_disc_reject(c, "settling:unconfigured");
    }

    DIR *d = opendir(usb_sysfs_root);
    if (!d) return usb_disc_reject(c, "sysfs-unreadable");
    size_t nl = strlen(name); int signal = 0, android = 0, storage = 0, blocked = 0; struct dirent *e; size_t used = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, name, nl) || e->d_name[nl] != ':') continue;
        if (strchr(e->d_name + nl + 1, ':')) continue;
        if (c->ifaces == USB_DISC_MAX_IFACES) { usb_disc_reject(c,"too-many-interfaces"); blocked=1; break; }
        c->ifaces++;
        char attr[40];
        const char *required[]={"bInterfaceClass","bInterfaceSubClass","bInterfaceProtocol"};
        for(unsigned k=0;k<3;k++) if(!usb_sysfs_text(e->d_name,required[k],attr,sizeof attr) ||
            strlen(attr)!=2 || strspn(attr,"0123456789abcdefABCDEF")!=2) {
            usb_disc_reject(c,"incomplete-interface");blocked=1;
        }
        unsigned cls = usb_sysfs_num(e->d_name, "bInterfaceClass", 16), sub = usb_sysfs_num(e->d_name, "bInterfaceSubClass", 16),
                 proto = usb_sysfs_num(e->d_name, "bInterfaceProtocol", 16);
        char drv[64], iname[64]; usb_sysfs_driver(e->d_name, drv, sizeof drv); usb_sysfs_text(e->d_name, "interface", iname, sizeof iname);
        if (used < sizeof c->summary) {
            int w = snprintf(c->summary + used, sizeof c->summary - used, "%s%02x/%02x/%02x%s%s", used ? "," : "", cls, sub, proto, *drv ? ":" : "", drv);
            if (w > 0) used += (size_t)w;
        }
        if (blocked) continue;
        if(cls==0x08)storage=1;
        if(cls==0x06 || (cls==0xff && sub==0x42 && proto==1) || strstr(iname,"MTP") || strstr(iname,"Android"))android=1;
        const char *deny = cls == 0x03 ? "hid" : cls == 0x07 ? "printer" : cls == 0x0b ? "smart-card"
                         : cls == 0x0e ? "video" : cls == 0x09 ? "hub" : (cls == 0xe0 && sub == 0x01 && proto == 0x01) ? "bluetooth" : NULL;
        if (deny) { usb_disc_reject(c, deny); blocked = 1; continue; }                                  /* R6 */
        if (*drv) {
            /* Control-only AOA probe does not claim/detach ordinary phone interfaces.
             * usbfs and an extra storage interface are common Android composites.
             * Storage without a positive Android interface is rejected below. */
            if (strcmp(drv, "usbfs") && strcmp(drv,"usb-storage") && strcmp(drv, "rndis_host") && strcmp(drv, "cdc_ether") && strcmp(drv, "cdc_ncm")) {
                char why[64]; snprintf(why, sizeof why, "driver:%s", drv); usb_disc_reject(c, why); blocked = 1; continue; /* R6 */
            }
        }
        /* ff/ff/ff without an Android interface name is an undefined vendor
         * protocol (also used by internal HU bridges), not evidence of a phone. */
        if (android || cls == 0x06 || (cls == 0xff && proto != 0xff) || (cls == 0xe0 && sub == 0x01 && proto == 0x03) || (cls == 0xef && sub == 0x04 && proto == 0x01) ||
            (cls == 0x02 && (sub == 0x06 || sub == 0x0d)) || (cls == 0x0a && *drv)) signal = 1;           /* R7 */
    }
    closedir(d);
    if (blocked) return 0;
    if(storage && !android)return usb_disc_reject(c,"storage-without-android-interface");
    if (!c->ifaces) { c->settling = 1; return usb_disc_reject(c, "settling:no-interfaces"); }           /* R5 */
    if (t - since < USB_SETTLE_NS) { c->settling = 1; return usb_disc_reject(c, "settling:new"); }       /* R5 */
    if (!signal) return usb_disc_reject(c, "no-android-signal");                                       /* R7 */
    if(c->ifaces!=usb_sysfs_num(name,"bNumInterfaces",10))return usb_disc_reject(c,"incomplete-interface-count");
    if(c->bus!=usb_sysfs_num(name,"busnum",10) || c->dev!=usb_sysfs_num(name,"devnum",10))return usb_disc_reject(c,"enumeration-changed");
    if (usb_disc_is_not_aoa(name, c->bus, c->dev)) return usb_disc_reject(c, "not-aoa-remembered");    /* R8 */
    c->eligible = 1; snprintf(c->reason, sizeof c->reason, "eligible");
    return 1;
}

/* One pass over sysfs. Fills up to max entries (all devices, for logging) and returns how many it filled; *picked
 * gets the number of selectable devices (eligible + accessory), *settling the number still settling. */
static unsigned usb_disc_scan(struct usb_cand *out, unsigned max, uint64_t t, unsigned *picked, unsigned *settling) {
    *picked = *settling = 0;
    DIR *d = opendir(usb_sysfs_root); if (!d) return 0;
    struct dirent *e; unsigned n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3)) continue;
        if(n==max){*picked=0;*settling=1;closedir(d);return 0;}
        if (usb_disc_eval(e->d_name, &out[n], t)) (*picked)++;
        else if (out[n].settling) (*settling)++;
        n++;
    }
    closedir(d);
    return n;
}

/* Log a device's verdict only when it changes (per port), so a 20 s wait doesn't flood the log. */
static struct { char port[80]; unsigned dev; char reason[64]; } usb_logged[USB_DISC_MAX];
static void usb_disc_log(const char *source, const struct usb_cand *c) {
    unsigned slot = USB_DISC_MAX;
    for (unsigned i = 0; i < USB_DISC_MAX; i++) {
        if (!strcmp(usb_logged[i].port, c->port)) { slot = i; break; }
        if (!usb_logged[i].port[0] && slot == USB_DISC_MAX) slot = i;
    }
    if (slot < USB_DISC_MAX && !strcmp(usb_logged[slot].port, c->port) && usb_logged[slot].dev == c->dev && !strcmp(usb_logged[slot].reason, c->reason)) return;
    if (slot < USB_DISC_MAX) {
        snprintf(usb_logged[slot].port, sizeof usb_logged[slot].port, "%s", c->port);
        usb_logged[slot].dev = c->dev; snprintf(usb_logged[slot].reason, sizeof usb_logged[slot].reason, "%s", c->reason);
    }
    printf("USB_CANDIDATE source=%s port=%s vid=%04x pid=%04x bus=%u dev=%u verdict=%s ifaces=%s\n",
           source, c->port, c->vid, c->pid, c->bus, c->dev, c->reason, *c->summary ? c->summary : "-");
}
