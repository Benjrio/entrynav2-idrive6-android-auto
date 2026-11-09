/* Android Auto input (channel 1): iDrive controller -> AA InputEventIndication (message 0x8001).
 * Message layout as in aasdk's InputEventIndication (f1x.studio, GPL-3.0-or-later):
 *   1 timestamp_us
 *   4 ButtonEvents        { 1 ButtonEvent { 1 scan_code, 2 is_pressed, 3 meta, 4 long_press } }
 *   6 RelativeInputEvents { 1 RelativeInputEvent { 1 scan_code, 2 delta (int32) } }
 * Service discovery advertises AA_INPUT_KEYCODES (InputChannel field 1, packed).
 *
 * Rules:
 * - Only advertised keycodes are accepted. MENU is never one: it always belongs to iDrive.
 * - A key is "down" until its "up"; aa_input_release_all() (session end, cancel, later: HMI focus loss) sends "up"
 *   for every held key, so the phone never sees a stuck key. A stray "up" for a key that isn't held is dropped.
 * - Held longer than AA_LONGPRESS_US: one extra "down" with long_press=1 (once per press).
 * - Rotation accumulates and is sent in reports of at most +/-AA_ROTARY_MAX, so a fast spin can't overflow anything.
 * - A full key queue drops the new event (counted); a dropped "down" is never followed by its "up".
 * - Single thread: every function here runs on hu-aa's main loop (the input backend polls from that loop).
 *
 * Needs, defined before inclusion: struct AaPb, aa_v(), aa_sub(), aa_var(), aa_send(), aa_gen, aa_bound, now().
 */
#ifndef AA_INPUT_H
#define AA_INPUT_H

#define AA_KEY_BACK 4u
#define AA_KEY_CALL 5u
#define AA_KEY_DPAD_UP 19u
#define AA_KEY_DPAD_DOWN 20u
#define AA_KEY_DPAD_LEFT 21u
#define AA_KEY_DPAD_RIGHT 22u
#define AA_KEY_DPAD_CENTER 23u
#define AA_KEY_MEDIA_NEXT 87u
#define AA_KEY_MEDIA_PREVIOUS 88u
#define AA_KEY_ROTARY 65536u
#define AA_KEY_MEDIA 65537u
#define AA_KEY_NAVIGATION 65538u
static const unsigned aa_input_keycodes[] = {
    AA_KEY_BACK, AA_KEY_CALL, AA_KEY_DPAD_UP, AA_KEY_DPAD_DOWN, AA_KEY_DPAD_LEFT, AA_KEY_DPAD_RIGHT,
    AA_KEY_DPAD_CENTER, AA_KEY_MEDIA_NEXT, AA_KEY_MEDIA_PREVIOUS, AA_KEY_ROTARY, AA_KEY_MEDIA, AA_KEY_NAVIGATION,
};
#define AA_INPUT_KEYCOUNT (sizeof aa_input_keycodes / sizeof aa_input_keycodes[0])
#define AA_INPUT_SLOTS 64u
#define AA_INPUT_HELD 8u
#define AA_ROTARY_MAX 8
#define AA_ROTARY_LIMIT 1000
#define AA_LONGPRESS_US 800000ULL

struct AaInputEvent { unsigned generation, code, down, longpress; uint64_t us; };
struct AaHeldKey { unsigned generation, code, long_sent, releasing; uint64_t since_us; };
static struct AaInputEvent aa_in_queue[AA_INPUT_SLOTS];
static unsigned aa_in_write, aa_in_read;
static struct AaHeldKey aa_in_held[AA_INPUT_HELD];
static int aa_in_rotary;
static unsigned aa_in_rotary_gen;
static unsigned aa_in_keys_sent, aa_in_rotary_sent, aa_in_dropped, aa_in_released, aa_in_longpress_sent;

