/*
 * mod_template.so — minimal natartos mod template.
 *
 * Copy this folder to mods/<yourmod>/, rename things, and build:
 *   aarch64-linux-gnu-gcc -shared -fPIC -O2 -nostdlib -I../../include \
 *     -Wl,-soname,mod_template.so -o mod_template.so mod.c
 *
 * A mod is an LD_PRELOAD .so that the loader injects into dji_glasses. It can
 *   (A) interpose libc functions to change behaviour, and/or
 *   (B) add an entry to the goggles' More menu.
 * It is self-contained: no libc, only raw ARM64 syscalls from natartos_common.h.
 */

#define NTLOG_PREFIX "template"
#include "natartos_common.h"

/* Runtime on/off switch. Your hook checks this flag file so the mod can be
 * toggled from the menu without reloading. */
static const char FLAG[] = "/data/local/tmp/natartos_template";

/* ---------------------------------------------------------------------------
 * (A) Interpose a libc function. Example: intercept open().
 *     Replace with the function your mod needs, or delete this block if your
 *     mod only adds a menu action.
 * ------------------------------------------------------------------------- */
int open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }

    /* Act only while the flag file exists; otherwise behave like stock. */
    if (natartos_flag_exists(FLAG)) {
        NTLOG(path ? path : "(null)");
        /* ... your logic here ... */
    }

    return (int)sys_openat(AT_FDCWD, path, flags, mode);
}

/* ---------------------------------------------------------------------------
 * (B) Add a More-menu entry (optional). The loader calls this on load and
 *     passes dlsym so you can reach the loader's menu API. Delete if unused.
 * ------------------------------------------------------------------------- */
int natartos_mod_register(void *(*dlsym_fn)(void *, const char *)) {
    natartos_menu_register_fn reg =
        (natartos_menu_register_fn)dlsym_fn((void *)0, "natartos_menu_register");
    if (reg) {
        natartos_menu_entry e = {
            .id       = "template",
            .name     = "Template",
            .subtitle = "Toggle example",
            .command  = "sh -c 'f=/data/local/tmp/natartos_template; "
                        "if [ -f $f ]; then rm -f $f; else touch $f; fi'",
            .priority = 500,
        };
        reg(&e);
    }
    return 0;
}
