/*
 * mod_splash.so — custom boot splash for DJI Goggles 3.
 *
 * Intercepts openat() to redirect DJI splash/boot screen bitmap
 * to a custom version when enabled via flag file.
 *
 * Flag: /data/local/tmp/natartos_splash (exists = custom splash)
 * Custom splash: /vendor/natartos/assets/splash_custom.bgra
 *
 * NOTE: The exact splash file path must be identified from device
 * inspection. DISCOVERY_MODE logs all image opens at startup.
 */

#define NTLOG_PREFIX "splash"
#include "natartos_common.h"

static void log_str(const char *s) { NTLOG(s); }

/* --- state --- */

static const char FLAG_PATH[] = "/data/local/tmp/natartos_splash";
static const char CUSTOM_SPLASH[] = "/vendor/natartos/assets/splash_custom.bgra";
static int g_enabled = 0;
static unsigned int g_call_count = 0;

static const char * const SPLASH_PREFIXES[] = {
    "/system/etc/extern_bitmap/boot_logo",
    "/system/etc/extern_bitmap/startup",
    "/etc/extern_bitmap/boot_logo",
    "/etc/extern_bitmap/startup",
    0
};

#define DISCOVERY_MODE 1

static int is_splash_path(const char *path) {
    if (!path) return 0;
    for (int i = 0; SPLASH_PREFIXES[i]; i++) {
        int plen = natartos_strlen(SPLASH_PREFIXES[i]);
        if (natartos_strncmp(path, SPLASH_PREFIXES[i], plen) == 0)
            return 1;
    }
    return 0;
}

static int is_image(const char *path) {
    if (!path) return 0;
    int len = natartos_strlen(path);
    if (len < 4) return 0;
    const char *ext = path + len - 4;
    if (ext[0] == '.') {
        if ((ext[1]=='b'||ext[1]=='B') && (ext[2]=='m'||ext[2]=='M') && (ext[3]=='p'||ext[3]=='P')) return 1;
        if ((ext[1]=='p'||ext[1]=='P') && (ext[2]=='n'||ext[2]=='N') && (ext[3]=='g'||ext[3]=='G')) return 1;
        if ((ext[1]=='j'||ext[1]=='J') && (ext[2]=='p'||ext[2]=='P') && (ext[3]=='g'||ext[3]=='G')) return 1;
    }
    if (len >= 5) {
        ext = path + len - 5;
        if (ext[0] == '.' &&
            (ext[1]=='b'||ext[1]=='B') && (ext[2]=='g'||ext[2]=='G') &&
            (ext[3]=='r'||ext[3]=='R') && (ext[4]=='a'||ext[4]=='A'))
            return 1;
    }
    return 0;
}

/* --- openat interposition --- */

static int do_open(const char *path, int flags, int mode) {
    g_call_count++;
    if ((g_call_count & 0xFF) == 0)
        g_enabled = natartos_flag_exists(FLAG_PATH);

#if DISCOVERY_MODE
    if (is_image(path))
        log_str(path);
#endif

    if (g_enabled && is_splash_path(path)) {
        if (sys_faccessat(AT_FDCWD, CUSTOM_SPLASH, R_OK) == 0) {
            log_str("REDIR splash");
            return (int)sys_openat(AT_FDCWD, CUSTOM_SPLASH, flags, mode);
        }
    }

    return (int)sys_openat(AT_FDCWD, path, flags, mode);
}

int open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    return do_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    return do_open(path, flags | O_LARGEFILE, mode);
}

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    (void)loader_dlsym;
    g_enabled = natartos_flag_exists(FLAG_PATH);
    log_str(g_enabled ? "mod_splash: enabled" : "mod_splash: disabled (no flag)");
    return 0;
}