static int aa_input_supported(unsigned code) {
    for (unsigned i = 0; i < AA_INPUT_KEYCOUNT; i++) if (aa_input_keycodes[i] == code) return 1;
    return 0;
}
static void aa_input_advertise(struct AaPb *codes) {
    for (unsigned i = 0; i < AA_INPUT_KEYCOUNT; i++) aa_var(codes, aa_input_keycodes[i]);
}
static const char *aa_input_keylist(void) {
    static char text[128]; size_t used = 0; text[0] = 0;
    for (unsigned i = 0; i < AA_INPUT_KEYCOUNT && used < sizeof text; i++)
        used += (size_t)snprintf(text + used, sizeof text - used, "%s%u", i ? "," : "", aa_input_keycodes[i]);
    return text;
}
static struct AaHeldKey *aa_input_held(unsigned code) {
    for (unsigned i = 0; i < AA_INPUT_HELD; i++)
        if (aa_in_held[i].code == code && aa_in_held[i].generation == aa_gen) return &aa_in_held[i];
    return NULL;
}
static int aa_input_enqueue(unsigned code, unsigned down, unsigned longpress, uint64_t us) {
    if (aa_in_write - aa_in_read >= AA_INPUT_SLOTS) { aa_in_dropped++; return 0; }
    aa_in_queue[aa_in_write % AA_INPUT_SLOTS] = (struct AaInputEvent){ aa_gen, code, down, longpress, us };
    aa_in_write++;
    return 1;
}

/* A key transition from the input backend. Returns 1 when queued. */
static int aa_key(unsigned code, int down, int longpress) {
    if (code == AA_KEY_ROTARY || !aa_input_supported(code)) {
        printf("AA_INPUT_REJECT generation=%u key=%u reason=not-advertised\n", aa_gen, code); return 0;
    }
    uint64_t us = now() / 1000; struct AaHeldKey *held = aa_input_held(code);
    if (!down) {
        if (!held || held->releasing) return 0;           /* stray up: never pressed (or already releasing) */
        if (!aa_input_enqueue(code, 0, 0, us)) { held->releasing = 1; return 0; }  /* pump will release it */
        memset(held, 0, sizeof *held);
        return 1;
    }
    if (held) {                                            /* repeat while held: only an explicit long press counts */
        if (!longpress || held->long_sent) return 0;
        if (!aa_input_enqueue(code, 1, 1, us)) return 0;
        held->long_sent = 1;
        return 1;
    }
    struct AaHeldKey *slot = NULL;
    for (unsigned i = 0; i < AA_INPUT_HELD && !slot; i++) if (!aa_in_held[i].code || aa_in_held[i].generation != aa_gen) slot = &aa_in_held[i];
    if (!slot) { aa_in_dropped++; return 0; }               /* more than AA_INPUT_HELD keys at once */
    if (!aa_input_enqueue(code, 1, longpress ? 1u : 0u, us)) return 0;
    *slot = (struct AaHeldKey){ aa_gen, code, longpress ? 1u : 0u, 0, us };
    return 1;
}
/* Relative rotation in detents (positive = clockwise, as the backend reports it). */
static void aa_rotary(int delta) {
    if (aa_in_rotary_gen != aa_gen) { aa_in_rotary = 0; aa_in_rotary_gen = aa_gen; }
    long sum = (long)aa_in_rotary + delta;
    if (sum > AA_ROTARY_LIMIT) sum = AA_ROTARY_LIMIT;
    if (sum < -AA_ROTARY_LIMIT) sum = -AA_ROTARY_LIMIT;
    aa_in_rotary = (int)sum;
}
/* Drop queued input and release every held key at the next pump. */
static void aa_input_release_all(const char *reason) {
    unsigned held = 0;
    for (unsigned i = 0; i < AA_INPUT_HELD; i++)
        if (aa_in_held[i].code && aa_in_held[i].generation == aa_gen) { aa_in_held[i].releasing = 1; held++; }
    aa_in_read = aa_in_write; aa_in_rotary = 0;
    printf("AA_INPUT_RELEASE_ALL generation=%u reason=%s held=%u\n", aa_gen, reason, held);
}

