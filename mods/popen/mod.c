/*
 * hook_popen.so — popen redirect for DJI Goggles 3.
 *
 * Intercepts popen("./bin/debug_script_*") and redirects to
 * /data/local/tmp/debug_script_*. Required for Doom and Stream (fb_override).
 */

#define NTLOG_PREFIX "popen"
#include "natartos_common.h"

#define my_strlen       natartos_strlen
#define my_strncmp      natartos_strncmp
#define my_strcpy       natartos_strcpy

#define RTLD_NEXT ((void *)-1)

typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn g_dlsym = 0;

/* ---- popen redirect ---- */

typedef void *(*popen_fn)(const char *, const char *);

static const char POPEN_SRC[] = "./bin/debug_script_";
static const char POPEN_DST[] = "/data/local/tmp/debug_script_";
#define POPEN_SRC_LEN 19
#define POPEN_DST_LEN 29
#define X_OK 1

void *popen(const char *command, const char *type) {
    static popen_fn real = 0;
    if (!real && g_dlsym) real = (popen_fn)g_dlsym(RTLD_NEXT, "popen");
    if (!real) return (void *)0;
    if (command && my_strncmp(command, POPEN_SRC, POPEN_SRC_LEN) == 0) {
        const char *tail = command + POPEN_SRC_LEN;
        int tail_len = my_strlen(tail);
        if (POPEN_DST_LEN + tail_len < 240) {
            char buf[256];
            my_strcpy(buf, POPEN_DST);
            my_strcpy(buf + POPEN_DST_LEN, tail);
            if (sys_faccessat(AT_FDCWD, buf, X_OK) == 0)
                return real(buf, type);
        }
    }
    return real(command, type);
}

__attribute__((constructor))
static void init_hook(void) {
    char exe[64];
    int fd = (int)sys_openat(AT_FDCWD, "/proc/self/cmdline", O_RDONLY, 0);
    if (fd >= 0) {
        long r = sys_read(fd, exe, sizeof(exe) - 1);
        sys_close(fd);
        if (r > 0) {
            exe[r] = 0;
            for (int i = 0; i + 11 <= r; i++)
                if (natartos_strncmp(exe + i, "dji_glasses", 11) == 0) {
                    NTLOG("hook_popen loaded");
                    return;
                }
        }
    }
}

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    g_dlsym = (dlsym_fn)loader_dlsym;
    NTLOG("init: dlsym ready");
    return 0;
}
