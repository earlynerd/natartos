#define _GNU_SOURCE
#include "package.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_NODES 512
#define MAX_DEPTH 8
#define PACKAGE_BYTES (64ULL * 1024 * 1024)
#define CACHE_BYTES (256ULL * 1024 * 1024)
#define LOG_BYTES (1024 * 1024)
#define CACHE_DEFAULT "/data/local/tmp/natartos-apps"
#define DATA_DEFAULT "/data/natartos/appdata"

static char error[512];
static volatile sig_atomic_t interrupted;
static int fail(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(error, sizeof(error), format, ap);
    va_end(ap);
    return -1;
}
static int syserror(const char *what) { return fail("%s: %s", what, strerror(errno)); }
static void on_signal(int sig) { interrupted = sig; }

static int directory_access(int parent, const char *name, int create, int access) {
    if (create && mkdirat(parent, name, 0700) && errno != EEXIST)
        return syserror("create directory");
    int fd = openat(parent, name, access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return syserror("open directory (links are not allowed)");
    return fd;
}
static int directory(int parent, const char *name, int create) {
    return directory_access(parent, name, create, O_RDONLY);
}

/* Walk absolute roots too: O_NOFOLLOW on just the final component is insufficient. */
static int root_directory(const char *path, int create, char resolved[PATH_MAX]) {
    if (*path != '/' || strlen(path) >= PATH_MAX) return fail("roots must be absolute paths");
    char copy[PATH_MAX];
    strcpy(copy, path);
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return syserror("open root");
    char *save, *part = strtok_r(copy, "/", &save);
    while (part) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            close(fd); return fail("dot components are not allowed in roots");
        }
        /* Ancestors require search access, not permission to list their contents. */
        int next = directory_access(fd, part, create, O_PATH);
        close(fd);
        if (next < 0) return -1;
        fd = next;
        part = strtok_r(NULL, "/", &save);
    }
    int readable = directory(fd, ".", 0);
    close(fd);
    if (readable < 0) return -1;
    fd = readable;
    struct stat st;
    if (fstat(fd, &st) || (create && (st.st_uid != geteuid() || (st.st_mode & 0022)))) {
        close(fd); return fail("storage root must be owned by this user and not writable by others");
    }
    if (!realpath(path, resolved)) { close(fd); return syserror("resolve root"); }
    return fd;
}

static int overlap(const char *a, const char *b) {
    size_t n = strlen(a);
    return !strncmp(a, b, n) && (b[n] == '/' || !b[n] || n == 1);
}

static int regular(int parent, const char *name, int flags, unsigned mode) {
    int fd = openat(parent, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, mode);
    if (fd < 0) return syserror("open file (links are not allowed)");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd); return fail("only regular files with one link are allowed");
    }
    return fd;
}

static int entry_open(int root, const char *path) {
    if (!app_path_valid(path)) return fail("unsafe relative path");
    char copy[APP_PATH_MAX];
    strcpy(copy, path);
    int fd = directory(root, ".", 0);
    if (fd < 0) return -1;
    char *save, *part = strtok_r(copy, "/", &save);
    for (;;) {
        char *next = strtok_r(NULL, "/", &save);
        int child = next ? directory(fd, part, 0) : regular(fd, part, O_RDONLY, 0);
        close(fd);
        if (child < 0 || !next) return child;
        fd = child; part = next;
    }
}

static int load_manifest(int root, const char *id, app_manifest *m) {
    int fd = regular(root, "app.json", O_RDONLY, 0);
    if (fd < 0) return -1;
    char text[APP_MANIFEST_MAX + 1];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof(text) && (n = read(fd, text + used, sizeof(text) - used)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd); return syserror("read manifest");
        }
        used += (size_t)n;
    }
    close(fd);
    if (app_manifest_parse(text, used, m, error, sizeof(error))) return -1;
    if (strcmp(id, m->id)) return fail("manifest id must match package directory");
    fd = entry_open(root, m->entry);
    if (fd < 0) return -1;
    int rc = app_elf_validate(fd, error, sizeof(error));
    close(fd);
    return rc;
}

