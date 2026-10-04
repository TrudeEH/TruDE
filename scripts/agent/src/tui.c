#include "seth.h"
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* The terminal renderer and editor need only libc. All coordinates are cells;
 * text positions are UTF-8 byte offsets. No escape from remote text is emitted. */
enum { NORMAL, MUTED, ACCENT, SURFACE, SELECTED, BORDER, FOCUS, CODE, HOVER };
typedef struct {
    wchar_t c;
    unsigned char color, bold;
} Cell;
typedef struct {
    char *s;
    size_t pos;
    int multiline, secret;
} Editor;
typedef struct {
    int x, y, w, h, id, index;
} Hit;
typedef struct {
    char *s, *key;
    int color, bold;
} Line;
typedef struct {
    Line *v;
    int len;
} Lines;
typedef struct {
    char *key, *label;
    Editor e;
    int kind;
} Field;
enum { MESSAGE, FORM, CONFIRM, PICK, CHECKS };
enum {
    A_NONE,
    A_NEW,
    A_FIND,
    A_RENAME,
    A_DELETE,
    A_FORK,
    A_RETRY,
    A_EXPORT,
    A_EVENTS,
    A_ATTACH,
    A_MCP_ADD,
    A_MCP_EDIT,
    A_MCP_IMPORT,
    A_MCP_REMOVE,
    A_MCP_TOOLS,
    A_TASK_NEW,
    A_TASK_EDIT,
    A_TASK_DELETE,
    A_TASK_TOOLS,
    A_PROFILE,
    A_PROVIDER,
    A_MODEL,
    A_LIMITS,
    A_WORKSPACE,
    A_PROMPT,
    A_PERMISSION,
    A_TIMER,
    A_APPROVE,
    A_ELICIT
};
typedef struct Modal {
    int kind, action, selected, scroll, count, focus;
    char *title, *text, *id;
    Field fields[64];
    J *choices, *value;
    struct Modal *parent;
} Modal;
typedef struct {
    J *config, *chat, *history, *tasklist, *expanded, *jobresult;
    MCP mcp;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int busy, job, result, tab, focus, history_sel, server_sel, task_sel, chat_scroll,
        detail_scroll, following, quit, w, h, paste;
    atomic_int finished, dirty, approval;
    char *prompt, *notice, *progress, *username, *request_title, *request_text, *timer;
    J *request_params, *response;
    int request_kind;
    Buf partial, input, pasted;
    Editor composer;
    Modal *modal;
    Cell *cells, *old;
    Hit hits[256];
    int hitcount;
    Lines transcript;
    struct termios term;
} UI;
static UI *active;
static volatile sig_atomic_t resized, terminated;
static void signals(int sig) {
    if (sig == SIGWINCH)
        resized = 1;
    else {
        terminated = 1;
        cancelled = 1;
        cancel_command();
    }
}
static void es(Editor *e, const char *s) {
    free(e->s);
    e->s = strdup(s ? s : "");
    e->pos = strlen(e->s);
}
static size_t prev(const char *s, size_t pos) {
    if (!pos)
        return 0;
    pos--;
    while (pos && ((unsigned char)s[pos] & 192) == 128)
        pos--;
    return pos;
}
static size_t next(const char *s, size_t pos) {
    if (!s[pos])
        return pos;
    pos++;
    while (s[pos] && ((unsigned char)s[pos] & 192) == 128)
        pos++;
    return pos;
}
static void insert(Editor *e, const char *s, size_t n) {
    if (strlen(e->s) + n > LIMIT)
        return;
    char *v = fmt("%.*s%.*s%s", (int)e->pos, e->s, (int)n, s, e->s + e->pos);
    free(e->s);
    e->s = v;
    e->pos += n;
}
static void remove_text(Editor *e, size_t from, size_t to) {
    memmove(e->s + from, e->s + to, strlen(e->s + to) + 1);
    e->pos = from;
}
static size_t startline(Editor *e) {
    size_t p = e->pos;
    while (p && e->s[p - 1] != '\n')
        p--;
    return p;
}
static size_t endline(Editor *e) {
    size_t p = e->pos;
    while (e->s[p] && e->s[p] != '\n')
        p++;
    return p;
}
static void vertical(Editor *e, int direction) {
    size_t start = startline(e), col = 0;
    for (size_t p = start; p < e->pos; p = next(e->s, p))
        col++;
    size_t target;
    if (direction < 0) {
        if (!start)
            return;
        target = start - 1;
        while (target && e->s[target - 1] != '\n')
            target--;
    } else {
        target = endline(e);
        if (!e->s[target])
            return;
        target++;
    }
    for (size_t i = 0; i < col && e->s[target] && e->s[target] != '\n'; i++)
        target = next(e->s, target);
    e->pos = target;
}
static void editor_key(Editor *e, int key, const char *s) {
    if (s && *s) {
        insert(e, s, strlen(s));
        return;
    }
    switch (key) {
    case 127:
    case 8:
        remove_text(e, prev(e->s, e->pos), e->pos);
        break;
    case 1003:
        remove_text(e, e->pos, next(e->s, e->pos));
        break;
    case 1004:
        e->pos = prev(e->s, e->pos);
        break;
    case 1005:
        e->pos = next(e->s, e->pos);
        break;
    case 1006:
        vertical(e, -1);
        break;
    case 1007:
        vertical(e, 1);
        break;
    case 1:
    case 1008:
        e->pos = startline(e);
        break;
    case 5:
    case 1009:
        e->pos = endline(e);
        break;
    case 21:
        remove_text(e, startline(e), e->pos);
        break;
    case 11:
        remove_text(e, e->pos, endline(e));
        break;
    case 10:
    case 1010:
        if (e->multiline)
            insert(e, "\n", 1);
        break;
    }
}
static void lines_free(Lines *l) {
    for (int i = 0; i < l->len; i++) {
        free(l->v[i].s);
        free(l->v[i].key);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}
static void line(Lines *l, const char *s, int color, int bold, const char *key) {
    l->v = realloc(l->v, (l->len + 1) * sizeof *l->v);
    l->v[l->len++] = (Line){strdup(s), key ? strdup(key) : NULL, color, bold};
}
static void text_lines(Lines *l, const char *s, int color, int markdown) {
    char *copy = strdup(s);
    char *p = copy;
    int code = 0;
    for (;;) {
        char *end = strchr(p, '\n');
        if (end)
            *end = 0;
        if (markdown && !strncmp(p, "```", 3)) {
            code = !code;
            line(l, *p ? p : "", MUTED, 0, NULL);
        } else {
            int bold = markdown && p[0] == '#';
            const char *v = p;
            if (bold) {
                while (*v == '#')
                    v++;
                if (*v == ' ')
                    v++;
            }
            line(l, v, code ? CODE : color, bold, NULL);
        }
        if (!end)
            break;
        p = end + 1;
    }
    free(copy);
}
static void transcript(UI *u) {
    lines_free(&u->transcript);
    J *m = jg(u->chat, "messages"), *results = jo(), *waiting = jo(), *matched = jo();
    for (size_t i = 0; m && i < m->len; i++) {
        J *v = m->v[i], *calls = jg(v, "tool_calls");
        for (size_t k = 0; calls && k < calls->len; k++) {
            char *key = fmt("%s:%zu:%zu", gs(u->chat, "id"), i, k);
            jset(waiting, gs(calls->v[k], "id"), js(key));
            free(key);
        }
        if (!strcmp(gs(v, "role"), "tool")) {
            const char *key = gs(waiting, gs(v, "tool_call_id"));
            if (*key) {
                jset(results, key, js(gs(v, "content")));
                char index[32];
                snprintf(index, sizeof index, "%zu", i);
                jset(matched, index, jb(1));
                jdel(waiting, gs(v, "tool_call_id"));
            }
        }
    }
    int speaker = 0;
    for (size_t i = 0; m && i < m->len; i++) {
        J *v = m->v[i];
        const char *role = gs(v, "role"), *content = gs(v, "content");
        J *calls = jg(v, "tool_calls");
        if (!strcmp(role, "user")) {
            if (u->transcript.len)
                line(&u->transcript, "", NORMAL, 0, NULL);
            line(&u->transcript, u->username, ACCENT, 0, NULL);
            text_lines(&u->transcript, content, NORMAL, 0);
            speaker = 1;
        } else if (!strcmp(role, "assistant")) {
            if ((*content || (calls && calls->len)) && speaker != 2) {
                if (u->transcript.len)
                    line(&u->transcript, "", NORMAL, 0, NULL);
                line(&u->transcript, "Seth", ACCENT, 0, NULL);
            } else if (*content && speaker == 2)
                line(&u->transcript, "", NORMAL, 0, NULL);
            if (*content) {
                char *trim = strdup(content);
                size_t n = strlen(trim);
                while (n && (trim[n - 1] == '\n' || trim[n - 1] == '\r'))
                    trim[--n] = 0;
                text_lines(&u->transcript, trim, NORMAL, 1);
                free(trim);
            }
            if (*content || (calls && calls->len))
                speaker = 2;
            for (size_t k = 0; calls && k < calls->len; k++) {
                J *call = calls->v[k], *f = jg(call, "function");
                char *key = fmt("%s:%zu:%zu", gs(u->chat, "id"), i, k);
                J *r = jg(results, key);
                const char *result = jstr(r), *status = !r ? "pending"
                                                        : !strncmp(result, "Tool denied", 11)
                                                            ? "denied"
                                                        : !strncmp(result, "Tool error:", 11) ||
                                                                !strncmp(result, "Interrupted", 11)
                                                            ? "failed"
                                                            : "done";
                Tool *t = mcp_tool(&u->mcp, gs(f, "name"));
                int expanded = gb(u->expanded, key, 0);
                char *label =
                    t ? fmt("%s %s / %s · %s", expanded ? "▼" : "▶", t->server, t->name, status)
                      : fmt("%s %s · %s", expanded ? "▼" : "▶", gs(f, "name"), status);
                line(&u->transcript, label, MUTED, 0, key);
                free(label);
                if (expanded) {
                    line(&u->transcript, "  Arguments", MUTED, 0, NULL);
                    J *args = jp(gs(f, "arguments"), NULL);
                    char *raw = args ? jd(args, 1) : strdup(gs(f, "arguments"));
                    jf(args);
                    text_lines(&u->transcript, raw, MUTED, 0);
                    free(raw);
                    line(&u->transcript, "  Result", MUTED, 0, NULL);
                    text_lines(&u->transcript, r ? result : "Awaiting result…", MUTED, 0);
                }
                free(key);
            }
        } else if (!strcmp(role, "tool")) {
            char index[32];
            snprintf(index, sizeof index, "%zu", i);
            if (!jg(matched, index)) {
                line(&u->transcript, "▶ Tool result", MUTED, 0, NULL);
                text_lines(&u->transcript, content, MUTED, 0);
            }
        }
    }
    if (u->partial.n) {
        if (speaker != 2) {
            line(&u->transcript, "", NORMAL, 0, NULL);
            line(&u->transcript, "Seth", ACCENT, 0, NULL);
        } else
            line(&u->transcript, "", NORMAL, 0, NULL);
        text_lines(&u->transcript, u->partial.s, NORMAL, 1);
    }
    if (*gs(u->chat, "summary")) {
        line(&u->transcript, "", NORMAL, 0, NULL);
        line(&u->transcript, "Context summary", ACCENT, 0, NULL);
        text_lines(&u->transcript, gs(u->chat, "summary"), MUTED, 0);
    }
    if (!u->transcript.len)
        text_lines(&u->transcript,
                   "Seth · your local assistant\n\nChoose a provider and model in Settings, then "
                   "send a message.\n\nFiles and shell commands use the workspace in "
                   "Settings.\nWeb search and file tools are enabled by default.",
                   NORMAL, 0);
    jf(results);
    jf(waiting);
    jf(matched);
}
static void cell(UI *u, int x, int y, wchar_t c, int color, int bold) {
    if (x < 0 || y < 0 || x >= u->w || y >= u->h)
        return;
    int width = wcwidth(c);
    if (width < 0)
        c = L'�', width = 1;
    u->cells[y * u->w + x] = (Cell){c, (unsigned char)color, (unsigned char)bold};
    if (width == 2 && x + 1 < u->w)
        u->cells[y * u->w + x + 1] = (Cell){0, (unsigned char)color, (unsigned char)bold};
}
static int draw_text(UI *u, int x, int y, int width, const char *s, int color, int bold,
                     int markdown) {
    mbstate_t st = {0};
    int used = 0;
    while (*s && used < width) {
        if (markdown && s[0] == '*' && s[1] == '*') {
            bold = !bold;
            s += 2;
            continue;
        }
        if (markdown && *s == '`') {
            s++;
            continue;
        }
        wchar_t c;
        size_t n = mbrtowc(&c, s, strlen(s), &st);
        if (n == (size_t)-1 || n == (size_t)-2) {
            memset(&st, 0, sizeof st);
            c = L'�';
            n = 1;
        }
        if (!n)
            break;
        if (c < 32 || c == 127)
            c = L' ';
        int w = wcwidth(c);
        if (w < 1) {
            s += n;
            continue;
        }
        if (used + w > width)
            break;
        cell(u, x + used, y, c, color, bold);
        used += w;
        s += n;
    }
    return used;
}
static void box(UI *u, int x, int y, int w, int h, const char *label, int focused) {
    if (w < 2 || h < 2)
        return;
    int c = focused ? FOCUS : BORDER;
    for (int i = 1; i < w - 1; i++) {
        cell(u, x + i, y, L'─', c, 0);
        cell(u, x + i, y + h - 1, L'─', c, 0);
    }
    for (int i = 1; i < h - 1; i++) {
        cell(u, x, y + i, L'│', c, 0);
        cell(u, x + w - 1, y + i, L'│', c, 0);
    }
    cell(u, x, y, L'┌', c, 0);
    cell(u, x + w - 1, y, L'┐', c, 0);
    cell(u, x, y + h - 1, L'└', c, 0);
    cell(u, x + w - 1, y + h - 1, L'┘', c, 0);
    char *s = fmt(" %s ", label);
    draw_text(u, x + 2, y, w - 4, s, NORMAL, 0, 0);
    free(s);
}
static void hit(UI *u, int x, int y, int w, int h, int id, int index) {
    if (u->hitcount < 256)
        u->hits[u->hitcount++] = (Hit){x, y, w, h, id, index};
}
static void fill(UI *u, int x, int y, int w, int h, int color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            cell(u, x + i, y + j, L' ', color, 0);
}
static void button(UI *u, int x, int y, const char *label, int id) {
    int w = (int)strlen(label) + 4;
    if (x + w > u->w - 1)
        return;
    int color = u->focus == id ? HOVER : SELECTED;
    fill(u, x, y, w, 1, color);
    draw_text(u, x + 2, y, w - 4, label, color, 0, 0);
    hit(u, x, y, w, 1, id, 0);
}
static void actions(UI *u, int x, int y, int width, const char *const *labels, const int *ids,
                    int count) {
    box(u, x, y, width, 3, "Actions", 0);
    int left = x + 2;
    for (int i = 0; i < count; i++) {
        button(u, left, y + 1, labels[i], ids[i]);
        left += (int)strlen(labels[i]) + 6;
    }
}
static Lines wrap(Lines *source, int width) {
    Lines out = {0};
    if (width < 1)
        return out;
    for (int i = 0; i < source->len; i++) {
        Line *l = &source->v[i];
        const char *p = l->s;
        if (!*p) {
            line(&out, "", l->color, l->bold, l->key);
            continue;
        }
        while (*p) {
            mbstate_t st = {0};
            const char *end = p;
            int cols = 0;
            while (*end) {
                wchar_t c;
                size_t n = mbrtowc(&c, end, strlen(end), &st);
                if (n == (size_t)-1 || n == (size_t)-2) {
                    memset(&st, 0, sizeof st);
                    n = 1;
                    c = L'�';
                }
                if (!n)
                    break;
                int w = wcwidth(c);
                if (w < 1)
                    w = 1;
                if (cols + w > width)
                    break;
                cols += w;
                end += n;
            }
            if (end == p)
                end = p + 1;
            char *part = strndup(p, end - p);
            line(&out, part, l->color, l->bold, l->key);
            free(part);
            p = end;
        }
    }
    return out;
}
static void view_lines(UI *u, Lines *l, int x, int y, int width, int height, int *scroll,
                       int follow, int tools) {
    Lines w = wrap(l, width - 1);
    int max = w.len - height;
    if (max < 0)
        max = 0;
    if (follow)
        *scroll = max;
    if (*scroll > max)
        *scroll = max;
    if (*scroll < 0)
        *scroll = 0;
    for (int r = 0; r < height && r + *scroll < w.len; r++) {
        Line *v = &w.v[r + *scroll];
        draw_text(u, x, y + r, width - 1, v->s, v->color, v->bold, v->color == NORMAL);
        if (tools && v->key) { /* key is represented by index in the unwrapped transcript. */
            for (int i = 0; i < l->len; i++)
                if (l->v[i].key && !strcmp(l->v[i].key, v->key)) {
                    hit(u, x, y + r, width - 1, 1, 3000, i);
                    break;
                }
        }
    }
    if (max) {
        int marker = *scroll * (height - 1) / (max ? max : 1);
        for (int r = 0; r < height; r++)
            cell(u, x + width - 1, y + r, r == marker ? L'█' : L'│', MUTED, 0);
    }
    lines_free(&w);
}
static void editor_draw(UI *u, Editor *e, int x, int y, int width, int height, int focused) {
    Lines l = {0};
    text_lines(&l, e->secret ? "(hidden — type to replace)" : e->s, NORMAL, 0);
    Lines w = wrap(&l, width);
    int cursorrow = 0, cursorcol = 0;
    mbstate_t st = {0};
    for (size_t p = 0; p < e->pos;) {
        wchar_t c;
        size_t n = mbrtowc(&c, e->s + p, strlen(e->s + p), &st);
        if (n == (size_t)-1 || n == (size_t)-2) {
            n = 1;
            c = L'�';
            memset(&st, 0, sizeof st);
        }
        if (c == '\n') {
            cursorrow++;
            cursorcol = 0;
        } else {
            int cw = wcwidth(c);
            if (cw < 1)
                cw = 1;
            if (cursorcol + cw > width) {
                cursorrow++;
                cursorcol = 0;
            }
            cursorcol += cw;
            if (cursorcol >= width && p + n < e->pos) {
                cursorrow++;
                cursorcol = 0;
            }
        }
        p += n;
    }
    int first = cursorrow >= height ? cursorrow - height + 1 : 0;
    for (int i = 0; i < height && i + first < w.len; i++)
        draw_text(u, x, y + i, width, w.v[i + first].s, NORMAL, 0, 0);
    if (focused) {
        if (cursorcol >= width) {
            cursorcol = 0;
            cursorrow++;
        }
        int cy = cursorrow - first;
        if (cy >= 0 && cy < height) {
            Cell *c = &u->cells[(y + cy) * u->w + x + cursorcol];
            c->color = SELECTED;
        }
    }
    lines_free(&l);
    lines_free(&w);
}
static void modal_free(Modal *m) {
    if (!m)
        return;
    for (int i = 0; i < m->count; i++) {
        free(m->fields[i].key);
        free(m->fields[i].label);
        free(m->fields[i].e.s);
    }
    free(m->title);
    free(m->text);
    free(m->id);
    jf(m->choices);
    jf(m->value);
    free(m);
}
static void close_modal(UI *u) {
    Modal *m = u->modal;
    if (!m)
        return;
    u->modal = m->parent;
    modal_free(m);
    u->dirty = 1;
}
static Modal *modal(UI *u, int kind, int action, const char *title, const char *text) {
    Modal *m = calloc(1, sizeof *m);
    m->kind = kind;
    m->action = action;
    m->title = strdup(title);
    m->text = strdup(text ? text : "");
    m->parent = u->modal;
    u->modal = m;
    u->dirty = 1;
    return m;
}
static void field_add(Modal *m, const char *key, const char *label, const char *value,
                      int multiline, int kind) {
    if (m->count >= 64)
        return;
    Field *f = &m->fields[m->count++];
    f->key = strdup(key);
    f->label = strdup(label);
    f->e.multiline = multiline;
    es(&f->e, value);
    f->kind = kind;
}
static void message(UI *u, const char *title, const char *text) {
    modal(u, MESSAGE, A_NONE, title, text);
}
static void notice_ui(UI *u, const char *text) {
    free(u->notice);
    u->notice = strdup(text);
    u->dirty = 1;
}
static void error_ui(UI *u) {
    message(u, "Could not complete action", *err ? err : "Unknown error");
    err[0] = 0;
}
static void help(UI *u) {
    if (u->modal && u->modal->action == 999) {
        close_modal(u);
        return;
    }
    if (u->modal)
        return;
    Modal *m =
        modal(u, MESSAGE, 999, "Chat help",
              "Enter sends · Shift+Enter adds a line · Ctrl+J adds a line\nCtrl+S sends · Tab / "
              "Shift+Tab move focus · Ctrl+L compose\nEscape stops or closes a dialog · Ctrl+Q "
              "quits\nF1–F4 views · F5 help · ? help outside text or empty composer\nCtrl+N "
              "creates a chat · click tool calls to open their details\nMouse wheel / PgUp / PgDn "
              "scroll the conversation\n\n/new        Start a new chat\n/fork       Branch this "
              "conversation\n/continue   Continue the current task\n/retry      Branch before the "
              "last request and retry\n/compact    Summarize old context, retaining full "
              "history\n/attach PATH  Add a workspace text file through MCP\n/export     Export "
              "the complete transcript as Markdown\n/events     Show activity and errors\n/help    "
              "   Open this help\n\nHistory: one click opens a saved chat. Find, Rename and "
              "Delete\nmanage saved chats. Retry preserves prior tool side effects.\n\nDialogs: "
              "Enter submits single-line fields; Ctrl+S saves forms.");
    (void)m;
}
static void refresh_history(UI *u) {
    jf(u->history);
    u->history = chats();
    u->history_sel = -1;
    for (size_t i = 0; i < u->history->len; i++)
        if (!strcmp(gs(u->history->v[i], "id"), gs(u->chat, "id")))
            u->history_sel = (int)i;
}
static void fresh(UI *u) {
    if (u->busy)
        return;
    jf(u->chat);
    u->chat = new_chat(u->config);
    u->history_sel = -1;
    u->chat_scroll = 0;
    u->following = 1;
    es(&u->composer, "");
    bfree(&u->partial);
    notice_ui(u, "");
    u->tab = 0;
    u->focus = 100;
}
static void start_job(UI *, int, const char *);
static void open_chat(UI *u, const char *id) {
    if (u->busy)
        return;
    int reconnect = 0;
    J *chat = load_chat(id);
    if (!chat) {
        error_ui(u);
        return;
    }
    if (strcmp(gs(chat, "workspace"), gs(u->config, "workspace"))) {
        notice_ui(u, "Chat uses its original workspace. Settings follows the selected chat.");
        jset(u->config, "workspace", js(gs(chat, "workspace")));
        save_config(u->config);
        reconnect = 1;
    }
    jf(u->chat);
    u->chat = chat;
    u->tab = 0;
    u->chat_scroll = 0;
    u->following = 1;
    bfree(&u->partial);
    refresh_history(u);
    u->dirty = 1;
    if (reconnect)
        start_job(u, 4, "");
}
static void update_worker(const char *kind, const char *text, void *opaque) {
    UI *u = opaque;
    pthread_mutex_lock(&u->mutex);
    if (!strcmp(kind, "delta"))
        bs(&u->partial, text);
    else if (!strcmp(kind, "status")) {
        free(u->progress);
        u->progress = strdup(text);
    } else if (!strcmp(kind, "notice")) {
        free(u->notice);
        u->notice = strdup(text);
    } else if (!strcmp(kind, "message"))
        bfree(&u->partial);
    u->dirty = 1;
    pthread_mutex_unlock(&u->mutex);
}
static J *ask_worker(UI *u, int kind, const char *title, const char *text, J *params) {
    pthread_mutex_lock(&u->mutex);
    u->request_kind = kind;
    free(u->request_title);
    free(u->request_text);
    u->request_title = strdup(title);
    u->request_text = strdup(text);
    jf(u->request_params);
    u->request_params = params ? jc(params) : NULL;
    u->approval = 1;
    u->dirty = 1;
    while (u->approval && !cancelled) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 100000000;
        if (t.tv_nsec >= 1000000000) {
            t.tv_sec++;
            t.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&u->condition, &u->mutex, &t);
    }
    J *r = u->response ? jc(u->response) : NULL;
    jf(u->response);
    u->response = NULL;
    u->approval = 0;
    pthread_mutex_unlock(&u->mutex);
    return r;
}
static int approve_worker(Tool *t, J *args, void *opaque) {
    UI *u = opaque;
    char *raw = jd(args, 1);
    Buf text = {0};
    bf(&text, "%s / %s\n\n%s", t->server, t->name, raw);
    free(raw);
    if (!strcmp(gs(t->conn ? t->conn->config : NULL, "builtin"), "filesystem") &&
        (!strcmp(t->name, "write_file") || !strcmp(t->name, "edit_file"))) {
        for (size_t i = 0; i < u->mcp.toolcount; i++) {
            Tool *reader = u->mcp.tools[i];
            if (reader->conn == t->conn && !strcmp(reader->name, "read_file")) {
                J *a = jo();
                jset(a, "path", js(gs(args, "path")));
                jset(a, "lines", jnum(100));
                char *current = mcp_call(&u->mcp, reader->public, a);
                jf(a);
                bf(&text, "\n\nCurrent file\n%s\n\nProposed %s\n%s", current ? current : err,
                   !strcmp(t->name, "edit_file") ? "replacement" : "content",
                   !strcmp(t->name, "edit_file") ? gs(args, "new_text") : gs(args, "content"));
                free(current);
                err[0] = 0;
                break;
            }
        }
    }
    J *answer = ask_worker(u, A_APPROVE, "Tool permission", text.s, NULL);
    bfree(&text);
    int yes = gb(answer, "yes", 0);
    jf(answer);
    return yes;
}
static J *elicit_worker(const char *server, J *params, void *opaque) {
    UI *u = opaque;
    char *title = fmt("%s requests input", server);
    J *answer = ask_worker(u, A_ELICIT, title, gs(params, "message"), params);
    free(title);
    if (!answer) {
        answer = jo();
        jset(answer, "action", js("cancel"));
    }
    return answer;
}
static void *worker(void *opaque) {
    UI *u = opaque;
    err[0] = 0;
    int result = 0;
    J *jobresult = NULL;
    Agent a = {.config = u->config,
               .chat = u->chat,
               .mcp = &u->mcp,
               .mutex = &u->mutex,
               .approve = approve_worker,
               .update = update_worker,
               .opaque = u};
    switch (u->job) {
    case 1:
        result = agent_run(&a, u->prompt, 0);
        break;
    case 2:
        result = agent_run(&a, "", 1);
        break;
    case 3:
        result = agent_compact(&a, 1);
        break;
    case 4: {
        MCP nextm;
        mcp_init(&nextm, u->config);
        nextm.elicit = elicit_worker;
        nextm.opaque = u;
        nextm.mutex = &u->mutex;
        int failed = mcp_connect(&nextm);
        pthread_mutex_lock(&u->mutex);
        mcp_close(&u->mcp);
        u->mcp = nextm;
        pthread_mutex_unlock(&u->mutex);
        char *text = failed ? fmt("%d MCP server(s) failed — see MCP tab", failed)
                            : fmt("%zu MCP tools connected", nextm.toolcount);
        update_worker("notice", text, u);
        free(text);
        break;
    }
    case 5:
        jobresult = models(u->config);
        result = jobresult ? 0 : -1;
        break;
    case 6:
        result = run_task(u->prompt);
        break;
    case 7:
        result = timer_change(!strcmp(u->prompt, "enable"));
        break;
    case 9:
        result = mcp_refresh(&u->mcp);
        break;
    case 8: {
        Tool *t = NULL;
        for (size_t i = 0; i < u->mcp.toolcount; i++)
            if (!strcmp(gs(u->mcp.tools[i]->conn->config, "builtin"), "filesystem") &&
                !strcmp(u->mcp.tools[i]->name, "read_file")) {
                t = u->mcp.tools[i];
                break;
            }
        if (!t)
            result = fail("Enable the filesystem MCP to attach files");
        else {
            J *args = jo();
            jset(args, "path", js(u->prompt));
            jset(args, "lines", jnum(1000));
            char *text = mcp_call(&u->mcp, t->public, args);
            jf(args);
            if (!text)
                result = -1;
            else {
                jobresult = js(text);
                free(text);
            }
        }
        break;
    }
    }
    pthread_mutex_lock(&u->mutex);
    if (result) {
        free(u->notice);
        u->notice = cancelled ? strdup("Stopped") : fmt("Error: %s", err);
    } else if (u->job == 1 || u->job == 2) {
        if (u->notice && !strncmp(u->notice, "Error:", 6)) {
            free(u->notice);
            u->notice = strdup("");
        }
    }
    free(u->progress);
    u->progress = strdup("");
    bfree(&u->partial);
    u->result = result;
    u->jobresult = jobresult;
    u->finished = 1;
    u->dirty = 1;
    pthread_mutex_unlock(&u->mutex);
    return NULL;
}
static void start_job(UI *u, int job, const char *text) {
    if (u->busy)
        return;
    cancelled = 0;
    free(u->prompt);
    u->prompt = strdup(text ? text : "");
    u->job = job;
    u->busy = 1;
    u->finished = 0;
    free(u->progress);
    u->progress = strdup(job == 9   ? (u->notice && *u->notice ? u->notice : "Ready")
                         : job == 4 ? "Connecting MCP servers…"
                         : job == 5 ? "Discovering models…"
                         : job == 6 ? "Running scheduled task…"
                                    : "Working…");
    if (job == 1 || job == 2) {
        free(u->notice);
        u->notice = strdup("");
        u->following = 1;
    }
    u->dirty = 1;
    int r = pthread_create(&u->thread, NULL, worker, u);
    if (r) {
        u->busy = 0;
        notice_ui(u, "Could not start worker");
    }
}
static void pick(UI *u, int action, const char *title, J *choices, int selected) {
    Modal *m = modal(u, PICK, action, title, "");
    m->choices = choices;
    m->selected = selected >= 0 ? selected : 0;
}
static void provider_form(UI *u) {
    Modal *m = modal(u, FORM, A_PROVIDER, "Provider settings", "");
    J *p = profile(u->config);
    field_add(m, "endpoint", "OpenAI-compatible endpoint", gs(p, "endpoint"), 0, 0);
    field_add(m, "model", "Model (or discover with Models)", gs(p, "model"), 0, 0);
    field_add(m, "apiKey", "API key", gs(p, "apiKey"), 0, 0);
    m->fields[2].e.secret = 1;
    field_add(m, "apiKeyEnv", "API key environment variable", gs(p, "apiKeyEnv"), 0, 0);
}
static void mcp_form(UI *u, int edit) {
    J *cfg = jg(u->config, "mcpServers"), *s = edit ? ji(cfg, (size_t)u->server_sel) : NULL;
    if (edit && !s)
        return;
    if (edit && *gs(s, "builtin")) {
        char *raw = jd(s, 1);
        Modal *m = modal(u, FORM, A_MCP_EDIT, "Edit bundled MCP server", "");
        m->id = strdup(s->key);
        field_add(m, "json", "Server JSON", raw, 1, 2);
        free(raw);
        return;
    }
    Modal *m = modal(u, FORM, edit ? A_MCP_EDIT : A_MCP_ADD,
                     edit ? "Edit MCP server" : "Add MCP server", "");
    if (edit)
        m->id = strdup(s->key);
    field_add(m, "name", "Name", edit ? s->key : "", 0, 0);
    field_add(m, "command", "Local program (leave empty for a URL)", gs(s, "command"), 0, 0);
    Buf args = {0};
    J *a = jg(s, "args");
    for (size_t i = 0; a && i < a->len; i++)
        bf(&args, "%s%s", i ? "\n" : "", jstr(a->v[i]));
    field_add(m, "args", "Arguments — one per line", args.s ? args.s : "", 1, 3);
    bfree(&args);
    field_add(m, "url", "Remote MCP URL (leave empty for local)", gs(s, "url"), 0, 0);
    field_add(m, "transport", "Transport: stdio / http / sse",
              *gs(s, "transport") ? gs(s, "transport") : "stdio", 0, 0);
    char *env = jg(s, "env") ? jd(jg(s, "env"), 1) : strdup("{}"),
         *headers = jg(s, "headers") ? jd(jg(s, "headers"), 1) : strdup("{}");
    field_add(m, "env", "Environment JSON — ${ENV_VAR} references supported", env, 1, 2);
    field_add(m, "headers", "Headers JSON — e.g. Authorization: Bearer ${TOKEN}", headers, 1, 2);
    free(env);
    free(headers);
}
static void task_form(UI *u, int edit) {
    J *t = edit ? ji(u->tasklist, (size_t)u->task_sel) : NULL;
    if (edit && !t)
        return;
    Modal *m = modal(u, FORM, edit ? A_TASK_EDIT : A_TASK_NEW,
                     edit ? "Edit scheduled task" : "Schedule a task", "");
    if (t)
        m->id = strdup(gs(t, "id"));
    m->value = t ? jc(t) : jo();
    field_add(m, "name", "Name", gs(t, "name"), 0, 0);
    field_add(m, "prompt", "Task instructions", gs(t, "prompt"), 1, 0);
    field_add(m, "cron", "Cron: minute hour day month weekday",
              *gs(t, "cron") ? gs(t, "cron") : "0 9 * * 1-5", 0, 0);
    char *tz = readfile("/etc/timezone", 512);
    if (tz)
        tz[strcspn(tz, "\r\n")] = 0;
    field_add(m, "timezone", "IANA timezone",
              *gs(t, "timezone") ? gs(t, "timezone")
              : tz               ? tz
                                 : "Etc/UTC",
              0, 0);
    free(tz);
    if (!jg(m->value, "allowedTools"))
        jset(m->value, "allowedTools", ja());
}
static void tool_picker(UI *u, int action, const char *server, J *selected) {
    Modal *m =
        modal(u, CHECKS, action,
              action == A_MCP_TOOLS ? "Enabled MCP tools" : "Extra tools allowed for this task",
              "Read tools from bundled servers are always permitted for scheduled tasks.");
    if (server)
        m->id = strdup(server);
    m->choices = ja();
    m->value = selected ? jc(selected) : ja();
    for (size_t i = 0; i < u->mcp.toolcount; i++) {
        Tool *t = u->mcp.tools[i];
        if (server && strcmp(t->server, server))
            continue;
        J *v = jo();
        jset(v, "name", js(action == A_MCP_TOOLS ? t->name : t->public));
        char *label = fmt("%s / %s%s", t->server, t->name, t->safe ? " (read)" : "");
        jset(v, "label", js(label));
        free(label);
        jadd(m->choices, v);
    }
}
static void response(UI *u, J *answer) {
    pthread_mutex_lock(&u->mutex);
    jf(u->response);
    u->response = answer;
    u->approval = 0;
    pthread_cond_signal(&u->condition);
    pthread_mutex_unlock(&u->mutex);
    while (u->modal && (u->modal->action == A_APPROVE || u->modal->action == A_ELICIT))
        close_modal(u);
}
static void dispatch(UI *, int, int);
static void submit(UI *u) {
    if (u->busy)
        return;
    char *text = strdup(u->composer.s);
    if (!*text) {
        free(text);
        return;
    }
    if (text[0] == '/') {
        char *space = strchr(text, ' ');
        char *arg = space ? space + 1 : (char *)"";
        if (space)
            *space = 0;
        if (!strcmp(text, "/new")) {
            fresh(u);
        } else if (!strcmp(text, "/help"))
            help(u);
        else if (!strcmp(text, "/continue"))
            start_job(u, 2, "");
        else if (!strcmp(text, "/compact"))
            start_job(u, 3, "");
        else if (!strcmp(text, "/export"))
            dispatch(u, 109, 0);
        else if (!strcmp(text, "/events"))
            dispatch(u, 115, 0);
        else if (!strcmp(text, "/fork"))
            dispatch(u, 113, 0);
        else if (!strcmp(text, "/retry"))
            dispatch(u, 114, 0);
        else if (!strcmp(text, "/attach") && *arg)
            start_job(u, 8, arg);
        else {
            if (space)
                *space = ' ';
            start_job(u, 1, text);
        }
        es(&u->composer, "");
    } else {
        start_job(u, 1, text);
        es(&u->composer, "");
    }
    free(text);
}
static J *form_value(Modal *m) {
    J *v = jo();
    for (int i = 0; i < m->count; i++) {
        Field *f = &m->fields[i];
        if (m->action == A_ELICIT && !f->e.s[0] && !contains(jg(m->value, "required"), f->key))
            continue;
        J *value = NULL;
        if (f->kind == 1) {
            char *end;
            double n = strtod(f->e.s, &end);
            if (!*f->e.s || *end) {
                jf(v);
                fail("%s must be a number", f->label);
                return NULL;
            }
            value = jnum(n);
        } else if (f->kind == 2) {
            value = jp(f->e.s, NULL);
            if (!value) {
                jf(v);
                fail("Invalid JSON in %s", f->label);
                return NULL;
            }
        } else if (f->kind == 3) {
            value = ja();
            char *copy = strdup(f->e.s), *save = NULL;
            for (char *p = strtok_r(copy, "\n", &save); p; p = strtok_r(NULL, "\n", &save))
                jadd(value, js(p));
            free(copy);
        } else
            value = js(f->e.s);
        jset(v, f->key, value);
    }
    return v;
}
static void save_modal(UI *u, int yes) {
    Modal *m = u->modal;
    if (!m)
        return;
    if (m->action == A_APPROVE) {
        J *v = jo();
        jset(v, "yes", jb(yes));
        response(u, v);
        return;
    }
    if (m->action == A_ELICIT) {
        J *v = jo();
        jset(v, "action", js(yes ? "accept" : "cancel"));
        if (yes && m->kind == FORM) {
            J *values = form_value(m);
            if (!values) {
                jf(v);
                error_ui(u);
                return;
            }
            if (m->value && validate_schema(m->value, values)) {
                jf(v);
                jf(values);
                error_ui(u);
                return;
            }
            jset(v, "content", values);
        }
        response(u, v);
        return;
    }
    if (m->kind == MESSAGE) {
        close_modal(u);
        return;
    }
    if (!yes) {
        close_modal(u);
        return;
    }
    int action = m->action;
    J *v = m->kind == FORM ? form_value(m) : NULL;
    if (m->kind == FORM && !v) {
        error_ui(u);
        return;
    }
    int r = 0, reconnect = 0;
    char *selected = m->choices && m->selected < (int)m->choices->len
                         ? strdup(jstr(m->choices->v[m->selected]))
                         : NULL;
    switch (action) {
    case A_FIND: {
        const char *query = gs(v, "query");
        J *choices = ja();
        for (size_t i = 0; i < u->history->len; i++) {
            J *chat = u->history->v[i];
            char *raw = jd(chat, 0);
            if (strcasestr(raw, query)) {
                J *choice = jo();
                jset(choice, "id", js(gs(chat, "id")));
                jset(choice, "label", js(gs(chat, "title")));
                jadd(choices, choice);
            }
            free(raw);
        }
        close_modal(u);
        pick(u, 700, "Search results", choices, 0);
        free(selected);
        jf(v);
        return;
    }
    case A_RENAME:
        jset(u->chat, "title", js(gs(v, "title")));
        r = save_chat(u->chat);
        break;
    case A_DELETE:
        r = delete_chat(m->id);
        if (!r && !strcmp(m->id, gs(u->chat, "id")))
            fresh(u);
        break;
    case A_PROFILE:
        if (selected) {
            jset(u->config, "profile", js(selected));
            r = save_config(u->config);
        }
        break;
    case A_PROVIDER: {
        J *p = profile(u->config), *copy = jc(u->config);
        for (size_t i = 0; i < v->len; i++)
            jset(p, v->v[i]->key, jc(v->v[i]));
        r = save_config(u->config);
        if (r) {
            jf(u->config);
            u->config = copy;
            u->mcp.config = u->config;
        } else
            jf(copy);
        break;
    }
    case A_MODEL:
        if (selected)
            jset(profile(u->config), "model", js(selected));
        else
            jset(profile(u->config), "model", js(gs(v, "model")));
        r = save_config(u->config);
        break;
    case A_LIMITS:
    case A_WORKSPACE:
    case A_PROMPT: {
        J *copy = jc(u->config);
        for (size_t i = 0; i < v->len; i++)
            jset(u->config, v->v[i]->key, jc(v->v[i]));
        r = save_config(u->config);
        if (r) {
            jf(u->config);
            u->config = copy;
            u->mcp.config = u->config;
        } else {
            jf(copy);
            if (action == A_WORKSPACE) {
                jset(u->chat, "workspace", js(gs(u->config, "workspace")));
                reconnect = 1;
            }
        }
        break;
    }
    case A_PERMISSION:
        if (selected) {
            if (!strcmp(selected, "auto") && m->kind == PICK) {
                close_modal(u);
                Modal *confirm =
                    modal(u, CONFIRM, A_PERMISSION, "Enable Auto",
                          "Allow tools to act without asking, including shell commands and "
                          "external MCP tools? This mode is saved until changed in Settings.");
                confirm->choices = ja();
                jadd(confirm->choices, js("auto"));
                free(selected);
                jf(v);
                return;
            }
            jset(u->config, "permissions", js(selected));
            r = save_config(u->config);
        }
        break;
    case A_MCP_ADD:
    case A_MCP_EDIT: {
        const char *name = action == A_MCP_ADD ? gs(v, "name") : m->id;
        J *existing = action == A_MCP_EDIT ? jg(jg(u->config, "mcpServers"), name) : NULL;
        if (action == A_MCP_ADD && jg(jg(u->config, "mcpServers"), name)) {
            r = fail("A server with this name already exists");
            break;
        }
        J *server = jg(v, "json") ? jc(jg(v, "json")) : existing ? jc(existing) : jo();
        if (!jg(v, "json")) {
            for (size_t i = 0; i < v->len; i++)
                jset(server, v->v[i]->key, jc(v->v[i]));
            jdel(server, "name");
            if (!*gs(server, "url"))
                jdel(server, "url");
            else
                jdel(server, "command");
        }
        if (!jg(server, "enabled"))
            jset(server, "enabled", jb(1));
        if (validate_server(name, server)) {
            jf(server);
            r = -1;
            break;
        }
        jset(jg(u->config, "mcpServers"), name, server);
        r = save_config(u->config);
        reconnect = !r;
        break;
    }
    case A_MCP_IMPORT: {
        J *servers = jg(v, "json"), *list = jg(servers, "mcpServers");
        if (!list)
            list = servers;
        if (!list || list->type != JOBJ) {
            r = fail("Import an object containing mcpServers");
            break;
        }
        for (size_t i = 0; i < list->len; i++)
            if (validate_server(list->v[i]->key, list->v[i])) {
                r = -1;
                break;
            }
        if (!r) {
            for (size_t i = 0; i < list->len; i++)
                jset(jg(u->config, "mcpServers"), list->v[i]->key, jc(list->v[i]));
            r = save_config(u->config);
            reconnect = !r;
        }
        break;
    }
    case A_MCP_REMOVE:
        jdel(jg(u->config, "mcpServers"), m->id);
        r = save_config(u->config);
        reconnect = !r;
        break;
    case A_MCP_TOOLS: {
        J *server = jg(jg(u->config, "mcpServers"), m->id);
        jset(server, "disabledTools", jc(m->value));
        r = save_config(u->config);
        reconnect = !r;
        break;
    }
    case A_TASK_NEW:
    case A_TASK_EDIT:
        jset(v, "allowedTools", jc(jg(m->value, "allowedTools")));
        r = task_save(v, u->config, &u->mcp, m->id);
        break;
    case A_TASK_DELETE:
        r = task_delete(m->id);
        break;
    case A_TASK_TOOLS:
        if (m->parent)
            jset(m->parent->value, "allowedTools", jc(m->value));
        break;
    case 700:
        if (m->choices && m->selected < (int)m->choices->len)
            open_chat(u, gs(m->choices->v[m->selected], "id"));
        break;
    default:
        break;
    }
    free(selected);
    jf(v);
    if (r) {
        error_ui(u);
        return;
    }
    if (u->modal == m)
        close_modal(u);
    refresh_history(u);
    jf(u->tasklist);
    u->tasklist = tasks();
    if (!u->tasklist)
        u->tasklist = ja();
    if (reconnect)
        start_job(u, 4, "");
    u->dirty = 1;
}
static void dispatch(UI *u, int id, int index) {
    if (id >= 10 && id < 15) {
        if (id == 14)
            help(u);
        else if (!u->modal) {
            u->tab = id - 10;
            u->detail_scroll = 0;
            u->focus = u->tab == 0 ? 100 : u->tab == 1 ? 200 : u->tab == 2 ? 400 : 500;
            u->dirty = 1;
        }
        return;
    }
    if (id == 3000) {
        Line *l = index < u->transcript.len ? &u->transcript.v[index] : NULL;
        if (l && l->key) {
            jset(u->expanded, l->key, jb(!gb(u->expanded, l->key, 0)));
            u->following = 0;
            u->dirty = 1;
        }
        return;
    }
    if (id == 301) {
        u->server_sel = index;
        u->detail_scroll = 0;
        u->dirty = 1;
        return;
    }
    if (id == 401) {
        u->task_sel = index;
        u->detail_scroll = 0;
        u->dirty = 1;
        return;
    }
    if (id == 100) {
        u->focus = 100;
        u->dirty = 1;
        return;
    }
    if (id == 106) {
        cancelled = 1;
        return;
    }
    if (u->busy)
        return;
    if (id == 102) {
        J *chat = ji(u->history, index);
        if (chat)
            open_chat(u, gs(chat, "id"));
        u->focus = 102;
        return;
    }
    switch (id) {
    case 101:
        fresh(u);
        break;
    case 103: {
        Modal *m = modal(u, FORM, A_FIND, "Find chats", "");
        field_add(m, "query", "Search text", "", 0, 0);
        break;
    }
    case 104: {
        Modal *m = modal(u, FORM, A_RENAME, "Rename chat", "");
        field_add(m, "title", "Title", gs(u->chat, "title"), 0, 0);
        break;
    }
    case 110: {
        Modal *m =
            modal(u, CONFIRM, A_DELETE, "Delete saved chat", "Delete this chat from history?");
        m->id = strdup(gs(u->chat, "id"));
        break;
    }
    case 105:
        submit(u);
        break;
    case 107:
        start_job(u, 3, "");
        break;
    case 109: {
        char *p = export_chat(u->chat);
        if (p) {
            message(u, "Export saved", p);
            free(p);
        } else
            error_ui(u);
        break;
    }
    case 113:
    case 114: {
        J *copy = jc(u->chat);
        char *key = uuid();
        jset(copy, "id", js(key));
        free(key);
        J *m = jg(copy, "messages");
        char *prompt = NULL;
        if (id == 114) {
            size_t n = m->len;
            while (n && strcmp(gs(m->v[n - 1], "role"), "user"))
                n--;
            if (!n) {
                jf(copy);
                notice_ui(u, "No user request to retry");
                break;
            }
            prompt = strdup(gs(m->v[n - 1], "content"));
            while (m->len >= n)
                jremove(m, m->len - 1);
            if (gn(copy, "compacted", 0) >= (double)n) {
                jset(copy, "compacted", jnum(0));
                jset(copy, "summary", js(""));
            }
        }
        char *title = fmt("%s (branch)", gs(copy, "title"));
        jset(copy, "title", js(title));
        free(title);
        jf(u->chat);
        u->chat = copy;
        save_chat(copy);
        refresh_history(u);
        notice_ui(u, "Created a branch; prior tool side effects remain.");
        if (prompt) {
            start_job(u, 1, prompt);
            free(prompt);
        }
        break;
    }
    case 115: {
        Buf b = {0};
        J *e = jg(u->chat, "events");
        for (size_t i = 0; e && i < e->len; i++)
            bf(&b, "%s  %s\n", gs(e->v[i], "at"), gs(e->v[i], "text"));
        message(u, "Activity", b.s ? b.s : "No events.");
        bfree(&b);
        break;
    }
    case 201:
        mcp_form(u, 0);
        break;
    case 202: {
        Modal *m = modal(u, FORM, A_MCP_IMPORT, "Import MCP servers", "");
        field_add(m, "json", "JSON", "{\n  \"mcpServers\": {}\n}", 1, 2);
        break;
    }
    case 203:
        mcp_form(u, 1);
        break;
    case 204: {
        J *s = ji(jg(u->config, "mcpServers"), u->server_sel);
        if (s) {
            jset(s, "enabled", jb(!gb(s, "enabled", 1)));
            if (save_config(u->config))
                error_ui(u);
            else
                start_job(u, 4, "");
        }
        break;
    }
    case 205: {
        J *s = ji(jg(u->config, "mcpServers"), u->server_sel);
        if (s) {
            tool_picker(u, A_MCP_TOOLS, s->key,
                        jg(s, "disabledTools")); /* Include disabled tools by requesting the
                                                    server's full list. */
            Server *conn = NULL;
            for (size_t i = 0; i < u->mcp.count; i++)
                if (!strcmp(u->mcp.servers[i]->name, s->key))
                    conn = u->mcp.servers[i];
            if (conn) {
                J *all = conn->listed;
                for (size_t i = 0; all && i < all->len; i++) {
                    const char *name = gs(all->v[i], "name");
                    int found = 0;
                    for (size_t k = 0; k < u->modal->choices->len; k++)
                        if (!strcmp(gs(u->modal->choices->v[k], "name"), name))
                            found = 1;
                    if (!found) {
                        J *c = jo();
                        jset(c, "name", js(name));
                        jset(c, "label", js(name));
                        jadd(u->modal->choices, c);
                    }
                }
            }
        }
        break;
    }
    case 206: {
        J *s = ji(jg(u->config, "mcpServers"), u->server_sel);
        if (s) {
            Modal *m = modal(u, CONFIRM, A_MCP_REMOVE, "Remove MCP server",
                             "Remove this server from Settings?");
            m->id = strdup(s->key);
        }
        break;
    }
    case 207:
        start_job(u, 4, "");
        break;
    case 402:
        task_form(u, 0);
        break;
    case 403:
        task_form(u, 1);
        break;
    case 404: {
        J *task = ji(u->tasklist, u->task_sel);
        if (task) {
            J *p = jo();
            jset(p, "enabled", jb(!gb(task, "enabled", 1)));
            J *v = task_patch(gs(task, "id"), p);
            jf(v);
            jf(p);
            jf(u->tasklist);
            u->tasklist = tasks();
            u->dirty = 1;
        }
        break;
    }
    case 405: {
        J *task = ji(u->tasklist, u->task_sel);
        if (task)
            start_job(u, 6, gs(task, "id"));
        break;
    }
    case 406: {
        J *task = ji(u->tasklist, u->task_sel);
        if (task && *gs(task, "lastChat"))
            open_chat(u, gs(task, "lastChat"));
        break;
    }
    case 407: {
        J *task = ji(u->tasklist, u->task_sel);
        if (task) {
            Modal *m =
                modal(u, CONFIRM, A_TASK_DELETE, "Delete schedule", "Delete this scheduled task?");
            m->id = strdup(gs(task, "id"));
        }
        break;
    }
    case 408:
        start_job(u, 7, u->timer && !strcmp(u->timer, "enabled") ? "disable" : "enable");
        break;
    case 501: {
        J *choices = ja(), *p = jg(u->config, "profiles");
        int selected = 0;
        for (size_t i = 0; i < p->len; i++) {
            jadd(choices, js(p->v[i]->key));
            if (!strcmp(p->v[i]->key, gs(u->config, "profile")))
                selected = i;
        }
        pick(u, A_PROFILE, "Provider profile", choices, selected);
        break;
    }
    case 502:
        provider_form(u);
        break;
    case 503:
        start_job(u, 5, "");
        break;
    case 504: {
        Modal *m = modal(u, FORM, A_MODEL, "Select model manually", "");
        field_add(m, "model", "Model ID", gs(profile(u->config), "model"), 0, 0);
        break;
    }
    case 505: {
        Modal *m = modal(u, FORM, A_WORKSPACE, "Workspace", "");
        field_add(m, "workspace", "Existing absolute directory", gs(u->config, "workspace"), 0, 0);
        break;
    }
    case 506: {
        J *choices = ja();
        const char *modes[] = {"ask", "read-only", "auto"};
        int sel = 0;
        for (int i = 0; i < 3; i++) {
            jadd(choices, js(modes[i]));
            if (!strcmp(modes[i], gs(u->config, "permissions")))
                sel = i;
        }
        pick(u, A_PERMISSION, "Tool permissions", choices, sel);
        break;
    }
    case 507: {
        Modal *m = modal(u, FORM, A_LIMITS, "Context and limits", "");
        const char *keys[] = {"contextWindow", "maxTokens", "maxSteps", "timeout"},
                   *labels[] = {"Context window (tokens)", "Maximum output tokens",
                                "Maximum tool rounds", "Request timeout (seconds)"};
        for (int i = 0; i < 4; i++) {
            char *s = fmt("%.0f", gn(u->config, keys[i], 0));
            field_add(m, keys[i], labels[i], s, 0, 1);
            free(s);
        }
        break;
    }
    case 508: {
        Modal *m = modal(u, FORM, A_PROMPT, "System instructions", "");
        field_add(m, "systemPrompt", "System prompt", gs(u->config, "systemPrompt"), 1, 0);
        break;
    }
    default:
        break;
    }
    u->dirty = 1;
}

