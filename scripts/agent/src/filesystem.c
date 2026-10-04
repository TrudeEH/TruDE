#include "seth.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char root[PATH_MAX];
static int inside(const char *p) {
    size_t n = strlen(root);
    return !strcmp(root, "/") || (!strncmp(p, root, n) && (p[n] == 0 || p[n] == '/'));
}
static char *resolve(const char *p, int create) {
    if (!*root) {
        const char *w = getenv("AGENT_WORKSPACE");
        if (!realpath(w ? w : ".", root)) {
            fail("Workspace: %s", strerror(errno));
            return NULL;
        }
    }
    if (!p || !*p) {
        fail("Path is required");
        return NULL;
    }
    char *raw = *p == '/' ? strdup(p) : fmt("%s/%s", root, p);
    char real[PATH_MAX];
    if (realpath(raw, real)) {
        free(raw);
        if (!inside(real)) {
            fail("Path is outside the workspace");
            return NULL;
        }
        return strdup(real);
    }
    if (!create || errno != ENOENT) {
        fail("%s: %s", raw, strerror(errno));
        free(raw);
        return NULL;
    } /* Normalize each component and reject dangling links at every ancestor. */
    char *copy = strdup(raw), *save = NULL;
    Buf norm = {0};
    bs(&norm, "/");
    for (char *s = strtok_r(copy, "/", &save); s; s = strtok_r(NULL, "/", &save)) {
        if (!strcmp(s, "."))
            continue;
        if (!strcmp(s, "..")) {
            if (norm.n > 1) {
                char *slash = strrchr(norm.s, '/');
                if (slash == norm.s)
                    norm.n = 1;
                else
                    norm.n = slash - norm.s;
                norm.s[norm.n] = 0;
            }
            continue;
        }
        if (norm.n > 1)
            bs(&norm, "/");
        bs(&norm, s);
        struct stat st;
        if (!lstat(norm.s, &st) && S_ISLNK(st.st_mode) && !realpath(norm.s, real)) {
            free(copy);
            free(raw);
            bfree(&norm);
            fail("Dangling symlink is not writable");
            return NULL;
        }
    }
    free(copy);
    free(raw);
    char *candidate = strdup(norm.s);
    bfree(&norm);
    if (!inside(candidate)) {
        free(candidate);
        fail("Path is outside the workspace");
        return NULL;
    }
    char *ancestor = strdup(candidate);
    Buf tail = {0};
    while (!realpath(ancestor, real)) {
        if (errno != ENOENT) {
            fail("Resolve path: %s", strerror(errno));
            free(ancestor);
            free(candidate);
            bfree(&tail);
            return NULL;
        }
        char *slash = strrchr(ancestor, '/');
        if (!slash || slash == ancestor) {
            strcpy(ancestor, "/");
            break;
        }
        char *next = fmt("/%s%s", slash + 1, tail.s ? tail.s : "");
        bfree(&tail);
        bs(&tail, next);
        free(next);
        *slash = 0;
    }
    if (!realpath(ancestor, real) || !inside(real)) {
        fail("Symlink points outside the workspace");
        free(ancestor);
        free(candidate);
        bfree(&tail);
        return NULL;
    }
    char *out = fmt("%s%s", real, tail.s ? tail.s : "");
    free(ancestor);
    free(candidate);
    bfree(&tail);
    return out;
}
static J *result(const char *text, int error) {
    J *j = jo(), *a = ja(), *v = jo();
    jset(v, "type", js("text"));
    jset(v, "text", js(text));
    jadd(a, v);
    jset(j, "content", a);
    if (error)
        jset(j, "isError", jb(1));
    return j;
}
static char *checkpoint(const char *file) {
    struct stat st;
    char *text = NULL;
    int mode = 0600;
    if (!stat(file, &st)) {
        if (!S_ISREG(st.st_mode) || st.st_size > LIMIT) {
            fail("Only text files up to 2 MB can be edited");
            return NULL;
        }
        text = readfile(file, LIMIT);
        if (!text)
            return NULL;
        mode = st.st_mode & 0777;
    } else if (errno != ENOENT) {
        fail("Inspect file: %s", strerror(errno));
        return NULL;
    }
    J *j = jo();
    char *id = uuid(), *t = now();
    jset(j, "id", js(id));
    jset(j, "file", js(file));
    jset(j, "root", js(root));
    jset(j, "content", text ? js(text) : jnull());
    jset(j, "mode", jnum(mode));
    jset(j, "created", js(t));
    free(t);
    free(text);
    char *p = fmt("%s/checkpoints/%s.json", state_dir, id);
    int r = atomic_json(p, j);
    jf(j);
    free(p);
    if (r) {
        free(id);
        return NULL;
    }
    return id;
}
static int putfile(const char *file, const char *text, mode_t mode) {
    if (strlen(text) > LIMIT)
        return fail("Text exceeds 2 MB");
    char *p = strdup(file), *slash = strrchr(p, '/');
    if (slash)
        *slash = 0;
    int r = mkdirs(p, 0700);
    free(p);
    if (r)
        return -1;
    int f = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    if (f < 0)
        return fail("Write file: %s", strerror(errno));
    r = writeall(f, text, strlen(text));
    if (!r && fsync(f))
        r = fail("Sync file: %s", strerror(errno));
    close(f);
    return r;
}
static void search(Buf *b, const char *folder, const char *query, int content, int *visited,
                   int *found, int depth) {
    if (depth > 64 || cancelled || *visited > 10000 || *found >= 100)
        return;
    DIR *d = opendir(folder);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && *visited < 10000 && *found < 100 && !cancelled) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".git") ||
            !strcmp(e->d_name, "node_modules"))
            continue;
        (*visited)++;
        char *p = fmt("%s/%s", folder, e->d_name);
        struct stat st;
        if (lstat(p, &st) || S_ISLNK(st.st_mode)) {
            free(p);
            continue;
        }
        const char *relative = p + strlen(root);
        if (*relative == '/')
            relative++;
        if (!content && strstr(relative, query)) {
            bf(b, "%s\n", relative);
            (*found)++;
        }
        if (S_ISDIR(st.st_mode))
            search(b, p, query, content, visited, found, depth + 1);
        else if (content && S_ISREG(st.st_mode) && st.st_size <= 500000) {
            char *s = readfile(p, 500000);
            if (s) {
                int line = 1;
                for (char *q = s; *q && *found < 100; line++) {
                    char *nl = strchr(q, '\n');
                    if (nl)
                        *nl = 0;
                    if (strstr(q, query)) {
                        bf(b, "%s:%d: %.250s\n", relative, line, q);
                        (*found)++;
                    }
                    if (!nl)
                        break;
                    q = nl + 1;
                }
                free(s);
            }
        }
        free(p);
    }
    closedir(d);
}
static int cporder(const void *a, const void *b) {
    return strcmp(gs(*(J *const *)b, "created"), gs(*(J *const *)a, "created"));
}
J *fs_call(const char *name, J *a) {
    err[0] = 0;
    char *p = NULL, *text = NULL, *id = NULL;
    Buf b = {0};
    struct stat st;
    if (!strcmp(name, "list_checkpoints")) {
        char *dir = fmt("%s/checkpoints", state_dir);
        DIR *d = opendir(dir);
        J *rows = ja();
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strstr(e->d_name, ".json"))
                    continue;
                char *path = fmt("%s/%s", dir, e->d_name);
                J *j = readjson(path, NULL);
                free(path);
                if (j && !strcmp(gs(j, "root"), root))
                    jadd(rows, j);
                else
                    jf(j);
            }
            closedir(d);
        }
        free(dir);
        qsort(rows->v, rows->len, sizeof *rows->v, cporder);
        for (size_t i = 0; i < rows->len && i < 50; i++)
            bf(&b, "%s %s %s\n", gs(rows->v[i], "id"), gs(rows->v[i], "created"),
               gs(rows->v[i], "file"));
        jf(rows);
        err[0] = 0;
    } else if (!strcmp(name, "shell")) {
        const char *cmd = gs(a, "command");
        int timeout = gn(a, "timeout", 60), code = 0;
        if (!*cmd || timeout < 1 || timeout > 300) {
            fail("Command required; timeout must be 1–300 seconds");
            goto done;
        }
        char *args[] = {"/bin/sh", "-c", (char *)cmd, NULL};
        text = command(args, root, timeout, 24000, &code);
        if (text)
            bf(&b, "Exit %d\n%s", code, text);
    } else if (!strcmp(name, "restore_checkpoint")) {
        const char *key = gs(a, "id");
        if (strlen(key) != 36 || strspn(key, "0123456789abcdef-") != 36) {
            fail("Invalid checkpoint ID");
            goto done;
        }
        char *path = fmt("%s/checkpoints/%s.json", state_dir, key);
        J *saved = readjson(path, NULL);
        free(path);
        if (!saved || strcmp(gs(saved, "root"), root)) {
            jf(saved);
            fail("Checkpoint does not belong to this workspace");
            goto done;
        }
        p = resolve(gs(saved, "file"), 1);
        if (p)
            id = checkpoint(p);
        if (id) {
            J *v = jg(saved, "content");
            if (v && v->type == JNULL) {
                if (unlink(p) && errno != ENOENT)
                    fail("Restore: %s", strerror(errno));
            } else if (!putfile(p, jstr(v), (mode_t)gn(saved, "mode", 0600)))
                chmod(p, (mode_t)gn(saved, "mode", 0600));
            if (!*err)
                bf(&b, "Restored %s. Previous state: %s", p, id);
        }
        jf(saved);
    } else {
        const char *path = gs(a, "path");
        if (!*path && (!strcmp(name, "list_directory") || !strcmp(name, "search_files")))
            path = ".";
        p = resolve(path, !strcmp(name, "write_file"));
        if (!p)
            goto done;
        if (!strcmp(name, "list_directory")) {
            DIR *d = opendir(p);
            if (!d) {
                fail("List directory: %s", strerror(errno));
                goto done;
            }
            struct dirent *e;
            int count = 0;
            while ((e = readdir(d)) && count < 500) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                char *entry = fmt("%s/%s", p, e->d_name);
                if (!lstat(entry, &st))
                    bf(&b, "%s %s\n",
                       S_ISDIR(st.st_mode)   ? "dir "
                       : S_ISLNK(st.st_mode) ? "link"
                                             : "file",
                       e->d_name);
                free(entry);
                count++;
            }
            closedir(d);
        } else if (!strcmp(name, "search_files")) {
            if (!*gs(a, "query")) {
                fail("Query is required");
                goto done;
            }
            int visited = 0, found = 0;
            search(&b, p, gs(a, "query"), gb(a, "content", 0), &visited, &found, 0);
            err[0] = 0;
            if (!found)
                bs(&b, "No matches.");
        } else if (!strcmp(name, "file_info")) {
            if (stat(p, &st)) {
                fail("Inspect: %s", strerror(errno));
                goto done;
            }
            bf(&b, "{\"path\": ");
            J *s = js(p);
            char *raw = jd(s, 0);
            jf(s);
            bs(&b, raw);
            free(raw);
            bf(&b, ", \"bytes\": %lld, \"mode\": \"%o\", \"modified\": %lld, \"directory\": %s}",
               (long long)st.st_size, st.st_mode & 0777, (long long)st.st_mtime,
               S_ISDIR(st.st_mode) ? "true" : "false");
        } else if (!strcmp(name, "read_file")) {
            if (stat(p, &st) || !S_ISREG(st.st_mode)) {
                fail("Path is not a regular file");
                goto done;
            }
            text = readfile(p, LIMIT);
            if (!text)
                goto done;
            int start = gn(a, "start", 1), count = gn(a, "lines", 200);
            if (start < 1 || count < 1 || count > 1000) {
                fail("Invalid line range");
                goto done;
            }
            int line = 1;
            for (char *q = text;; line++) {
                char *nl = strchr(q, '\n');
                size_t n = nl ? (size_t)(nl - q) : strlen(q);
                if (line >= start && line < start + count && b.n < 16000) {
                    bf(&b, "%d: ", line);
                    bput(&b, q, n > 16000 - b.n ? 16000 - b.n : n);
                    bs(&b, "\n");
                }
                if (!nl)
                    break;
                q = nl + 1;
            }
            bf(&b, "[%d lines total]", line);
        } else if (!strcmp(name, "write_file") || !strcmp(name, "edit_file")) {
            if (!jg(a, !strcmp(name, "write_file") ? "content" : "new_text") ||
                jg(a, !strcmp(name, "write_file") ? "content" : "new_text")->type != JSTR) {
                fail("Text content is required");
                goto done;
            }
            if (!strcmp(name, "edit_file") || jg(a, "expected_content")) {
                text = readfile(p, LIMIT);
                if (!text)
                    goto done;
            }
            char *newtext = NULL;
            if (!strcmp(name, "edit_file")) {
                const char *old = gs(a, "old_text"), *replacement = gs(a, "new_text");
                char *hit = *old ? strstr(text, old) : NULL;
                if (!hit || strstr(hit + strlen(old), old)) {
                    fail("old_text must match exactly once");
                    goto done;
                }
                newtext = fmt("%.*s%s%s", (int)(hit - text), text, replacement, hit + strlen(old));
            } else {
                if (jg(a, "expected_content") && strcmp(text, gs(a, "expected_content"))) {
                    fail("File changed since it was read");
                    goto done;
                }
                newtext = strdup(gs(a, "content"));
            }
            if (strlen(newtext) > LIMIT) {
                free(newtext);
                fail("Text exceeds 2 MB");
                goto done;
            }
            id = checkpoint(p);
            if (id && !putfile(p, newtext, 0600))
                bf(&b, "%s %s. Checkpoint: %s", !strcmp(name, "edit_file") ? "Edited" : "Wrote", p,
                   id);
            free(newtext);
        } else
            fail("Unknown filesystem tool");
    }
