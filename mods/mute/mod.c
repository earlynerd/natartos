/*
 * mod_mute.so — audio control for DJI Goggles 3 (tinyalsa).
 *
 * Interposes tinyalsa's pcm_write (the mod must be in LD_PRELOAD, which
 * inject.sh does for every enabled mod). While a mute flag applies, silence
 * is written in place of the caller's samples. The caller's buffer is never
 * modified: it is const and may hold a sound that is played again later.
 *
 * Flags (touch = active, rm = inactive), checked every 256 writes:
 *   /data/local/tmp/natartos_mute          — mute ALL audio (the menu entry toggles this)
 *   /data/local/tmp/natartos_mute_startup  — mute the first ~300 writes after launch
 *   /data/local/tmp/natartos_mute_buttons  — mute short writes (button/UI sounds)
 */

#define NTLOG_PREFIX "mute"
#include "natartos_common.h"

#define RTLD_NEXT ((void *)-1)
#define RTLD_NOLOAD 4

#ifndef MUTE_FLAG_DIR
#define MUTE_FLAG_DIR "/data/local/tmp/"   /* host tests point this elsewhere */
#endif
static const char FLAG_MUTE_ALL[]     = MUTE_FLAG_DIR "natartos_mute";
static const char FLAG_MUTE_STARTUP[] = MUTE_FLAG_DIR "natartos_mute_startup";
static const char FLAG_MUTE_BUTTONS[] = MUTE_FLAG_DIR "natartos_mute_buttons";

#define STARTUP_CALLS 300
/* pcm_write's count is in BYTES (tinyalsa derives the frame count from it).
 * "Short" = up to 256 frames of 16-bit stereo. This heuristic is untested. */
#define SHORT_WRITE_BYTES (256 * 4)
/* Silence source. 65520 bytes is a whole number of frames for 16-bit audio
 * with 1-8 channels and 32-bit audio with 1-7, so chunks stay frame-aligned. */
#define SILENCE_BYTES 65520

typedef void *(*dlsym_fn)(void *, const char *);
typedef void *(*dlopen_fn)(const char *, int);
typedef int (*pcm_write_fn)(void *pcm, const void *data, unsigned int count);

static dlsym_fn g_dlsym = 0;
static pcm_write_fn g_real_pcm_write = 0;
static int g_mute_all = 0;
static int g_mute_startup = 0;
static int g_mute_buttons = 0;
static unsigned int g_call_count = 0;
static unsigned char g_silence[SILENCE_BYTES];   /* zero-filled, never written */

/* The next pcm_write after this library (tinyalsa's), as the overlay does for
 * push_frame; fall back to looking in an already-loaded libtinyalsa.so. */
static void resolve_real(void) {
    if (g_real_pcm_write || !g_dlsym) return;
    g_real_pcm_write = (pcm_write_fn)g_dlsym(RTLD_NEXT, "pcm_write");
    if (!g_real_pcm_write) {
        dlopen_fn my_dlopen = (dlopen_fn)g_dlsym(RTLD_DEFAULT, "dlopen");
        void *tinyalsa = my_dlopen ? my_dlopen("libtinyalsa.so", RTLD_NOLOAD) : 0;
        if (tinyalsa) g_real_pcm_write = (pcm_write_fn)g_dlsym(tinyalsa, "pcm_write");
    }
}

static void read_flags(void) {
    g_mute_all     = natartos_flag_exists(FLAG_MUTE_ALL);
    g_mute_startup = natartos_flag_exists(FLAG_MUTE_STARTUP);
    g_mute_buttons = natartos_flag_exists(FLAG_MUTE_BUTTONS);
}

/* Write `count` bytes of silence, in frame-aligned chunks. */
static int write_silence(void *pcm, unsigned int count) {
    int rc = 0;
    while (count) {
        unsigned int n = count > SILENCE_BYTES ? SILENCE_BYTES : count;
        rc = g_real_pcm_write(pcm, g_silence, n);
        if (rc) return rc;
        count -= n;
    }
    return rc;
}

/* --- interpose pcm_write --- */

int pcm_write(void *pcm, const void *data, unsigned int count) {
    g_call_count++;
    if (g_call_count == 1 || (g_call_count & 0xFF) == 0) read_flags();
    if (!g_real_pcm_write) {
        resolve_real();
        if (!g_real_pcm_write) {
            if (g_call_count == 1) NTLOG("pcm_write: real function not found; audio dropped");
            return -1;
        }
    }
    if (g_call_count == 1) NTLOG("pcm_write: FIRST CALL (interposition OK)");

    int mute = g_mute_all ||
               (g_mute_startup && g_call_count <= STARTUP_CALLS) ||
               (g_mute_buttons && count <= SHORT_WRITE_BYTES);
    return mute ? write_silence(pcm, count) : g_real_pcm_write(pcm, data, count);
}

/* --- natartos_mod_init: called by loader with its dlsym --- */

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    if (!loader_dlsym) { NTLOG("mod_mute: no dlsym"); return -1; }
    g_dlsym = (dlsym_fn)loader_dlsym;
    resolve_real();
    read_flags();
    NTLOG(g_real_pcm_write ? "mod_mute: pcm_write resolved OK" :
                             "mod_mute: pcm_write not resolved yet (retried on first write)");
    NTLOG(g_mute_all     ? "mod_mute: ALL muted" :
          g_mute_startup ? "mod_mute: startup muted" :
          g_mute_buttons ? "mod_mute: buttons muted" :
                           "mod_mute: state=normal");
    return 0;
}

int natartos_mod_register(void *(*dlsym_fn)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)dlsym_fn(RTLD_DEFAULT, "natartos_menu_register");
    if (reg) {
        natartos_menu_entry e = {
            .id       = "mute",
            .name     = "Audio Control",
            .subtitle = "Toggle mute all",
            /* Same flag pcm_write checks (FLAG_MUTE_ALL). */
            .command  = "sh -c '"
                        "f=" MUTE_FLAG_DIR "natartos_mute; "
                        "if [ -f $f ]; then rm -f $f; else touch $f; fi"
                        "'",
            .priority = 400,
        };
        reg(&e);
        NTLOG("mod_mute: menu registered");
    }
    return 0;
}