static void modal_draw(UI *u) {
    Modal *m = u->modal;
    int previousfocus = u->focus;
    if (m->kind == FORM)
        u->focus = m->focus == m->count ? 801 : m->focus == m->count + 1 ? 802 : 0;
    int width = u->w * 4 / 5;
    if (width > 108)
        width = 108;
    if (width < 40)
        width = u->w - 4;
    int height = u->h - 8;
    if (height < 12)
        height = u->h - 4;
    int x = (u->w - width) / 2, y = (u->h - height) / 2;
    fill(u, x + 1, y + 1, width, height, SURFACE);
    fill(u, x, y, width, height, NORMAL);
    box(u, x, y, width, height, m->title, 1);
    int bottom = y + height - 3, inner = width - 6;
    u->hitcount = 0;
    if (m->kind == FORM) {
        int focused = m->focus < m->count ? m->focus : m->count - 1;
        int space = height - 6;
        int start = 0, total = 0;
        for (int i = 0; i <= focused; i++)
            total += m->fields[i].e.multiline ? 7 : 4;
        while (total > space && start < focused) {
            total -= m->fields[start].e.multiline ? 7 : 4;
            start++;
        }
        int row = y + 2;
        for (int i = start; i < m->count; i++) {
            Field *f = &m->fields[i];
            int h = f->e.multiline ? 6 : 3;
            if (row + h > bottom)
                break;
            box(u, x + 2, row, width - 4, h, f->label, m->focus == i);
            editor_draw(u, &f->e, x + 3, row + 1, width - 6, h - 2, m->focus == i);
            hit(u, x + 3, row + 1, width - 6, h - 2, 800, i);
            row += h + 1;
        }
        if (start > 0)
            draw_text(u, x + width - 6, y + 1, 3, "↑", MUTED, 0, 0);
        if (start < m->count - 1)
            draw_text(u, x + width - 6, bottom - 1, 3, "↓", MUTED, 0, 0);
        int bx = x + 3;
        button(u, bx, bottom + 1, "Save", 801);
        button(u, bx + 12, bottom + 1, "Cancel", 802);
        if (m->action == A_TASK_NEW || m->action == A_TASK_EDIT)
            button(u, bx + 26, bottom + 1, "Allowed tools", 804);
    } else if (m->kind == PICK || m->kind == CHECKS) {
        int top = y + 2, rows = height - 6;
        int count = m->choices ? (int)m->choices->len : 0;
        if (m->selected < 0)
            m->selected = 0;
        if (m->selected >= count)
            m->selected = count ? count - 1 : 0;
        if (m->selected < m->scroll)
            m->scroll = m->selected;
        if (m->selected >= m->scroll + rows)
            m->scroll = m->selected - rows + 1;
        for (int i = 0; i < rows && i + m->scroll < count; i++) {
            int index = i + m->scroll;
            J *v = m->choices->v[index];
            const char *label = v->type == JSTR ? jstr(v) : gs(v, "label");
            char *text;
            if (m->kind == CHECKS) {
                int checked = contains(m->value, gs(v, "name"));
                if (m->action == A_MCP_TOOLS)
                    checked = !checked;
                text = fmt("[%s] %s", checked ? "x" : " ", label);
            } else
                text = strdup(label);
            int c = index == m->selected ? SELECTED : NORMAL;
            fill(u, x + 3, top + i, inner, 1, c);
            draw_text(u, x + 3, top + i, inner, text, c, 0, 0);
            free(text);
            hit(u, x + 3, top + i, inner, 1, 803, index);
        }
        button(u, x + 3, bottom + 1, m->kind == CHECKS ? "Save" : "Select", 801);
        button(u, x + 18, bottom + 1, "Cancel", 802);
    } else {
        Lines l = {0};
        text_lines(&l, m->text, NORMAL, 0);
        view_lines(u, &l, x + 3, y + 2, inner, height - 6, &m->scroll, 0, 0);
        lines_free(&l);
        if (m->kind == CONFIRM) {
            const char *yes = m->action == A_APPROVE      ? "Allow once"
                              : m->action == A_PERMISSION ? "Enable Auto"
                                                          : "Confirm";
            int a = x + 3, b = x + 22;
            fill(u, a, bottom + 1, 15, 1, m->selected == 1 ? SELECTED : SURFACE);
            draw_text(u, a + 1, bottom + 1, 13, yes, m->selected == 1 ? SELECTED : SURFACE, 0, 0);
            hit(u, a, bottom + 1, 15, 1, 801, 0);
            fill(u, b, bottom + 1, 12, 1, m->selected == 0 ? SELECTED : SURFACE);
            draw_text(u, b + 1, bottom + 1, 10, m->action == A_APPROVE ? "Deny" : "Cancel",
                      m->selected == 0 ? SELECTED : SURFACE, 0, 0);
            hit(u, b, bottom + 1, 12, 1, 802, 0);
        } else
            button(u, x + 3, bottom + 1, "Close", 802);
    }
    u->focus = previousfocus;
}
static void draw_chat(UI *u) {
    int side = u->w * 26 / 100;
    if (side < 22)
        side = 22;
    int body = u->h - 4;
    box(u, 0, 3, side, body, "History", u->focus == 102);
    button(u, 2, 5, "+ New", 101);
    button(u, 13, 5, "Find", 103);
    int rows = body - 8, start = u->history_sel >= rows ? u->history_sel - rows + 1 : 0;
    for (int i = 0; i < rows && u->history && i + start < (int)u->history->len; i++) {
        int index = i + start, c = index == u->history_sel ? SELECTED : NORMAL;
        fill(u, 2, 7 + i, side - 4, 1, c);
        draw_text(u, 3, 7 + i, side - 6, gs(u->history->v[index], "title"), c, 0, 0);
        hit(u, 2, 7 + i, side - 4, 1, 102, index);
    }
    button(u, 2, u->h - 4, "Rename", 104);
    button(u, 13, u->h - 4, "Delete", 110);
    int x = side, width = u->w - side;
    int logheight = u->h - 13;
    if (logheight < 5)
        logheight = 5;
    box(u, x, 3, width, logheight, gs(u->chat, "title"), u->focus == 108);
    transcript(u);
    hit(u, x + 1, 4, width - 2, logheight - 2, 108, 0);
    view_lines(u, &u->transcript, x + 2, 5, width - 4, logheight - 4, &u->chat_scroll, u->following,
               1);
    int cy = 3 + logheight;
    box(u, x, cy, width, 6, "Message", u->focus == 100);
    editor_draw(u, &u->composer, x + 2, cy + 1, width - 4, 4, u->focus == 100);
    hit(u, x + 1, cy + 1, width - 2, 4, 100, 0);
    const char *labels[] = {"Send", "Stop", "Compact", "Export"};
    int ids[] = {105, 106, 107, 109};
    actions(u, x, cy + 6, width, labels, ids, 4);
}
static void draw_mcp(UI *u) {
    int side = u->w * 36 / 100, height = u->h - 7;
    if (side < 26)
        side = 26;
    box(u, 0, 3, side, height, "Servers", u->focus == 200);
    box(u, side, 3, u->w - side, height, "Server details", u->focus == 208);
    J *cfg = jg(u->config, "mcpServers");
    if (u->server_sel >= (int)cfg->len)
        u->server_sel = cfg->len ? cfg->len - 1 : 0;
    for (size_t i = 0; i < cfg->len && i < (size_t)(height - 4); i++) {
        J *v = cfg->v[i];
        Server *s = i < u->mcp.count ? u->mcp.servers[i] : NULL;
        int count = 0;
        for (size_t k = 0; k < u->mcp.toolcount; k++)
            if (s == u->mcp.tools[k]->conn)
                count++;
        char *text = fmt("%s  %s  (%d tools)", v->key,
                         s                     ? s->state
                         : gb(v, "enabled", 1) ? "not connected"
                                               : "disabled",
                         count);
        int c = (int)i == u->server_sel ? SELECTED : NORMAL;
        fill(u, 2, 5 + i, side - 4, 1, c);
        draw_text(u, 2, 5 + i, side - 4, text, c, 0, 0);
        free(text);
        hit(u, 2, 5 + i, side - 4, 1, 301, i);
    }
    Server *s = u->server_sel < (int)u->mcp.count ? u->mcp.servers[u->server_sel] : NULL;
    Lines l = {0};
    if (s) {
        line(&l, s->name, ACCENT, 1, NULL);
        char *text = fmt("%s · %s", s->modern ? "MCP v2 / 2026" : "MCP v2 / legacy",
                         s->kind == 0   ? "stdio"
                         : s->kind == 2 ? "SSE"
                                        : "Streamable HTTP");
        line(&l, text, MUTED, 0, NULL);
        free(text);
        if (s->detail && *s->detail)
            text_lines(&l, s->detail, MUTED, 0);
        if (s->instructions && *s->instructions) {
            line(&l, "", NORMAL, 0, NULL);
            text_lines(&l, s->instructions, MUTED, 0);
        }
        for (size_t i = 0; i < u->mcp.toolcount; i++) {
            Tool *t = u->mcp.tools[i];
            if (t->conn != s)
                continue;
            line(&l, "", NORMAL, 0, NULL);
            text = fmt("%s%s", t->name, t->safe ? " (read)" : "");
            line(&l, text, ACCENT, 0, NULL);
            free(text);
            text = fmt("Alias: %s", t->public);
            line(&l, text, MUTED, 0, NULL);
            free(text);
            text_lines(&l, gs(jg(t->definition, "function"), "description"), NORMAL, 0);
        }
    }
    view_lines(u, &l, side + 2, 5, u->w - side - 4, height - 4, &u->detail_scroll, 0, 0);
    lines_free(&l);
    const char *labels[] = {"Add", "Import", "Edit", "Toggle", "Tools", "Remove", "Reconnect"};
    int ids[] = {201, 202, 203, 204, 205, 206, 207};
    actions(u, 0, u->h - 4, u->w, labels, ids, 7);
}
static void draw_tasks(UI *u) {
    int side = u->w * 36 / 100, height = u->h - 7;
    box(u, 0, 3, side, height, "Scheduled tasks", u->focus == 400);
    box(u, side, 3, u->w - side, height, "Task details", u->focus == 409);
    if (u->task_sel >= (int)u->tasklist->len)
        u->task_sel = u->tasklist->len ? u->tasklist->len - 1 : 0;
    for (size_t i = 0; i < u->tasklist->len && i < (size_t)(height - 4); i++) {
        J *v = u->tasklist->v[i];
        char *text = fmt("%s  %s", gb(v, "enabled", 1) ? "●" : "○", gs(v, "name"));
        int c = (int)i == u->task_sel ? SELECTED : NORMAL;
        fill(u, 2, 5 + i, side - 4, 1, c);
        draw_text(u, 2, 5 + i, side - 4, text, c, 0, 0);
        free(text);
        hit(u, 2, 5 + i, side - 4, 1, 401, i);
    }
    Lines l = {0};
    J *t = ji(u->tasklist, u->task_sel);
    if (t) {
        line(&l, gs(t, "name"), ACCENT, 1, NULL);
        text_lines(&l, gs(t, "prompt"), NORMAL, 0);
        line(&l, "", NORMAL, 0, NULL);
        const char *keys[] = {"cron",    "timezone", "nextRun",   "lastRun",  "lastStatus",
                              "profile", "model",    "workspace", "lastError"},
                   *labels[] = {"Cron",     "Timezone", "Next run",  "Last run", "Status",
                                "Provider", "Model",    "Workspace", "Error"};
        for (int i = 0; i < 9; i++) {
            char *s = fmt("%s: %s", labels[i], *gs(t, keys[i]) ? gs(t, keys[i]) : "—");
            text_lines(&l, s, i == 4 ? ACCENT : MUTED, 0);
            free(s);
        }
        line(&l, "", NORMAL, 0, NULL);
        line(&l, "Bundled read tools + these extra permissions:", MUTED, 0, NULL);
        J *allowed = jg(t, "allowedTools");
        for (size_t i = 0; allowed && i < allowed->len; i++)
            line(&l, jstr(allowed->v[i]), MUTED, 0, NULL);
    } else
        line(&l, "No scheduled tasks. Use New to add one.", MUTED, 0, NULL);
    char *timer = fmt("\nBackground timer: %s", u->timer ? u->timer : "checking");
    text_lines(&l, timer, MUTED, 0);
    free(timer);
    view_lines(u, &l, side + 2, 5, u->w - side - 4, height - 4, &u->detail_scroll, 0, 0);
    lines_free(&l);
    const char *labels[] = {"New", "Edit", "Pause", "Run", "Result", "Delete", "Timer"};
    int ids[] = {402, 403, 404, 405, 406, 407, 408};
    actions(u, 0, u->h - 4, u->w, labels, ids, 7);
}
static void draw_settings(UI *u) {
    int width = u->w / 2;
    box(u, 0, 3, width, u->h - 4, "Connection", u->focus >= 501 && u->focus <= 504);
    box(u, width, 3, u->w - width, u->h - 4, "Agent", u->focus >= 505 && u->focus <= 508);
    J *p = profile(u->config);
    char *text = fmt("%s\n\nEndpoint: %s\nModel: %s\nAPI key: %s\nKey variable: %s", gs(p, "label"),
                     gs(p, "endpoint"), *gs(p, "model") ? gs(p, "model") : "choose a model",
                     *gs(p, "apiKey") ? "configured" : "none",
                     *gs(p, "apiKeyEnv") ? gs(p, "apiKeyEnv") : "none");
    Lines l = {0};
    text_lines(&l, text, NORMAL, 0);
    free(text);
    int scroll = 0;
    view_lines(u, &l, 3, 5, width - 6, 10, &scroll, 0, 0);
    lines_free(&l);
    button(u, 3, 17, "Profile", 501);
    button(u, 3, 20, "Edit connection", 502);
    button(u, 3, 23, "Discover / Test", 503);
    button(u, 3, 26, "Manual model", 504);
    text = fmt("Seth\n\nWorkspace: %s\nPermissions: %s\nContext window: %.0f\nOutput tokens: "
               "%.0f\nTool rounds: %.0f\nTimeout: %.0f seconds",
               gs(u->config, "workspace"), gs(u->config, "permissions"),
               gn(u->config, "contextWindow", 0), gn(u->config, "maxTokens", 0),
               gn(u->config, "maxSteps", 0), gn(u->config, "timeout", 0));
    text_lines(&l, text, NORMAL, 0);
    free(text);
    view_lines(u, &l, width + 3, 5, width - 6, 10, &scroll, 0, 0);
    lines_free(&l);
    button(u, width + 3, 17, "Workspace", 505);
    button(u, width + 3, 20, "Tool permissions", 506);
    button(u, width + 3, 23, "Context and limits", 507);
    button(u, width + 3, 26, "System instructions", 508);
}
static const char *styles[] = {
    "\033[38;2;255;255;255m\033[48;2;34;34;38m", "\033[38;2;170;170;170m\033[48;2;34;34;38m",
    "\033[38;2;255;190;111m\033[48;2;34;34;38m", "\033[38;2;255;255;255m\033[48;2;56;56;60m",
    "\033[38;2;34;34;38m\033[48;2;255;190;111m", "\033[38;2;170;170;170m\033[48;2;34;34;38m",
    "\033[38;2;255;190;111m\033[48;2;34;34;38m", "\033[38;2;255;255;255m\033[48;2;56;56;60m",
    "\033[38;2;34;34;38m\033[48;2;255;163;72m"};
