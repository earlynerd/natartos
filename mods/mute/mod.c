/*
 * mod_mute.so — audio control for DJI Goggles 3 (tinyalsa).
 *
 * Interposes pcm_write (tinyalsa). Zeroes audio buffer based on flags.
 *
 * Flags (touch = active, rm = inactive):
 *   /data/local/tmp/natartos_mute          — mute ALL audio
 *   /data/local/tmp/natartos_mute_startup  — mute startup sound only (first ~5s)
 *   /data/local/tmp/natartos_mute_buttons  — mute button/UI sounds (short buffers)
 *
 * Checks flags every 256 calls to avoid excessive syscalls.
 * Uses natartos_mod_init protocol to get dlsym from loader.
 */

#define NTLOG_PREFIX "mute"
#include "natartos_common.h"

#define RTLD_NEXT ((void *)-1)

static const char FLAG_MUTE_ALL[]     = "/data/local/tmp/natartos_mute";
static const char FLAG_MUTE_STARTUP[] = "/data/local/tmp/natartos_mute_startup";
static const char FLAG_MUTE_BUTTONS[] = "/data/local/tmp/natartos_mute_buttons";

static void log_str(const char *s) { NTLOG(s); }

/* --- state --- */

#define RTLD_NOLOAD 4

typedef void *(*dlopen_fn)(const char *, int);
typedef int (*pcm_write_fn)(void *pcm, const void *data, unsigned int count);
static pcm_write_fn g_real_pcm_write = 0;
static int g_mute_all = 0;
static int g_mute_startup = 0;
static int g_mute_buttons = 0;
static unsigned int g_call_count = 0;
#define STARTUP_CALLS 300
#define BUTTON_FRAME_THRESHOLD 256

/* --- interpose pcm_write --- */

int pcm_write(void *pcm, const void *data, unsigned int count) {
    g_call_count++;
    if (g_call_count == 1)
        log_str("pcm_write: FIRST CALL (interposition OK)");
    if (g_call_count <= 1 || (g_call_count & 0xFF) == 0) {
        g_mute_all     = natartos_flag_exists(FLAG_MUTE_ALL);
        g_mute_startup = natartos_flag_exists(FLAG_MUTE_STARTUP);
        g_mute_buttons = natartos_flag_exists(FLAG_MUTE_BUTTONS);
    }

    if (!g_real_pcm_write)
        return 0;

    int should_mute = 0;
    if (g_mute_all)
        should_mute = 1;
    else if (g_mute_startup && g_call_count <= STARTUP_CALLS)
        should_mute = 1;
    else if (g_mute_buttons && count <= BUTTON_FRAME_THRESHOLD)
        should_mute = 1;

    if (should_mute) {
        unsigned int frame_size = 4;
        natartos_memset((void *)data, 0, (long)count * frame_size);
    }

    return g_real_pcm_write(pcm, data, count);
}

/* --- natartos_mod_init: called by loader with its dlsym --- */

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    if (!loader_dlsym) {
        log_str("mod_mute: no dlsym");
        return -1;
    }

    dlopen_fn my_dlopen = (dlopen_fn)loader_dlsym(RTLD_DEFAULT, "dlopen");
    void *tinyalsa = 0;
    if (my_dlopen)
        tinyalsa = my_dlopen("libtinyalsa.so", RTLD_NOLOAD);
    if (tinyalsa)
        g_real_pcm_write = (pcm_write_fn)loader_dlsym(tinyalsa, "pcm_write");
    else
        g_real_pcm_write = 0;
    g_mute_all     = natartos_flag_exists(FLAG_MUTE_ALL);
    g_mute_startup = natartos_flag_exists(FLAG_MUTE_STARTUP);
    g_mute_buttons = natartos_flag_exists(FLAG_MUTE_BUTTONS);

    log_str(g_real_pcm_write ? "mod_mute: pcm_write resolved OK" :
                                "mod_mute: pcm_write NOT found");
    log_str(g_mute_all     ? "mod_mute: ALL muted" :
            g_mute_startup ? "mod_mute: startup muted" :
            g_mute_buttons ? "mod_mute: buttons muted" :
                             "mod_mute: state=normal");
    return g_real_pcm_write ? 0 : -2;
}

int natartos_mod_register(void *(*dlsym_fn)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)dlsym_fn(RTLD_DEFAULT, "natartos_menu_register");
    if (reg) {
        natartos_menu_entry e = {
            .id       = "mute",
            .name     = "Audio Control",
            .subtitle = "Toggle mute",
            .command  = "sh -c '"
                        "f=/data/local/tmp/natartos_mute_all; "
                        "if [ -f $f ]; then rm -f $f; else touch $f; fi"
                        "'",
            .priority = 400,
        };
        reg(&e);
        log_str("mod_mute: menu registered");
    }
    return 0;
}
