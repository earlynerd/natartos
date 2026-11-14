/*
 * mod_input.so — programmatic key/touch injection into EW UI.
 *
 * Signal handler (SIGRTMIN+2) sets a flag.
 * A pthread worker polls the flag, finds root via heap scan,
 * and calls CoreRoot_DriveKeyboardHitting from thread context.
 *
 * Signal protocol:
 *   Reads /data/local/tmp/natartos_input_cmd to decide what to inject.
 *   File format: one line, e.g. "key 4" (keyCode=4 = RIGHT).
 *   If no file: injects KeyRight down+up (open menu from home screen).
 */

#define NTLOG_PREFIX "input"
#include "natartos_common.h"

#define INPUT_SIGNAL 36
#define SA_RESTART   0x10000000
#define __NR_rt_sigaction 134

static void log_str(const char *s)                       { NTLOG(s); }
static void log_ptr(const char *prefix, unsigned long v)  { NTLOG_HEX(prefix, v); }

/* --- dlsym from loader --- */

static void *(*g_dlsym)(void *, const char *) = 0;

/* --- EW API function pointers --- */

typedef int (*drive_keyboard_fn)(void *root, int keyCode, int charCode, int down);

static drive_keyboard_fn g_drive_key     = 0;
static void             *g_vmt_addr      = 0;
static void * volatile   g_root          = 0;

#define EW_KEY_UP       1
#define EW_KEY_DOWN     2
#define EW_KEY_LEFT     3
#define EW_KEY_RIGHT    4
#define EW_KEY_OK       6
#define EW_KEY_MENU    18
#define EW_KEY_EXIT    19
#define EW_KEY_POWER   20

/* --- Pending key (signal -> thread communication) --- */

static volatile int g_pending_key = 0;

/* --- Heap scan for root object --- */

static int parse_hex(const char *s, unsigned long *out, int *adv) {
    unsigned long v = 0;
    int i = 0;
    while (1) {
        char c = s[i];
        if (c >= '0' && c <= '9')      v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else break;
        i++;
    }
    *out = v;
    *adv = i;
    return i > 0;
}

static void *find_root(void) {
    if (!g_vmt_addr) return 0;
    unsigned long needle = (unsigned long)g_vmt_addr;

    int mapsfd = (int)sys_openat(AT_FDCWD, "/proc/self/maps", O_RDONLY, 0);
    if (mapsfd < 0) { log_str("find_root: can't open maps"); return 0; }

    int memfd = (int)sys_openat(AT_FDCWD, "/proc/self/mem", O_RDONLY, 0);
    if (memfd < 0) { sys_close(mapsfd); log_str("find_root: can't open mem"); return 0; }

    void *result = 0;
    int regions = 0;

    char mapbuf[4096];
    char line[256];
    int lpos = 0;
    long rn;

    while (!result && (rn = sys_read(mapsfd, mapbuf, sizeof(mapbuf))) > 0) {
        for (long ri = 0; ri < rn && !result; ri++) {
            if (mapbuf[ri] != '\n' && lpos < (int)sizeof(line) - 1) {
                line[lpos++] = mapbuf[ri];
                continue;
            }
            line[lpos] = 0;
            lpos = 0;

            unsigned long start = 0, end = 0;
            int a = 0, p = 0;
            if (!parse_hex(line, &start, &a)) continue;
            p = a;
            if (line[p] != '-') continue;
            p++;
            if (!parse_hex(line + p, &end, &a)) continue;
            p += a;
            if (line[p] != ' ') continue;
            p++;

            if (line[p] != 'r') continue;

            int has_slash = 0;
            for (int j = p; line[j]; j++) {
                if (line[j] == '/') { has_slash = 1; break; }
            }
            if (has_slash) continue;

            unsigned long size = end - start;
            if (size < 8) continue;

            regions++;
            unsigned char chunk[4096];
            for (unsigned long addr = start; addr < end && !result; addr += sizeof(chunk)) {
                long want = (long)sizeof(chunk);
                if (addr + (unsigned long)want > end) want = (long)(end - addr);

                if (sys_lseek(memfd, (long)addr, 0) < 0) break;
                long got = sys_read(memfd, chunk, want);
                if (got < 8) break;

                for (long off = 0; off + 8 <= got; off += 8) {
                    unsigned long val = *(unsigned long *)(chunk + off);
                    if (val == needle) {
                        result = (void *)(addr + (unsigned long)off);
                        break;
                    }
                }
            }
        }
    }

    sys_close(memfd);
    sys_close(mapsfd);

    log_ptr("find_root: scanned regions=", (unsigned long)regions);
    if (result) {
        log_ptr("find_root: FOUND root=", (unsigned long)result);
    } else {
        log_str("find_root: vtable not found in anon memory");
    }
    return result;
}