done:;
    J *r = result(*err ? err : b.s ? b.s : "No entries.", !!*err);
    free(p);
    free(text);
    free(id);
    bfree(&b);
    return r;
}
static void tool(J *a, const char *name, const char *description, const char *schema, int safe) {
    J *j = jo(), *s = jp(schema, NULL), *an = jo();
    jset(j, "name", js(name));
    jset(j, "description", js(description));
    jset(j, "inputSchema", s);
    jset(an, "readOnlyHint", jb(safe));
    jset(an, "destructiveHint", jb(!safe));
    jset(j, "annotations", an);
    jadd(a, j);
}
J *fs_tools(void) {
    if (!*root) {
        const char *w = getenv("AGENT_WORKSPACE");
        if (!realpath(w ? w : ".", root)) {
            fail("Cannot resolve workspace");
            return ja();
        }
    }
    J *a = ja();
    tool(a, "list_directory", "List workspace directory entries.",
         "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"default\":\".\"}}}",
         1);
    tool(a, "read_file", "Read UTF-8 text, with optional line range.",
         "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"start\":{\"type\":"
         "\"integer\",\"minimum\":1},\"lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":"
         "1000}},\"required\":[\"path\"]}",
         1);
    tool(a, "file_info", "Inspect size, permissions and modification time.",
         "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":["
         "\"path\"]}",
         1);
    tool(a, "search_files", "Find names or literal content recursively; skips symlinks and .git.",
         "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"query\":{\"type\":"
         "\"string\"},\"content\":{\"type\":\"boolean\"}},\"required\":[\"query\"]}",
         1);
    tool(a, "write_file",
         "Write text with a checkpoint. expected_content prevents conflicting overwrites.",
         "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{"
         "\"type\":\"string\"},\"expected_content\":{\"type\":\"string\"}},\"required\":[\"path\","
         "\"content\"]}",
         0);
    tool(a, "edit_file", "Replace one exact occurrence of text, with a checkpoint.",
         "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{"
         "\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_"
         "text\",\"new_text\"]}",
         0);
    tool(a, "list_checkpoints", "List recent restore checkpoints in this workspace.",
         "{\"type\":\"object\",\"properties\":{}}", 1);
    tool(
        a, "restore_checkpoint", "Restore a file version; checkpoint the current version first.",
        "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        0);
    tool(a, "shell", "Run /bin/sh with the user's OS permissions; commands are not sandboxed.",
         "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout\":{"
         "\"type\":\"integer\",\"minimum\":1,\"maximum\":300}},\"required\":[\"command\"]}",
         0);
    return a;
}
