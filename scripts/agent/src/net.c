#include "seth.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static void option(Buf *b, const char *key, const char *value) {
    bf(b, "%s = \"", key);
    for (const char *p = value; *p; p++) {
        if (*p == '\\' || *p == '"')
            bput(b, "\\", 1);
        if (*p == '\n')
            bs(b, "\\n");
        else if (*p == '\r')
            bs(b, "\\r");
        else
            bput(b, p, 1);
    }
    bs(b, "\"\n");
}
static void options(Buf *b, const char *url, J *headers, int timeout) {
    option(b, "url", url);
    option(b, "proto", "=http,https");
    option(b, "proto-redir", "=http,https");
    bf(b, "max-time = %d\nconnect-timeout = 10\n", timeout);
    option(b, "user-agent", "Seth/" VERSION);
    for (size_t i = 0; headers && i < headers->len; i++) {
        J *v = headers->v[i];
        char *s = fmt("%s: %s", v->key, jstr(v));
        option(b, "header", s);
        free(s);
    }
}
static int temp(char path[64]) {
    strcpy(path, "/tmp/seth-XXXXXX");
    int f = mkstemp(path);
    if (f < 0)
        return fail("Temporary file: %s", strerror(errno));
    fcntl(f, F_SETFD, FD_CLOEXEC);
    return f;
}
void http_free(Http *h) {
    free(h->body);
    free(h->headers);
    free(h->effective);
    memset(h, 0, sizeof *h);
}
static int http_impl(const char *url, const char *method, J *headers, J *body, int timeout,
                     size_t cap, Chunk chunk, void *arg, Http *h, int ignore_cancel) {
    memset(h, 0, sizeof *h);
    if (valid_url(url, 0))
        return -1;
    char hp[64], bp[64] = {0};
    int f = temp(hp);
    if (f < 0)
        return -1;
    close(f);
    Buf cfg = {0};
    options(&cfg, url, headers, timeout);
    option(&cfg, "dump-header", hp);
    option(&cfg, "write-out", "%{stderr}\nSETH_EFFECTIVE:%{url_effective}\n");
    if (method)
        option(&cfg, "request", method);
    if (body) {
        f = temp(bp);
        if (f < 0) {
            unlink(hp);
            bfree(&cfg);
            return -1;
        }
        char *s = jd(body, 0);
        int r = writeall(f, s, strlen(s));
        free(s);
        close(f);
        if (r) {
            unlink(hp);
            unlink(bp);
            bfree(&cfg);
            return -1;
        }
        char *ref = fmt("@%s", bp);
        option(&cfg, "data-binary", ref);
        free(ref);
    }
    char *args[] = {"curl",         "--silent", "--show-error", "--no-buffer", "--location",
                    "--max-redirs", "5",        "--config",     "-",           NULL};
    Proc p;
    if (spawn(&p, args, NULL, NULL)) {
        unlink(hp);
        if (*bp)
            unlink(bp);
        bfree(&cfg);
        return -1;
    }
    int r = writeall(p.in, cfg.s, cfg.n);
    close(p.in);
    p.in = -1;
    bfree(&cfg);
    Buf out = {0};
    double end = mono() + timeout + 1;
    int eof = 0, status = 0;
    while (!r && !eof) {
        if ((!ignore_cancel && cancelled) || mono() > end) {
            r = fail(cancelled ? "Stopped" : "HTTP request timed out");
            break;
        }
        struct pollfd pollers[2] = {{p.out, POLLIN, 0}, {p.error, POLLIN, 0}};
        poll(pollers, 2, 80);
        drain_error(&p);
        if (pollers[0].revents) {
            char buf[8192];
            ssize_t n = read(p.out, buf, sizeof buf);
            if (n > 0) {
                if (out.n + (size_t)n > cap) {
                    r = fail("HTTP response exceeds %zu bytes", cap);
                    break;
                }
                bput(&out, buf, n);
                if (chunk)
                    chunk(buf, n, arg);
            } else if (!n)
                eof = 1;
        }
    }
    if (!r) {
        while (waitpid(p.pid, &status, WNOHANG) == 0) {
            if ((!ignore_cancel && cancelled) || mono() > end) {
                r = fail("HTTP request timed out");
                break;
            }
            struct timespec t = {0, 10000000};
            nanosleep(&t, NULL);
        }
        if (!r) {
            p.pid = 0;
            drain_error(&p);
            if (!WIFEXITED(status) || WEXITSTATUS(status))
                r = fail("HTTP transport: %s", p.errors.s ? p.errors.s : "curl failed");
        }
    }
    char *effective = p.errors.s ? strstr(p.errors.s, "SETH_EFFECTIVE:") : NULL;
    if (effective) {
        effective += 15;
        h->effective = strndup(effective, strcspn(effective, "\r\n"));
    }
    stopproc(&p);
    h->headers = readfile(hp, 65536);
    unlink(hp);
    if (*bp)
        unlink(bp);
    if (h->headers) {
        const char *q = h->headers;
        while ((q = strstr(q, "HTTP/"))) {
            if (q == h->headers || q[-1] == '\n') {
                const char *space = strchr(q, ' ');
                if (space)
                    h->status = atoi(space + 1);
            }
            q += 5;
        }
    }
    h->body = out.s ? out.s : strdup("");
    if (!h->effective)
        h->effective = strdup(url);
    if (!r && (h->status < 200 || h->status >= 300))
        r = fail("HTTP %d: %.900s", h->status, h->body);
    return r;
}
int http(const char *url, const char *method, J *headers, J *body, int timeout, size_t cap,
         Chunk chunk, void *arg, Http *h) {
    return http_impl(url, method, headers, body, timeout, cap, chunk, arg, h, 0);
}
int http_notify(const char *url, J *headers, J *body) {
    Http response = {0};
    int r = http_impl(url, "POST", headers, body, 2, 65536, NULL, NULL, &response, 1);
    http_free(&response);
    return r;
}
int http_close_session(const char *url, J *headers) {
    Http response = {0};
    int r = http_impl(url, "DELETE", headers, NULL, 2, 65536, NULL, NULL, &response, 1);
    http_free(&response);
    return r;
}
int http_stream(Proc *p, const char *url, J *headers, int timeout) {
    if (valid_url(url, 0))
        return -1;
    Buf cfg = {0};
    options(&cfg, url, headers, timeout);
    char *args[] = {"curl", "--silent", "--show-error", "--no-buffer", "--config", "-", NULL};
    if (spawn(p, args, NULL, NULL)) {
        bfree(&cfg);
        return -1;
    }
    int r = writeall(p->in, cfg.s, cfg.n);
    close(p->in);
    p->in = -1;
    bfree(&cfg);
    return r;
}
int http_listen(Proc *p, const char *url, J *headers, J *request, int timeout) {
    if (valid_url(url, 0))
        return -1;
    Buf cfg = {0};
    options(&cfg, url, headers, timeout);
    char *raw = jd(request, 0);
    option(&cfg, "data-binary", raw);
    free(raw);
    char *args[] = {"curl", "--silent", "--show-error", "--no-buffer", "--config", "-", NULL};
    if (spawn(p, args, NULL, NULL)) {
        bfree(&cfg);
        return -1;
    }
    int r = writeall(p->in, cfg.s, cfg.n);
    close(p->in);
    p->in = -1;
    bfree(&cfg);
    return r;
}