static DIR *listing(int fd) {
    int copy = directory(fd, ".", 0);
    if (copy < 0) return NULL;
    DIR *d = fdopendir(copy);
    if (!d) { close(copy); syserror("read directory"); }
    return d;
}
static int compare_name(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}
static int write_all(int fd, const void *buffer, size_t length) {
    const unsigned char *p = buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return syserror("write staged file");
        p += n; length -= (size_t)n;
    }
    return 0;
}
static void hash_number(app_sha256 *hash, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned i=0; i<8; ++i) bytes[i]=(unsigned char)(value>>(8*i));
    app_sha256_update(hash, bytes, sizeof(bytes));
}

typedef struct {
    app_sha256 hash;
    uint64_t bytes;
    unsigned nodes;
} tree_state;

/* Hash a sorted, framed tree; optionally copy the same opened files. Never follow links. */
static int tree_walk(int source, int dest, const char *prefix, unsigned depth, tree_state *state) {
    if (depth > MAX_DEPTH) return fail("package directory depth exceeded");
    DIR *d = listing(source);
    if (!d) return -1;
    char **names = calloc(MAX_NODES, sizeof(*names));
    if (!names) { closedir(d); return fail("out of memory"); }
    unsigned count = 0;
    int rc = -1;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (count == MAX_NODES || !app_path_valid(e->d_name) || strchr(e->d_name, '/')) {
            fail("too many entries or non-portable package filename"); goto done;
        }
        names[count] = strdup(e->d_name);
        if (!names[count]) { fail("out of memory"); goto done; }
        ++count;
        errno = 0;
    }
    if (errno) { syserror("enumerate package"); goto done; }
    qsort(names, count, sizeof(*names), compare_name);
    for (unsigned i=0; i<count; ++i) {
        if (interrupted) { fail("interrupted"); goto done; }
        if (++state->nodes > MAX_NODES) { fail("package entry limit exceeded"); goto done; }
        char path[APP_PATH_MAX];
        int length = snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "", names[i]);
        if (length < 0 || (size_t)length >= sizeof(path)) { fail("package path too long"); goto done; }
        struct stat st;
        if (fstatat(source, names[i], &st, AT_SYMLINK_NOFOLLOW)) { syserror("stat package"); goto done; }
        unsigned char type = S_ISDIR(st.st_mode) ? 'D' : 'F';
        app_sha256_update(&state->hash, &type, 1);
        hash_number(&state->hash, (uint64_t)length);
        app_sha256_update(&state->hash, path, (size_t)length);
        if (S_ISDIR(st.st_mode)) {
            int src = directory(source, names[i], 0);
            int dst = dest < 0 ? -1 : directory(dest, names[i], 1);
            if (src < 0 || (dest >= 0 && dst < 0)) {
                if (src >= 0) close(src);
                if (dst >= 0) close(dst);
                goto done;
            }
            int result = tree_walk(src, dst, path, depth + 1, state);
            close(src); if (dst >= 0) close(dst);
            if (result) goto done;
        } else if (S_ISREG(st.st_mode)) {
            int src = regular(source, names[i], O_RDONLY, 0);
            if (src < 0) goto done;
            if (fstat(src, &st) || st.st_size < 0 || (uint64_t)st.st_size > PACKAGE_BYTES - state->bytes) {
                close(src); fail("package size limit exceeded"); goto done;
            }
            uint64_t expected = (uint64_t)st.st_size, copied = 0;
            hash_number(&state->hash, expected);
            state->bytes += expected;
            int dst = dest < 0 ? -1 : regular(dest, names[i], O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (dest >= 0 && dst < 0) { close(src); goto done; }
            unsigned char buffer[32768];
            ssize_t n;
            int failed = 0;
            for (;;) {
                if (interrupted) { fail("interrupted"); failed = 1; break; }
                n = read(src, buffer, sizeof(buffer));
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { syserror("read package file"); failed = 1; break; }
                if (!n) break;
                copied += (uint64_t)n;
                if (copied > expected) { fail("package changed while being read"); failed = 1; break; }
                app_sha256_update(&state->hash, buffer, (size_t)n);
                if (dst >= 0 && write_all(dst, buffer, (size_t)n)) { failed = 1; break; }
            }
            if (!failed && copied != expected) { fail("package changed while being read"); failed = 1; }
            if (dst >= 0) {
                if (!failed && fsync(dst)) { syserror("sync staged file"); failed = 1; }
                if (close(dst) && !failed) { syserror("close staged file"); failed = 1; }
            }
            close(src);
            if (failed) goto done;
        } else { fail("links and special files are not allowed in packages"); goto done; }
    }
    rc = 0;
    if (dest >= 0 && fsync(dest)) rc = syserror("sync staged directory");
