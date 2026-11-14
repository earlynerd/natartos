/*
 * mod_menu v2 — hook MoreMenu Postreload, dump diagnostics, inject test item.
 *
 * Injection controlled by flag file: /data/local/tmp/natartos_inject_menu
 * Touch the file to enable, rm to disable.
 *
 * Build: aarch64-linux-gnu-gcc -shared -fPIC -O2 -fno-builtin -nostdlib
 *        -Wl,-soname,mod_menu.so -o mod_menu.so mod_diag.c
 */

#define AT_FDCWD (-100)
#define PROT_READ   1
#define PROT_WRITE  2
#define PAGE_SIZE   4096
#define RTLD_DEFAULT ((void *)0)
#define F_OK 0

#define __NR_openat      56
#define __NR_write        64
#define __NR_close        57
#define __NR_mprotect    226
#define __NR_faccessat    48

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

static long sys_faccessat(int dirfd, const char *path, int mode) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = mode;
    register long x3 __asm__("x3") = 0;
    register long x8 __asm__("x8") = __NR_faccessat;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

#define LOG_PATH "/data/local/tmp/natartos_menu.log"
#define INJECT_FLAG "/data/local/tmp/natartos_inject_menu"

static void log_str(const char *s) {
    int fd = (int)sys_openat(AT_FDCWD, LOG_PATH,
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

static void log_int(const char *prefix, int val) {
    char buf[80];
    int i = 0;
    const char *p = prefix;
    while (*p && i < 50) buf[i++] = *p++;
    if (val < 0) { buf[i++] = '-'; val = -val; }
    char digits[12];
    int d = 0;
    if (val == 0) digits[d++] = '0';
    while (val > 0) { digits[d++] = '0' + val % 10; val /= 10; }
    while (d > 0) buf[i++] = digits[--d];
    buf[i] = 0;
    log_str(buf);
}

/* ---- Resolved symbols ---- */

static void *(*g_dlsym)(void *, const char *) = 0;

/* EW runtime */
typedef void *(*ew_new_obj_fn)(void *cls, int arg);
static ew_new_obj_fn g_ew_new_obj = 0;  /* EwNewObjectIndirect */

typedef void *(*ew_new_str_fn)(const char *s);
static ew_new_str_fn g_ew_new_str = 0;  /* EwNewStringAnsi */

/* EW slot: 16-byte struct {object, method} */
typedef struct { void *object; void *method; } XSlot;
typedef XSlot (*ew_new_slot_fn)(void *object, void *method);
static ew_new_slot_fn g_ew_new_slot = 0;

/* Menu item activation: binds slot to OnActivate */
typedef void (*onset_activate_fn)(void *self, XSlot slot);
static onset_activate_fn g_on_set_activate = 0;

/* Menu item class descriptor */
static void *g_vthis_indexed_image_action = 0;

/* Caption setter: MxBindableMenuItem__OnSetCaption(self, caption_string) */
typedef void (*onset_caption_fn)(void *self, void *caption);
static onset_caption_fn g_set_caption = 0;

/* AddItemFromData: MxBindableAbstractMenu_AddItemFromData(menu, data) */
typedef void (*add_from_data_fn)(void *menu, void *data);
static add_from_data_fn g_add_from_data = 0;

/* CoreGroup__AddBehind(group, view, behind) */
typedef void (*add_behind_fn)(void *group, void *view, void *behind);
static add_behind_fn g_add_behind = 0;

/* ---- MoreMenu hook state ---- */

typedef void (*postreload_fn)(void *self);
static void **g_more_vmt = 0;
static postreload_fn g_more_orig_postreload = 0;
static int g_hook_fired = 0;
static int g_injected = 0;

/* ---- Natartos item activation callback ---- */

static void natartos_on_activate(void *self) {
    log_str("=== NATARTOS ITEM ACTIVATED ===");
    log_ptr("  self=", (unsigned long)self);
}

/* ---- Item injection ---- */

static void try_inject_item(void *menu_self) {
    if (sys_faccessat(AT_FDCWD, INJECT_FLAG, F_OK) != 0) {
        log_str("INJECT: flag not set, skipping");
        return;
    }

    if (g_injected) {
        log_str("INJECT: already injected, skipping");
        return;
    }

    log_str("=== INJECT START ===");

    if (!g_ew_new_obj) {
        log_str("INJECT: EwNewObjectIndirect not resolved");
        return;
    }
    if (!g_vthis_indexed_image_action) {
        log_str("INJECT: class descriptor not resolved");
        return;
    }

    /* Step 1: Create an IndexedImageActionMenuItem via EwNewObjectIndirect.
     * This allocates + calls __Init which chains up the class hierarchy. */
    log_str("INJECT: creating IndexedImageActionMenuItem...");
    void *new_item = g_ew_new_obj(g_vthis_indexed_image_action, 0);
    log_ptr("INJECT: new_item=", (unsigned long)new_item);

    if (!new_item) {
        log_str("INJECT: EwNewObjectIndirect returned NULL!");
        return;
    }

    /* Log the new item's vtable for debugging */
    void **item_vmt = *(void ***)new_item;
    log_ptr("INJECT: item vmt=", (unsigned long)item_vmt);

    /* Step 2: Try to set caption text.
     * Create EW string from ASCII, then call OnSetCaption. */
    if (g_ew_new_str && g_set_caption) {
        log_str("INJECT: setting caption...");
        void *label = g_ew_new_str("Natartos");
        log_ptr("INJECT: label str=", (unsigned long)label);
        if (label) {
            g_set_caption(new_item, label);
            log_str("INJECT: caption set OK");
        }
    } else {
        log_str("INJECT: caption funcs not available, skip label");
    }

    /* Step 3: Bind activation callback via EwNewSlot + OnSetOnActivate */
    if (g_ew_new_slot && g_on_set_activate) {
        log_str("INJECT: binding activation slot...");
        XSlot slot = g_ew_new_slot(new_item, (void *)natartos_on_activate);
        log_ptr("INJECT: slot.obj=", (unsigned long)slot.object);
        log_ptr("INJECT: slot.method=", (unsigned long)slot.method);
        g_on_set_activate(new_item, slot);
        log_str("INJECT: activation bound");
    } else {
        log_str("INJECT: slot/activate not resolved, no action binding");
    }

    /* Step 4: Add item to menu via vtable slot 60 (AddItem).
     * AddItem(self, item) internally does CoreGroup__AddBehind(self, item, self+0xb68). */
    void **menu_vmt = *(void ***)menu_self;
    typedef void (*add_item_fn)(void *self, void *item);
    add_item_fn add_item = (add_item_fn)menu_vmt[60];
    log_ptr("INJECT: AddItem fn=", (unsigned long)add_item);

    log_str("INJECT: calling AddItem...");
    add_item(menu_self, new_item);
    log_str("INJECT: AddItem returned");

    /* Step 5: Bump item count at +648 */
    int *count_ptr = (int *)((char *)menu_self + 648);
    int old_count = *count_ptr;
    (*count_ptr)++;
    log_int("INJECT: count ", old_count);
    log_int("INJECT: count now ", *count_ptr);

    g_injected = 1;
    log_str("=== INJECT DONE ===");
}

/* ---- Postreload hook ---- */

static void more_postreload_hook(void *self) {
    g_hook_fired++;
    log_str("=== MORE_POSTRELOAD_HOOK ===");
    log_ptr("  self=", (unsigned long)self);
    log_int("  fire#=", g_hook_fired);

    /* Call original first — it sets up focus, title, etc. */
    if (g_more_orig_postreload) {
        g_more_orig_postreload(self);
        log_str("  original returned");
    }

    /* Diagnostics: dump menu structure */
    void **vmt = *(void ***)self;
    log_ptr("  vmt=", (unsigned long)vmt);

    void *item_provider = *(void **)((char *)self + 592);
    log_ptr("  itemProvider(+592)=", (unsigned long)item_provider);

    int item_count = *(int *)((char *)self + 648);
    log_int("  itemCount(+648)=", item_count);

    /* Dump item provider vtable if available */
    if (item_provider) {
        void **prov_vmt = *(void ***)item_provider;
        log_ptr("  provider vmt=", (unsigned long)prov_vmt);
        log_str("  provider vtable slots 0-15:");
        for (int s = 0; s < 16; s++) {
            if (prov_vmt[s]) {
                log_ptr("    prov[", (unsigned long)s);
                log_ptr("    ]=", (unsigned long)prov_vmt[s]);
            }
        }
    }

    /* Dump key offsets */
    void *outline_9b0 = *(void **)((char *)self + 0x9b0);
    void *outline_ac0 = *(void **)((char *)self + 0xac0);
    void *subview_4f0 = *(void **)((char *)self + 0x4f0);
    void *group_b68 = (void *)((char *)self + 0xb68);
    log_ptr("  outline(+0x9b0)=", (unsigned long)outline_9b0);
    log_ptr("  outline(+0xac0)=", (unsigned long)outline_ac0);
    log_ptr("  subview(+0x4f0)=", (unsigned long)subview_4f0);
    log_ptr("  group(+0xb68)=", (unsigned long)group_b68);

    /* Dump existing items by walking CoreOutline at +0xac0.
     * CoreOutline stores views; try to read view pointers. */
    if (outline_ac0) {
        log_str("  existing items (from outline +0xac0):");
        /* CoreOutline: first child typically at +0x58 (after base CoreRectView fields) */
        /* Actually use CoreOutline_GetViewAtIndex if we have it */
    }

    /* Dump data source at +576 (0x240) — MxBindableAbstractMenu stores it there */
    void *datasource = *(void **)((char *)self + 576);
    log_ptr("  datasource(+576)=", (unsigned long)datasource);
    if (datasource) {
        void **ds_vmt = *(void ***)datasource;
        log_ptr("  ds vmt=", (unsigned long)ds_vmt);
    }

    /* Now try injection */
    try_inject_item(self);

    log_str("=== END MORE_POSTRELOAD ===");
}

/* ---- API compatibility ---- */

#define MAX_MENU_ITEMS 16
#define MENU_TARGET_SETTINGS 1
#define MENU_TARGET_MORE     2

static struct {
    const char *label;
    int target;
} g_items[MAX_MENU_ITEMS];
static int g_item_count = 0;

int natartos_menu_register(const char *label, int target) {
    if (!label || g_item_count >= MAX_MENU_ITEMS) return -1;
    log_str("menu_register:");
    log_str(label);
    g_items[g_item_count].label = label;
    g_items[g_item_count].target = target ? target : (MENU_TARGET_SETTINGS | MENU_TARGET_MORE);
    return g_item_count++;
}

/* ---- vtable patching ---- */

static int patch_vmt_slot(void **vmt, int slot, void *new_fn, void **out_orig) {
    if (!vmt || !vmt[slot]) return -1;

    *out_orig = vmt[slot];

    unsigned long page = (unsigned long)&vmt[slot] & ~(PAGE_SIZE - 1);
    long mp_ret = sys_mprotect((void *)page, PAGE_SIZE * 2, PROT_READ | PROT_WRITE);
    if (mp_ret < 0) {
        log_ptr("  mprotect FAIL=", (unsigned long)mp_ret);
        return -2;
    }

    vmt[slot] = new_fn;
    sys_mprotect((void *)page, PAGE_SIZE * 2, PROT_READ);
    return 0;
}

/* ---- Init ---- */

int natartos_mod_init(void *(*loader_dlsym)(void *, const char *)) {
    g_dlsym = loader_dlsym;
    if (!g_dlsym) {
        log_str("mod_init: no dlsym");
        return -1;
    }

    log_str("=== mod_menu v2 init ===");

    /* Resolve MoreMenu vtable */
    g_more_vmt = (void **)g_dlsym(RTLD_DEFAULT, "__vmt_DuThemedControlsMoreMenu");
    log_ptr("MoreMenu vmt=", (unsigned long)g_more_vmt);

    if (!g_more_vmt) {
        log_str("FATAL: MoreMenu vtable not found");
        return -2;
    }

    /* Resolve EW runtime functions */
    g_ew_new_obj = (ew_new_obj_fn)g_dlsym(RTLD_DEFAULT, "EwNewObjectIndirect");
    log_ptr("EwNewObjectIndirect=", (unsigned long)g_ew_new_obj);

    g_ew_new_str = (ew_new_str_fn)g_dlsym(RTLD_DEFAULT, "EwNewStringAnsi");
    log_ptr("EwNewStringAnsi=", (unsigned long)g_ew_new_str);

    g_ew_new_slot = (ew_new_slot_fn)g_dlsym(RTLD_DEFAULT, "EwNewSlot");
    log_ptr("EwNewSlot=", (unsigned long)g_ew_new_slot);

    /* Resolve item activation binding */
    g_on_set_activate = (onset_activate_fn)g_dlsym(RTLD_DEFAULT,
        "MxBindableMenuItem_OnSetOnActivate");
    log_ptr("OnSetOnActivate=", (unsigned long)g_on_set_activate);

    /* Resolve class descriptor for IndexedImageActionMenuItem */
    g_vthis_indexed_image_action = g_dlsym(RTLD_DEFAULT,
        "__vthis_DuThemedControlsIndexedImageActionMenuItem");
    log_ptr("vthis_IndexedImageAction=", (unsigned long)g_vthis_indexed_image_action);

    /* Resolve caption setter */
    g_set_caption = (onset_caption_fn)g_dlsym(RTLD_DEFAULT,
        "MxBindableMenuItem__OnSetCaption");
    log_ptr("OnSetCaption=", (unsigned long)g_set_caption);

    /* Resolve AddItemFromData */
    g_add_from_data = (add_from_data_fn)g_dlsym(RTLD_DEFAULT,
        "MxBindableAbstractMenu_AddItemFromData");
    log_ptr("AddItemFromData=", (unsigned long)g_add_from_data);

    /* Resolve CoreGroup__AddBehind */
    g_add_behind = (add_behind_fn)g_dlsym(RTLD_DEFAULT, "CoreGroup__AddBehind");
    log_ptr("AddBehind=", (unsigned long)g_add_behind);

    /* Resolve MenuItemProvider vtable for reference */
    void *prov_vmt = g_dlsym(RTLD_DEFAULT, "__vmt_DuThemedControlsMenuItemProvider");
    log_ptr("MenuItemProvider vmt=", (unsigned long)prov_vmt);

    /* Hook MoreMenu Postreload (slot 58) */
    int ret = patch_vmt_slot(g_more_vmt, 58,
        (void *)more_postreload_hook, (void **)&g_more_orig_postreload);
    log_ptr("hook slot58 ret=", (unsigned long)(ret + 10));
    log_ptr("  orig=", (unsigned long)g_more_orig_postreload);

    log_str("=== mod_menu v2 init done ===");
    return 1;
}
