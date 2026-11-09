/* Host tests for src/aa-input.h: exact protobuf bytes of InputEventIndication and the key/rotary rules.
 * Build and run on Linux/WSL:  sh tests/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- the minimal environment aa-input.h expects (same encoders as src/aa-protocol.h) --- */
static void require(int ok, const char *label) { if (!ok) { printf("FATAL %s\n", label); exit(2); } }
static unsigned aa_gen = 1, aa_bound = 1;
static uint64_t fake_ns = 5000000000ULL;
static uint64_t now(void) { return fake_ns; }
struct AaPb { uint8_t b[2048]; unsigned n; };
static void aa_var(struct AaPb *p, uint64_t v) { do { require(p->n < sizeof(p->b), "bound"); unsigned x = v & 127; v >>= 7; p->b[p->n++] = (uint8_t)(x | (v ? 128 : 0)); } while (v); }
static void aa_v(struct AaPb *p, unsigned field, uint64_t v) { aa_var(p, (uint64_t)field * 8); aa_var(p, v); }
static void aa_b(struct AaPb *p, unsigned field, const uint8_t *b, unsigned n) { aa_var(p, (uint64_t)field * 8 + 2); aa_var(p, n); memcpy(p->b + p->n, b, n); p->n += n; }
static void aa_sub(struct AaPb *p, unsigned field, struct AaPb *s) { aa_b(p, field, s->b, s->n); }
#define MAXSENT 128
static struct { unsigned channel, id, n; uint8_t b[256]; } sent[MAXSENT];
static unsigned nsent, send_fail_after = MAXSENT; static int send_ok = 1;
static int aa_send(unsigned channel, unsigned id, const uint8_t *body, unsigned n, int encrypted, int control) {
    (void)encrypted; (void)control;
    if (!send_ok || nsent == send_fail_after) return 0;
    require(nsent < MAXSENT && n <= sizeof sent[0].b, "test capture bound");
    sent[nsent].channel = channel; sent[nsent].id = id; sent[nsent].n = n; memcpy(sent[nsent].b, body, n); nsent++;
    return 1;
}
#include "../src/aa-input.h"

/* --- a small decoder for what was sent --- */
struct Decoded { int kind; unsigned code, down, longpress, meta; int64_t delta; uint64_t ts; }; /* kind 4 = button, 6 = relative */
static int getvar(const uint8_t *p, unsigned n, unsigned *i, uint64_t *v) {
    *v = 0; for (unsigned s = 0; s < 70; s += 7) { if (*i >= n) return 0; unsigned b = p[(*i)++]; *v |= (uint64_t)(b & 127) << s; if (!(b & 128)) return 1; } return 0;
}
/* Returns 1 for a well-formed report with exactly one event. */
static int decode(unsigned idx, struct Decoded *d) {
    memset(d, 0, sizeof *d);
    if (sent[idx].channel != 1 || sent[idx].id != 0x8001) return 0;
    const uint8_t *p = sent[idx].b; unsigned n = sent[idx].n, i = 0, events = 0; uint64_t tag, v;
    while (i < n) {
        if (!getvar(p, n, &i, &tag)) return 0;
        if (tag == 0x08) { if (!getvar(p, n, &i, &d->ts)) return 0; continue; }
        if (tag != 0x22 && tag != 0x32) return 0;
        d->kind = tag == 0x22 ? 4 : 6;
        uint64_t len; if (!getvar(p, n, &i, &len) || i + len > n) return 0;
        unsigned end = i + (unsigned)len;
        while (i < end) {                                   /* list: repeated field 1 */
            if (!getvar(p, end, &i, &tag) || tag != 0x0a) return 0;
            uint64_t elen; if (!getvar(p, end, &i, &elen) || i + elen > end) return 0;
            unsigned eend = i + (unsigned)elen; events++;
            while (i < eend) {
                if (!getvar(p, eend, &i, &tag) || (tag & 7) != 0 || !getvar(p, eend, &i, &v)) return 0;
                switch (tag >> 3) {
                case 1: d->code = (unsigned)v; break;
                case 2: if (d->kind == 4) d->down = (unsigned)v; else d->delta = (int64_t)v; break;
                case 3: d->meta = (unsigned)v; break;
                case 4: d->longpress = (unsigned)v; break;
                default: return 0;
                }
            }
        }
    }
    return events == 1;
}
static void reset(void) {
    nsent = 0; send_ok = 1; send_fail_after = MAXSENT; aa_bound = 1; aa_gen++;
    aa_in_read = aa_in_write; aa_in_rotary = 0;
    aa_in_keys_sent = aa_in_rotary_sent = aa_in_dropped = aa_in_released = aa_in_longpress_sent = 0;
}
static int key_report(unsigned idx, unsigned code, unsigned down, unsigned longpress) {
    struct Decoded d; return decode(idx, &d) && d.kind == 4 && d.code == code && d.down == down && d.longpress == longpress && d.meta == 0;
}