done:
    for (unsigned i=0; i<count; ++i) free(names[i]);
    free(names); closedir(d);
    return rc;
}

static int fingerprint(int source, int dest, char digest[65], uint64_t *bytes) {
    tree_state state = {0};
    app_sha256_init(&state.hash);
    static const char domain[] = "natartos-sd-package-v1";
    app_sha256_update(&state.hash, domain, sizeof(domain));
    if (tree_walk(source, dest, "", 0, &state)) return -1;
    app_sha256_final(&state.hash, digest);
    if (bytes) *bytes = state.bytes;
    return 0;
}

/* Removal is called only beneath the locked, runner-owned cache, never on SD/data roots. */
static int remove_tree(int parent, const char *name, unsigned depth) {
    if (depth > MAX_DEPTH + 3) return fail("cache removal depth exceeded");
    int fd = directory(parent, name, 0);
    if (fd < 0) return -1;
    DIR *d = listing(fd);
    if (!d) { close(fd); return -1; }
    int rc = 0;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) { rc = syserror("stat cache entry"); break; }
        if (S_ISDIR(st.st_mode)) rc = remove_tree(fd, e->d_name, depth + 1);
        else if (unlinkat(fd, e->d_name, 0)) rc = syserror("remove cache file");
        if (rc) break;
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate cache");
    closedir(d); close(fd);
    if (!rc && unlinkat(parent, name, AT_REMOVEDIR)) rc = syserror("remove cache directory");
    return rc;
}
static int digest_name(const char *name) {
    if (strlen(name) != 64) return 0;
    for (; *name; ++name) if (!strchr("0123456789abcdef", *name)) return 0;
    return 1;
}
static int clean_app(int app, int all) {
    DIR *d = listing(app);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strncmp(e->d_name, ".partial-", 9) || (all && digest_name(e->d_name))) {
            if (remove_tree(app, e->d_name, 0)) { rc = -1; break; }
        }
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate app cache");
    closedir(d);
    return rc;
}

