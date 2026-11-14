/*
 * fb_receiver — USB screen streaming receiver for DJI Goggles 3.
 *
 * Reads framed JPEG from stdin (piped via ADB from toolya browser),
 * decodes to BGRA via stb_image, writes to fb_shared.
 * hook_overlay.so inject thread picks up fb_shared → DMA-buf → display.
 *
 * Protocol (stdin): [u32le jpeg_size][jpeg_bytes] per frame, continuously.
 * Same framing as fb_stream uses for output — symmetric.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -o fb_receiver fb_receiver.c -lm
 * (stb_image.h must be in include path)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#define FB_WIDTH   1920
#define FB_HEIGHT  1080
#define FB_SIZE    (FB_WIDTH * FB_HEIGHT * 4)
#define SHM_HEADER 64
#define SHM_SIZE   (SHM_HEADER + FB_SIZE)
#define SHM_PATH   "/data/local/tmp/fb_shared"
#define FLAG_PATH  "/data/local/tmp/fb_override"
#define LOG_PATH   "/data/local/tmp/fb_receiver.log"
#define MAX_FRAME  (4 * 1024 * 1024)

static int g_shm_fd = -1;
static unsigned char *g_shm_map = NULL;
static volatile unsigned int *g_shm_counter;
static unsigned char *g_shm_pixels;
static FILE *g_log = NULL;
static volatile int g_exit = 0;
static pid_t g_watcher_pid = -1;

#define EV_KEY      0x01
#define KEY_BACK    158
#define EVDEV_PATH  "/dev/input/event0"

static void logmsg(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void sigterm_handler(int sig) {
    (void)sig;
    g_exit = 1;
}

static int read_exact(int fd, void *buf, int n) {
    int total = 0;
    while (total < n) {
        if (g_exit) return -1;
        int got = read(fd, (char *)buf + total, n - total);
        if (got <= 0) return -1;
        total += got;
    }
    return total;
}

/* Fork a child that monitors gpio_keys for Back button → SIGTERM parent */
static void start_back_watcher(void) {
    struct {
        unsigned long tv_sec;
        unsigned long tv_usec;
        unsigned short type;
        unsigned short code;
        unsigned int value;
    } ie;

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid > 0) { g_watcher_pid = pid; return; }

    /* Child process */
    int ev = open(EVDEV_PATH, O_RDONLY);
    if (ev < 0) _exit(1);
    while (read(ev, &ie, sizeof(ie)) == (int)sizeof(ie)) {
        if (ie.type == EV_KEY && ie.code == KEY_BACK && ie.value == 1) {
            kill(getppid(), SIGTERM);
            close(ev);
            _exit(0);
        }
    }
    close(ev);
    _exit(1);
}

static int shm_init(void) {
    g_shm_fd = open(SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (g_shm_fd < 0) { logmsg("shm open: %s\n", strerror(errno)); return -1; }
    if (ftruncate(g_shm_fd, SHM_SIZE) < 0) { logmsg("ftruncate: %s\n", strerror(errno)); return -1; }
    g_shm_map = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_shm_map == MAP_FAILED) { logmsg("mmap: %s\n", strerror(errno)); return -1; }
    g_shm_counter = (volatile unsigned int *)g_shm_map;
    g_shm_pixels = g_shm_map + SHM_HEADER;
    memset(g_shm_map, 0, SHM_HEADER);
    return 0;
}

static void shm_write(const unsigned char *bgra, int w, int h) {
    if (w == FB_WIDTH && h == FB_HEIGHT) {
        memcpy(g_shm_pixels, bgra, FB_SIZE);
    } else {
        memset(g_shm_pixels, 0, FB_SIZE);
        float sx = (float)FB_WIDTH / w, sy = (float)FB_HEIGHT / h;
        float scale = sx < sy ? sx : sy;
        int dw = (int)(w * scale), dh = (int)(h * scale);
        int ox = (FB_WIDTH - dw) / 2, oy = (FB_HEIGHT - dh) / 2;
        for (int dy = 0; dy < dh; dy++) {
            int src_y = dy * h / dh;
            unsigned char *dst = g_shm_pixels + ((oy + dy) * FB_WIDTH + ox) * 4;
            const unsigned char *src_row = bgra + src_y * w * 4;
            for (int dx = 0; dx < dw; dx++) {
                int src_x = dx * w / dw;
                const unsigned char *sp = src_row + src_x * 4;
                dst[0] = sp[0]; dst[1] = sp[1]; dst[2] = sp[2]; dst[3] = sp[3];
                dst += 4;
            }
        }
    }
    __atomic_add_fetch(g_shm_counter, 1, __ATOMIC_RELEASE);
}

