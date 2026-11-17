/*
 * Host check for mods/mute/mod.c, compiled against
 * tests/sd_apps/shim/natartos_common.h with the flag files in the working
 * directory. It plays patterned buffers through the interposed pcm_write and
 * checks what reaches the "real" pcm_write:
 *   - unmuted: the samples, unchanged;
 *   - muted: silence of exactly the same byte count, in frame-aligned chunks;
 *   - the caller's buffer is never modified;
 *   - the menu command toggles the flag pcm_write reads.
 * Prints MUTE_CHECK_OK and exits 0 when every check passes.
 */
#define _POSIX_C_SOURCE 200809L
#define MUTE_FLAG_DIR "./"
#include "../../mods/mute/mod.c"

#include <stdio.h>
#include <stdlib.h>

#define BIG 150000u   /* more than two 65520-byte silence chunks */

static unsigned char pattern[BIG], received[BIG];
static unsigned int received_len, writes;
static char menu_command[512];
static int failures;

#define CHECK(cond, what) do { if (cond) printf("ok: %s\n", what); \
                               else { printf("FAIL: %s\n", what); failures++; } } while (0)

static int fake_pcm_write(void *pcm, const void *data, unsigned int count) {
    (void)pcm;
    if (received_len + count > sizeof(received)) return -5;
    memcpy(received + received_len, data, count);
    received_len += count;
    writes++;
    return 0;
}

static int fake_register(const natartos_menu_entry *e) {
    snprintf(menu_command, sizeof(menu_command), "%s", e->command ? e->command : "");
    return 0;
}

static void *fake_dlsym(void *handle, const char *name) {
    if (handle == RTLD_NEXT && !strcmp(name, "pcm_write")) return (void *)fake_pcm_write;
    if (handle == RTLD_DEFAULT && !strcmp(name, "natartos_menu_register")) return (void *)fake_register;
    return NULL;
}

static unsigned char sample(unsigned i) { return (unsigned char)(i % 251 + 1); }   /* never 0 */

static int is_silence(unsigned n) {
    for (unsigned i = 0; i < n; i++) if (received[i]) return 0;
    return 1;
}

static int pattern_intact(void) {
    for (unsigned i = 0; i < BIG; i++) if (pattern[i] != sample(i)) return 0;
    return 1;
}

static int play(unsigned count) {
    received_len = writes = 0;
    return pcm_write(NULL, pattern, count);
}

/* Flags are re-read on every 256th write; play small writes up to that point. */
static void reread_flags(void) {
    do play(4); while ((g_call_count & 0xFF) != 0);
}

static void set_flag(const char *path, int on) {
    if (on) { FILE *f = fopen(path, "w"); if (f) fclose(f); }
    else unlink(path);
}

int main(void) {
    for (unsigned i = 0; i < BIG; i++) pattern[i] = sample(i);
    set_flag(FLAG_MUTE_ALL, 0); set_flag(FLAG_MUTE_STARTUP, 0); set_flag(FLAG_MUTE_BUTTONS, 0);

    natartos_mod_init(fake_dlsym);
    CHECK(g_real_pcm_write == fake_pcm_write, "real pcm_write resolved through RTLD_NEXT");

    CHECK(play(4000) == 0 && received_len == 4000 && !memcmp(received, pattern, 4000),
          "unmuted audio passes through unchanged");

    set_flag(FLAG_MUTE_ALL, 1);
    reread_flags();
    CHECK(play(BIG) == 0 && received_len == BIG && is_silence(BIG),
          "muted write is replaced by silence of the same byte count");
    CHECK(writes == 3, "long silence is written in 65520-byte chunks");
    CHECK(pattern_intact(), "caller's buffer is not modified");

    set_flag(FLAG_MUTE_ALL, 0);
    reread_flags();
    CHECK(play(4000) == 0 && !memcmp(received, pattern, 4000), "unmuting restores the samples");

    set_flag(FLAG_MUTE_BUTTONS, 1);
    reread_flags();
    CHECK(play(SHORT_WRITE_BYTES) == 0 && received_len == SHORT_WRITE_BYTES && is_silence(SHORT_WRITE_BYTES),
          "button mode silences short writes");
    CHECK(play(SHORT_WRITE_BYTES + 4) == 0 && !memcmp(received, pattern, SHORT_WRITE_BYTES + 4),
          "button mode passes longer writes");
    set_flag(FLAG_MUTE_BUTTONS, 0);
    reread_flags();

    natartos_mod_register(fake_dlsym);
    CHECK(strstr(menu_command, "natartos_mute;") != NULL, "menu command names the mute-all flag");
    int rc = system(menu_command);
    reread_flags();
    CHECK(rc == 0 && natartos_flag_exists(FLAG_MUTE_ALL) && play(64) == 0 && is_silence(64),
          "menu command turns mute on");
    rc = system(menu_command);
    reread_flags();
    CHECK(rc == 0 && !natartos_flag_exists(FLAG_MUTE_ALL) && play(64) == 0 && !memcmp(received, pattern, 64),
          "menu command turns mute off again");

    if (failures) { printf("MUTE_CHECK_FAILED=%d\n", failures); return 1; }
    puts("MUTE_CHECK_OK");
    return 0;
}