static int cache_usage(int root, unsigned depth, uint64_t *bytes, unsigned *nodes) {
    if (depth > MAX_DEPTH + 3) return fail("unexpected cache depth");
    DIR *d = listing(root);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    errno = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (++*nodes > 65536 || fstatat(root, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            rc = fail("cannot measure cache or cache entry limit exceeded"); break;
        }
        if (S_ISDIR(st.st_mode)) {
            int child = directory(root, e->d_name, 0);
            if (child < 0) { rc = -1; break; }
            rc = cache_usage(child, depth + 1, bytes, nodes);
            close(child);
        } else if (S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size <= CACHE_BYTES - *bytes) {
            *bytes += (uint64_t)st.st_size;
        } else rc = fail("cache is full or contains an unexpected file");
        if (rc) break;
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate cache usage");
    closedir(d);
    return rc;
}

static int stage(int source, int packages, int app, const char *id, char digest[65], app_manifest *manifest) {
    uint64_t bytes;
    if (load_manifest(source, id, manifest) || fingerprint(source, -1, digest, &bytes) || clean_app(app, 0)) return -1;
    struct stat st;
    if (!fstatat(app, digest, &st, AT_SYMLINK_NOFOLLOW)) {
        int cached = directory(app, digest, 0);
        if (cached < 0) return -1;
        char actual[65];
        int rc = fingerprint(cached, -1, actual, NULL) || load_manifest(cached, id, manifest);
        close(cached);
        if (rc) return -1;
        if (strcmp(digest, actual)) return fail("cached package was modified; clean this app before retrying");
        return 0;
    }
    if (errno != ENOENT) return syserror("check cache");
    uint64_t used = 0;
    unsigned nodes = 0;
    if (cache_usage(packages, 0, &used, &nodes)) return -1;
    if (bytes > CACHE_BYTES - used) return fail("cache budget exceeded; clean inactive app versions");
    struct statvfs fs;
    if (fstatvfs(app, &fs)) return syserror("measure free storage");
    if (!fs.f_frsize || fs.f_bavail < (bytes + 2*1024*1024) / fs.f_frsize + 1)
        return fail("insufficient internal storage (2 MiB reserve required)");
    char partial[64];
    snprintf(partial, sizeof(partial), ".partial-%ld", (long)getpid());
    if (mkdirat(app, partial, 0700)) return syserror("create staging directory");
    int dst = directory(app, partial, 0);
    int rc = -1;
    char copied[65], verified[65];
    if (dst < 0) goto done;
    if (fingerprint(source, dst, copied, NULL) || strcmp(digest, copied)) {
        if (!*error) fail("source changed during copy");
        goto done;
    }
    if (fingerprint(dst, -1, verified, NULL) || strcmp(digest, verified)) {
        if (!*error) fail("staged copy verification failed");
        goto done;
    }
    if (load_manifest(dst, id, manifest)) goto done;
    int entry = entry_open(dst, manifest->entry);
    if (entry < 0) goto done;
    int mode_result = fchmod(entry, 0700) || fsync(entry);
    close(entry);
    if (mode_result) { syserror("set executable permissions"); goto done; }
    if (fsync(dst) || renameat(app, partial, app, digest) || fsync(app)) {
        syserror("publish staged package"); goto done;
    }
    rc = 0;
done:
    if (dst >= 0) close(dst);
    if (rc) {
        char original[sizeof(error)];
        strcpy(original, error);
        remove_tree(app, partial, 0);
        strcpy(error, original);
    }
    return rc;
}

static int64_t milliseconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int run_app(int package_fd, const char *package, const char *data, app_manifest *m, int logs, unsigned timeout) {
    int executable_fd = entry_open(package_fd, m->entry);
    if (executable_fd < 0) return -1;
    int log = regular(logs, "last-run.log", O_WRONLY | O_CREAT, 0600);
    if (log < 0) { close(executable_fd); return -1; }
    if (ftruncate(log, 0)) { close(executable_fd); close(log); return syserror("truncate app log"); }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) { close(executable_fd); close(log); return syserror("create output pipe"); }
    pid_t parent = getpid(), pid = fork();
    if (pid < 0) { close(executable_fd); close(log); close(pipefd[0]); close(pipefd[1]); return syserror("start application"); }
    if (!pid) {
        /* Parent death cannot leave the direct app running with an unlocked cache. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || setpgid(0, 0)) _exit(126);
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        close(pipefd[0]); close(log);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        if (input > STDERR_FILENO) close(input);
        if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
        char executable[PATH_MAX], package_env[PATH_MAX + 32], data_env[PATH_MAX + 32];
        char id_env[APP_ID_MAX + 32];
        if (snprintf(executable, sizeof(executable), "%s/%s", package, m->entry) >= (int)sizeof(executable)) _exit(126);
        snprintf(package_env, sizeof(package_env), "NATARTOS_APP_PACKAGE=%s", package);
        snprintf(data_env, sizeof(data_env), "NATARTOS_APP_DATA=%s", data);
        snprintf(id_env, sizeof(id_env), "NATARTOS_APP_ID=%s", m->id);
#ifdef SD_APPS_HOST_TEST
        char *env[] = {"PATH=/usr/bin:/bin", "LANG=C", package_env, data_env, id_env, NULL};
#else
        char *env[] = {"PATH=/system/bin:/system/xbin", "LANG=C", package_env, data_env, id_env, NULL};
#endif
        char *argv[APP_ARGS_MAX + 2];
        argv[0] = executable;
        for (unsigned i=0; i<m->argc; ++i) argv[i+1] = m->args[i];
        argv[m->argc+1] = NULL;
        if (fchdir(package_fd)) { dprintf(2, "fchdir: %s\n", strerror(errno)); _exit(126); }
        /* Execute the opened staged inode, without resolving the absolute path again. */
        fexecve(executable_fd, argv, env);
        dprintf(2, "fexecve: %s\n", strerror(errno));
        _exit(127);
    }
    close(executable_fd);
    close(pipefd[1]);
    setpgid(pid, pid); /* Child also sets its group before exec; close the scheduling race. */
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    int status = 0, exited = 0, stopping = 0, result = 0, log_failed = 0;
    size_t logged = 0;
    int64_t start = milliseconds(), kill_at = 0;
    while (!exited) {
        int64_t now = milliseconds();
        if (!stopping && (interrupted || (timeout && now - start >= (int64_t)timeout*1000))) {
            result = interrupted ? 128 + interrupted : 124;
            kill(-pid, SIGTERM); stopping = 1; kill_at = now + 2000;
        }
        if (stopping && now >= kill_at) { kill(-pid, SIGKILL); kill_at = INT64_MAX; }
        struct pollfd p = {pipefd[0], POLLIN, 0};
        poll(&p, 1, 50);
        if (p.revents & (POLLHUP | POLLERR)) {
            struct timespec pause = {0, 50000000};
            nanosleep(&pause, NULL);
        }
        unsigned char buffer[4096];
        /* Limit each drain pass, including when an app writes continuously. */
        for (unsigned i=0; i<16; ++i) {
            ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
            if (n <= 0) break;
            size_t keep = (size_t)n;
            if (keep > LOG_BYTES - logged) keep = LOG_BYTES - logged;
            if (keep && write_all(log, buffer, keep)) {
                log_failed = 1;
                if (!stopping) { kill(-pid, SIGTERM); stopping = 1; kill_at = now + 2000; }
            }
            logged += keep;
        }
        siginfo_t child = {0};
        /* Reserve the leader PID until group cleanup, avoiding PID/PGID reuse. */
        int waited = waitid(P_PID, (id_t)pid, &child, WEXITED | WNOHANG | WNOWAIT);
        if (!waited && child.si_pid == pid) exited = 1;
        else if (waited < 0 && errno != EINTR) {
            syserror("wait for application"); log_failed = 1;
            kill(-pid, SIGKILL); exited = 1;
        }
    }
    /* Descendants do not get to keep using the package after their leader exits. */
    kill(-pid, SIGTERM);
    struct timespec grace = {0, 100000000};
    nanosleep(&grace, NULL);
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        syserror("reap application"); log_failed = 1; break;
    }
    /* Drain remaining output, but never wait for a detached process to close the pipe. */
    unsigned char tail[4096];
    for (unsigned i=0; i<256; ++i) {
        ssize_t n = read(pipefd[0], tail, sizeof(tail));
        if (n <= 0) break;
        size_t keep = (size_t)n;
        if (keep > LOG_BYTES - logged) keep = LOG_BYTES - logged;
        if (keep && write_all(log, tail, keep)) log_failed = 1;
        logged += keep;
    }
    close(pipefd[0]);
    if (fsync(log)) { syserror("sync app log"); log_failed = 1; }
    close(log);
    if (log_failed) return -1;
    if (!result) result = WIFEXITED(status) ? WEXITSTATUS(status) :
                         WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
    printf("exit=%d log_bytes=%zu\n", result, logged);
    return result;
}