/* ── Splash screen (shown while waiting for connection) ── */

#define GW 5
#define GH 7
#define GLYPH_N 12

static const unsigned char g_font[GLYPH_N][GH] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x11}, /* M */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x1E,0x11,0x11,0x1E,0x12,0x11,0x11}, /* R */
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, /* . */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
};

static int gidx(char c) {
    switch (c) {
        case 'A': return 0;  case 'C': return 1;  case 'D': return 2;
        case 'E': return 3;  case 'M': return 4;  case 'P': return 5;
        case 'R': return 6;  case 'S': return 7;  case 'T': return 8;
        case 'Y': return 9;  case '.': return 10; default:  return 11;
    }
}

static void draw_text(unsigned char *fb, const char *t, int ox, int oy, int sc,
                      unsigned char cb, unsigned char cg, unsigned char cr) {
    int x = ox;
    for (int i = 0; t[i]; i++) {
        const unsigned char *gl = g_font[gidx(t[i])];
        for (int r = 0; r < GH; r++) {
            unsigned char bits = gl[r];
            for (int c = 0; c < GW; c++) {
                if (!(bits & (0x10 >> c))) continue;
                for (int sy = 0; sy < sc; sy++) {
                    int py = oy + r * sc + sy;
                    if (py < 0 || py >= FB_HEIGHT) continue;
                    for (int sx = 0; sx < sc; sx++) {
                        int px = x + c * sc + sx;
                        if (px < 0 || px >= FB_WIDTH) continue;
                        unsigned char *p = fb + (py * FB_WIDTH + px) * 4;
                        p[0] = cb; p[1] = cg; p[2] = cr; p[3] = 255;
                    }
                }
            }
        }
        x += (GW + 1) * sc;
    }
}

static int text_w(const char *t, int sc) {
    int n = 0; while (t[n]) n++;
    return n > 0 ? n * (GW + 1) * sc - sc : 0;
}

static void fill_rect(unsigned char *fb, int x, int y, int w, int h,
                      unsigned char rb, unsigned char rg, unsigned char rr) {
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= FB_HEIGHT) continue;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < 0 || px >= FB_WIDTH) continue;
            unsigned char *p = fb + (py * FB_WIDTH + px) * 4;
            p[0] = rb; p[1] = rg; p[2] = rr; p[3] = 255;
        }
    }
}

