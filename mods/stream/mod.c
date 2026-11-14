/*
 * mod_stream.so — PC Stream menu entry for DJI Goggles 3.
 *
 * Registers "PC Stream" in the natartos menu. On tap, creates a FIFO pipe
 * and launches fb_receiver in pipe mode with a splash screen. The web UI
 * (or any ADB client) writes JPEG frames to the pipe.
 */

#define NTLOG_PREFIX "stream"
#include "natartos_common.h"

#define RTLD_DEFAULT ((void *)0)
typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn g_dlsym = 0;

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
                    NTLOG("mod_stream loaded");
                    return;
                }
        }
    }
}

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    g_dlsym = (dlsym_fn)loader_dlsym;
    return 0;
}

int natartos_mod_register(void *(*loader_dlsym)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)loader_dlsym(RTLD_DEFAULT, "natartos_menu_register");
    if (!reg) { NTLOG("menu_register not found"); return 0; }

    natartos_menu_entry stream = {
        .id       = "stream",
        .name     = "PC Stream",
        .subtitle = "Screen share to goggles",
        .icon_path = "/vendor/natartos/icons/natartos_icon_stream.rgba",
        .command  = "nohup sh -c '"
                    "rm -f /data/local/tmp/fb_pipe; "
                    "mkfifo /data/local/tmp/fb_pipe; "
                    "/vendor/natartos/apps/stream/fb_receiver /data/local/tmp/fb_pipe; "
                    "rm -f /data/local/tmp/fb_pipe"
                    "' >/dev/null 2>&1 &",
        .priority = 200,
    };
    reg(&stream);
    return 1;
}