static void json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static int list_apps(int apps) {
    DIR *d = listing(apps);
    if (!d) return -1;
    struct dirent *e;
    unsigned count = 0;
    int rc = 0;
    errno = 0;
    while ((e = readdir(d))) {
        if (!app_id_valid(e->d_name)) continue;
        if (++count > 128) { rc = fail("app catalog exceeds 128 entries"); break; }
        int root = directory(apps, e->d_name, 0);
        app_manifest m;
        char digest[65];
        if (root < 0 || load_manifest(root, e->d_name, &m) || fingerprint(root, -1, digest, NULL)) {
            fprintf(stderr, "skip %s: %s\n", e->d_name, error);
        } else {
            fputs("{\"id\":", stdout); json_string(m.id);
            fputs(",\"name\":", stdout); json_string(m.name);
            fputs(",\"version\":", stdout); json_string(m.version);
            fputs("}\n", stdout);
        }
        if (root >= 0) close(root);
        errno = 0;
    }
    if (!rc && errno) rc = syserror("enumerate apps");
    closedir(d);
    return rc;
}

static int discover(char result[PATH_MAX]) {
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts) return syserror("read mount table");
    struct mntent *m;
    unsigned matches = 0;
    while ((m = getmntent(mounts))) {
        if (strncmp(m->mnt_dir, "/mnt/media_rw/", 14) || !strcmp(m->mnt_dir, "/mnt/media_rw/emulated")) continue;
        if (strcmp(m->mnt_type, "exfat") && strcmp(m->mnt_type, "vfat")) continue;
        if (snprintf(result, PATH_MAX, "%s/natartos/apps", m->mnt_dir) >= PATH_MAX) {
            endmntent(mounts); return fail("SD mount path too long");
        }
        ++matches;
    }
    endmntent(mounts);
    if (matches != 1) return fail("expected one external FAT/exFAT mount; specify --apps explicitly");
    return 0;
}

