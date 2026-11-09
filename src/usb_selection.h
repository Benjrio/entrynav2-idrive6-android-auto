/* Phone selection policy, read once from the private runtime directory (usb-device.conf).
 *   <port> <VID> <PID>    one phone by identity on one port (original format)
 *   auto                  only a device already in standard AOA accessory mode (original format)
 *   port <p> [<p> ...]    any AOA-capable phone, only on these sysfs port paths (up to USB_MAX_PORTS)
 *   any                   any AOA-capable phone on any port, when exactly one candidate passes the filter
 * '#' starts a comment; blank lines are ignored; exactly one directive line. The port/any policies select with
 * usb_discovery.h (sysfs only); AOA negotiation still validates the protocol, one device at a time.
 */
#pragma once
enum usb_policy_kind { USB_POLICY_NONE, USB_POLICY_FIXED, USB_POLICY_AUTO, USB_POLICY_PORTS, USB_POLICY_ANY };
#define USB_MAX_PORTS 4
static enum usb_policy_kind usb_policy;
static char usb_policy_ports[USB_MAX_PORTS][80];
static unsigned usb_policy_nports;
static unsigned usb_expected_vid, usb_expected_pid;
static char usb_configured_port[80];

static const char *usb_policy_name(void) {
    switch (usb_policy) {
    case USB_POLICY_FIXED: return "port-vid-pid";
    case USB_POLICY_AUTO: return "auto-accessory-only";
    case USB_POLICY_PORTS: return "port";
    case USB_POLICY_ANY: return "any";
    default: return "none";
    }
}
static int usb_policy_discovers(void) { return usb_policy == USB_POLICY_PORTS || usb_policy == USB_POLICY_ANY; }

/* Strict sysfs device name: <bus>-<port>[.<port>...], e.g. 1-1 or 1-1.2. */
static int usb_port_name_valid(const char *p) {
    size_t n = strlen(p), i = 0, digits;
    if (!n || n >= sizeof usb_configured_port) return 0;
    for (digits = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++) digits++;
    if (!digits || i >= n || p[i++] != '-') return 0;
    for (;;) {
        for (digits = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++) digits++;
        if (!digits) return 0;
        if (i == n) return 1;
        if (p[i++] != '.') return 0;
    }
}
static int usb_policy_has_port(const char *p) {
    for (unsigned i = 0; i < usb_policy_nports; i++) if (!strcmp(usb_policy_ports[i], p)) return 1;
    return 0;
}

/* Returns 1 and sets the policy, or 0 (the caller reports USB_CONFIG_ERROR). */
static int usb_phone_config(void) {
    usb_policy = USB_POLICY_NONE; usb_policy_nports = 0; usb_expected_vid = usb_expected_pid = 0;
    usb_configured_port[0] = 0;
    int fd = open("usb-device.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st; char b[520] = {0};
    /* The bound includes comments: an oversized example config would prevent startup before parsing "any". */
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > 512) { close(fd); return 0; }
    ssize_t n = read(fd, b, sizeof(b) - 1); close(fd);
    if (n != st.st_size || memchr(b, 0, (size_t)n)) return 0;

    char *directive = NULL, *save = NULL;
    for (char *line = strtok_r(b, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        if (line[strspn(line, " \t\r")] == 0) continue;
        if (directive) return 0;  /* more than one directive */
        directive = line;
    }
    if (!directive) return 0;
    char *tok[USB_MAX_PORTS + 2]; unsigned ntok = 0; char *ts = NULL;
    for (char *t = strtok_r(directive, " \t\r", &ts); t; t = strtok_r(NULL, " \t\r", &ts)) {
        if (ntok == USB_MAX_PORTS + 2) return 0;
        tok[ntok++] = t;
    }
    if (ntok == 1 && !strcmp(tok[0], "auto")) { usb_policy = USB_POLICY_AUTO; return 1; }
    if (ntok == 1 && !strcmp(tok[0], "any")) { usb_policy = USB_POLICY_ANY; return 1; }
    if (!strcmp(tok[0], "port")) {
        if (ntok < 2 || ntok > USB_MAX_PORTS + 1) return 0;
        for (unsigned i = 1; i < ntok; i++) {
            if (!usb_port_name_valid(tok[i]) || usb_policy_has_port(tok[i])) return 0;
            strcpy(usb_policy_ports[usb_policy_nports++], tok[i]);
        }
        usb_policy = USB_POLICY_PORTS; return 1;
    }
    if (ntok != 3) return 0;
    /* Original "<port> <VID> <PID>" rules, unchanged. */
    const char *port = tok[0], *vid = tok[1], *pid = tok[2];
    if (strlen(port) >= sizeof usb_configured_port || !strchr(port, '-') || strspn(port, "0123456789-.") != strlen(port)) return 0;
    if (strlen(vid) != 4 || strlen(pid) != 4 || strspn(vid, "0123456789abcdefABCDEF") != 4 || strspn(pid, "0123456789abcdefABCDEF") != 4) return 0;
    usb_expected_vid = (unsigned)strtoul(vid, NULL, 16); usb_expected_pid = (unsigned)strtoul(pid, NULL, 16);
    if (!usb_expected_vid) return 0;
    strcpy(usb_configured_port, port);
    usb_policy = USB_POLICY_FIXED; return 1;
}
static int usb_phone_matches(unsigned vid, unsigned pid) {
    return usb_policy == USB_POLICY_FIXED && usb_expected_vid != 0 && vid == usb_expected_vid && pid == usb_expected_pid;
}
