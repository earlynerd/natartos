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
static unsigned long *g_got_slot = 0;

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

static void do_capture(void) {
    if (find_all_framebuffers() == 0) { NTLOG("capture: no fb"); return; }
    static const char OUT[] = "/data/local/tmp/screencap.bin";
    int ofd = (int)sys_openat(AT_FDCWD, OUT, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ofd < 0) { NTLOG("capture: open fail"); return; }
    long written = 0;
    while (written < FRAME_SIZE) {
        long chunk = FRAME_SIZE - written;
        if (chunk > 65536) chunk = 65536;
        long w = sys_write(ofd, (char *)g_framebuffers[0] + written, chunk);
        if (w <= 0) break;
        written += w;
    }
    sys_close(ofd);
    NTLOG(written == FRAME_SIZE ? "capture: OK" : "capture: short");
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
static volatile int g_inject_push_ok = 0;

static volatile void *g_saved_display_handle = 0;
static volatile int g_saved_plane_id = 0;
static char g_frame_buf_copy[512] __attribute__((aligned(16)));
static void *g_pixel_buf = 0;
static volatile int g_push_params_ready = 0;

static void start_inject_thread(void);

static void overlay_cleanup(void) {
    NTLOG("cleanup: stopping overlay");
    g_inject_active = 0;
    if (g_inject_fd >= 0) { sys_close(g_inject_fd); g_inject_fd = -1; }

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
    while (1) {
        sys_nanosleep(&ts, 0);
        if (g_inject_active) { cooldown = 4; continue; }
        if (cooldown > 0) { cooldown--; continue; }
        if (sys_faccessat(AT_FDCWD, OVR, 0) != 0) continue;
        int fd = (int)sys_openat(AT_FDCWD, SHM, O_RDONLY, 0);
        if (fd < 0) { NTLOG("watcher: no fb_shared yet"); continue; }
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

/* ---- GOT patching ---- */

#define PUSH_FRAME_GOT_OFFSET 0x1ffc8

static int g_push_count = 0;

static unsigned long find_lib_base(const char *lib_name) {
    int fd = (int)sys_openat(AT_FDCWD, MAPS_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;

    char buf[512];
    int buf_len = 0;
    int lib_name_len = my_strlen(lib_name);
    unsigned long result = 0;

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

        int has_lib = 0;
        for (int i = 0; i + lib_name_len <= nl; i++) {
            if (my_strncmp(buf + i, lib_name, lib_name_len) == 0) { has_lib = 1; break; }
        }

        if (has_lib) {
            const char *p = buf;
            unsigned long start = parse_hex(&p);
            if (*p == '-') p++;
            parse_hex(&p);
            while (*p == ' ') p++;
            if (p[0] == 'r' && (p[1] == '-' || p[1] == 'w') && (p[2] == 'x')) {
                result = start;
                break;
            }
            if (!result) result = start;
        }

        int remaining = buf_len - nl - 1;
        for (int i = 0; i < remaining; i++) buf[i] = buf[nl + 1 + i];
        buf_len = remaining;
    }
    sys_close(fd);
    return result;
}

static int my_push_frame_hook(void *display_handle, int plane_id, void *frame_buf) {
    g_push_count++;
    natartos_health_signal++;

    /* FPS every 600 frames */
    {
        static struct timespec_t ts_start;
        static int fps_base_count = 0;
        if (g_push_count == 1) {
            sys_clock_gettime(CLOCK_MONOTONIC, &ts_start);
            fps_base_count = 1;
        } else if ((g_push_count % 600) == 0) {
            struct timespec_t ts_now;
            sys_clock_gettime(CLOCK_MONOTONIC, &ts_now);
            long elapsed_ms = (ts_now.tv_sec - ts_start.tv_sec) * 1000
                            + (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000;
            int frames = g_push_count - fps_base_count;
            int fps = (elapsed_ms > 0) ? (int)((long)frames * 1000 / elapsed_ms) : 0;
            char msg[80], tmp[32];
            int p = 0;
            my_memcpy(msg+p, "FPS: ", 5); p += 5;
            fmt_dec(tmp, fps); my_memcpy(msg+p, tmp, my_strlen(tmp)); p += my_strlen(tmp);
            my_memcpy(msg+p, " cnt=", 5); p += 5;
            fmt_dec(tmp, g_push_count); my_memcpy(msg+p, tmp, my_strlen(tmp)); p += my_strlen(tmp);
            msg[p] = 0;
            NTLOG(msg);
            ts_start = ts_now;
            fps_base_count = g_push_count;
        }
    }

    /* Frame injection: check fb_override, open fb_shared, start inject thread */
    static int s_override_active = 0;
    static int s_shm_fd = -1;

    if ((g_push_count & 0x3F) == 1) {
        static const char SHM_PATH[] = "/data/local/tmp/fb_shared";
        static const char OVR_FLAG[] = "/data/local/tmp/fb_override";
        s_override_active = (sys_faccessat(AT_FDCWD, OVR_FLAG, 0) == 0);
        if (s_override_active && g_fb_count == 0) find_all_framebuffers();
        if (s_override_active && s_shm_fd < 0) {
            s_shm_fd = (int)sys_openat(AT_FDCWD, SHM_PATH, O_RDONLY, 0);
            if (s_shm_fd >= 0) {
                g_inject_fd = s_shm_fd;
                g_inject_active = 1;
                start_inject_thread();
                NTLOG_INT("inject: fd=", s_shm_fd);
            }
        }
    }

    /* Save frame params for pixel buffer */
    g_saved_display_handle = display_handle;
    g_saved_plane_id = plane_id;
    if (frame_buf && !g_push_params_ready) {
        my_memcpy(g_frame_buf_copy, frame_buf, 512);
        if (!g_pixel_buf) {
            g_pixel_buf = sys_mmap(0, FRAME_SIZE, PROT_READ|PROT_WRITE,
                                   MAP_SHARED|MAP_ANONYMOUS, -1, 0);
            if (g_pixel_buf == MAP_FAILED) g_pixel_buf = 0;
        }
        if (g_pixel_buf) {
            *(unsigned long *)(g_frame_buf_copy + 0x20) = (unsigned long)g_pixel_buf;
            g_push_params_ready = 1;
            NTLOG_HEX("pixel_buf=", (unsigned long)g_pixel_buf);
        }
    }

    return g_real_push_frame(display_handle, plane_id, frame_buf);
}

static void do_got_patch(void) {
    unsigned long disp_base = find_lib_base("libdisp_eng.so");
    if (!disp_base) { NTLOG("GOT: libdisp_eng.so not found"); return; }
    unsigned long got_addr = disp_base + PUSH_FRAME_GOT_OFFSET;

    unsigned long page = got_addr & ~0xFFFUL;
    sys_mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE);

    unsigned long *slot = (unsigned long *)got_addr;
    unsigned long old_val = *slot;
    g_real_push_frame = (push_frame_fn_t)old_val;

    NTLOG_HEX("GOT: slot=", got_addr);
    NTLOG_HEX("GOT: old=", old_val);

    *slot = (unsigned long)my_push_frame_hook;
    g_got_slot = slot;

    NTLOG_HEX("GOT: PATCHED new=", (unsigned long)my_push_frame_hook);
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

    NTLOG("hook_overlay loaded");

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

    if (is_glasses) {
        NTLOG("GOT: patching in dji_glasses");
        do_got_patch();
    }
}

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    g_dlsym = (dlsym_fn)loader_dlsym;
    NTLOG("init: starting watcher");
    start_watcher_thread();
    return 0;
}
