#include "seth.h"
#include <string.h>
#include <unistd.h>

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
