/*
 * hook_bitmap.so — redirect /system/etc/extern_bitmap/ → /vendor/custom/extern_bitmap/
 * + log all image open() calls to /data/local/tmp/open_log.txt
 *
 * Flag control:
 *   /data/local/tmp/natartos_background — if exists, redirect background_disconnect.jpg
 *   Other bitmaps always redirect if custom file exists (screenprotectors etc.)
 *   Without flag: background stays stock.
 *
 * Self-contained: no libc dependency, raw ARM64 syscalls only.
 * Build: aarch64-linux-gnu-gcc -shared -fPIC -O2 -nostdlib -I../include
 *        -Wl,-soname,hook_bitmap.so -o hook_bitmap.so mod.c
 */

#include "natartos_common.h"

static int ends_with(const char *s, const char *suffix) {
    int sl = natartos_strlen(s);
    int xl = natartos_strlen(suffix);
    if (sl < xl) return 0;
    return natartos_strncmp(s + sl - xl, suffix, xl) == 0;
}

static const char LOG_PATH[] = "/data/local/tmp/open_log.txt";

static void log_open(const char *tag, const char *path) {
    int fd = (int)sys_openat(AT_FDCWD, LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    sys_write_raw(fd, tag, natartos_strlen(tag));
    sys_write_raw(fd, " ", 1);
    sys_write_raw(fd, path, natartos_strlen(path));
    sys_write_raw(fd, "\n", 1);
    sys_close(fd);
}

static int is_image_path(const char *path) {
    if (!path) return 0;
    if (ends_with(path, ".jpg")) return 1;
    if (ends_with(path, ".jpeg")) return 1;
    if (ends_with(path, ".bmp")) return 1;
    if (ends_with(path, ".png")) return 1;
    if (ends_with(path, ".BMP")) return 1;
    if (ends_with(path, ".JPG")) return 1;
    if (ends_with(path, ".PNG")) return 1;
    int len = natartos_strlen(path);
    for (int i = 0; i + 6 <= len; i++) {
        if (path[i]=='b' && path[i+1]=='i' && path[i+2]=='t' &&
            path[i+3]=='m' && path[i+4]=='a' && path[i+5]=='p')
            return 1;
    }
    return 0;
}

static const char SRC1[] = "/system/etc/extern_bitmap/";
static const char SRC2[] = "/etc/extern_bitmap/";
static const char DST[]  = "/vendor/custom/extern_bitmap/";
#define SRC1_LEN 26
#define SRC2_LEN 19
#define DST_LEN 29

static const char BG_FLAG[]    = "/data/local/tmp/natartos_background";
static const char BG_FILENAME[] = "background_disconnect.jpg";
#define BG_FILENAME_LEN 25

static int g_bg_enabled = -1;
static unsigned int g_open_count = 0;

static int is_background(const char *tail) {
    return natartos_strncmp(tail, BG_FILENAME, BG_FILENAME_LEN) == 0;
}

static int try_redirect(const char *path, int src_len, int flags, int mode) {
    const char *tail = path + src_len;
    int tail_len = natartos_strlen(tail);

    if (is_background(tail) && !g_bg_enabled)
        return -1;

    if (DST_LEN + tail_len < 500) {
        char buf[512];
        natartos_strcpy(buf, DST);
        natartos_strcpy(buf + DST_LEN, tail);
        if (sys_faccessat(AT_FDCWD, buf, R_OK) == 0) {
            log_open("REDIR", buf);
            return (int)sys_openat(AT_FDCWD, buf, flags, mode);
        }
    }
    return -1;
}

static int do_open(const char *path, int flags, int mode) {
    g_open_count++;
    if (g_bg_enabled < 0 || (g_open_count & 0xFF) == 0)
        g_bg_enabled = (sys_faccessat(AT_FDCWD, BG_FLAG, F_OK) == 0);

    if (path && is_image_path(path))
        log_open("OPEN", path);

    if (path) {
        int r;
        if (natartos_strncmp(path, SRC1, SRC1_LEN) == 0) {
            r = try_redirect(path, SRC1_LEN, flags, mode);
            if (r >= 0) return r;
        }
        if (natartos_strncmp(path, SRC2, SRC2_LEN) == 0) {
            r = try_redirect(path, SRC2_LEN, flags, mode);
            if (r >= 0) return r;
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

/* The image loader decodes via openat(), not open() — so the redirect must cover
 * it too, or the displayed background loads the stock file. For an absolute path
 * the dirfd is ignored, so route it through the same redirect; relative paths
 * pass through untouched with their original dirfd. */
int openat(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    if (path && path[0] == '/') return do_open(path, flags, mode);
    return (int)sys_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    if (path && path[0] == '/') return do_open(path, flags | O_LARGEFILE, mode);
    return (int)sys_openat(dirfd, path, flags | O_LARGEFILE, mode);
}

int natartos_mod_register(void *(*dlsym_fn)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)dlsym_fn((void *)0, "natartos_menu_register");
    if (reg) {
        natartos_menu_entry e = {
            .id       = "background",
            .name     = "Background",
            .subtitle = "Toggle custom",
            .command  = "sh -c '"
                        "f=/data/local/tmp/natartos_background; "
                        "if [ -f $f ]; then rm -f $f; else touch $f; fi"
                        "'",
            .priority = 300,
        };
        reg(&e);
    }
    return 0;
}