/* --- Read key command from file --- */

static int read_key_cmd(void) {
    int key_code = EW_KEY_RIGHT;
    int fd = (int)sys_openat(AT_FDCWD, "/data/local/tmp/natartos_input_cmd", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[64];
        long n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) {
            buf[n] = 0;
            if (buf[0] == 'k' && buf[1] == 'e' && buf[2] == 'y' && buf[3] == ' ') {
                int val = 0;
                for (int i = 4; buf[i] >= '0' && buf[i] <= '9'; i++)
                    val = val * 10 + (buf[i] - '0');
                if (val > 0) key_code = val;
            }
        }
    }
    return key_code;
}

/* --- Signal handler: async-signal-safe, just sets flag --- */

static void input_signal_handler(int sig) {
    (void)sig;
    int key = read_key_cmd();
    g_pending_key = key;
}

/* --- Worker thread: polls flag, does actual injection --- */

static void *input_worker(void *arg) {
    (void)arg;
    log_str("worker: started");

    while (1) {
        sys_nanosleep_ms(50);

        int key = g_pending_key;
        if (!key) continue;
        g_pending_key = 0;

        if (!g_root) {
            log_str("worker: searching for root...");
            g_root = find_root();
            log_ptr("worker: root=", (unsigned long)g_root);
            if (!g_root) {
                log_str("worker: root not found");
                continue;
            }
        }

        log_ptr("worker: inject key=", (unsigned long)key);
        g_drive_key((void *)g_root, key, 0, 1);
        g_drive_key((void *)g_root, key, 0, 0);
        log_str("worker: done");
    }
    return 0;
}

/* --- Signal setup via syscall --- */

struct kernel_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask[2];
};

static int setup_signal(int signum, void (*handler)(int)) {
    struct kernel_sigaction sa;
    for (int i = 0; i < (int)(sizeof(sa) / sizeof(long)); i++)
        ((long *)&sa)[i] = 0;
    sa.handler = handler;
    sa.flags = SA_RESTART;

    register long x0 __asm__("x0") = signum;
    register long x1 __asm__("x1") = (long)&sa;
    register long x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 8;
    register long x8 __asm__("x8") = __NR_rt_sigaction;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return (int)x0;
}

/* --- Exported init called by natartos loader --- */

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    g_dlsym = loader_dlsym;
    if (!g_dlsym) {
        log_str("mod_input: no dlsym");
        return -1;
    }

    log_str("mod_input: init");

    g_drive_key = (drive_keyboard_fn)g_dlsym(RTLD_DEFAULT,
                                              "CoreRoot_DriveKeyboardHitting");
    g_vmt_addr = g_dlsym(RTLD_DEFAULT, "__vmt_DuFPVApplication");

    log_ptr("  DriveKeyboardHitting=", (unsigned long)g_drive_key);
    log_ptr("  vmt_DuFPV=", (unsigned long)g_vmt_addr);

    if (!g_drive_key) {
        log_str("mod_input: DriveKeyboardHitting not found");
        return -2;
    }
    if (!g_vmt_addr) {
        log_str("mod_input: vmt_DuFPVApplication not found");
        return -3;
    }

    int ret = setup_signal(INPUT_SIGNAL, input_signal_handler);
    log_ptr("  sigaction ret=", (unsigned long)ret);

    typedef int (*pthread_create_fn)(void *, const void *, void *(*)(void *), void *);
    pthread_create_fn my_pthread_create =
        (pthread_create_fn)g_dlsym(RTLD_DEFAULT, "pthread_create");

    if (my_pthread_create) {
        unsigned long tid = 0;
        int tret = my_pthread_create((void *)&tid, 0, input_worker, 0);
        log_ptr("  pthread_create ret=", (unsigned long)tret);
    } else {
        log_str("mod_input: pthread_create not found (worker disabled)");
    }

    log_str("mod_input: ready");
    return 0;
}
