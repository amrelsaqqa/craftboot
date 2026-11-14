/*
 * MediaTek MT7621 -- debugging knobs that read or patch guest memory:
 * MT7621_CP_SIG/MT7621_CP_PATCH (patches words at fixed deltas from a
 * signature), MT7621_RAMDUMP (one-shot RAM snapshot) and MT7621_RAMWIN
 * (repeated window dumps).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "mt7621.h"

/*
 * MT7621_CP_SIG / MT7621_CP_PATCH: debug-only guest code patcher.
 *
 * Finds a hex signature in guest RAM (the agent's /init is unpacked into a
 * ramfs, so its text is an ordinary page-cache page) and overwrites words at
 * fixed deltas from the match.  Used to neutralise one branch at a time in
 * the netinstall discovery responder and see which condition produces the
 * NACK it answers with.
 */
static int mt7621_hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void mt7621_cp_apply(MT7621State *s, hwaddr at)
{
    const char *p = getenv("MT7621_CP_PATCH");
    char buf[256];

    if (!p || strlen(p) >= sizeof(buf)) {
        return;
    }
    strcpy(buf, p);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long delta, word;

        if (sscanf(tok, "%li:%li", &delta, &word) != 2) {
            continue;
        }
        address_space_stl_le(&address_space_memory, at + delta, (uint32_t)word,
                             MEMTXATTRS_UNSPECIFIED, NULL);
        fprintf(stderr, "cp: patched %08" HWADDR_PRIx " <- %08lx\n",
                at + delta, word);
    }
}

static void mt7621_cp_tick(void *opaque)
{
    MT7621State *s = opaque;
    const char *sig_hex = getenv("MT7621_CP_SIG");
    uint8_t sig[16];
    unsigned siglen = 0, i;
    static uint8_t chunk[4 << 20];
    hwaddr base;

    if (s->cp_done || !sig_hex) {
        return;
    }
    for (i = 0; sig_hex[i] && sig_hex[i + 1] && siglen < sizeof(sig); i += 2) {
        int hi = mt7621_hexval(sig_hex[i]);
        int lo = mt7621_hexval(sig_hex[i + 1]);

        if (hi < 0 || lo < 0) {
            break;
        }
        sig[siglen++] = (hi << 4) | lo;
    }
    if (siglen < 4) {
        s->cp_done = true;
        return;
    }
    for (base = 0; base + sizeof(chunk) <= 0x10000000;
         base += sizeof(chunk) - sizeof(sig)) {
        uint8_t *hit;

        address_space_read(&address_space_memory, base,
                           MEMTXATTRS_UNSPECIFIED, chunk, sizeof(chunk));
        hit = memmem(chunk, sizeof(chunk), sig, siglen);
        if (hit) {
            hwaddr at = base + (hit - chunk);

            fprintf(stderr, "cp: signature at %08" HWADDR_PRIx "\n", at);
            mt7621_cp_apply(s, at);
            s->cp_done = true;
            return;
        }
    }
    timer_mod(s->cp_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2000);
}

/*
 * MT7621_RAMDUMP=<ms>:<path> - one-shot dump of guest RAM, so runtime state
 * can be inspected without the monitor (its unix socket does not survive).
 */
static void mt7621_ramdump(void *opaque)
{
    const char *spec = getenv("MT7621_RAMDUMP");
    char *copy, *colon, *path;
    static uint8_t buf[1 << 20];
    hwaddr a;
    FILE *f;

    if (!spec) {
        return;
    }
    copy = g_strdup(spec);
    colon = strchr(copy, ':');
    if (!colon) {
        g_free(copy);
        return;
    }
    *colon = 0;
    path = colon + 1;
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ramdump: cannot open %s\n", path);
        g_free(copy);
        return;
    }
    for (a = 0; a < 0x10000000; a += sizeof(buf)) {
        address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                           buf, sizeof(buf));
        fwrite(buf, 1, sizeof(buf), f);
    }
    fclose(f);
    fprintf(stderr, "ramdump: wrote %s\n", path);
    g_free(copy);
}

/*
 * MT7621_RAMWIN=first_ms:step_ms:count:start_hex:len_hex:prefix - dump a
 * window of guest RAM a few times in a row (prefix.N), for state that only
 * exists briefly during startup.
 */
static void mt7621_ramwin(void *opaque)
{
    MT7621State *s = opaque;
    const char *spec = getenv("MT7621_RAMWIN");
    char *copy, *tok, *save = NULL;
    long first, step, count, start, len;
    char *prefix;
    char path[256];
    static uint8_t buf[1 << 20];
    FILE *f;
    hwaddr a;

    if (!spec || s->ramwin_n >= 1000) {
        return;
    }
    copy = g_strdup(spec);
    tok = strtok_r(copy, ":", &save);
    first = tok ? strtol(tok, NULL, 0) : 0;
    (void)first;
    tok = strtok_r(NULL, ":", &save); step = tok ? strtol(tok, NULL, 0) : 1000;
    tok = strtok_r(NULL, ":", &save); count = tok ? strtol(tok, NULL, 0) : 0;
    tok = strtok_r(NULL, ":", &save); start = tok ? strtol(tok, NULL, 16) : 0;
    tok = strtok_r(NULL, ":", &save); len = tok ? strtol(tok, NULL, 16) : 0;
    tok = strtok_r(NULL, ":", &save); prefix = tok;
    if (!prefix || len <= 0 || len > (long)sizeof(buf) || count <= 0) {
        g_free(copy);
        return;
    }
    snprintf(path, sizeof(path), "%s.%u", prefix, s->ramwin_n);
    f = fopen(path, "wb");
    if (f) {
        for (a = start; a < (hwaddr)(start + len); a += 4096) {
            address_space_read(&address_space_memory, a,
                               MEMTXATTRS_UNSPECIFIED, buf, 4096);
            fwrite(buf, 1, 4096, f);
        }
        fclose(f);
    }
    s->ramwin_n++;
    timer_mod(s->ramwin_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + step);
    if (s->ramwin_n >= (unsigned)count) {
        fprintf(stderr, "ramwin: %u windows written\n", s->ramwin_n);
    }
    g_free(copy);
}


void mt7621_dump_init(MT7621State *s)
{
    const char *spec;

    if ((spec = getenv("MT7621_RAMWIN"))) {
        long first = strtol(spec, NULL, 0);

        s->ramwin_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mt7621_ramwin, s);
        timer_mod(s->ramwin_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + first);
    }
    if ((spec = getenv("MT7621_RAMDUMP"))) {
        long ms = strtol(spec, NULL, 0);

        s->ramdump_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mt7621_ramdump, s);
        timer_mod(s->ramdump_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ms);
    }
    if (getenv("MT7621_CP_SIG")) {
        s->cp_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mt7621_cp_tick, s);
        timer_mod(s->cp_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 8000);
    }
}
