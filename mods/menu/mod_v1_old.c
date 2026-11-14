/*
 * mod_menu.so — inject mod items directly into DJI "More" settings menu.
 *
 * Each mod registers its label via natartos_menu_register() (exported).
 * DefineMenu hook adds all registered items to root_menu as scrollable entries.
 *
 * Two-phase init:
 *   Phase 1 (natartos_mod_init): hook DefineMenu vtables
 *   Phase 2 (natartos_mod_register in other mods): they call our exported
 *            natartos_menu_register() which is now resolvable via dlsym
 *
 * Build: aarch64-linux-gnu-gcc -shared -fPIC -O2 -fno-builtin -nostdlib
 *        -Wl,-soname,mod_menu.so -o mod_menu.so mod_menu.c
 */

#define AT_FDCWD (-100)
#define O_RDONLY    0
#define PROT_READ   1
#define PROT_WRITE  2
#define PAGE_SIZE   4096

#define __NR_openat      56
#define __NR_write        64
#define __NR_close        57
#define __NR_mprotect    226

/* --- syscall wrappers --- */

static long sys_openat(int dirfd, const char *path, int flags, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = mode;
    register long x8 __asm__("x8") = __NR_openat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static long sys_write_sc(int fd, const void *buf, long count) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = __NR_write;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long sys_close(int fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = __NR_close;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long sys_mprotect(void *addr, long len, int prot) {
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = len;
    register long x2 __asm__("x2") = prot;
    register long x8 __asm__("x8") = __NR_mprotect;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

/* --- string helpers --- */

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* --- logging --- */

static void log_str(const char *s) {
    int fd = (int)sys_openat(AT_FDCWD, "/data/local/tmp/natartos_menu.log",
                              0x441 /* O_WRONLY|O_CREAT|O_APPEND */, 0644);
    if (fd >= 0) {
        sys_write_sc(fd, s, my_strlen(s));
        sys_write_sc(fd, "\n", 1);
        sys_close(fd);
    }
}

static void log_ptr(const char *prefix, unsigned long val) {
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
    log_str(buf);
}

/* --- Saved dlsym pointer from loader --- */

static void *(*g_dlsym)(void *, const char *) = 0;
#define RTLD_DEFAULT ((void *)0)

/* === Menu item registry === */

#define MAX_MENU_ITEMS 16
#define ITEM_ID_BASE 0xF000

#define MENU_TARGET_SETTINGS  1
#define MENU_TARGET_MORE      2

static struct {
    const char *label;
    int target;
} g_items[MAX_MENU_ITEMS];
static int g_item_count = 0;

/*
 * Exported — other mods find via dlsym(RTLD_DEFAULT, "natartos_menu_register")
 * in their natartos_mod_register phase (all .so already loaded).
 * target: bitmask of MENU_TARGET_SETTINGS (1) | MENU_TARGET_MORE (2).
 * Returns item index (>=0) or -1 on error.
 */
int natartos_menu_register(const char *label, int target) {
    if (!label || g_item_count >= MAX_MENU_ITEMS) return -1;
    log_str("menu_register:");
    log_str(label);
    log_ptr("  target=", (unsigned long)target);
    g_items[g_item_count].label = label;
    g_items[g_item_count].target = target ? target : (MENU_TARGET_SETTINGS | MENU_TARGET_MORE);
    return g_item_count++;
}

/* === EW / Menu API function pointers === */

typedef void  (*define_menu_fn)(void *tree);
typedef void *(*ew_new_string_ansi_fn)(const char *);
typedef void *(*add_item2_fn)(void *menu, void *label, int id);
typedef void *(*menuitem_menu_fn)(void *item);

static ew_new_string_ansi_fn g_new_string   = 0;
static add_item2_fn          g_add_item2    = 0;
static menuitem_menu_fn      g_mi_menu      = 0;

/* --- vtable hook state --- */

#define MAX_VMT_HOOKS 16
#define DEFINE_MENU_SLOT 11

static struct {
    void **vmt;
    define_menu_fn original;
    const char *name;
} g_hooks[MAX_VMT_HOOKS];
static int g_hook_count = 0;

/* --- DefineMenu hook --- */

static void natartos_define_menu_hook(void *tree) {
    log_str("hook: ENTER");
    log_ptr("hook: tree=", (unsigned long)tree);

    void **vmt = *(void ***)tree;
    log_ptr("hook: vmt=", (unsigned long)vmt);

    /* Identify which hooked vtable fired */
    define_menu_fn original = 0;
    int hook_idx = -1;
    for (int i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].vmt == vmt) {
            original = g_hooks[i].original;
            hook_idx = i;
            break;
        }
    }
    if (!original && g_hook_count > 0) {
        original = g_hooks[0].original;
        hook_idx = 0;
    }
    log_ptr("hook: matched hook_idx=", (unsigned long)(hook_idx + 1));
    if (hook_idx >= 0 && g_hooks[hook_idx].name)
        log_str(g_hooks[hook_idx].name);
    log_ptr("hook: original=", (unsigned long)original);

    if (!original) { log_str("hook: no original, bail"); return; }

    original(tree);
    log_str("hook: original returned");

    /* Dump first few fields of tree for structure analysis */
    for (int off = 0x40; off <= 0xC0; off += 0x10) {
        void *val = *(void **)((char *)tree + off);
        if (val) {
            log_ptr("hook: tree+", (unsigned long)off);
            log_ptr("  =", (unsigned long)val);
        }
    }

    void *root_menu = *(void **)((char *)tree + 0x80);
    if (!root_menu || !g_new_string || !g_add_item2) {
        log_str("hook: no root_menu or symbols, skip");
        return;
    }
    log_ptr("hook: root_menu=", (unsigned long)root_menu);
    log_ptr("hook: items to add=", (unsigned long)g_item_count);

    /* Add each registered item directly into root menu (flat, scrollable) */
    for (int i = 0; i < g_item_count; i++) {
        void *title = g_new_string(g_items[i].label);
        if (!title) continue;

        void *item = g_add_item2(root_menu, title, ITEM_ID_BASE + i);
        if (item) {
            /* Zero +0x44 so LoadMenuItemData uses EwGetAutoObject path */
            *(int *)((char *)item + 0x44) = 0;
            if (g_mi_menu) g_mi_menu(item);
            log_str("hook: added:");
            log_str(g_items[i].label);
        }
    }

    /* If no mods registered items, add a placeholder */
    if (g_item_count == 0) {
        void *title = g_new_string("Natartos");
        if (title) {
            void *item = g_add_item2(root_menu, title, ITEM_ID_BASE);
            if (item) {
                *(int *)((char *)item + 0x44) = 0;
                if (g_mi_menu) g_mi_menu(item);
            }
        }
        log_str("hook: placeholder added");
    }

    log_str("hook: done");
}

/* --- All known UavMenuTree vtable names --- */

static const char * const g_vmt_names[] = {
    "__vmt_UavInvalidUavMenuTree",
    "__vmt_UavWa020UavMenuTree",
    "__vmt_UavWa140UavMenuTree",
    "__vmt_UavWa233UavMenuTree",
    "__vmt_UavWa520UavMenuTree",
    "__vmt_UavWa521UavMenuTree",
    "__vmt_UavWa530UavMenuTree",
    "__vmt_UavWm169unitUavMenuTree",
    "__vmt_UavZa530UavMenuTree",
    "__vmt_UavZa5305UavMenuTree",
    0
};

/* --- Phase 1: hook vtables --- */

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    g_dlsym = loader_dlsym;
    if (!g_dlsym) {
        log_str("mod_init: no dlsym provided");
        return -1;
    }

    log_str("mod_init: resolving EW symbols");

    g_new_string = (ew_new_string_ansi_fn)g_dlsym(RTLD_DEFAULT, "EwNewStringAnsi");
    g_add_item2  = (add_item2_fn)g_dlsym(RTLD_DEFAULT, "MxMenuDefinitionMenu_AddItem2");
    g_mi_menu    = (menuitem_menu_fn)g_dlsym(RTLD_DEFAULT, "MxMenuDefinitionMenuItem_Menu");

    log_ptr("  EwNewStringAnsi=", (unsigned long)g_new_string);
    log_ptr("  AddItem2=", (unsigned long)g_add_item2);
    log_ptr("  MenuItem_Menu=", (unsigned long)g_mi_menu);

    if (!g_add_item2) {
        log_str("mod_init: AddItem2 not found, aborting");
        return -2;
    }

    int patched = 0;
    for (int i = 0; g_vmt_names[i]; i++) {
        void **vmt = (void **)g_dlsym(RTLD_DEFAULT, g_vmt_names[i]);
        if (!vmt) continue;

        log_str(g_vmt_names[i]);
        log_ptr("  slot[11]=", (unsigned long)vmt[DEFINE_MENU_SLOT]);

        define_menu_fn orig = (define_menu_fn)vmt[DEFINE_MENU_SLOT];
        if (!orig) continue;
        if (g_hook_count >= MAX_VMT_HOOKS) break;

        g_hooks[g_hook_count].vmt = vmt;
        g_hooks[g_hook_count].original = orig;
        g_hooks[g_hook_count].name = g_vmt_names[i];
        g_hook_count++;

        unsigned long page = (unsigned long)&vmt[DEFINE_MENU_SLOT] & ~(PAGE_SIZE - 1);
        long mp_ret = sys_mprotect((void *)page, PAGE_SIZE * 2,
                                   PROT_READ | PROT_WRITE);
        if (mp_ret < 0) {
            log_ptr("  mprotect FAIL ret=", (unsigned long)mp_ret);
            continue;
        }

        vmt[DEFINE_MENU_SLOT] = (void *)natartos_define_menu_hook;
        sys_mprotect((void *)page, PAGE_SIZE * 2, PROT_READ);
        log_str("  patched OK");
        patched++;
    }

    log_ptr("mod_init: patched vtables=", (unsigned long)patched);
    return patched;
}
