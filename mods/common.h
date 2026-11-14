/*
 * natartos_common.h — shared syscall wrappers, string helpers, log macros.
 * All functions are static inline — no runtime dependency between mods.
 */

#ifndef NATARTOS_COMMON_H
#define NATARTOS_COMMON_H

#define AT_FDCWD      (-100)
#define O_RDONLY      0
#define O_WRONLY      1
#define O_CREAT       0100
#define O_TRUNC       01000
#define O_APPEND      02000
#define RTLD_DEFAULT  ((void *)0)
#define RTLD_NEXT     ((void *)-1)

#define PROT_READ   1
#define PROT_WRITE  2
#define PAGE_SIZE   4096

#define __NR_openat      56
#define __NR_read         63
#define __NR_write        64
#define __NR_close        57
#define __NR_faccessat    48
#define __NR_mprotect    226
#define __NR_nanosleep   101

struct timespec_t { long tv_sec; long tv_nsec; };

/* --- Raw syscalls (aarch64) --- */

static inline long sys_openat(int dirfd, const char *path, int flags, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = mode;
    register long x8 __asm__("x8") = __NR_openat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static inline long sys_read(int fd, void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_read;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long sys_write_sc(int fd, const void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_write;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long sys_close(int fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = __NR_close;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long sys_faccessat(int dirfd, const char *path, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = mode;
    register long x3 __asm__("x3") = 0;
    register long x8 __asm__("x8") = __NR_faccessat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static inline long sys_mprotect(void *addr, long len, int prot) {
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = len;
    register long x2 __asm__("x2") = prot;
    register long x8 __asm__("x8") = __NR_mprotect;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long sys_nanosleep(const struct timespec_t *req, struct timespec_t *rem) {
    register long x0 __asm__("x0") = (long)req;
    register long x1 __asm__("x1") = (long)rem;
    register long x8 __asm__("x8") = __NR_nanosleep;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

/* --- String helpers --- */

static inline int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int my_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static inline void my_memset(void *dst, int c, long n) {
    unsigned char *p = (unsigned char *)dst;
    while (n-- > 0) *p++ = (unsigned char)c;
}

/* --- Logging --- */

static inline void natartos_log(const char *log_path, const char *msg) {
    int fd = (int)sys_openat(AT_FDCWD, log_path,
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sys_write_sc(fd, msg, my_strlen(msg));
        sys_write_sc(fd, "\n", 1);
        sys_close(fd);
    }
}

static inline void natartos_log_ptr(const char *log_path, const char *prefix, unsigned long val) {
    char buf[80];
    int i = 0;
    const char *p = prefix;
    while (*p && i < 50) buf[i++] = *p++;
    buf[i++] = '0'; buf[i++] = 'x';
    int started = 0;
    for (int j = 60; j >= 0; j -= 4) {
        int n = (val >> j) & 0xf;
        if (!started && n == 0 && j > 0) continue;
        started = 1;
        buf[i++] = n < 10 ? '0' + n : 'a' + n - 10;
    }
    if (!started) buf[i++] = '0';
    buf[i] = 0;
    natartos_log(log_path, buf);
}

#endif /* NATARTOS_COMMON_H */