static int aa_input_send_key(unsigned code, unsigned down, unsigned longpress, uint64_t us) {
    struct AaPb event = {0}, list = {0}, message = {0};
    aa_v(&event, 1, code); aa_v(&event, 2, down); aa_v(&event, 3, 0); aa_v(&event, 4, longpress);
    aa_sub(&list, 1, &event); aa_v(&message, 1, us); aa_sub(&message, 4, &list);
    if (!aa_send(1, 0x8001, message.b, message.n, 1, 0)) return 0;
    aa_in_keys_sent++; if (longpress && down) aa_in_longpress_sent++;
    printf("AA_INPUT generation=%u key=%u down=%u longpress=%u timestamp_us=%llu\n", aa_gen, code, down, longpress, (unsigned long long)us);
    return 1;
}
static int aa_input_send_rotary(int delta, uint64_t us) {
    struct AaPb event = {0}, list = {0}, message = {0};
    aa_v(&event, 1, AA_KEY_ROTARY); aa_v(&event, 2, (uint64_t)(int64_t)delta);
    aa_sub(&list, 1, &event); aa_v(&message, 1, us); aa_sub(&message, 6, &list);
    if (!aa_send(1, 0x8001, message.b, message.n, 1, 0)) return 0;
    aa_in_rotary_sent++;
    printf("AA_INPUT generation=%u key=%u delta=%d timestamp_us=%llu\n", aa_gen, AA_KEY_ROTARY, delta, (unsigned long long)us);
    return 1;
}

/* Called from aa_controls() once AA is running. Returns 0 only when sending failed (session error). */
static int aa_input_pump(void) {
    if (!aa_bound) {
        unsigned stale = aa_in_write - aa_in_read;
        if (stale || aa_in_rotary) printf("AA_INPUT_DISCARD generation=%u queued=%u rotary=%d reason=not-bound\n", aa_gen, stale, aa_in_rotary);
        aa_in_read = aa_in_write; aa_in_rotary = 0;
        for (unsigned i = 0; i < AA_INPUT_HELD; i++) if (aa_in_held[i].generation == aa_gen) memset(&aa_in_held[i], 0, sizeof aa_in_held[i]);
        return 1;
    }
    uint64_t us = now() / 1000;
    for (unsigned i = 0; i < AA_INPUT_HELD; i++) {           /* releases first: never leave a key down */
        struct AaHeldKey *k = &aa_in_held[i];
        if (!k->code || k->generation != aa_gen || !k->releasing) continue;
        if (!aa_input_send_key(k->code, 0, 0, us)) return 0;
        aa_in_released++; memset(k, 0, sizeof *k);
    }
    while (aa_in_read != aa_in_write) {
        struct AaInputEvent e = aa_in_queue[aa_in_read % AA_INPUT_SLOTS];
        if (e.generation == aa_gen && !aa_input_send_key(e.code, e.down, e.longpress, e.us)) return 0;
        aa_in_read++;
    }
    if (aa_in_rotary_gen != aa_gen) { aa_in_rotary = 0; aa_in_rotary_gen = aa_gen; }
    while (aa_in_rotary) {
        int step = aa_in_rotary > AA_ROTARY_MAX ? AA_ROTARY_MAX : aa_in_rotary < -AA_ROTARY_MAX ? -AA_ROTARY_MAX : aa_in_rotary;
        if (!aa_input_send_rotary(step, us)) return 0;
        aa_in_rotary -= step;
    }
    for (unsigned i = 0; i < AA_INPUT_HELD; i++) {           /* long press synthesis */
        struct AaHeldKey *k = &aa_in_held[i];
        if (!k->code || k->generation != aa_gen || k->long_sent || us - k->since_us < AA_LONGPRESS_US) continue;
        if (!aa_input_send_key(k->code, 1, 1, us)) return 0;
        k->long_sent = 1;
    }
    return 1;
}

/* PROVISIONAL, not called yet: HMIControl.CCUFrontEvent (entity, action) as the HMI sees it. Only these three
 * entity values are known (kizapp's original mapping: 13 rotary steps, 8 centre push, 2 BACK); the action encoding
 * for keys is assumed nonzero = pressed. Confirm both with car logs before wiring a backend to this. */
static void aa_input_ccu_front_event(unsigned entity, int action) {
    if (entity == 13) aa_rotary(action);
    else if (entity == 8) aa_key(AA_KEY_DPAD_CENTER, action != 0, 0);
    else if (entity == 2) aa_key(AA_KEY_BACK, action != 0, 0);
    else printf("AA_INPUT_UNMAPPED source=ccu-front-event entity=%u action=%d\n", entity, action);
}
#endif
