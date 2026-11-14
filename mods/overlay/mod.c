/*
 * hook_overlay.so — display overlay + screencap + health signal for DJI Goggles 3.
 *
 * GOT-patches duss_hal_display_push_frame in libdisp_eng.so.
 * Provides:
 *   - Frame injection via fb_shared (Doom, PC Stream)
 *   - DMA-buf screencap (SIGWINCH → /data/local/tmp/screencap.bin)
 *   - Health signal for loader anti-brick
 *   - ioctl EVIOCGRAB block (so input mod can work)
 */

#define NTLOG_PREFIX "overlay"
#include "natartos_common.h"

#define sys_write       sys_write_raw
#define my_strlen       natartos_strlen
#define my_strncmp      natartos_strncmp
#define my_strcpy       natartos_strcpy
#define my_memcpy       natartos_memcpy

#define SIGUSR1    10
#define SIGURG     23
#define SIGWINCH   28

#define PROT_EXEC   4

#define __NR_rt_sigaction 134
#define __NR_mmap       222
#define __NR_ftruncate   46
#define __NR_pread64     67
#define __NR_munmap     215
#define MS_INVALIDATE   2
#define __NR_msync      227
#define __NR_ioctl       29
#define EVIOCGRAB_VAL    0x40044590
#define __NR_clock_gettime 113

#define MAP_SHARED  1
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED  ((void *)-1)
#define CLOCK_MONOTONIC 1

#define FRAME_SIZE 8294400L
static const char MAPS_PATH[] = "/proc/self/maps";

/* ---- Extra syscalls ---- */

static long sys_pread64(int fd, void *buf, long count, long offset) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x3 __asm__("x3") = offset;
    register long x8 __asm__("x8") = __NR_pread64;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static void *sys_mmap(void *addr, long len, int prot, int flags, int fd, long offset) {
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = len;
    register long x2 __asm__("x2") = prot;
    register long x3 __asm__("x3") = flags;
    register long x4 __asm__("x4") = fd;
    register long x5 __asm__("x5") = offset;
    register long x8 __asm__("x8") = __NR_mmap;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
    return (void *)x0;
}

static long sys_ioctl(int fd, unsigned long req, unsigned long arg) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = req;
    register long x2 __asm__("x2") = arg;
    register long x8 __asm__("x8") = __NR_ioctl;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long sys_clock_gettime(int clk, struct timespec_t *tp) {
    register long x0 __asm__("x0") = clk;
    register long x1 __asm__("x1") = (long)tp;
    register long x8 __asm__("x8") = __NR_clock_gettime;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static long sys_rt_sigaction(int sig, const void *act, void *oldact, long sigsetsize) {
    register long x0 __asm__("x0") = sig;
    register long x1 __asm__("x1") = (long)act;
    register long x2 __asm__("x2") = (long)oldact;
    register long x3 __asm__("x3") = sigsetsize;
    register long x8 __asm__("x8") = __NR_rt_sigaction;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

/* ---- Helpers ---- */

static unsigned long parse_hex(const char **p) {
    unsigned long v = 0;
    while (1) {
        char c = **p;
        if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (c - 'A' + 10);
        else break;
        (*p)++;
    }
    return v;
}

static void fmt_ptr(char *out, unsigned long v) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int n = (v >> (i * 4)) & 0xf;
        out[2 + (15 - i)] = "0123456789abcdef"[n];
    }
    out[18] = 0;
}

static void fmt_dec(char *out, int v) {
    if (v < 0) { *out++ = '-'; v = -v; }
    char tmp[16]; int i = 0;
    if (v == 0) { *out++ = '0'; *out = 0; return; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) *out++ = tmp[--i];
    *out = 0;
}

static void fmt_u32(char *out, unsigned int v) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        int n = (v >> (i * 4)) & 0xf;
        out[2 + (7 - i)] = "0123456789abcdef"[n];
    }
    out[10] = 0;
}

/* ---- Signal structs ---- */

struct kernel_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask[2];
};
#define SA_RESTART 0x10000000

/* ---- Globals ---- */

__attribute__((weak)) void *dlsym(void *, const char *);
#define RTLD_NEXT    ((void *)-1)
#define RTLD_DEFAULT ((void *)0)

typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn g_dlsym = 0;