static void test_advertise(void) {
    struct AaPb codes = {0}; aa_input_advertise(&codes);
    unsigned i = 0, k = 0; uint64_t v; int ok = 1;
    while (i < codes.n) { if (!getvar(codes.b, codes.n, &i, &v) || k >= AA_INPUT_KEYCOUNT || v != aa_input_keycodes[k]) ok = 0; k++; }
    CHECK(ok && k == AA_INPUT_KEYCOUNT, "advertised keycodes decode back to the table");
    CHECK(!aa_input_supported(82) && !aa_input_supported(3), "MENU (82) and HOME (3) are not advertised");
    CHECK(aa_input_supported(AA_KEY_ROTARY) && aa_input_supported(AA_KEY_DPAD_CENTER) && aa_input_supported(AA_KEY_BACK), "core keys advertised");
}

static void test_exact_bytes(void) {
    reset(); fake_ns = 7000000000ULL;                        /* ts = 7,000,000 us */
    CHECK(aa_key(AA_KEY_DPAD_CENTER, 1, 0) && aa_input_pump() && nsent == 1, "one report for a press");
    static const uint8_t press[] = { 0x08, 0xc0, 0x9f, 0xab, 0x03, 0x22, 0x0a, 0x0a, 0x08, 0x08, 0x17, 0x10, 0x01, 0x18, 0x00, 0x20, 0x00 };
    CHECK(sent[0].n == sizeof press && !memcmp(sent[0].b, press, sizeof press), "press bytes: field1 ts, field4 { field1 { 1:23 2:1 3:0 4:0 } }");
    nsent = 0; aa_rotary(-2); CHECK(aa_input_pump() && nsent == 1, "one report for a rotation");
    static const uint8_t rot[] = { 0x08, 0xc0, 0x9f, 0xab, 0x03, 0x32, 0x11, 0x0a, 0x0f, 0x08, 0x80, 0x80, 0x04,
                                   0x10, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
    CHECK(sent[0].n == sizeof rot && !memcmp(sent[0].b, rot, sizeof rot), "rotation bytes: field6 { field1 { 1:65536 2:-2 (int32 as 10-byte varint) } }");
    aa_key(AA_KEY_DPAD_CENTER, 0, 0); aa_input_pump();
}

static void test_rules(void) {
    reset();
    CHECK(!aa_key(82, 1, 0), "MENU is rejected");
    CHECK(!aa_key(AA_KEY_ROTARY, 1, 0), "rotary is not a key");
    CHECK(!aa_key(AA_KEY_BACK, 0, 0) && aa_input_pump() && nsent == 0, "stray up is dropped and nothing is sent");

    reset(); aa_rotary(20); aa_input_pump();
    struct Decoded d[3]; int ok = nsent == 3;
    for (unsigned i = 0; ok && i < 3; i++) ok = decode(i, &d[i]) && d[i].kind == 6 && d[i].code == AA_KEY_ROTARY;
    CHECK(ok && d[0].delta == 8 && d[1].delta == 8 && d[2].delta == 4, "fast spin split into +8,+8,+4");
    reset(); aa_rotary(3); aa_rotary(-5); aa_input_pump();
    CHECK(nsent == 1 && decode(0, &d[0]) && d[0].delta == -2, "rotation accumulates before the pump");
    reset(); aa_rotary(1000000); CHECK(aa_in_rotary == AA_ROTARY_LIMIT, "accumulated rotation is bounded");

    reset(); fake_ns = 10000000000ULL;
    aa_key(AA_KEY_DPAD_CENTER, 1, 0); aa_input_pump();
    fake_ns += 500000000ULL; aa_input_pump();
    CHECK(nsent == 1, "no long press before 800 ms");
    fake_ns += 400000000ULL; aa_input_pump(); aa_input_pump();
    CHECK(nsent == 2 && key_report(1, AA_KEY_DPAD_CENTER, 1, 1), "one synthetic long press after 800 ms");
    aa_key(AA_KEY_DPAD_CENTER, 0, 0); aa_input_pump();
    CHECK(nsent == 3 && key_report(2, AA_KEY_DPAD_CENTER, 0, 0) && aa_in_longpress_sent == 1, "release after long press");

    reset(); aa_key(AA_KEY_BACK, 1, 0); aa_key(AA_KEY_BACK, 1, 1); aa_key(AA_KEY_BACK, 1, 1); aa_input_pump();
    CHECK(nsent == 2 && key_report(1, AA_KEY_BACK, 1, 1), "explicit long press from the backend, only once");
    aa_key(AA_KEY_BACK, 0, 0); aa_input_pump();

    reset(); aa_key(AA_KEY_DPAD_CENTER, 1, 0); aa_key(AA_KEY_BACK, 1, 0); aa_input_pump(); nsent = 0;
    aa_rotary(5); aa_key(AA_KEY_DPAD_UP, 1, 0); aa_input_release_all("test");
    aa_input_pump();
    int ups = 0; for (unsigned i = 0; i < nsent; i++) { struct Decoded x; if (decode(i, &x) && x.kind == 4 && !x.down) ups++; }
    CHECK(ups == 2 && nsent == 2 && aa_in_rotary == 0, "release-all releases sent presses and drops unsent input (%u sent, %d ups)", nsent, ups);
    aa_input_pump(); CHECK(nsent == 2, "nothing more after release-all");

    reset(); aa_bound = 0; aa_key(AA_KEY_DPAD_CENTER, 1, 0); aa_rotary(2); aa_input_pump();
    CHECK(nsent == 0 && !aa_input_held(AA_KEY_DPAD_CENTER), "not bound: discarded, nothing held");

    reset(); aa_key(AA_KEY_DPAD_CENTER, 1, 0); aa_gen++; aa_input_pump();
    CHECK(nsent == 0, "event of an older session is not sent");
    CHECK(aa_key(AA_KEY_DPAD_CENTER, 1, 0), "a key held in an older session doesn't block the new one");
    aa_key(AA_KEY_DPAD_CENTER, 0, 0); aa_input_pump();

    reset(); unsigned accepted = 0;
    for (unsigned i = 0; i < 40; i++) { accepted += (unsigned)aa_key(AA_KEY_DPAD_CENTER, 1, 0); accepted += (unsigned)aa_key(AA_KEY_DPAD_CENTER, 0, 0); }
    CHECK(accepted == AA_INPUT_SLOTS && aa_in_dropped > 0, "queue keeps %u events, the rest are dropped (accepted %u)", AA_INPUT_SLOTS, accepted);
    aa_input_pump();
    CHECK(!aa_input_held(AA_KEY_DPAD_CENTER), "after overflow no key is left held");
    int downs = 0; ups = 0; for (unsigned i = 0; i < nsent; i++) { struct Decoded x; if (decode(i, &x)) { if (x.down) downs++; else ups++; } }
    CHECK(downs == ups, "every down that was sent has its up (%d/%d)", downs, ups);

    reset(); const unsigned keys[] = { 4, 5, 19, 20, 21, 22, 23, 87, 88 }; accepted = 0;
    for (unsigned i = 0; i < 9; i++) accepted += (unsigned)aa_key(keys[i], 1, 0);
    CHECK(accepted == AA_INPUT_HELD, "at most %u keys held at once (accepted %u)", AA_INPUT_HELD, accepted);
    aa_input_release_all("test"); aa_input_pump();

    reset(); aa_key(AA_KEY_BACK, 1, 0); send_ok = 0;
    CHECK(!aa_input_pump(), "a send failure is reported to the caller");
}

static void test_provisional_ccu(void) {
    reset(); aa_input_ccu_front_event(13, 2); aa_input_ccu_front_event(8, 1); aa_input_ccu_front_event(8, 0); aa_input_ccu_front_event(99, 1);
    aa_input_pump();
    struct Decoded d; int ok = nsent == 3 && decode(2, &d) && d.kind == 6 && d.delta == 2;
    CHECK(ok && key_report(0, AA_KEY_DPAD_CENTER, 1, 0) && key_report(1, AA_KEY_DPAD_CENTER, 0, 0), "provisional CCUFrontEvent mapping (keys first, then rotation)");
}

/* An odd number of transitions leaves CENTER down and fills the queue behind BACK. */
static void fill_after_back(void) {
    for (unsigned i = 0; i < AA_INPUT_SLOTS - 1; i++) aa_key(AA_KEY_DPAD_CENTER, !(i % 2), 0);
}

static void test_overflow_release(void) {
    reset(); aa_key(AA_KEY_BACK, 1, 0); fill_after_back();
    CHECK(!aa_key(AA_KEY_BACK, 0, 0) && !aa_key(AA_KEY_DPAD_CENTER, 0, 0), "two releases overflow a full queue");
    CHECK(!aa_key(AA_KEY_BACK, 1, 0) && !aa_key(AA_KEY_BACK, 1, 1), "pending release blocks repeat and long press");
    fake_ns += 1000000000ULL;
    CHECK(aa_input_pump() && nsent == 66, "64 queued transitions precede two fallback releases");
    CHECK(key_report(0, AA_KEY_BACK, 1, 0) && key_report(64, AA_KEY_BACK, 0, 0) &&
          key_report(65, AA_KEY_DPAD_CENTER, 0, 0), "overflow releases follow their pending presses");
    aa_input_pump(); CHECK(nsent == 66, "no duplicate release or synthetic long press after overflow");
    CHECK(aa_key(AA_KEY_BACK, 1, 0) && aa_key(AA_KEY_BACK, 0, 0) && aa_input_pump() && nsent == 68,
          "new press works after overflow recovery");

    reset(); aa_key(AA_KEY_BACK, 1, 0); fill_after_back(); aa_key(AA_KEY_BACK, 0, 0);
    send_fail_after = AA_INPUT_SLOTS;
    CHECK(!aa_input_pump() && nsent == 64, "failure sending fallback release is reported");
    send_fail_after = MAXSENT;
    CHECK(aa_input_pump() && nsent == 65 && key_report(64, AA_KEY_BACK, 0, 0), "retry preserves the required fallback release");
}

static void test_release_all_delivery(void) {
    reset(); aa_key(AA_KEY_BACK, 1, 0); aa_input_pump(); aa_key(AA_KEY_BACK, 0, 0);
    aa_input_release_all("queued-up"); aa_input_release_all("repeated"); aa_input_pump();
    CHECK(nsent == 2 && key_report(1, AA_KEY_BACK, 0, 0), "release-all preserves an up for a press already sent");
    aa_input_release_all("already-released"); aa_input_pump();
    CHECK(nsent == 2, "release-all is idempotent");

    reset(); aa_key(AA_KEY_BACK, 1, 0); aa_key(AA_KEY_BACK, 0, 0); aa_rotary(3);
    aa_input_release_all("unsent"); aa_input_pump();
    CHECK(nsent == 0, "discarding an unsent press sends neither down nor stray up");

    reset(); aa_key(AA_KEY_BACK, 1, 0); aa_input_pump(); aa_key(AA_KEY_BACK, 0, 0);
    aa_input_release_all("retry"); send_ok = 0;
    CHECK(!aa_input_pump() && nsent == 1, "release-all reports a send failure");
    CHECK(!aa_key(AA_KEY_BACK, 1, 1), "failed release blocks a new press");
    aa_input_release_all("retry-again"); send_ok = 1;
    CHECK(aa_input_pump() && nsent == 2 && key_report(1, AA_KEY_BACK, 0, 0), "failed release remains pending");
    aa_input_pump(); CHECK(nsent == 2, "successful retry sends exactly one release");

    reset(); aa_key(AA_KEY_BACK, 1, 0); aa_input_pump(); aa_input_release_all("old-session");
    aa_gen++; aa_input_pump(); aa_input_release_all("new-session"); aa_input_pump();
    CHECK(nsent == 1, "new session does not send releases for the old phone session");
    CHECK(aa_key(AA_KEY_BACK, 1, 0) && aa_input_pump() && nsent == 2 && key_report(1, AA_KEY_BACK, 1, 0),
          "old pending release does not block a new session");

    reset(); aa_key(AA_KEY_BACK, 1, 0); send_ok = 0; aa_input_pump();
    aa_input_release_all("failed-down"); send_ok = 1; aa_input_pump();
    CHECK(nsent == 0, "a failed down does not create phone-held state");
}

int main(void) {
    test_advertise(); test_exact_bytes(); test_rules(); test_provisional_ccu();
    test_overflow_release(); test_release_all_delivery();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
