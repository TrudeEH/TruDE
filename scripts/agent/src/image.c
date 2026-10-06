#include "seth.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/wait.h>

/* Never pass clipboard content to a shell. Only fixed MIME names are allowed. */
char *clipboard_image(void) {
    /* Missing clipboard tools must not delay ordinary terminal text pastes. */
    const char *path = getenv("PATH");
    char *paths = strdup(path ? path : "/usr/bin:/bin"), *save = NULL;
    int available = 0;
    for (char *dir = strtok_r(paths, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char *file = fmt("%s/wl-paste", dir);
        available |= access(file, X_OK) == 0;
        free(file);
    }
    free(paths);
    if (!available || !getenv("WAYLAND_DISPLAY"))
        return NULL;
    char *types[] = {"wl-paste", "--list-types", NULL};
    int code = 0;
    char *list = command(types, NULL, 2, 8192, &code);
    if (!list || code) {
        free(list);
        return NULL;
    }
    const char *mimes[] = {"image/png", "image/jpeg", "image/webp", "image/gif"};
    const char *mime = NULL;
    for (size_t i = 0; i < 4 && !mime; i++) {
        char *p = list;
        while (*p) {
            char *end = strchr(p, '\n');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n == strlen(mimes[i]) && !memcmp(p, mimes[i], n))
                mime = mimes[i];
            if (!end)
                break;
            p = end + 1;
        }
    }
    free(list);
    if (!mime)
        return NULL;
    char *script = fmt("wl-paste --no-newline --type %s | base64 -w 0", mime);
    char *args[] = {"sh", "-c", script, NULL};
    char *data = command(args, NULL, 5, 8 * LIMIT + 1, &code);
    free(script);
    if (!data || code || !*data || strlen(data) > 8 * LIMIT ||
        strspn(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != strlen(data)) {
        free(data);
        fail("Could not read clipboard image (maximum 12 MiB)");
        return strdup("");
    }
    char *url = fmt("data:%s;base64,%s", mime, data);
    free(data);
    return url;
}

/* Clipboard programs receive bytes on stdin, never shell interpolation. */
int clipboard_copy(const void *data, size_t length, const char *mime) {
    char *wayland[] = {"wl-copy", "--type", (char *)mime, NULL};
    char *x11[] = {"xclip", "-selection", "clipboard", "-target", (char *)mime, NULL};
    char **argv = getenv("WAYLAND_DISPLAY") ? wayland : getenv("DISPLAY") ? x11 : NULL;
    if (!argv)
        return fail("No graphical clipboard. Set WAYLAND_DISPLAY or DISPLAY.");
    Proc p;
    if (spawn(&p, argv, NULL, NULL))
        return fail("Clipboard unavailable. Install wl-clipboard (Wayland) or xclip (X11).");
    size_t sent = 0;
    double deadline = mono() + 5;
    int status = 0, exited = 0;
    while (mono() < deadline) {
        struct pollfd f[3] = {{p.in, POLLOUT, 0}, {p.out, POLLIN, 0}, {p.error, POLLIN, 0}};
        poll(f, 3, 20);
        if (p.in >= 0 && (f[0].revents & POLLOUT)) {
            ssize_t n = write(p.in, (const char *)data + sent, length - sent);
            if (n > 0) sent += (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
            if (sent == length) { close(p.in); p.in = -1; }
        }
        char discard[4096];
        if (f[1].revents) { ssize_t ignored = read(p.out, discard, sizeof discard); (void)ignored; }
        drain_error(&p);
        if (waitpid(p.pid, &status, WNOHANG) == p.pid) { exited = 1; break; }
    }
    if (exited) p.pid = 0;
    int ok = exited && sent == length && WIFEXITED(status) && !WEXITSTATUS(status);
    stopproc(&p);
    return ok ? 0 : fail("Clipboard copy failed or timed out.");
}
int clipboard_copy_image(const char *url) {
    const char *mimes[] = {"image/png", "image/jpeg", "image/webp", "image/gif"};
    const char *mime = NULL, *encoded = NULL;
    for (size_t i = 0; i < 4; i++) {
        char *prefix = fmt("data:%s;base64,", mimes[i]);
        if (!strncmp(url, prefix, strlen(prefix))) {
            mime = mimes[i]; encoded = url + strlen(prefix);
        }
        free(prefix);
    }
    if (!encoded) return fail("Only embedded PNG, JPEG, WebP and GIF images can be copied.");
    Buf decoded = {0};
    unsigned bits = 0;
    int count = 0;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (const char *p = encoded; *p && *p != '='; p++) {
        const char *digit = strchr(alphabet, *p);
        if (!digit) { bfree(&decoded); return fail("Invalid image encoding."); }
        bits = (bits << 6) | (unsigned)(digit - alphabet);
        count += 6;
        if (count >= 8) {
            count -= 8;
            char byte = (char)(bits >> count);
            bput(&decoded, &byte, 1);
        }
    }
    int result = clipboard_copy(decoded.s, decoded.n, mime);
    bfree(&decoded);
    return result;
}