volatile unsigned int natartos_health_signal = 0;

typedef int (*push_frame_fn_t)(void *, int, void *);
static push_frame_fn_t g_real_push_frame;

/* ---- Inline patch helpers ---- */

static void flush_icache(void *addr, long size) {
    unsigned long p = (unsigned long)addr & ~63UL;
    unsigned long end = (unsigned long)addr + size;
    for (; p < end; p += 64) {
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    p = (unsigned long)addr & ~63UL;
    for (; p < end; p += 64) {
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static unsigned char g_saved_prologue[16];
static unsigned char g_hook_patch[16];
static void *g_patch_target;

static int inline_patch(void *target, void *hook) {
    unsigned long page = (unsigned long)target & ~0xFFFUL;
    if (sys_mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) < 0)
        return -1;

    g_patch_target = target;
    my_memcpy(g_saved_prologue, target, 16);

    /* Prepare hook redirect: LDR X16, [PC, #8]; BR X16; <hook_addr> */
    unsigned int ldr_x16 = 0x58000050;
    unsigned int br_x16  = 0xD61F0200;
    unsigned long hook_addr = (unsigned long)hook;
    my_memcpy(g_hook_patch, &ldr_x16, 4);
    my_memcpy(g_hook_patch + 4, &br_x16, 4);
    my_memcpy(g_hook_patch + 8, &hook_addr, 8);

    /* Apply hook redirect */
    my_memcpy(target, g_hook_patch, 16);
    flush_icache(target, 16);

    return 0;
}

/* Restore-call-repatch: avoids trampoline relocation of PC-relative insns */
static int call_real_push_frame(void *dh, int plane, void *fb) {
    my_memcpy(g_patch_target, g_saved_prologue, 16);
    flush_icache(g_patch_target, 16);
    int ret = ((push_frame_fn_t)g_patch_target)(dh, plane, fb);
    my_memcpy(g_patch_target, g_hook_patch, 16);
    flush_icache(g_patch_target, 16);
    return ret;
}

/* ---- DMA-buf framebuffers ---- */

#define MAX_FRAMEBUFFERS 4
static void *g_framebuffers[MAX_FRAMEBUFFERS];
static int g_fb_count = 0;

static int find_all_framebuffers(void) {
    int fd = (int)sys_openat(AT_FDCWD, MAPS_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;

    void *local_fbs[MAX_FRAMEBUFFERS];
    int local_count = 0;
    char buf[512];
    int buf_len = 0;

    while (1) {
        if (buf_len < (int)sizeof(buf) - 1) {
            long got = sys_read(fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
            if (got <= 0 && buf_len == 0) break;
            if (got > 0) buf_len += got;
        }
        buf[buf_len] = 0;

        int nl = -1;
        for (int i = 0; i < buf_len; i++) {
            if (buf[i] == '\n') { nl = i; break; }
        }
        if (nl < 0) {
            if (buf_len >= (int)sizeof(buf) - 1) { buf_len = 0; continue; }
            break;
        }

        int has_dm = 0;
        for (int i = 0; i + 5 <= nl; i++) {
            if (buf[i]=='d' && buf[i+1]=='m' && buf[i+2]=='a' &&
                buf[i+3]=='b' && buf[i+4]=='u' && buf[i+5]=='f') { has_dm = 1; break; }
        }

        if (has_dm) {
            const char *p = buf;
            unsigned long start = parse_hex(&p);
            if (*p == '-') p++;
            unsigned long end = parse_hex(&p);
            if ((long)(end - start) == FRAME_SIZE && local_count < MAX_FRAMEBUFFERS)
                local_fbs[local_count++] = (void *)start;
        }

        int remaining = buf_len - nl - 1;
        for (int i = 0; i < remaining; i++) buf[i] = buf[nl + 1 + i];
        buf_len = remaining;
    }
    sys_close(fd);

    for (int i = 0; i < local_count; i++)
        g_framebuffers[i] = local_fbs[i];
    __atomic_store_n(&g_fb_count, local_count, __ATOMIC_RELEASE);
    return local_count;
}

/* ---- Screencap ---- */

static void write_status(const char *msg) {
    static const char LOG[] = "/data/local/tmp/screencap.log";
    int fd = (int)sys_openat(AT_FDCWD, LOG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { sys_write(fd, msg, my_strlen(msg)); sys_close(fd); }
}

static int fb_is_blank(void *fb) {
    unsigned long *p = (unsigned long *)fb;
    for (int i = 0; i < 64; i++) { if (p[i]) return 0; }
    return 1;
}

static void do_capture(void) {
    int count = find_all_framebuffers();
    if (count == 0) { NTLOG("capture: no fb"); write_status("FAIL"); return; }

    /* Pick the first non-blank buffer (back-buffer may be empty) */
    int idx = 0;
    for (int i = 0; i < count; i++) {
        if (!fb_is_blank(g_framebuffers[i])) { idx = i; break; }
    }

    static const char OUT[] = "/data/local/tmp/screencap.bin";
    int ofd = (int)sys_openat(AT_FDCWD, OUT, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ofd < 0) { NTLOG("capture: open fail"); write_status("FAIL"); return; }
    long written = 0;
    while (written < FRAME_SIZE) {
        long chunk = FRAME_SIZE - written;
        if (chunk > 65536) chunk = 65536;
        long w = sys_write(ofd, (char *)g_framebuffers[idx] + written, chunk);
        if (w <= 0) break;
        written += w;
    }
    sys_close(ofd);
    if (written == FRAME_SIZE) {
        NTLOG("capture: OK"); write_status("OK");
    } else {
        NTLOG("capture: short"); write_status("FAIL");
    }
}

static void do_fb_write(void) {
    int count = find_all_framebuffers();
    if (count == 0) return;
    static const char IN[] = "/data/local/tmp/fb_write.bin";
    int ifd = (int)sys_openat(AT_FDCWD, IN, O_RDONLY, 0);
    if (ifd < 0) return;
    long total = 0;
    while (total < FRAME_SIZE) {
        long chunk = FRAME_SIZE - total;
        if (chunk > 65536) chunk = 65536;
        long r = sys_read(ifd, (char *)g_framebuffers[0] + total, chunk);
        if (r <= 0) break;
        total += r;
    }
    sys_close(ifd);
    if (total != FRAME_SIZE) return;
    for (int i = 1; i < count; i++) my_memcpy(g_framebuffers[i], g_framebuffers[0], FRAME_SIZE);
    NTLOG("fb_write: OK");
}

static void sigwinch_handler(int sig) { (void)sig; do_capture(); }
static void sigurg_handler(int sig) { (void)sig; do_fb_write(); }

/* ---- ioctl intercept: block EVIOCGRAB ---- */

int ioctl(int fd, unsigned long request, unsigned long arg) {
    if (request == EVIOCGRAB_VAL) return 0;
    return (int)sys_ioctl(fd, request, arg);
}

/* ---- Frame injection ---- */

typedef int (*pthread_create_fn)(unsigned long *, const void *, void *(*)(void *), void *);

#define SHM_HEADER_G 64
static int g_inject_fd = -1;
static volatile unsigned int g_inject_last_counter = 0;
static volatile int g_inject_active = 0;
static volatile int g_inject_frames = 0;

static void start_inject_thread(void);

static void overlay_cleanup(void) {
    NTLOG("cleanup: stopping overlay");
    g_inject_active = 0;
    if (g_inject_fd >= 0) { sys_close(g_inject_fd); g_inject_fd = -1; }

    /* Clear DMA-bufs so splash doesn't persist after inject stops */
    int fbc = __atomic_load_n(&g_fb_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < fbc; i++)
        natartos_memset(g_framebuffers[i], 0, FRAME_SIZE);
    NTLOG_INT("cleanup: cleared fbs=", fbc);

    typedef int (*system_fn)(const char *);
    system_fn sys_fn = g_dlsym ? (system_fn)g_dlsym(RTLD_DEFAULT, "system") : 0;
    if (sys_fn) {
        sys_fn("kill $(pidof doom_goggles) 2>/dev/null; "
               "kill $(pidof fb_receiver) 2>/dev/null; "
               "rm -f /data/local/tmp/fb_override /data/local/tmp/fb_shared "
               "/data/local/tmp/fb_stop");
    }

    NTLOG("cleanup: done");
}

static void *inject_thread_fn(void *arg) {
    (void)arg;
    NTLOG("inject thr: start");
    struct timespec_t ts = { 0, 16666666 };
    int logged_fb = 0;
    int check_counter = 0;

    while (1) {
        sys_nanosleep(&ts, 0);
        if (!g_inject_active) break;
        if (g_inject_fd < 0) continue;

        check_counter++;
        if ((check_counter & 0x7F) == 0) {
            if (sys_faccessat(AT_FDCWD, "/data/local/tmp/fb_override", 0) != 0) {
                NTLOG("inject thr: fb_override gone");
                overlay_cleanup();
                break;
            }
        }
        if ((check_counter & 0x7F) == 1) {
            if (sys_faccessat(AT_FDCWD, "/data/local/tmp/fb_stop", 0) == 0) {
                NTLOG("inject thr: fb_stop found");
                overlay_cleanup();
                return 0;
            }
        }

        int fbc = __atomic_load_n(&g_fb_count, __ATOMIC_ACQUIRE);
        if (fbc == 0) {
            find_all_framebuffers();
            fbc = g_fb_count;
            if (fbc == 0) continue;
        }

        unsigned int counter = 0;
        sys_pread64(g_inject_fd, &counter, 4, 0);
        if (counter == 0) continue;
        int new_frame = (counter != g_inject_last_counter);
        g_inject_last_counter = counter;

        long total = 0;
        while (total < FRAME_SIZE) {
            long got = sys_pread64(g_inject_fd, (char *)g_framebuffers[0] + total,
                                   FRAME_SIZE - total, SHM_HEADER_G + total);
            if (got <= 0) break;
            total += got;
        }

        for (int i = 1; i < fbc; i++)
            my_memcpy(g_framebuffers[i], g_framebuffers[0], FRAME_SIZE);

        g_inject_frames++;
    }
    NTLOG("inject thr: exit");
    return 0;
}

static void start_inject_thread(void) {
    pthread_create_fn pcreate = g_dlsym ? (pthread_create_fn)g_dlsym(RTLD_DEFAULT, "pthread_create") : 0;
    if (!pcreate) return;
    unsigned long tid;
    pcreate(&tid, 0, inject_thread_fn, 0);
}

/* ---- Watcher thread ---- */

static void *watcher_thread_fn(void *arg) {
    (void)arg;
    static const char OVR[] = "/data/local/tmp/fb_override";
    static const char SHM[] = "/data/local/tmp/fb_shared";
    struct timespec_t ts = { 0, 500000000 };
    NTLOG("watcher: start");
    int cooldown = 0;
    int logged_no_shm = 0;
    while (1) {
        sys_nanosleep(&ts, 0);
        if (g_inject_active) { cooldown = 4; logged_no_shm = 0; continue; }
        if (cooldown > 0) { cooldown--; continue; }
        if (sys_faccessat(AT_FDCWD, OVR, 0) != 0) continue;
        int fd = (int)sys_openat(AT_FDCWD, SHM, O_RDONLY, 0);
        if (fd < 0) { if (!logged_no_shm) { NTLOG("watcher: waiting for fb_shared"); logged_no_shm = 1; } continue; }
        NTLOG("watcher: fb_shared opened, starting inject");
        g_inject_fd = fd;
        g_inject_active = 1;
        if (g_fb_count == 0) find_all_framebuffers();
        g_inject_frames = 0;
        g_inject_last_counter = 0;
        start_inject_thread();
    }
    return 0;
}

static void start_watcher_thread(void) {
    pthread_create_fn pcreate = g_dlsym ? (pthread_create_fn)g_dlsym(RTLD_DEFAULT, "pthread_create") : 0;
    if (!pcreate) { NTLOG("watcher: pthread_create not found"); return; }
    unsigned long tid;
    int ret = pcreate(&tid, 0, watcher_thread_fn, 0);
    NTLOG_INT("watcher: pthread_create ret=", ret);
}

static int g_push_count = 0;

/*
 * LD_PRELOAD interposition: export this name so the dynamic linker
 * resolves all calls to duss_hal_display_push_frame to our version.
 * The real function is resolved via dlsym(RTLD_NEXT) in mod_init.
 */
int duss_hal_display_push_frame(void *display_handle, int plane_id, void *frame_buf) {
    g_push_count++;
    natartos_health_signal++;

    if (g_push_count == 1)
        NTLOG("push_frame: HOOK ALIVE");

    /* Frame injection: check fb_override, open fb_shared, start inject thread */
    static int s_shm_fd = -1;

    if ((g_push_count & 0x3F) == 1) {
        static const char SHM_PATH[] = "/data/local/tmp/fb_shared";
        static const char OVR_FLAG[] = "/data/local/tmp/fb_override";
        int ovr = (sys_faccessat(AT_FDCWD, OVR_FLAG, 0) == 0);
        if (ovr && g_fb_count == 0) find_all_framebuffers();
        if (ovr && s_shm_fd < 0) {
            s_shm_fd = (int)sys_openat(AT_FDCWD, SHM_PATH, O_RDONLY, 0);
            if (s_shm_fd >= 0) {
                g_inject_fd = s_shm_fd;
                g_inject_active = 1;
                start_inject_thread();
                NTLOG_INT("inject: fd=", s_shm_fd);
            }
        }
    }

    /* Skip push_frame when injecting — inject thread paints DMA-bufs directly */
    if (g_inject_active) {
        static int s_skip_logged = 0;
        if (!s_skip_logged) { NTLOG("push_frame: SKIP (inject)"); s_skip_logged = 1; }
        return 0;
    }

    if (g_real_push_frame) {
        static int s_was_injecting = 0;
        static int s_resume_logged = 0;
        if (g_inject_frames > 0) s_was_injecting = 1;
        if (s_was_injecting && !s_resume_logged) {
            NTLOG("push_frame: RESUME passthrough");
            s_resume_logged = 1;
        }
        return g_real_push_frame(display_handle, plane_id, frame_buf);
    }
    return 0;
}

/* ---- Signal handlers ---- */

static void sigusr1_handler(int sig) {
    (void)sig;
    int fd = (int)sys_openat(AT_FDCWD, "/data/local/tmp/fb_stop",
                             O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd >= 0) sys_close(fd);
}

/* ---- Constructor ---- */

__attribute__((constructor))
static void init_hook(void) {
    struct kernel_sigaction sa = { .handler = sigwinch_handler, .flags = SA_RESTART };
    sys_rt_sigaction(SIGWINCH, &sa, 0, 8);

    struct kernel_sigaction sa2 = { .handler = sigurg_handler, .flags = SA_RESTART };
    sys_rt_sigaction(SIGURG, &sa2, 0, 8);

    struct kernel_sigaction sa3 = { .handler = sigusr1_handler, .flags = SA_RESTART };
    sys_rt_sigaction(SIGUSR1, &sa3, 0, 8);

    char exe[256];
    int cmd_fd = (int)sys_openat(AT_FDCWD, "/proc/self/cmdline", O_RDONLY, 0);
    int is_glasses = 0;
    if (cmd_fd >= 0) {
        long r = sys_read(cmd_fd, exe, sizeof(exe) - 1);
        sys_close(cmd_fd);
        if (r > 0) {
            exe[r] = 0;
            for (int i = 0; i + 11 <= r; i++) {
                if (my_strncmp(exe + i, "dji_glasses", 11) == 0) {
                    is_glasses = 1; break;
                }
            }
        }
    }

    if (!is_glasses) return;

    NTLOG("hook_overlay loaded");
}

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    g_dlsym = (dlsym_fn)loader_dlsym;

    /* Resolve the REAL push_frame behind our LD_PRELOAD interposition */
    push_frame_fn_t real_fn = (push_frame_fn_t)loader_dlsym(RTLD_NEXT,
                                                              "duss_hal_display_push_frame");
    if (!real_fn) {
        NTLOG("WARN: push_frame not found via RTLD_NEXT");
    } else {
        NTLOG_HEX("real push_frame=", (unsigned long)real_fn);
        /* Inline-patch the real function to redirect to our hook */
        if (inline_patch((void *)real_fn,
                         (void *)duss_hal_display_push_frame) == 0) {
            g_real_push_frame = call_real_push_frame;
            NTLOG("inline patch: OK");
        } else {
            NTLOG("inline patch: FAILED (mprotect)");
            g_real_push_frame = real_fn;
        }
    }

    NTLOG("init: starting watcher");
    start_watcher_thread();
    return 0;
}