static void usage(void) {
    puts("sd_app_runner [--apps ABS] [--cache ABS] [--data ABS] [--timeout SECONDS]\n"
         "              list | validate ID | stage ID | run ID | clean ID\n"
         "Defaults: external SD/natartos/apps; cache " CACHE_DEFAULT "; data " DATA_DEFAULT "\n"
         "Static ARM64 apps only. Host-test builds accept their build architecture.\n"
         "Exit: app status, 124 timeout, 125 runner error, 128+signal cancellation.");
}

int main(int argc, char **argv) {
    const char *apps_path = NULL, *cache_path = CACHE_DEFAULT, *data_path = DATA_DEFAULT;
    unsigned timeout = 0;
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i += 2) {
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        if (i+1 == argc) { usage(); return 125; }
        if (!strcmp(argv[i], "--apps")) apps_path = argv[i+1];
        else if (!strcmp(argv[i], "--cache")) cache_path = argv[i+1];
        else if (!strcmp(argv[i], "--data")) data_path = argv[i+1];
        else if (!strcmp(argv[i], "--timeout")) {
            char *end;
            errno = 0;
            unsigned long n = strtoul(argv[i+1], &end, 10);
            if (errno || !*argv[i+1] || *end || n > 86400 || *argv[i+1] == '-') { usage(); return 125; }
            timeout = (unsigned)n;
        } else { usage(); return 125; }
    }
    if (i >= argc) { usage(); return 125; }
    const char *action = argv[i++], *id = i < argc ? argv[i++] : NULL;
    if (i != argc || (strcmp(action, "list") && strcmp(action, "validate") &&
        strcmp(action, "stage") && strcmp(action, "run") && strcmp(action, "clean")) ||
        (!strcmp(action, "list") ? id != NULL : !id || !app_id_valid(id))) {
        usage(); return 125;
    }
    umask(0077);
    struct sigaction sa = {0};
    signal(SIGCHLD, SIG_DFL);
    sa.sa_handler = on_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    char discovered[PATH_MAX], apps_abs[PATH_MAX], cache_abs[PATH_MAX], data_abs[PATH_MAX];
    int apps = -1, source = -1, cache = -1, lock = -1, packages = -1, app = -1, data = -1, logs = -1, staged = -1;
    int rc = -1;
    app_manifest manifest;
    if (strcmp(action, "clean")) {
        if (!apps_path) {
            if (discover(discovered)) goto done;
            apps_path = discovered;
        }
        apps = root_directory(apps_path, 0, apps_abs);
        if (apps < 0) goto done;
        if (!strcmp(action, "list")) { rc = list_apps(apps); goto done; }
        source = directory(apps, id, 0);
        if (source < 0) goto done;
        if (!strcmp(action, "validate")) {
            char digest[65];
            uint64_t bytes;
            if (load_manifest(source, id, &manifest) || fingerprint(source, -1, digest, &bytes)) goto done;
            printf("valid id=%s sha256=%s bytes=%llu\n", id, digest, (unsigned long long)bytes);
            rc = 0; goto done;
        }
    }
    cache = root_directory(cache_path, 1, cache_abs);
    if (cache < 0) goto done;
    if (apps >= 0 && (overlap(apps_abs, cache_abs) || overlap(cache_abs, apps_abs))) {
        fail("source and cache roots must be separate"); goto done;
    }
    lock = regular(cache, ".lock", O_RDWR | O_CREAT, 0600);
    if (lock < 0) goto done;
    if (flock(lock, LOCK_EX | LOCK_NB)) { fail("another app runner is active"); goto done; }
    packages = directory(cache, "packages", 1);
    if (packages < 0) goto done;
    app = directory(packages, id, 1);
    if (app < 0) goto done;
    if (!strcmp(action, "clean")) { rc = clean_app(app, 1); goto done; }
    char digest[65];
    if (stage(source, packages, app, id, digest, &manifest)) goto done;
    char package[PATH_MAX];
    if (snprintf(package, sizeof(package), "%s/packages/%s/%s", cache_abs, id, digest) >= (int)sizeof(package)) {
        fail("cache path too long"); goto done;
    }
    if (!strcmp(action, "stage")) { puts(package); rc = 0; goto done; }
    data = root_directory(data_path, 1, data_abs);
    if (data < 0) goto done;
    if (overlap(cache_abs, data_abs) || overlap(data_abs, cache_abs) ||
        overlap(apps_abs, data_abs) || overlap(data_abs, apps_abs)) {
        fail("source, cache and data roots must be separate"); goto done;
    }
    int app_data = directory(data, id, 1);
    if (app_data < 0) goto done;
    close(app_data);
    char save[PATH_MAX];
    if (snprintf(save, sizeof(save), "%s/%s", data_abs, id) >= (int)sizeof(save)) { fail("data path too long"); goto done; }
    logs = directory(cache, "logs", 1);
    if (logs < 0) goto done;
    staged = directory(app, digest, 0);
    if (staged < 0) goto done;
    fflush(stdout);
    rc = run_app(staged, package, save, &manifest, logs, timeout);
done:
    if (rc < 0) fprintf(stderr, "sd_app_runner: %s\n", *error ? error : "operation failed");
    int fds[] = {staged, logs, data, app, packages, lock, cache, source, apps};
    for (unsigned k=0; k<sizeof(fds)/sizeof(*fds); ++k) if (fds[k] >= 0) close(fds[k]);
    return rc < 0 ? (interrupted ? 128 + interrupted : 125) :
           !rc && interrupted ? 128 + interrupted : rc;
}
