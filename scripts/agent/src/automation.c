#include "seth.h"
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static pthread_mutex_t tzmutex = PTHREAD_MUTEX_INITIALIZER;
static int field(const char *text, int low, int high, int *bits) {
    char *copy = strdup(text), *save = NULL;
    int seen = 0;
    for (char *p = strtok_r(copy, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        int step = 1, start, end;
        char *slash = strchr(p, '/');
        if (slash) {
            *slash++ = 0;
            char *q;
            long n = strtol(slash, &q, 10);
            if (*q || n < 1 || n > high - low + 1) {
                free(copy);
                return fail("Invalid cron step");
            }
            step = (int)n;
        }
        if (!strcmp(p, "*")) {
            start = low;
            end = high;
        } else {
            char *q;
            start = strtol(p, &q, 10);
            if (q == p) {
                free(copy);
                return fail("Cron fields use numbers, *, ranges, lists and steps");
            }
            if (*q == '-') {
                char *z;
                end = strtol(q + 1, &z, 10);
                if (z == q + 1 || *z) {
                    free(copy);
                    return fail("Invalid cron range");
                }
            } else if (!*q)
                end = slash ? high : start;
            else {
                free(copy);
                return fail("Invalid cron field");
            }
        }
        if (start < low || end > high || end < start) {
            free(copy);
            return fail("Cron value outside %d–%d", low, high);
        }
        for (int n = start; n <= end; n += step)
            bits[n] = 1;
        seen = 1;
    }
    free(copy);
    return seen ? 0 : fail("Empty cron field");
}
char *next_run(const char *cron, const char *timezone, time_t from) {
    char *copy = strdup(cron), *save = NULL, *parts[6] = {0};
    int n = 0;
    for (char *p = strtok_r(copy, " \t", &save); p && n < 6; p = strtok_r(NULL, " \t", &save))
        parts[n++] = p;
    if (n != 5) {
        free(copy);
        fail("Use five cron fields: minute hour day month weekday");
        return NULL;
    }
    int min[60] = {0}, hour[24] = {0}, day[32] = {0}, month[13] = {0}, week[8] = {0};
    if (field(parts[0], 0, 59, min) || field(parts[1], 0, 23, hour) ||
        field(parts[2], 1, 31, day) || field(parts[3], 1, 12, month) ||
        field(parts[4], 0, 7, week)) {
        free(copy);
        return NULL;
    }
    int domwild = parts[2][0] == '*', dowwild = parts[4][0] == '*';
    week[0] |= week[7];
    free(copy);
    if (!timezone || !*timezone)
        timezone = "Etc/UTC";
    if (*timezone == '/' || strstr(timezone, "..")) {
        fail("Use an IANA timezone");
        return NULL;
    }
    char *path = fmt("/usr/share/zoneinfo/%s", timezone);
    struct stat st;
    int exists = !stat(path, &st) && S_ISREG(st.st_mode);
    free(path);
    if (!exists) {
        fail("Unknown IANA timezone: %s", timezone);
        return NULL;
    }
    pthread_mutex_lock(&tzmutex);
    const char *old = getenv("TZ");
    char *previous = old ? strdup(old) : NULL;
    setenv("TZ", timezone, 1);
    tzset();
    time_t first = from - (from % 60) + 60, found = 0;
    for (time_t t = first; t < first + 8LL * 366 * 86400; t += 60) {
        struct tm tm;
        localtime_r(&t, &tm);
        int d = day[tm.tm_mday], w = week[tm.tm_wday];
        int match = (domwild || dowwild) ? d && w : d || w;
        if (min[tm.tm_min] && hour[tm.tm_hour] && month[tm.tm_mon + 1] && match) {
            found = t;
            break;
        }
    }
    if (previous) {
        setenv("TZ", previous, 1);
        free(previous);
    } else
        unsetenv("TZ");
    tzset();
    pthread_mutex_unlock(&tzmutex);
    if (!found) {
        fail("Cron has no occurrence within eight years");
        return NULL;
    }
    char s[40];
    struct tm tm;
    gmtime_r(&found, &tm);
    strftime(s, sizeof s, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    return strdup(s);
}
static char *timezone_default(void) {
    char *s = readfile("/etc/timezone", 512);
    if (s) {
        s[strcspn(s, "\r\n")] = 0;
        return s;
    }
    err[0] = 0;
    char p[PATH_MAX];
    if (realpath("/etc/localtime", p)) {
        char *v = strstr(p, "/zoneinfo/");
        if (v)
            return strdup(v + 10);
    }
    return strdup("Etc/UTC");
}
J *native_tools(int background) {
    J *a = ja();
    const char *names[] = {"automations_list", "automations_create", "automations_update",
                           "automations_delete"},
               *descriptions[] = {"List scheduled tasks and their latest results.",
                                  "Schedule a recurring task using five cron fields. Changes "
                                  "require approval; read tools are allowed by default.",
                                  "Update a scheduled task. Requires approval.",
                                  "Delete a scheduled task. Requires approval."};
    for (int i = 0; i < (background ? 1 : 4); i++) {
        J *d = jo(), *f = jo(), *schema;
        if (i == 0)
            schema = jp("{\"type\":\"object\",\"properties\":{}}", NULL);
        else if (i == 3)
            schema = jp("{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},"
                        "\"required\":[\"id\"]}",
                        NULL);
        else {
            schema = jp(
                "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{"
                "\"type\":\"string\"},\"prompt\":{\"type\":\"string\"},\"cron\":{\"type\":"
                "\"string\"},\"timezone\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"},"
                "\"allowedTools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}",
                NULL);
            J *r = ja();
            if (i == 1) {
                jadd(r, js("name"));
                jadd(r, js("prompt"));
                jadd(r, js("cron"));
            } else
                jadd(r, js("id"));
            jset(schema, "required", r);
        }
        jset(d, "type", js("function"));
        jset(f, "name", js(names[i]));
        jset(f, "description", js(descriptions[i]));
        jset(f, "parameters", schema);
        jset(d, "function", f);
        jadd(a, d);
    }
    return a;
}
J *task_patch(const char *id, J *patch) {
    if (lock_store("tasks"))
        return NULL;
    J *all = tasks(), *out = NULL;
    for (size_t i = 0; all && i < all->len; i++)
        if (!strcmp(gs(all->v[i], "id"), id)) {
            J *task = all->v[i];
            for (size_t k = 0; k < patch->len; k++)
                jset(task, patch->v[k]->key, jc(patch->v[k]));
            if (!save_tasks(all))
                out = jc(task);
            break;
        }
    if (!out && !*err)
        fail("Task no longer exists");
    jf(all);
    unlock_store("tasks");
    return out;
}
int task_save(J *input, J *config, MCP *mcp, const char *id) {
    const char *fields[] = {"name", "prompt", "cron"};
    for (int k = 0; k < 3; k++)
        if (!*gs(input, fields[k]))
            return fail("%s is required", fields[k]);
    if (strlen(gs(input, "name")) > 120 || strlen(gs(input, "prompt")) > 12000)
        return fail("Name or prompt is too long");
    J *allowed = jg(input, "allowedTools");
    if (allowed && allowed->type != JARR)
        return fail("allowedTools must be an array");
    for (size_t i = 0; allowed && i < allowed->len; i++)
        if (!mcp_tool(mcp, jstr(allowed->v[i])))
            return fail("Allowed tool is not connected: %s", jstr(allowed->v[i]));
    if (jg(input, "enabled") && jg(input, "enabled")->type != JBOOL)
        return fail("enabled must be boolean");
    char *tz = *gs(input, "timezone") ? strdup(gs(input, "timezone")) : timezone_default();
    char *next = next_run(gs(input, "cron"), tz, time(NULL));
    if (!next) {
        free(tz);
        return -1;
    }
    if (!*gs(profile(config), "model")) {
        free(tz);
        free(next);
        return fail("Select a model before scheduling a task");
    }
    J *task = jc(input);
    jset(task, "timezone", js(tz));
    jset(task, "nextRun", js(next));
    free(tz);
    free(next);
    if (!allowed)
        jset(task, "allowedTools", ja());
    if (!jg(task, "enabled"))
        jset(task, "enabled", jb(1));
    jset(task, "workspace", js(gs(config, "workspace")));
    jset(task, "profile", js(gs(config, "profile")));
    jset(task, "model", js(gs(profile(config), "model")));
    if (id && *id) {
        J *saved = task_patch(id, task);
        jf(task);
        if (!saved)
            return -1;
        jf(saved);
        return 0;
    }
    if (lock_store("tasks")) {
        jf(task);
        return -1;
    }
    J *all = tasks();
    if (!all) {
        jf(task);
        unlock_store("tasks");
        return -1;
    }
    char *key = uuid();
    jset(task, "id", js(key));
    free(key);
    jset(task, "lastStatus", js("never run"));
    jadd(all, task);
    int r = save_tasks(all);
    jf(all);
    unlock_store("tasks");
    return r;
}
int task_delete(const char *id) {
    if (lock_store("tasks"))
        return -1;
    J *all = tasks();
    int r = -1;
    for (size_t i = 0; all && i < all->len; i++)
        if (!strcmp(gs(all->v[i], "id"), id)) {
            jremove(all, i);
            r = save_tasks(all);
            break;
        }
    if (r && !*err)
        fail("Task no longer exists");
    jf(all);
    unlock_store("tasks");
    return r;
}
char *native_call(const char *name, J *args, J *config, MCP *mcp) {
    err[0] = 0;
    if (!strcmp(name, "automations_list")) {
        J *all = tasks();
        if (!all)
            return NULL;
        char *s = jd(all, 1);
        jf(all);
        return s;
    }
    if (!strcmp(name, "automations_delete"))
        return task_delete(gs(args, "id")) ? NULL : strdup("Task deleted.");
    J *input = jc(args);
    const char *id = !strcmp(name, "automations_update") ? gs(args, "id") : NULL;
    if (id) {
        J *all = tasks(), *found = NULL;
        for (size_t i = 0; all && i < all->len; i++)
            if (!strcmp(gs(all->v[i], "id"), id))
                found = all->v[i];
        if (!found) {
            jf(all);
            jf(input);
            fail("Unknown task");
            return NULL;
        }
        J *combined = jc(found);
        for (size_t k = 0; k < args->len; k++)
            jset(combined, args->v[k]->key, jc(args->v[k]));
        jf(all);
        jf(input);
        input = combined;
    }
    int r = task_save(input, config, mcp, id);
    jf(input);
    return r ? NULL
             : strdup("Task saved. Enable the background timer in Automation for unattended "
                      "execution.");
}
int run_task(const char *id) {
    char *lockname = fmt("task-%s", id);
    if (lock_store(lockname)) {
        free(lockname);
        return -1;
    }
    J *all = tasks(), *task = NULL;
    for (size_t i = 0; all && i < all->len; i++)
        if (!strcmp(gs(all->v[i], "id"), id))
            task = jc(all->v[i]);
    jf(all);
    int r = 0;
    J *config = NULL, *chat = NULL;
    MCP m;
    memset(&m, 0, sizeof m);
    if (!task) {
        r = fail("Task no longer exists");
        goto done;
    }
    char *next = next_run(gs(task, "cron"), gs(task, "timezone"), time(NULL));
    if (!next) {
        r = -1;
        goto done;
    }
    J *patch = jo();
    char *t = now();
    jset(patch, "nextRun", js(next));
    jset(patch, "lastRun", js(t));
    jset(patch, "lastStatus", js("running"));
    jset(patch, "lastError", js(""));
    free(t);
    free(next);
    J *changed = task_patch(id, patch);
    jf(patch);
    jf(changed);
    if (!changed) {
        r = -1;
        goto done;
    }
    config = load_config();
    if (!config) {
        r = -1;
        goto done;
    }
    jset(config, "workspace", js(gs(task, "workspace")));
    jset(config, "profile", js(gs(task, "profile")));
    J *p = profile(config);
    if (!p) {
        r = fail("Task provider profile no longer exists");
        goto done;
    }
    jset(p, "model", js(gs(task, "model")));
    if (validate_config(config)) {
        r = -1;
        goto done;
    }
    chat = new_chat(config);
    char *title = fmt("Scheduled: %s", gs(task, "name"));
    jset(chat, "title", js(title));
    free(title);
    save_chat(chat);
    patch = jo();
    jset(patch, "lastChat", js(gs(chat, "id")));
    changed = task_patch(id, patch);
    jf(changed);
    jf(patch);
    mcp_init(&m, config);
    mcp_connect(&m);
    Agent a = {.config = config, .chat = chat, .mcp = &m, .background = task};
    r = agent_run(&a, gs(task, "prompt"), 0);
done:
    if (task) {
        char saved[2048];
        snprintf(saved, sizeof saved, "%s", err);
        J *finalpatch = jo();
        jset(finalpatch, "lastStatus", js(r ? "failed" : "completed"));
        jset(finalpatch, "lastError", js(r ? saved : ""));
        if (chat)
            jset(finalpatch, "lastChat", js(gs(chat, "id")));
        J *finalchanged = task_patch(id, finalpatch);
        jf(finalchanged);
        jf(finalpatch);
        snprintf(err, sizeof err, "%s", saved);
    }
    mcp_close(&m);
    jf(config);
    jf(chat);
    jf(task);
    unlock_store(lockname);
    free(lockname);
    return r;
}
int run_due(void) {
    if (lock_store("scheduler"))
        return -1;
    J *all = tasks();
    char *t = now();
    int result = 0;
    if (!all)
        result = -1;
    for (size_t i = 0; all && i < all->len && !cancelled; i++) {
        J *task = all->v[i];
        if (!gb(task, "enabled", 1) || strcmp(gs(task, "nextRun"), t) > 0)
            continue;
        err[0] = 0;
        int r = run_task(gs(task, "id"));
        printf("%s: %s\n", gs(task, "name"), r ? err : "completed");
        if (r)
            result = -1;
    }
    free(t);
    jf(all);
    unlock_store("scheduler");
    return result;
}
char *timer_status(void) {
    char *args[] = {"systemctl", "--user", "is-enabled", "dotfiles-agent.timer", NULL};
    int code;
    char *s = command(args, NULL, 3, 2000, &code);
    if (!s || code) {
        free(s);
        err[0] = 0;
        return strdup("disabled / unavailable");
    }
    s[strcspn(s, "\r\n")] = 0;
    return s;
}
int timer_change(int enabled) {
    if (enabled) {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        char *dir =
            xdg ? fmt("%s/systemd/user", xdg) : fmt("%s/.config/systemd/user", getenv("HOME"));
        if (mkdirs(dir, 0700)) {
            free(dir);
            return -1;
        }
        char *service = fmt("%s/dotfiles-agent.service", dir),
             *timer = fmt("%s/dotfiles-agent.timer", dir);
        Buf path = {0};
        for (const char *s = executable; *s; s++) {
            if (*s == '%')
                bs(&path, "%%");
            else if (*s == '"' || *s == '\\') {
                bs(&path, "\\");
                bput(&path, s, 1);
            } else
                bput(&path, s, 1);
        }
        char *unit = fmt(
            "[Unit]\nDescription=Seth scheduled tasks\n[Service]\nType=oneshot\nExecStart=\"%s\" "
            "--run-due\nEnvironment=PATH=%%h/.local/bin:/usr/local/bin:/usr/bin:/"
            "bin\nUMask=0077\nTimeoutStartSec=30min\n",
            path.s);
        bfree(&path);
        int r = private_text(service, unit);
        if (!r)
            r = private_text(
                timer,
                "[Unit]\nDescription=Check Seth scheduled tasks\n[Timer]\nOnCalendar=*-*-* "
                "*:*:00\nPersistent=true\nAccuracySec=1s\n[Install]\nWantedBy=timers.target\n");
        free(dir);
        free(service);
        free(timer);
        free(unit);
        if (r)
            return -1;
        char *args[] = {"systemctl", "--user", "daemon-reload", NULL};
        int code = 0;
        char *s = command(args, NULL, 5, 2000, &code);
        if (!s || code) {
            if (s)
                fail("systemd: %s", s);
            free(s);
            return -1;
        }
        free(s);
    }
    char *args[] = {
        "systemctl", "--user", enabled ? "enable" : "disable", "--now", "dotfiles-agent.timer",
        NULL};
    int code = 0;
    char *s = command(args, NULL, 5, 2000, &code);
    if (!s || code) {
        if (s)
            fail("systemd: %s", s);
        free(s);
        return -1;
    }
    free(s);
    return 0;
}