static void splash_screen(void) {
    /* Dark background */
    fill_rect(g_shm_pixels, 0, 0, FB_WIDTH, FB_HEIGHT, 0x28, 0x22, 0x1C);

    /* Monitor outline */
    int mw = 320, mh = 200, mx = (FB_WIDTH - mw) / 2, my = 240;
    fill_rect(g_shm_pixels, mx, my, mw, mh, 0x88, 0x88, 0x88);
    fill_rect(g_shm_pixels, mx + 3, my + 3, mw - 6, mh - 6, 0x28, 0x22, 0x1C);
    fill_rect(g_shm_pixels, (FB_WIDTH - 60) / 2, my + mh + 4, 60, 8, 0x60, 0x60, 0x60);
    fill_rect(g_shm_pixels, (FB_WIDTH - 120) / 2, my + mh + 14, 120, 4, 0x60, 0x60, 0x60);

    /* Play triangle inside monitor */
    int tcx = FB_WIDTH / 2 - 15, tcy = my + mh / 2;
    for (int dy = -20; dy <= 20; dy++) {
        int span = 30 * (20 - (dy < 0 ? -dy : dy)) / 20;
        if (span < 1) continue;
        fill_rect(g_shm_pixels, tcx, tcy + dy, span, 1, 0xDD, 0xDD, 0xDD);
    }

    /* "PC STREAM" — scale 8 */
    const char *title = "PC STREAM";
    int tw = text_w(title, 8);
    draw_text(g_shm_pixels, title, (FB_WIDTH - tw) / 2, 500, 8, 0xFF, 0xFF, 0xFF);

    /* "READY..." — scale 3 */
    const char *sub = "READY...";
    int sw = text_w(sub, 3);
    draw_text(g_shm_pixels, sub, (FB_WIDTH - sw) / 2, 580, 3, 0x70, 0x70, 0x70);

    /* Accent line */
    fill_rect(g_shm_pixels, (FB_WIDTH - 300) / 2, 610, 300, 2, 0x50, 0x50, 0x50);

    __atomic_add_fetch(g_shm_counter, 1, __ATOMIC_RELEASE);
    logmsg("splash shown\n");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    /* SIGTERM without SA_RESTART so blocking read() returns EINTR */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);

    g_log = fopen(LOG_PATH, "w");
    logmsg("fb_receiver: start\n");

    unsigned int decoded = 0, dropped = 0;
    unsigned char *buf = NULL;
    int input_fd = 0;

    if (shm_init() < 0) { logmsg("FATAL: shm\n"); return 1; }

    /* Set fb_override flag → hook_overlay.so starts injecting */
    int ff = open(FLAG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ff >= 0) close(ff);

    /* Show splash while waiting for connection */
    splash_screen();

    /* Monitor Back button — child sends SIGTERM on press */
    start_back_watcher();

    /* Open input: FIFO (blocks until writer connects or SIGTERM) or stdin */
    if (argc > 1) {
        logmsg("input: %s (waiting for writer)\n", argv[1]);
        input_fd = open(argv[1], O_RDONLY);
        if (input_fd < 0) {
            if (g_exit) logmsg("interrupted by Back\n");
            else logmsg("open %s: %s\n", argv[1], strerror(errno));
            goto cleanup;
        }
        logmsg("writer connected\n");
    } else {
        logmsg("input: stdin\n");
    }

    buf = malloc(MAX_FRAME);
    if (!buf) { logmsg("FATAL: malloc\n"); goto cleanup; }

    /* Read framed JPEGs: [u32le size][jpeg bytes] */
    while (1) {
        unsigned int sz = 0;
        if (read_exact(input_fd, &sz, 4) < 0) break;
        if (sz == 0 || sz > MAX_FRAME) { logmsg("bad frame size: %u\n", sz); break; }
        if (read_exact(input_fd, buf, sz) < 0) break;

        int w, h, ch;
        unsigned char *rgba = stbi_load_from_memory(buf, sz, &w, &h, &ch, 4);
        if (!rgba) { dropped++; continue; }

        /* RGBA → BGRA in-place */
        int n = w * h;
        for (int i = 0; i < n; i++) {
            unsigned char *p = rgba + i * 4;
            unsigned char t = p[0]; p[0] = p[2]; p[2] = t;
        }

        shm_write(rgba, w, h);
        stbi_image_free(rgba);
        decoded++;

        if ((decoded & 0xFF) == 0)
            logmsg("frames: %u dec, %u drop\n", decoded, dropped);
    }

    free(buf);
cleanup:
    if (g_watcher_pid > 0) {
        kill(g_watcher_pid, SIGKILL);
        waitpid(g_watcher_pid, NULL, WNOHANG);
    }
    unlink(FLAG_PATH);
    if (input_fd > 0) close(input_fd);
    if (g_shm_map) munmap(g_shm_map, SHM_SIZE);
    if (g_shm_fd >= 0) close(g_shm_fd);
    logmsg("fb_receiver: exit (dec=%u drop=%u)\n", decoded, dropped);
    if (g_log) fclose(g_log);
    return 0;
}
