#include "seth.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdarg.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
_Thread_local char err[2048];
atomic_int cancelled;
static atomic_int command_pid;
void cancel_command(void) {
    pid_t p = atomic_load(&command_pid);
    if (p > 0)
        kill(-p, SIGKILL);
}
char executable[PATH_MAX], config_dir[PATH_MAX], data_dir[PATH_MAX], state_dir[PATH_MAX];
int fail(const char *f, ...) {
    va_list a;
    va_start(a, f);
    vsnprintf(err, sizeof err, f, a);
    va_end(a);
    return -1;
}
void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (!b->s)
            exit(1);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
void bs(Buf *b, const char *s) {
    if (s)
        bput(b, s, strlen(s));
}
char *fmt(const char *f, ...) {
    char *s;
    va_list a;
    va_start(a, f);
    if (vasprintf(&s, f, a) < 0)
        exit(1);
    va_end(a);
    return s;
}
void bf(Buf *b, const char *f, ...) {
    char *s;
    va_list a;
    va_start(a, f);
    if (vasprintf(&s, f, a) < 0)
        exit(1);
    va_end(a);
    bs(b, s);
    free(s);
}
void bfree(Buf *b) {
    free(b->s);
    memset(b, 0, sizeof *b);
}
int writeall(int fd, const void *s, size_t n) {
    const char *p = s;
    double deadline = mono() + 10;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (cancelled || mono() > deadline)
                    return fail(cancelled ? "Stopped" : "Process input timed out");
                struct pollfd ready = {fd, POLLOUT, 0};
                poll(&ready, 1, 80);
                continue;
            }
            return fail("Write: %s", strerror(errno));
        }
        p += r;
        n -= r;
    }
    return 0;
}
char *readfile(const char *p, size_t max) {
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fail("%s: %s", p, strerror(errno));
        return NULL;
    }
    Buf b = {0};
    char s[8192];
    ssize_t n;
    while ((n = read(fd, s, sizeof s)) > 0) {
        if (b.n + (size_t)n > max) {
            close(fd);
            bfree(&b);
            fail("File exceeds %zu bytes", max);
            return NULL;
        }
        if (memchr(s, 0, n)) {
            close(fd);
            bfree(&b);
            fail("Binary data is unsupported");
            return NULL;
        }
        bput(&b, s, n);
    }
    close(fd);
    if (n < 0) {
        bfree(&b);
        fail("Read failed");
        return NULL;
    }
    return b.s ? b.s : strdup("");
}
int mkdirs(const char *path, mode_t mode) {
    char *p = strdup(path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, mode) && errno != EEXIST) {
                free(p);
                return fail("Create directory: %s", strerror(errno));
            }
            *s = '/';
        }
    int r = mkdir(p, mode);
    free(p);
    return r && errno != EEXIST ? fail("Create directory: %s", strerror(errno)) : 0;
}
char *uuid(void) {
    unsigned char b[16];
    if (getrandom(b, 16, 0) != 16) {
        int f = open("/dev/urandom", O_RDONLY);
        if (f < 0 || read(f, b, 16) != 16)
            exit(1);
        close(f);
    }
    b[6] = (b[6] & 15) | 64;
    b[8] = (b[8] & 63) | 128;
    return fmt("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
               b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
               b[15]);
}
char *now(void) {
    char s[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(s, sizeof s, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return strdup(s);
}
void trim_utf8(char *s, size_t limit) {
    if (strlen(s) <= limit)
        return;
    while (limit && ((unsigned char)s[limit] & 192) == 128)
        limit--;
    s[limit] = 0;
}
double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int private_text(const char *p, const char *s) {
    char *id = uuid(), *tmp = fmt("%s.%s.tmp", p, id);
    free(id);
    int f = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int r = f < 0 ? fail("Create %s: %s", p, strerror(errno)) : writeall(f, s, strlen(s));
    if (f >= 0) {
        if (!r && fsync(f))
            r = fail("Sync %s: %s", p, strerror(errno));
        close(f);
    }
    if (!r && rename(tmp, p))
        r = fail("Save %s: %s", p, strerror(errno));
    if (!r) {
        char *d = strdup(p), *slash = strrchr(d, '/');
        if (slash) {
            *slash = 0;
            f = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (f >= 0) {
                fsync(f);
                close(f);
            }
        }
        free(d);
    }
    unlink(tmp);
    free(tmp);
    return r;
}
int atomic_json(const char *p, J *j) {
    char *s = jd(j, 1);
    int r = private_text(p, s);
    free(s);
    return r;
}
J *readjson(const char *p, J *fallback) {
    char *s = readfile(p, 32 * LIMIT);
    if (!s) {
        if (errno == ENOENT) {
            err[0] = 0;
            return jc(fallback);
        }
        return NULL;
    }
    char *e = NULL;
    J *j = jp(s, &e);
    free(s);
    if (!j)
        fail("%s: %s", p, e ? e : "Invalid JSON");
    free(e);
    return j;
}
int contains(J *j, const char *s) {
    for (size_t i = 0; j && i < j->len; i++)
        if (!strcmp(jstr(ji(j, i)), s))
            return 1;
    return 0;
}
char *expand(const char *s) {
    Buf b = {0};
    for (const char *p = s; *p;) {
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (!end) {
                bfree(&b);
                fail("Unclosed environment reference");
                return NULL;
            }
            char *k = strndup(p + 2, end - p - 2);
            const char *v = getenv(k);
            if (!v) {
                fail("Missing environment variable %s", k);
                free(k);
                bfree(&b);
                return NULL;
            }
            bs(&b, v);
            free(k);
            p = end + 1;
        } else
            bput(&b, p++, 1);
    }
    return b.s ? b.s : strdup("");
}
int valid_url(const char *s, int endpoint) {
    if (strncmp(s, "http://", 7) && strncmp(s, "https://", 8))
        return fail("Use an HTTP(S) URL");
    const char *p = strstr(s, "://") + 3, *end = strpbrk(p, "/?#");
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (!n || memchr(p, '@', n) || strchr(s, '\r') || strchr(s, '\n') || strchr(s, ' '))
        return fail("Invalid URL or embedded credentials");
    if (endpoint && (strchr(p, '?') || strchr(p, '#')))
        return fail("Endpoint cannot include a query or fragment");
    return 0;
}
char *urlencode(const char *s) {
    Buf b = {0};
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-_.~", *p))
            bput(&b, (const char *)p, 1);
        else
            bf(&b, "%%%02X", *p);
    }
    return b.s ? b.s : strdup("");
}
int spawn(Proc *p, char *const argv[], char *const env[], const char *cwd) {
    int a[2], b[2], c[2];
    memset(p, 0, sizeof *p);
    p->in = p->out = p->error = -1;
    if (pipe2(a, O_CLOEXEC) || pipe2(b, O_CLOEXEC) || pipe2(c, O_CLOEXEC))
        return fail("Create process pipes: %s", strerror(errno));
    posix_spawn_file_actions_t act;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&act);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_adddup2(&act, a[0], 0);
    posix_spawn_file_actions_adddup2(&act, b[1], 1);
    posix_spawn_file_actions_adddup2(&act, c[1], 2);
    if (cwd)
        posix_spawn_file_actions_addchdir_np(&act, cwd);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attr, 0);
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &def);
    int r = posix_spawnp(&p->pid, argv[0], &act, &attr, argv, env ? env : environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&act);
    close(a[0]);
    close(b[1]);
    close(c[1]);
    if (r) {
        close(a[1]);
        close(b[0]);
        close(c[0]);
        p->pid = 0;
        return fail("Start %s: %s", argv[0], strerror(r));
    }
    p->in = a[1];
    p->out = b[0];
    p->error = c[0];
    fcntl(p->in, F_SETFL, O_NONBLOCK);
    fcntl(p->out, F_SETFL, O_NONBLOCK);
    fcntl(p->error, F_SETFL, O_NONBLOCK);
    return 0;
}
void stopproc(Proc *p) {
    if (p->pid > 0) {
        kill(-p->pid, SIGTERM);
        double end = mono() + .15;
        while (waitpid(p->pid, NULL, WNOHANG) == 0 && mono() < end) {
            struct timespec t = {0, 10000000};
            nanosleep(&t, NULL);
        }
        kill(-p->pid, SIGKILL);
        while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) {
        }
        p->pid = 0;
    }
    if (p->in >= 0)
        close(p->in);
    if (p->out >= 0)
        close(p->out);
    if (p->error >= 0)
        close(p->error);
    p->in = p->out = p->error = -1;
    bfree(&p->pending);
    bfree(&p->errors);
}
void drain_error(Proc *p) {
    char s[4096];
    ssize_t n;
    while ((n = read(p->error, s, sizeof s)) > 0) {
        bput(&p->errors, s, n);
        if (p->errors.n > 4096) {
            memmove(p->errors.s, p->errors.s + p->errors.n - 4096, 4096);
            p->errors.n = 4096;
            p->errors.s[4096] = 0;
        }
    }
}
char *proc_line(Proc *p, int ms) {
    double end = mono() + ms / 1000.;
    for (;;) {
        if (p->pending.s) {
            char *nl = strchr(p->pending.s, '\n');
            if (nl) {
                size_t len = nl - p->pending.s;
                char *s = strndup(p->pending.s, len);
                memmove(p->pending.s, nl + 1, p->pending.n - len);
                p->pending.n -= len + 1;
                return s;
            }
        }
        if (cancelled) {
            fail("Stopped");
            return NULL;
        }
        int left = (int)((end - mono()) * 1000);
        if (left <= 0) {
            fail("MCP request timed out");
            return NULL;
        }
        struct pollfd f[2] = {{p->out, POLLIN, 0}, {p->error, POLLIN, 0}};
        int r = poll(f, 2, left > 100 ? 100 : left);
        if (r < 0 && errno != EINTR) {
            fail("Poll: %s", strerror(errno));
            return NULL;
        }
        drain_error(p);
        if (f[0].revents) {
            char s[8192];
            ssize_t n = read(p->out, s, sizeof s);
            if (n > 0) {
                if (p->pending.n + (size_t)n > 8 * LIMIT) {
                    fail("MCP frame too large");
                    return NULL;
                }
                bput(&p->pending, s, n);
            } else if (!n) {
                fail("MCP server disconnected%s%s", p->errors.n ? ": " : "",
                     p->errors.s ? p->errors.s : "");
                return NULL;
            }
        }
    }
}
char *command(char *const argv[], const char *cwd, int timeout, size_t cap, int *code) {
    Proc p;
    if (spawn(&p, argv, NULL, cwd))
        return NULL;
    atomic_store(&command_pid, p.pid);
    close(p.in);
    p.in = -1;
    Buf out = {0};
    double end = mono() + timeout;
    int eof = 0;
    while (!eof) {
        if (cancelled || mono() > end) {
            fail(cancelled ? "Stopped" : "Command timed out");
            stopproc(&p);
            atomic_store(&command_pid, 0);
            bfree(&out);
            return NULL;
        }
        struct pollfd f[2] = {{p.out, POLLIN, 0}, {p.error, POLLIN, 0}};
        poll(f, 2, 100);
        char s[8192];
        for (int i = 0; i < 2; i++)
            if (f[i].revents) {
                ssize_t n = read(f[i].fd, s, sizeof s);
                if (n > 0) {
                    size_t keep = out.n < cap ? cap - out.n : 0;
                    if (keep > (size_t)n)
                        keep = n;
                    bput(&out, s, keep);
                } else if (!n && i == 0)
                    eof = 1;
            }
    }
    int status = 0;
    while (waitpid(p.pid, &status, WNOHANG) == 0) {
        if (cancelled || mono() > end) {
            fail(cancelled ? "Stopped" : "Command timed out");
            stopproc(&p);
            atomic_store(&command_pid, 0);
            bfree(&out);
            return NULL;
        }
        struct timespec wait = {0, 10000000};
        nanosleep(&wait, NULL);
    }
    p.pid = 0;
    atomic_store(&command_pid, 0);
    stopproc(&p);
    if (code)
        *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return out.s ? out.s : strdup("");
}
/* SHA-256 keeps public tool names compatible with existing saved chats. */
static uint32_t rr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static void hash(const unsigned char *p, size_t n, unsigned char out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t total = ((n + 9 + 63) / 64) * 64;
    unsigned char *b = calloc(1, total);
    memcpy(b, p, n);
    b[n] = 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++)
        b[total - 1 - i] = bits >> (i * 8);
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)b[off + i * 4] << 24 | (uint32_t)b[off + i * 4 + 1] << 16 |
                   (uint32_t)b[off + i * 4 + 2] << 8 | b[off + i * 4 + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
                   w[i - 7] + (rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = h[0], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], z = h[7], v = h[1];
        for (int i = 0; i < 64; i++) {
            uint32_t t = z + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] +
                         w[i],
                     u = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & v) ^ (a & c) ^ (v & c));
            z = g;
            g = f;
            f = e;
            e = d + t;
            d = c;
            c = v;
            v = a;
            a = t + u;
        }
        h[0] += a;
        h[1] += v;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += z;
    }
    free(b);
    for (int i = 0; i < 32; i++)
        out[i] = h[i / 4] >> (24 - 8 * (i % 4));
}
void alias(char out[64], const char *server, const char *tool) {
    char *base = fmt("%s__%s", server, tool);
    size_t n = strlen(base);
    if (n > 47)
        n = 47;
    for (size_t i = 0; i < n; i++)
        out[i] =
            isalnum((unsigned char)base[i]) || base[i] == '_' || base[i] == '-' ? base[i] : '_';
    out[n++] = '_';
    Buf b = {0};
    bs(&b, server);
    bput(&b, "\0", 1);
    bs(&b, tool);
    unsigned char h[32];
    hash((unsigned char *)b.s, b.n, h);
    for (int i = 0; i < 6; i++)
        snprintf(out + n + i * 2, 3, "%02x", h[i]);
    bfree(&b);
    free(base);
}
