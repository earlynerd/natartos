/*
 * hook_audio.so — redirect /system/etc/audio/ → /vendor/custom/audio/
 *
 * Same pattern as hook_bitmap: interpose open/open64, redirect if custom
 * file exists. User controls replacement by placing WAV files in
 * /vendor/custom/audio/<same filename>.
 *
 * Self-contained: no libc dependency, raw ARM64 syscalls only.
 * Build: aarch64-linux-gnu-gcc -shared -fPIC -O2 -nostdlib -I../include
 *        -Wl,-soname,hook_audio.so -o hook_audio.so mod.c
 */

#include "natartos_common.h"

static const char LOG_PATH[] = "/data/local/tmp/natartos_audio.log";

static void log_open(const char *tag, const char *path) {
    int fd = (int)sys_openat(AT_FDCWD, LOG_PATH,
                              O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    sys_write_raw(fd, tag, natartos_strlen(tag));
    sys_write_raw(fd, " ", 1);
    sys_write_raw(fd, path, natartos_strlen(path));
    sys_write_raw(fd, "\n", 1);
    sys_close(fd);
}

static const char SRC_PREFIX[] = "/system/etc/audio/";
#define SRC_PREFIX_LEN 18
static const char DST_PREFIX[] = "/vendor/custom/audio/";
#define DST_PREFIX_LEN 21

static int try_redirect_audio(const char *path, int flags, int mode) {
    const char *tail = path + SRC_PREFIX_LEN;
    int tail_len = natartos_strlen(tail);

    if (DST_PREFIX_LEN + tail_len < 500) {
        char buf[512];
        natartos_strcpy(buf, DST_PREFIX);
        natartos_strcpy(buf + DST_PREFIX_LEN, tail);
        if (sys_faccessat(AT_FDCWD, buf, R_OK) == 0) {
            log_open("REDIR", buf);
            return (int)sys_openat(AT_FDCWD, buf, flags, mode);
        }
    }
    return -1;
}

static int do_open(const char *path, int flags, int mode) {
    if (path && natartos_strncmp(path, SRC_PREFIX, SRC_PREFIX_LEN) == 0) {
        log_open("OPEN", path);
        int r = try_redirect_audio(path, flags, mode);
        if (r >= 0) return r;
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