static size_t active_estimate(J *chat) {
    J *a = ja(), *m = jg(chat, "messages");
    for (size_t i = (size_t)gn(chat, "compacted", 0); m && i < m->len; i++)
        jadd(a, jc(m->v[i]));
    size_t n = estimate(a);
    jf(a);
    return n;
}
static void render(UI *u) {
    if (pthread_mutex_trylock(&u->mutex))
        return;
    for (int i = 0; i < u->w * u->h; i++)
        u->cells[i] = (Cell){L' ', NORMAL, 0};
    u->hitcount = 0;
    box(u, 0, 0, u->w, 3, "Views", 0);
    const char *tabs[] = {"F1 Chat", "F2 MCP servers", "F3 Automation", "F4 Settings",
                          "F5 Help (?)"};
    int x = 2;
    for (int i = 0; i < 5; i++) {
        int w = strlen(tabs[i]) + 4;
        int color =
            ((u->modal && u->modal->action == 999) ? i == 4 : u->tab == i) ? SELECTED : SURFACE;
        fill(u, x, 1, w, 1, color);
        draw_text(u, x + 2, 1, w - 4, tabs[i], color, 0, 0);
        hit(u, x, 1, w, 1, 10 + i, 0);
        x += w + 2;
    }
    if (u->w < 80 || u->h < 30)
        draw_text(u, 2, 5, u->w - 4, "Resize the terminal to at least 80 columns × 30 rows.",
                  ACCENT, 0, 0);
    else if (u->tab == 0)
        draw_chat(u);
    else if (u->tab == 1)
        draw_mcp(u);
    else if (u->tab == 2)
        draw_tasks(u);
    else
        draw_settings(u);
    char *status = fmt(
        "%s  %s / %s · %s · Context ≈ %zu / %.0f",
        u->busy                   ? u->progress && *u->progress ? u->progress : "Working…"
        : u->notice && *u->notice ? u->notice
                                  : "Ready",
        gs(profile(u->config), "label"),
        *gs(profile(u->config), "model") ? gs(profile(u->config), "model") : "choose a model",
        gs(u->config, "permissions"), active_estimate(u->chat), gn(u->config, "contextWindow", 0));
    draw_text(u, 1, u->h - 1, u->w - 2, status, u->busy ? ACCENT : MUTED, 0, 0);
    free(status);
    if (u->modal)
        modal_draw(u);
    Buf out = {0};
    int color = -1, bold = -1;
    for (int y = 0; y < u->h; y++)
        for (int x2 = 0; x2 < u->w; x2++) {
            Cell c = u->cells[y * u->w + x2];
            if (!memcmp(&c, &u->old[y * u->w + x2], sizeof c))
                continue;
            if (!c.c)
                continue;
            bf(&out, "\033[%d;%dH", y + 1, x2 + 1);
            if (color != c.color) {
                bs(&out, styles[c.color]);
                color = c.color;
            }
            if (bold != c.bold) {
                bs(&out, c.bold ? "\033[1m" : "\033[22m");
                bold = c.bold;
            }
            mbstate_t state = {0};
            char s[MB_LEN_MAX];
            size_t n = wcrtomb(s, c.c, &state);
            if (n == (size_t)-1)
                bs(&out, "?");
            else
                bput(&out, s, n);
        }
    memcpy(u->old, u->cells, (size_t)u->w * u->h * sizeof *u->old);
    u->dirty = 0;
    pthread_mutex_unlock(&u->mutex);
    if (out.n)
        writeall(1, out.s, out.n);
    bfree(&out);
}
static void resize_ui(UI *u) {
    struct winsize ws;
    if (!ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col && ws.ws_row) {
        u->w = ws.ws_col;
        u->h = ws.ws_row;
    } else {
        u->w = 124;
        u->h = 38;
    }
    free(u->cells);
    free(u->old);
    u->cells = calloc((size_t)u->w * u->h, sizeof *u->cells);
    u->old = calloc((size_t)u->w * u->h, sizeof *u->old);
    u->dirty = 1;
}
static void checkbox(Modal *m, int index) {
    J *v = ji(m->choices, index);
    if (!v)
        return;
    const char *name = gs(v, "name");
    int found = 0;
    for (size_t i = 0; i < m->value->len; i++)
        if (!strcmp(jstr(m->value->v[i]), name)) {
            jremove(m->value, i);
            found = 1;
            break;
        }
    if (!found)
        jadd(m->value, js(name));
}
static void mouse(UI *u, int buttoncode, int x, int y, int release) {
    if (release)
        return;
    if (buttoncode & 64) {
        int down = buttoncode & 1;
        if (u->modal) {
            Modal *m = u->modal;
            if (m->kind == PICK || m->kind == CHECKS)
                m->selected += down ? 1 : -1;
            else
                m->scroll += down ? 3 : -3;
        } else if (u->tab == 0) {
            u->following = 0;
            u->chat_scroll += down ? 3 : -3;
        } else
            u->detail_scroll += down ? 3 : -3;
        u->dirty = 1;
        return;
    }
    if ((buttoncode & 3) != 0)
        return;
    for (int i = u->hitcount - 1; i >= 0; i--) {
        Hit *h = &u->hits[i];
        if (x < h->x || x >= h->x + h->w || y < h->y || y >= h->y + h->h)
            continue;
        if (u->modal) {
            Modal *m = u->modal;
            if (h->id == 800)
                m->focus = h->index;
            else if (h->id == 801)
                save_modal(u, 1);
            else if (h->id == 802)
                save_modal(u, 0);
            else if (h->id == 803) {
                m->selected = h->index;
                if (m->kind == CHECKS)
                    checkbox(m, h->index);
                else
                    save_modal(u, 1);
            } else if (h->id == 804)
                tool_picker(u, A_TASK_TOOLS, NULL, jg(m->value, "allowedTools"));
        } else {
            u->focus = h->id;
            dispatch(u, h->id, h->index);
        }
        u->dirty = 1;
        return;
    }
}
static void cycle_focus(UI *u, int backwards) {
    int ids[256], n = 0;
    for (int i = 0; i < u->hitcount; i++) {
        int id = u->hits[i].id;
        if (id >= 3000 || id == 301 || id == 401 || id < 100)
            continue;
        int seen = 0;
        for (int k = 0; k < n; k++)
            if (ids[k] == id)
                seen = 1;
        if (!seen)
            ids[n++] = id;
    }
    int at = -1;
    for (int i = 0; i < n; i++)
        if (ids[i] == u->focus)
            at = i;
    if (n)
        u->focus = ids[(at + (backwards ? -1 : 1) + n) % n];
}
static void key(UI *u, int code, const char *text) {
    u->dirty = 1;
    if (code == 17 || code == 3) {
        u->quit = 1;
        cancelled = 1;
        return;
    }
    if (code == 1015) {
        help(u);
        return;
    }
    if (u->modal) {
        Modal *m = u->modal;
        if (code == 27) {
            save_modal(u, 0);
            return;
        }
        if (code == 9 || code == 1011) {
            if (m->kind == FORM)
                m->focus = (m->focus + (code == 1011 ? -1 : 1) + m->count + 2) % (m->count + 2);
            else if (m->kind == CONFIRM)
                m->selected = !m->selected;
            return;
        }
        if (code == 19) {
            save_modal(u, 1);
            return;
        }
        if (m->kind == FORM) {
            if (code == 13) {
                if (m->focus >= m->count)
                    save_modal(u, m->focus == m->count);
                else if (m->fields[m->focus].e.multiline)
                    editor_key(&m->fields[m->focus].e, 10, NULL);
                else
                    save_modal(u, 1);
                return;
            }
            if (m->focus < m->count) {
                Editor *e = &m->fields[m->focus].e;
                if (e->secret && text && *text) {
                    es(e, "");
                    e->secret = 0;
                }
                editor_key(e, code, text);
            }
            return;
        }
        if (m->kind == PICK || m->kind == CHECKS) {
            if (code == 1006)
                m->selected--;
            else if (code == 1007)
                m->selected++;
            else if (code == 13 || code == 32) {
                if (m->kind == CHECKS)
                    checkbox(m, m->selected);
                else
                    save_modal(u, 1);
            } else if (text && *text) {
                for (size_t i = 0; i < m->choices->len; i++) {
                    J *v = m->choices->v[i];
                    const char *label = v->type == JSTR ? jstr(v) : gs(v, "label");
                    if (tolower((unsigned char)*label) == tolower((unsigned char)*text)) {
                        m->selected = i;
                        break;
                    }
                }
            }
            return;
        }
        if (m->kind == CONFIRM) {
            if (code == 1004 || code == 1005 || code == 32)
                m->selected = !m->selected;
            else if (code == 13)
                save_modal(u, m->selected == 1);
            else if (text && (!strcmp(text, "y") || !strcmp(text, "Y")))
                save_modal(u, 1);
            else if (text && (!strcmp(text, "n") || !strcmp(text, "N")))
                save_modal(u, 0);
            return;
        }
        if (code == 13 || code == 32) {
            close_modal(u);
            return;
        }
        if (code == 1006 || code == 1012)
            m->scroll -= code == 1012 ? 10 : 1;
        if (code == 1007 || code == 1013)
            m->scroll += code == 1013 ? 10 : 1;
        return;
    }
    if (code >= 1016 && code <= 1019) {
        dispatch(u, 10 + code - 1016, 0);
        return;
    }
    if (text && !strcmp(text, "?") && (u->focus != 100 || !*u->composer.s)) {
        help(u);
        return;
    }
    if (code == 14) {
        fresh(u);
        return;
    }
    if (code == 12) {
        u->tab = 0;
        u->focus = 100;
        return;
    }
    if (code == 27) {
        cancelled = 1;
        return;
    }
    if (code == 9 || code == 1011) {
        cycle_focus(u, code == 1011);
        return;
    }
    if (code == 19) {
        if (u->tab == 0)
            submit(u);
        return;
    }
    if (code == 1012 || code == 1013) {
        if (u->tab == 0) {
            u->following = 0;
            u->chat_scroll += code == 1012 ? -10 : 10;
        } else
            u->detail_scroll += code == 1012 ? -10 : 10;
        return;
    }
    if (u->tab == 0 && u->focus == 100) {
        if (code == 13)
            submit(u);
        else
            editor_key(&u->composer, code, text);
        return;
    }
    if (code == 13 || code == 32) {
        if (u->focus == 102)
            dispatch(u, 102, u->history_sel >= 0 ? u->history_sel : 0);
        else
            dispatch(u, u->focus, 0);
        return;
    }
    if (code == 1006 || code == 1007) {
        int change = code == 1006 ? -1 : 1;
        if (u->tab == 0 && u->focus == 102) {
            int i = u->history_sel + change;
            if (i >= 0 && i < (int)u->history->len)
                dispatch(u, 102, i);
        } else if (u->tab == 1) {
            int i = u->server_sel + change;
            if (i >= 0 && i < (int)u->mcp.count)
                u->server_sel = i;
        } else if (u->tab == 2) {
            int i = u->task_sel + change;
            if (i >= 0 && i < (int)u->tasklist->len)
                u->task_sel = i;
        } else if (u->tab == 0) {
            u->following = 0;
            u->chat_scroll += change;
        } else
            u->detail_scroll += change;
    }
}
static void consume_input(UI *u, int flush) {
    while (u->input.n) {
        char *p = u->input.s;
        size_t used = 0;
        if (u->paste) {
            char *end = strstr(p, "\033[201~");
            if (!end) {
                if (u->input.n > 6) {
                    bput(&u->pasted, p, u->input.n - 6);
                    used = u->input.n - 6;
                } else
                    return;
            } else {
                bput(&u->pasted, p, end - p);
                used = end - p + 6;
                u->paste = 0;
                Editor *e = u->modal && u->modal->kind == FORM && u->modal->focus < u->modal->count
                                ? &u->modal->fields[u->modal->focus].e
                            : u->tab == 0 && u->focus == 100 ? &u->composer
                                                             : NULL;
                if (e) {
                    Buf clean = {0};
                    for (size_t i = 0; i < u->pasted.n; i++) {
                        unsigned char c = u->pasted.s[i];
                        if (c == '\r') {
                            if (i + 1 < u->pasted.n && u->pasted.s[i + 1] == '\n')
                                continue;
                            c = '\n';
                        }
                        if (c == '\n' && !e->multiline)
                            c = ' ';
                        if (c >= 32 || c == '\n' || c == '\t')
                            bput(&clean, (char *)&c, 1);
                    }
                    insert(e, clean.s ? clean.s : "", clean.n);
                    bfree(&clean);
                    u->dirty = 1;
                }
                bfree(&u->pasted);
            }
        } else if ((unsigned char)p[0] == 27) {
            if (u->input.n == 1) {
                if (!flush)
                    return;
                key(u, 27, NULL);
                used = 1;
            } else if (p[1] == '[') {
                size_t end = 2;
                while (end < u->input.n &&
                       !((unsigned char)p[end] >= 64 && (unsigned char)p[end] <= 126))
                    end++;
                if (end == u->input.n)
                    return;
                used = end + 1;
                char *seq = strndup(p + 2, end - 2 + 1);
                int code = 0;
                if (seq[0] == '<') {
                    int b, x, y;
                    if (sscanf(seq, "<%d;%d;%d", &b, &x, &y) == 3)
                        mouse(u, b, x - 1, y - 1, seq[strlen(seq) - 1] == 'm');
                } else if (!strcmp(seq, "200~")) {
                    u->paste = 1;
                } else if (!strcmp(seq, "A"))
                    code = 1006;
                else if (!strcmp(seq, "B"))
                    code = 1007;
                else if (!strcmp(seq, "C"))
                    code = 1005;
                else if (!strcmp(seq, "D"))
                    code = 1004;
                else if (!strcmp(seq, "H") || !strcmp(seq, "1~") || !strcmp(seq, "7~"))
                    code = 1008;
                else if (!strcmp(seq, "F") || !strcmp(seq, "4~") || !strcmp(seq, "8~"))
                    code = 1009;
                else if (!strcmp(seq, "3~"))
                    code = 1003;
                else if (!strcmp(seq, "5~"))
                    code = 1012;
                else if (!strcmp(seq, "6~"))
                    code = 1013;
                else if (!strcmp(seq, "Z"))
                    code = 1011;
                else if (!strcmp(seq, "11~"))
                    code = 1016;
                else if (!strcmp(seq, "12~"))
                    code = 1017;
                else if (!strcmp(seq, "13~"))
                    code = 1018;
                else if (!strcmp(seq, "14~"))
                    code = 1019;
                else if (!strcmp(seq, "15~"))
                    code = 1015;
                else if (seq[strlen(seq) - 1] == 'u') {
                    int val = 0, mods = 1;
                    if (sscanf(seq, "%d;%d", &val, &mods) >= 1) {
                        if (val == 13)
                            code = (mods & 2) ? 1010 : 13;
                        else if (val == 9)
                            code = (mods & 2) ? 1011 : 9;
                        else if (mods & 4)
                            code = val < 128 ? tolower(val) - 'a' + 1 : 0;
                        else if (val >= 32 && val <= 0x10ffff) {
                            char utf[MB_LEN_MAX];
                            mbstate_t st = {0};
                            size_t n = wcrtomb(utf, (wchar_t)val, &st);
                            if (n != (size_t)-1) {
                                utf[n] = 0;
                                key(u, 0, utf);
                            }
                        }
                    }
                } else if (!strncmp(seq, "27;2;13", 7))
                    code = 1010;
                if (code)
                    key(u, code, NULL);
                free(seq);
            } else if (p[1] == 'O') {
                if (u->input.n < 3)
                    return;
                used = 3;
                int code = p[2] >= 'P' && p[2] <= 'S' ? 1016 + p[2] - 'P'
                           : p[2] == 'H'              ? 1008
                           : p[2] == 'F'              ? 1009
                                                      : 0;
                if (code)
                    key(u, code, NULL);
            } else {
                used = 2;
            }
        } else {
            unsigned char c = p[0];
            if (c < 32 || c == 127) {
                key(u, c, NULL);
                used = 1;
            } else {
                size_t n = c < 128            ? 1
                           : (c & 224) == 192 ? 2
                           : (c & 240) == 224 ? 3
                           : (c & 248) == 240 ? 4
                                              : 1;
                if (u->input.n < n)
                    return;
                char *s = strndup(p, n);
                key(u, c == 32 ? 32 : 0, s);
                free(s);
                used = n;
            }
        }
        if (!used)
            return;
        memmove(u->input.s, u->input.s + used, u->input.n - used);
        u->input.n -= used;
        u->input.s[u->input.n] = 0;
    }
}
static void show_request(UI *u) {
    if (!u->approval || u->modal)
        return;
    if (u->request_kind == A_APPROVE) {
        modal(u, CONFIRM, A_APPROVE, u->request_title, u->request_text);
        return;
    }
    J *p = u->request_params;
    if (!strcmp(gs(p, "mode"), "url")) {
        char *text = fmt("%s\n\nSign-in URL:\n%s\n\nOpen this URL in your browser, then continue.",
                         u->request_text, gs(p, "url"));
        modal(u, CONFIRM, A_ELICIT, u->request_title, text);
        free(text);
        return;
    }
    Modal *m = modal(u, FORM, A_ELICIT, u->request_title, u->request_text);
    J *schema = jg(p, "requestedSchema"), *props = jg(schema, "properties");
    m->value = schema ? jc(schema) : jo();
    for (size_t i = 0; props && i < props->len && i < 64; i++) {
        J *v = props->v[i];
        char *value = jg(v, "default") ? jd(jg(v, "default"), 0) : strdup("");
        if (jg(v, "default") && jg(v, "default")->type == JSTR) {
            free(value);
            value = strdup(gs(v, "default"));
        }
        field_add(m, v->key, *gs(v, "title") ? gs(v, "title") : v->key, value, 0,
                  !strcmp(gs(v, "type"), "number") || !strcmp(gs(v, "type"), "integer") ? 1
                  : !strcmp(gs(v, "type"), "boolean")                                   ? 2
                                                                                        : 0);
        free(value);
    }
    if (!m->count) {
        close_modal(u);
        modal(u, CONFIRM, A_ELICIT, u->request_title, u->request_text);
    }
}
static void finish_job(UI *u) {
    if (!u->finished)
        return;
    pthread_join(u->thread, NULL);
    u->busy = 0;
    u->finished = 0;
    cancelled = 0;
    if (u->modal && (u->modal->action == A_APPROVE || u->modal->action == A_ELICIT))
        close_modal(u);
    if (u->job == 5 && u->jobresult && !u->jobresult->len) {
        message(u, "No models returned",
                "Enter a model manually in Settings, or load a model on the provider.");
    } else if (u->job == 5 && u->jobresult) {
        int selected = 0;
        for (size_t i = 0; i < u->jobresult->len; i++)
            if (!strcmp(jstr(u->jobresult->v[i]), gs(profile(u->config), "model")))
                selected = i;
        pick(u, A_MODEL, "Available models", u->jobresult, selected);
        u->jobresult = NULL;
    } else if (u->job == 8 && u->jobresult) {
        char *text = fmt("%s%sAttachment: %s\n%s", u->composer.s, *u->composer.s ? "\n\n" : "",
                         u->prompt, jstr(u->jobresult));
        es(&u->composer, text);
        free(text);
    }
    jf(u->jobresult);
    u->jobresult = NULL;
    if (u->job == 7) {
        free(u->timer);
        u->timer = timer_status();
    }
    refresh_history(u);
    jf(u->tasklist);
    u->tasklist = tasks();
    if (!u->tasklist)
        u->tasklist = ja();
    u->dirty = 1;
}
static void cleanup_terminal(void) {
    if (!active)
        return; /* Restore keyboard state before leaving the alternate screen. */
    const char *s = "\033[<u\033[>4;0m\033[?2004l\033[?1000l\033[?1006l\033[0m\033[?25h\033[?1049l";
    writeall(1, s, strlen(s));
    tcsetattr(0, TCSANOW, &active->term);
    active = NULL;
}
int tui(J *config) {
    if (!isatty(0) || !isatty(1))
        return fail(
            "Seth needs an interactive terminal (use --check or --run-due for headless runs)");
    UI u = {0};
    u.config = config;
    u.chat = new_chat(config);
    u.expanded = jo();
    u.notice = strdup("");
    u.progress = strdup("");
    u.focus = 100;
    u.following = 1;
    u.history_sel = -1;
    u.composer.multiline = 1;
    es(&u.composer, "");
    char *fallback_name = NULL;
    const char *name = getenv("USER");
    if (!name || !*name)
        name = getenv("LOGNAME");
    if (!name || !*name) {
        name = "User";
        char *users = readfile("/etc/passwd", LIMIT);
        if (users) {
            char *save = NULL;
            for (char *row = strtok_r(users, "\n", &save); row; row = strtok_r(NULL, "\n", &save)) {
                char *colon = strchr(row, ':');
                if (!colon)
                    continue;
                char *uid = strchr(colon + 1, ':');
                if (uid && strtoul(uid + 1, NULL, 10) == (unsigned long)getuid()) {
                    *colon = 0;
                    fallback_name = strdup(row);
                    name = fallback_name;
                    break;
                }
            }
            free(users);
        }
        err[0] = 0;
    }
    u.username = strdup(name);
    free(fallback_name);
    name = u.username;
    mbstate_t st = {0};
    wchar_t first;
    size_t n = mbrtowc(&first, name, strlen(name), &st);
    if (n != (size_t)-1 && n != (size_t)-2 && n) {
        char cap[MB_LEN_MAX];
        memset(&st, 0, sizeof st);
        size_t k = wcrtomb(cap, towupper(first), &st);
        if (k != (size_t)-1) {
            char *v = fmt("%.*s%s", (int)k, cap, name + n);
            free(u.username);
            u.username = v;
        }
    }
    pthread_mutex_init(&u.mutex, NULL);
    pthread_cond_init(&u.condition, NULL);
    mcp_init(&u.mcp, config);
    u.mcp.elicit = elicit_worker;
    u.mcp.opaque = &u;
    u.mcp.mutex = &u.mutex;
    refresh_history(&u);
    u.tasklist = tasks();
    if (!u.tasklist)
        u.tasklist = ja();
    u.timer = timer_status();
    tcgetattr(0, &u.term);
    struct termios raw = u.term;
    cfmakeraw(&raw);
    raw.c_oflag |= OPOST;
    tcsetattr(0, TCSANOW, &raw);
    active = &u;
    atexit(cleanup_terminal);
    signal(SIGWINCH, signals);
    signal(SIGHUP, signals);
    signal(SIGTERM, signals);
    signal(SIGINT, signals);
    const char *init =
        "\033[?1049h\033[?25l\033[?2004h\033[?1000h\033[?1006h\033[>1u\033[>4;2m\033[2J";
    writeall(1, init, strlen(init));
    resize_ui(&u);
    start_job(&u, 4, "");
    double refresh = mono(), inputtime = mono();
    while (!u.quit && !terminated) {
        finish_job(&u);
        if (resized) {
            resized = 0;
            resize_ui(&u);
        }
        if (!pthread_mutex_trylock(&u.mutex)) {
            show_request(&u);
            pthread_mutex_unlock(&u.mutex);
        }
        if (u.dirty)
            render(&u);
        struct pollfd p = {0, POLLIN, 0};
        int r = poll(&p, 1, 40);
        if (r > 0 && p.revents & POLLIN) {
            char buf[8192];
            ssize_t nread = read(0, buf, sizeof buf);
            if (nread <= 0)
                break;
            bput(&u.input, buf, nread);
            consume_input(&u, 0);
            inputtime = mono();
        } else if (u.input.n && mono() - inputtime > .08)
            consume_input(&u, 1);
        if (!u.busy && mono() - refresh > 5) {
            refresh = mono();
            start_job(&u, 9, "");
            J *list = tasks();
            if (list) {
                jf(u.tasklist);
                u.tasklist = list;
            }
            u.dirty = 1;
        }
    }
    cancelled = 1;
    pthread_cond_signal(&u.condition);
    if (u.busy)
        pthread_join(u.thread, NULL);
    cleanup_terminal();
    while (u.modal)
        close_modal(&u);
    mcp_close(&u.mcp);
    jf(u.chat);
    jf(u.history);
    jf(u.tasklist);
    jf(u.expanded);
    jf(u.jobresult);
    jf(u.request_params);
    jf(u.response);
    free(u.prompt);
    free(u.notice);
    free(u.progress);
    free(u.username);
    free(u.request_title);
    free(u.request_text);
    free(u.timer);
    free(u.composer.s);
    free(u.cells);
    free(u.old);
    bfree(&u.partial);
    bfree(&u.input);
    bfree(&u.pasted);
    lines_free(&u.transcript);
    pthread_mutex_destroy(&u.mutex);
    pthread_cond_destroy(&u.condition);
    return 0;
}
