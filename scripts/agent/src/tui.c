#include "seth.h"
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* The terminal renderer and editor need only libc. All coordinates are cells;
 * text positions are UTF-8 byte offsets. No escape from remote text is emitted. */
enum { NORMAL, MUTED, ACCENT, SURFACE, SELECTED, BORDER, FOCUS, CODE, HOVER, ERROR };
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
    A_MEMORY_EDIT,
    A_MEMORY_DELETE,
    A_PROFILE,
    A_PROVIDER,
    A_MODEL,
    A_LIMITS,
    A_WORKSPACE,
    A_CHAT_WORKSPACE,
    A_PROMPT,
    A_PERMISSION,
    A_TIMER,
    A_APPROVE,
    A_ELICIT,
    A_SETTINGS_AUTO
};
typedef struct Modal {
    int kind, action, selected, scroll, count, focus;
    char *title, *text, *id;
    Field fields[64];
    J *choices, *value;
    struct Modal *parent;
} Modal;
typedef struct {
    J *config, *chat, *history, *tasklist, *expanded, *jobresult, *mcp_config, *memories;
    MCP mcp;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int busy, job, result, tab, focus, history_sel, server_sel, task_sel, memory_sel, chat_scroll,
        detail_scroll, following, chat_max_scroll, quit, w, h, paste, memory_reload;
    atomic_int finished, dirty, approval;
    char *prompt, *notice, *progress, *username, *request_title, *request_text, *timer, *memory_error,
         *memory_target;
    J *request_params, *response, *images, *jobimages, *steering;
    unsigned image_number;
    int request_kind;
    Buf partial, input, pasted;
    Editor composer;
    Modal *modal, *settings, *settings_drafts[3];
    int settings_section;
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
static const char *spinner(void) {
    static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return frames[(int)(mono() * 10) % 10];
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
    for (size_t i = 0; m && i <= m->len; i++) {
        if (i == (size_t)gn(u->chat, "compacted", 0) && *gs(u->chat, "summary")) {
            line(&u->transcript, "", NORMAL, 0, NULL);
            line(&u->transcript, "Context summary", ACCENT, 0, NULL);
            text_lines(&u->transcript, gs(u->chat, "summary"), MUTED, 0);
            speaker = 0;
        }
        if (i == m->len)
            break;
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
            J *stats = jg(v, "responseStats");
            if (stats) {
                char *label = jg(stats, "outputTokens")
                    ? fmt("%s in %.2f s · %.1f TPS", gs(stats, "status"),
                          gn(stats, "elapsedSeconds", 0),
                          gn(stats, "outputTokens", 0) / gn(stats, "providerSeconds", 1))
                    : fmt("%s in %.2f s · TPS unavailable", gs(stats, "status"),
                          gn(stats, "elapsedSeconds", 0));
                line(&u->transcript, label, MUTED, 0, NULL);
                free(label);
            }
            if (*content || (calls && calls->len))
                speaker = 2;
            for (size_t k = 0; calls && k < calls->len; k++) {
                J *call = calls->v[k], *f = jg(call, "function");
                char *key = fmt("%s:%zu:%zu", gs(u->chat, "id"), i, k);
                J *r = jg(results, key);
                const char *result = jstr(r), *status = !r ? u->busy ? spinner() : "interrupted"
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
    for (size_t i = 0; u->steering && i < u->steering->len; i++) {
        line(&u->transcript, "", NORMAL, 0, NULL);
        line(&u->transcript, "Queued steering message", ACCENT, 0, NULL);
        text_lines(&u->transcript, gs(u->steering->v[i], "content"), NORMAL, 0);
    }
    if (u->partial.n) {
        if (speaker != 2) {
            line(&u->transcript, "", NORMAL, 0, NULL);
            line(&u->transcript, "Seth", ACCENT, 0, NULL);
        } else
            line(&u->transcript, "", NORMAL, 0, NULL);
        text_lines(&u->transcript, u->partial.s, NORMAL, 1);
    }
    if (!u->transcript.len)
        text_lines(&u->transcript,
                   "Seth · your local assistant\n\nChoose a provider and model in Settings, then "
                   "send a message.\n\nFiles and shell commands use this chat's workspace, shown "
                   "under Actions.\nUse /workspace or click the path to change it.\nWeb search "
                   "and file tools are enabled by default.",
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
static void padded_button(UI *u, int x, int y, const char *label, int id, int padding) {
    int w = (int)strlen(label) + 2 * padding;
    if (x + w > u->w - 1)
        return;
    int color = u->focus == id ? HOVER : SELECTED;
    fill(u, x, y, w, 1, color);
    draw_text(u, x + padding, y, w - 2 * padding, label, color, 0, 0);
    hit(u, x, y, w, 1, id, 0);
}
static void button(UI *u, int x, int y, const char *label, int id) {
    padded_button(u, x, y, label, id, 2);
}
static void icon_button(UI *u, int x, int y, wchar_t icon, int id) {
    int color = u->focus == id ? HOVER : SELECTED;
    fill(u, x, y, 3, 1, color);
    cell(u, x + 1, y, icon, color, 0);
    hit(u, x, y, 3, 1, id, 0);
}
static int actions(UI *u, int x, int y, int width, const char *const *labels, const int *ids,
                   int count, int padding) {
    box(u, x, y, width, 3, "Actions", 0);
    int left = x + 2;
    for (int i = 0; i < count; i++) {
        padded_button(u, left, y + 1, labels[i], ids[i], padding);
        left += (int)strlen(labels[i]) + 3 * padding;
    }
    return left;
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
static int view_lines(UI *u, Lines *l, int x, int y, int width, int height, int *scroll,
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
    return max;
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
    int marker = 0;
    for (int row = 0; row < w.len && row < first + height; row++) {
        const char *text = w.v[row].s;
        if (row >= first)
            draw_text(u, x, y + row - first, width, text, NORMAL, 0, 0);
        int col = 0;
        mbstate_t state = {0};
        for (size_t p = 0; text[p];) {
            if (e == &u->composer && !strncmp(text + p, "[Pasted image ", 14))
                marker = 1;
            wchar_t c;
            size_t n = mbrtowc(&c, text + p, strlen(text + p), &state);
            if (n == (size_t)-1 || n == (size_t)-2) {
                n = 1;
                c = L'�';
                memset(&state, 0, sizeof state);
            }
            if (marker && row >= first && col < width)
                u->cells[(y + row - first) * u->w + x + col].color = ACCENT;
            int cw = wcwidth(c);
            col += cw > 0 ? cw : 1;
            if (c == L']')
                marker = 0;
            p += n;
        }
    }
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
/* Settings use the same field/editor model as forms, without a modal overlay. */
static void settings_load(UI *u, int section) {
    modal_free(u->settings_drafts[section]);
    Modal *m = calloc(1, sizeof *m);
    u->settings = u->settings_drafts[section] = m;
    u->settings_section = section;
    m->kind = FORM;
    if (section == 0) {
        J *p = profile(u->config);
        field_add(m, "endpoint", "OpenAI-compatible endpoint", gs(p, "endpoint"), 0, 0);
        field_add(m, "model", "Model ID", gs(p, "model"), 0, 0);
        field_add(m, "apiKey", "API key", gs(p, "apiKey"), 0, 0);
        m->fields[2].e.secret = 1;
        field_add(m, "apiKeyEnv", "API key environment variable", gs(p, "apiKeyEnv"), 0, 0);
        field_add(m, "vision", "Vision for this model", model_vision(u->config) ? "true" : "false", 0, 4);
    } else if (section == 1) {
        field_add(m, "workspace", "Default workspace (new chats only)", gs(u->config, "workspace"), 0, 0);
        field_add(m, "permissions", "Tool permissions: ask / read-only / auto", gs(u->config, "permissions"), 0, 5);
        const char *keys[] = {"contextWindow", "maxTokens", "maxSteps", "timeout"};
        const char *labels[] = {"Context window (tokens)", "Maximum output tokens", "Maximum tool rounds", "Request timeout (seconds)"};
        for (int i = 0; i < 4; i++) {
            char *v = fmt("%.0f", gn(u->config, keys[i], 0));
            field_add(m, keys[i], labels[i], v, 0, 1);
            free(v);
        }
    } else
        field_add(m, "systemPrompt", "System instructions", gs(u->config, "systemPrompt"), 1, 0);
    u->focus = 520;
    u->dirty = 1;
}
static Editor *settings_editor(UI *u) {
    int i = u->focus - 520;
    return !u->modal && u->tab == 4 && u->settings && i >= 0 && i < u->settings->count &&
                   u->settings->fields[i].kind != 4 && u->settings->fields[i].kind != 5
               ? &u->settings->fields[i].e : NULL;
}
static void settings_toggle(Field *f, int backwards) {
    if (f->kind == 4)
        es(&f->e, !strcmp(f->e.s, "true") ? "false" : "true");
    else {
        const char *modes[] = {"ask", "read-only", "auto"};
        int i = !strcmp(f->e.s, "read-only") ? 1 : !strcmp(f->e.s, "auto") ? 2 : 0;
        es(&f->e, modes[(i + (backwards ? 2 : 1)) % 3]);
    }
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
              "quits\nF1–F5 views · F6 help · ? help outside text or empty composer\nCtrl+V pastes clipboard images · Ctrl+N "
              "creates a chat · click tool calls to open their details\nMouse wheel / PgUp / PgDn "
              "scroll the conversation\n\n/new        Start a new chat\n/fork       Branch this "
              "conversation\n/continue   Continue the current task\n/retry      Branch before the "
              "last request and retry\n/compact    Summarize old context, retaining full "
              "history\n/workspace  Choose a workspace for this chat only\n/attach PATH  Add a "
              "workspace text file through MCP\n/export     Export "
              "the complete transcript as Markdown\n/events     Show activity and errors\n/help    "
              "   Open this help\n\nHistory: \uf067 New · \uf002 Find · \uf040 Rename · \uf1f8 Delete\n"
              "One click opens a saved chat. Find, Rename and "
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
static void start_job(UI *, int, const char *);
static void fresh(UI *u) {
    if (u->busy)
        return;
    while (u->steering->len)
        jremove(u->steering, 0);
    jf(u->chat);
    u->chat = new_chat(u->config);
    u->history_sel = -1;
    u->chat_scroll = 0;
    u->following = 1;
    es(&u->composer, "");
    jf(u->images);
    u->images = NULL;
    bfree(&u->partial);
    notice_ui(u, "");
    u->tab = 0;
    u->focus = 100;
    if (strcmp(gs(u->mcp.config, "workspace"), gs(u->chat, "workspace")))
        start_job(u, 4, "");
}
static void open_chat(UI *u, const char *id) {
    if (u->busy)
        return;
    int reconnect = 0;
    J *chat = load_chat(id);
    if (!chat) {
        error_ui(u);
        return;
    }
    while (u->steering->len)
        jremove(u->steering, 0);
    reconnect = strcmp(gs(chat, "workspace"), gs(u->mcp.config, "workspace")) != 0;
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
static J *memory_request(UI *u, const char *name, J *args) {
    Tool *tool = NULL;
    for (size_t i = 0; i < u->mcp.toolcount; i++) {
        Tool *t = u->mcp.tools[i];
        if (!strcmp(gs(t->conn->config, "builtin"), "memory") && !strcmp(t->name, name)) {
            tool = t;
            break;
        }
    }
    if (!tool) {
        fail("Enable the memory server and its %s tool in MCP servers", name);
        return NULL;
    }
    J *params = jo();
    jset(params, "name", js(name));
    jset(params, "arguments", jc(args));
    J *reply = mcp_request(&u->mcp, tool->conn, "tools/call", params,
                          (int)gn(u->config, "timeout", 180));
    jf(params);
    if (!reply)
        return NULL;
    const char *text = gs(ji(jg(reply, "content"), 0), "text");
    J *result = gb(reply, "isError", 0) ? NULL : jp(text, NULL);
    if (!result)
        fail("%s", *text ? text : "Invalid memory response");
    jf(reply);
    return result;
}
static J *memory_list(UI *u) {
    J *rows = ja(), *args = jo();
    jset(args, "limit", jnum(100));
    for (int offset = 0; offset < 10000; offset += 100) {
        jset(args, "offset", jnum(offset));
        J *page = memory_request(u, "list_memories", args);
        if (!page || page->type != JARR) {
            if (page)
                fail("Invalid memory list");
            jf(page);
            jf(args);
            jf(rows);
            return NULL;
        }
        size_t count = page->len;
        for (size_t i = 0; i < count; i++)
            jadd(rows, jc(page->v[i]));
        jf(page);
        if (count < 100)
            break;
    }
    jf(args);
    return rows;
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
               .opaque = u, .images = u->jobimages, .steering = u->steering};
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
        J *config = jc(u->config);
        jset(config, "workspace", js(gs(u->chat, "workspace")));
        mcp_init(&nextm, config);
        nextm.elicit = elicit_worker;
        nextm.opaque = u;
        nextm.mutex = &u->mutex;
        int failed = mcp_connect(&nextm);
        pthread_mutex_lock(&u->mutex);
        mcp_close(&u->mcp);
        jf(u->mcp_config);
        u->mcp_config = config;
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
    case 9: {
        char *status = timer_status();
        jobresult = js(status);
        free(status);
        result = mcp_refresh(&u->mcp);
        break;
    }
    case 10:
        jobresult = memory_list(u);
        result = jobresult ? 0 : -1;
        break;
    case 11: {
        J *request = jp(u->prompt, NULL);
        jobresult = request ? memory_request(u, gs(request, "name"), jg(request, "arguments"))
                            : NULL;
        jf(request);
        result = jobresult ? 0 : -1;
        break;
    }
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
    jset(u->mcp.config, "timeout", jc(jg(u->config, "timeout")));
    cancelled = 0;
    free(u->prompt);
    u->prompt = strdup(text ? text : "");
    jf(u->jobimages);
    u->jobimages = job == 1 ? jc(u->images) : NULL;
    u->job = job;
    u->busy = 1;
    u->finished = 0;
    free(u->progress);
    u->progress = strdup(job == 9   ? (u->notice && *u->notice ? u->notice : "Ready")
                         : job == 10 ? "Loading memories…"
                         : job == 11 ? "Updating memory…"
                         : job == 4 ? "Connecting MCP servers…"
                         : job == 5 ? "Discovering models…"
                         : job == 6 ? "Running scheduled task…"
                                    : "Working…");
    if (job == 1 || job == 2) {
        free(u->notice);
        u->notice = strdup("");
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
    field_add(m, "vision", "Vision for this model", model_vision(u->config) ? "true" : "false", 0, 4);
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
static void prune_images(UI *u) {
    for (size_t i = 0; u->images && i < u->images->len;) {
        if (!strstr(u->composer.s, gs(u->images->v[i], "label")))
            jremove(u->images, i);
        else
            i++;
    }
}
/* Return true when an image paste was handled, including rejected pastes. */
static int paste_image(UI *u) {
    if (u->busy)
        return 0;
    char *url = clipboard_image();
    if (!url)
        return 0;
    if (!*url)
        notice_ui(u, err);
    else if (!model_vision(u->config))
        notice_ui(u, "Warning: this model has no confirmed vision support; image not pasted. Enable vision for this model in Provider settings if supported.");
    else {
        prune_images(u);
        if (!u->images)
            u->images = ja();
        if (u->images->len >= 4) {
            notice_ui(u, "Maximum four images per message");
            free(url);
            return 1;
        }
        char *label = fmt("[Pasted image %u]", ++u->image_number);
        J *image = jo();
        jset(image, "label", js(label));
        jset(image, "url", js(url));
        jadd(u->images, image);
        insert(&u->composer, label, strlen(label));
        free(label);
    }
    free(url);
    u->dirty = 1;
    return 1;
}
static void submit(UI *u) {
    if (u->busy && u->job != 1 && u->job != 2 && u->job != 3)
        return;
    prune_images(u);
    if (u->images && u->images->len && !model_vision(u->config)) {
        notice_ui(u, "Warning: selected model has no confirmed vision support; remove images or choose a vision model.");
        return;
    }
    char *text = strdup(u->composer.s);
    if (!*text) {
        free(text);
        return;
    }
    if (u->busy) {
        J *msg = jo();
        jset(msg, "role", js("user"));
        jset(msg, "content", js(text));
        if (u->images && u->images->len)
            jset(msg, "images", jc(u->images));
        pthread_mutex_lock(&u->mutex);
        jadd(u->steering, msg);
        pthread_mutex_unlock(&u->mutex);
        es(&u->composer, "");
        jf(u->images);
        u->images = NULL;
        notice_ui(u, "Steering message queued for the next model turn");
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
        else if (!strcmp(text, "/workspace"))
            dispatch(u, 116, 0);
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
    jf(u->images);
    u->images = NULL;
    free(text);
}
static J *form_value(Modal *m) {
    J *v = jo();
    for (int i = 0; i < m->count; i++) {
        Field *f = &m->fields[i];
        if (m->action == A_ELICIT && !f->e.s[0] && !contains(jg(m->value, "required"), f->key))
            continue;
        J *value = NULL;
        if (f->kind == 4)
            value = jb(!strcmp(f->e.s, "true"));
        else if (f->kind == 1) {
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
static void settings_save(UI *u, int confirmed) {
    if (!u->settings || u->busy) {
        notice_ui(u, "Wait for the active operation before saving settings.");
        return;
    }
    J *v = form_value(u->settings);
    if (!v) {
        error_ui(u);
        return;
    }
    if (u->settings_section == 1 && !strcmp(gs(v, "permissions"), "auto") &&
        strcmp(gs(u->config, "permissions"), "auto") && !confirmed) {
        jf(v);
        modal(u, CONFIRM, A_SETTINGS_AUTO, "Enable Auto",
              "Allow tools to act without asking, including shell commands and external MCP tools? "
              "This mode is saved until changed in Settings.");
        return;
    }
    J *copy = jc(u->config), *target = u->settings_section == 0 ? profile(copy) : copy;
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(v->v[i]->key, "vision"))
            jset(target, v->v[i]->key, jc(v->v[i]));
    if (u->settings_section == 0) {
        if (!jg(target, "visionModels"))
            jset(target, "visionModels", jo());
        jset(jg(target, "visionModels"), gs(target, "model"), jc(jg(v, "vision")));
    }
    jf(v);
    if (save_config(copy)) {
        jf(copy);
        error_ui(u);
        return;
    }
    jf(u->config);
    u->config = copy;
    settings_load(u, u->settings_section);
    notice_ui(u, "Settings saved.");
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
    if ((m->action == A_MEMORY_EDIT || m->action == A_MEMORY_DELETE) && u->busy)
        return;
    if (m->action == A_SETTINGS_AUTO) {
        close_modal(u);
        settings_save(u, 1);
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
    case A_MEMORY_EDIT:
    case A_MEMORY_DELETE: {
        J *args = v ? jc(v) : jo(), *request = jo();
        if (m->id && *m->id) {
            jset(args, "id", js(m->id));
            jset(args, "expected_content", js(gs(m->value, "content")));
            jset(args, "expected_title", js(gs(m->value, "title")));
        }
        jset(request, "name", js(action == A_MEMORY_EDIT ? "store_memory" : "delete_memory"));
        jset(request, "arguments", args);
        char *text = jd(request, 0);
        start_job(u, 11, text);
        free(text);
        jf(request);
        free(selected);
        jf(v);
        return;
    }
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
            if (strcmp(v->v[i]->key, "vision"))
                jset(p, v->v[i]->key, jc(v->v[i]));
        if (!jg(p, "visionModels"))
            jset(p, "visionModels", jo());
        jset(jg(p, "visionModels"), gs(p, "model"), jc(jg(v, "vision")));
        r = save_config(u->config);
        if (r) {
            jf(u->config);
            u->config = copy;
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
        } else
            jf(copy);
        break;
    }
    case A_CHAT_WORKSPACE: {
        char path[PATH_MAX];
        struct stat st;
        const char *workspace = gs(v, "workspace");
        if (*workspace != '/' || !realpath(workspace, path) || stat(path, &st) ||
            !S_ISDIR(st.st_mode)) {
            r = fail("Workspace must be an existing absolute directory");
            break;
        }
        J *chat = jc(u->chat);
        jset(chat, "workspace", js(path));
        jdel(chat, "guidance");
        r = save_chat(chat);
        if (r)
            jf(chat);
        else {
            jf(u->chat);
            u->chat = chat;
            reconnect = strcmp(path, gs(u->mcp.config, "workspace")) != 0;
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
    if (action == A_PROFILE || action == A_MODEL || action == A_PROVIDER)
        settings_load(u, 0);
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
    if (id >= 10 && id < 16) {
        if (id == 15)
            help(u);
        else if (!u->modal) {
            u->tab = id - 10;
            u->detail_scroll = 0;
            u->focus = u->tab == 0 ? 100 : u->tab == 1 ? 200 : u->tab == 2 ? 400
                         : u->tab == 3 ? 600 : 500;
            u->dirty = 1;
            if (u->tab == 3) {
                u->memory_reload = u->busy;
                if (!u->busy)
                    start_job(u, 10, "");
            }
        }
        return;
    }
    if (id >= 510 && id <= 512) {
        int section = id - 510;
        if (!u->settings_drafts[section])
            settings_load(u, section);
        else {
            u->settings = u->settings_drafts[section];
            u->settings_section = section;
            u->focus = 520 + u->settings->focus;
        }
        return;
    }
    if (id == 530) {
        settings_save(u, 0);
        return;
    }
    if (id == 531) {
        settings_load(u, u->settings_section);
        notice_ui(u, "Unsaved edits discarded.");
        return;
    }
    if (id >= 520 && id < 526 && u->settings) {
        Field *f = &u->settings->fields[id - 520];
        if (id - 520 < u->settings->count && (f->kind == 4 || f->kind == 5))
            settings_toggle(f, 0);
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
    if (id == 601) {
        u->memory_sel = index;
        u->focus = 600;
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
    case 116: {
        Modal *m = modal(u, FORM, A_CHAT_WORKSPACE, "Chat workspace",
                         "Applies only to this chat. New chats use the default in Settings.");
        field_add(m, "workspace", "Existing absolute directory", gs(u->chat, "workspace"), 0, 0);
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
            jf(u->images);
            u->images = jc(jg(m->v[n - 1], "images"));
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
            jf(u->images);
            u->images = NULL;
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
    case 602:
    case 603:
    case 604: {
        J *memory = ji(u->memories, u->memory_sel);
        if (!memory)
            break;
        if (id == 602) {
            char *text = fmt("%s\n\n%s", gs(memory, "title"), gs(memory, "content"));
            message(u, "Read memory", text);
            free(text);
        } else {
            Modal *m = modal(u, id == 603 ? FORM : CONFIRM,
                             id == 603 ? A_MEMORY_EDIT : A_MEMORY_DELETE,
                             id == 603 ? "Edit memory" : "Delete memory",
                             "Permanently delete this saved memory?");
            m->id = strdup(gs(memory, "id"));
            m->value = jc(memory);
            if (id == 603) {
                field_add(m, "title", "Title", gs(memory, "title"), 0, 0);
                field_add(m, "content", "Memory", gs(memory, "content"), 1, 0);
            }
        }
        break;
    }
    case 605:
        start_job(u, 10, "");
        break;
    case 606: {
        Modal *m = modal(u, FORM, A_MEMORY_EDIT, "New memory", "");
        field_add(m, "title", "Title", "", 0, 0);
        field_add(m, "content", "Memory", "", 1, 0);
        break;
    }
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
        Modal *m = modal(u, FORM, A_WORKSPACE, "Default workspace",
                         "Used for new chats. Change the current chat with /workspace.");
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
        int hint = m->action == A_CHAT_WORKSPACE || m->action == A_WORKSPACE;
        int focused = m->focus < m->count ? m->focus : m->count - 1;
        int space = height - 6 - (hint ? 2 : 0);
        int start = 0, total = 0;
        for (int i = 0; i <= focused; i++)
            total += m->fields[i].e.multiline ? 7 : 4;
        while (total > space && start < focused) {
            total -= m->fields[start].e.multiline ? 7 : 4;
            start++;
        }
        int row = y + 2;
        if (hint) {
            draw_text(u, x + 3, row, inner, m->text, MUTED, 0, 0);
            row += 2;
        }
        for (int i = start; i < m->count; i++) {
            Field *f = &m->fields[i];
            int h = f->e.multiline ? 6 : 3;
            if (row + h > bottom)
                break;
            box(u, x + 2, row, width - 4, h, f->label, m->focus == i);
            if (f->kind == 4)
                draw_text(u, x + 3, row + 1, width - 6,
                          !strcmp(f->e.s, "true") ? "[●] On" : "[ ] Off",
                          m->focus == i ? ACCENT : NORMAL, 0, 0);
            else
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
                              : (m->action == A_PERMISSION || m->action == A_SETTINGS_AUTO) ? "Enable Auto"
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
    int toolbar_x = (side - (4 * 3 + 3 * 2)) / 2;
    /* Nerd Font Font Awesome: plus, search, pencil, trash. */
    icon_button(u, toolbar_x, 5, L'\uf067', 101);
    icon_button(u, toolbar_x + 5, 5, L'\uf002', 103);
    icon_button(u, toolbar_x + 10, 5, L'\uf040', 104);
    icon_button(u, toolbar_x + 15, 5, L'\uf1f8', 110);
    int rows = body - 5, start = u->history_sel >= rows ? u->history_sel - rows + 1 : 0;
    for (int i = 0; i < rows && u->history && i + start < (int)u->history->len; i++) {
        int index = i + start, c = index == u->history_sel ? SELECTED : NORMAL;
        fill(u, 2, 7 + i, side - 4, 1, c);
        draw_text(u, 3, 7 + i, side - 6, gs(u->history->v[index], "title"), c, 0, 0);
        hit(u, 2, 7 + i, side - 4, 1, 102, index);
    }
    int x = side, width = u->w - side;
    int logheight = u->h - 13;
    if (logheight < 5)
        logheight = 5;
    box(u, x, 3, width, logheight, gs(u->chat, "title"), u->focus == 108);
    transcript(u);
    hit(u, x + 1, 4, width - 2, logheight - 2, 108, 0);
    u->chat_max_scroll = view_lines(u, &u->transcript, x + 2, 5, width - 4,
                                    logheight - 4, &u->chat_scroll, u->following, 1);
    u->following = u->chat_scroll == u->chat_max_scroll;
    int cy = 3 + logheight;
    box(u, x, cy, width, 6, "Message", u->focus == 100);
    editor_draw(u, &u->composer, x + 2, cy + 1, width - 4, 4, u->focus == 100);
    hit(u, x + 1, cy + 1, width - 2, 4, 100, 0);
    const char *labels[] = {"Send", "Stop", "Compact", "Export"};
    int ids[] = {105, 106, 107, 109};
    int padding = width < 70 ? 1 : 2;
    int left = actions(u, x, cy + 6, width, labels, ids, 4, padding);
    int color = u->focus == 116 ? HOVER : SELECTED;
    char *workspace = fmt("Workspace: %s", gs(u->chat, "workspace"));
    int available = x + width - 2 - left;
    int w = (int)strlen(workspace) + 2 * padding;
    if (w > available)
        w = available;
    fill(u, left, cy + 7, w, 1, color);
    draw_text(u, left + padding, cy + 7, w - 2 * padding, workspace, color, 0, 0);
    if ((int)strlen(workspace) > w - 2 * padding)
        cell(u, left + w - padding - 1, cy + 7, L'…', color, 0);
    free(workspace);
    hit(u, left, cy + 7, w, 1, 116, 0);
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
    actions(u, 0, u->h - 4, u->w, labels, ids, 7, 2);
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
    actions(u, 0, u->h - 4, u->w, labels, ids, 7, 2);
}
static void draw_memories(UI *u) {
    int side = u->w * 32 / 100, height = u->h - 7;
    if (side < 24)
        side = 24;
    box(u, 0, 3, side, height, "Memories", u->focus == 600);
    box(u, side, 3, u->w - side, height, "Memory details", u->focus == 608);
    hit(u, 1, 4, side - 2, height - 2, 600, 0);
    hit(u, side + 1, 4, u->w - side - 2, height - 2, 608, 0);
    int count = u->memories ? (int)u->memories->len : 0;
    if (u->memory_sel >= count)
        u->memory_sel = count ? count - 1 : 0;
    int rows = height - 4, start = u->memory_sel >= rows ? u->memory_sel - rows + 1 : 0;
    for (int i = 0; i < rows && i + start < count; i++) {
        int index = i + start, color = index == u->memory_sel ? SELECTED : NORMAL;
        J *memory = u->memories->v[index];
        fill(u, 2, 5 + i, side - 4, 1, color);
        draw_text(u, 3, 5 + i, side - 6,
                  *gs(memory, "title") ? gs(memory, "title") : gs(memory, "content"), color, 0, 0);
        hit(u, 2, 5 + i, side - 4, 1, 601, index);
    }
    Lines lines = {0};
    J *memory = ji(u->memories, u->memory_sel);
    if (u->memory_error && *u->memory_error)
        text_lines(&lines, u->memory_error, MUTED, 0);
    else if (memory) {
        line(&lines, *gs(memory, "title") ? gs(memory, "title") : "Untitled memory", ACCENT, 1, NULL);
        char *meta = fmt("Created: %s\nUpdated: %s\n", gs(memory, "created"), gs(memory, "updated"));
        text_lines(&lines, meta, MUTED, 0);
        free(meta);
        text_lines(&lines, gs(memory, "content"), NORMAL, 0);
    } else
        line(&lines, u->busy && u->job == 10 ? "Loading memories…" : "No saved memories.",
             MUTED, 0, NULL);
    view_lines(u, &lines, side + 2, 5, u->w - side - 4, height - 4, &u->detail_scroll, 0, 0);
    lines_free(&lines);
    const char *labels[] = {"New", "Read", "Edit", "Delete", "Refresh"};
    int ids[] = {606, 602, 603, 604, 605};
    actions(u, 0, u->h - 4, u->w, labels, ids, 5, 2);
}
static void draw_settings(UI *u) {
    if (!u->settings)
        settings_load(u, 0);
    Modal *m = u->settings;
    button(u, 2, 3, "Connection", 510);
    button(u, 18, 3, "Agent", 511);
    button(u, 29, 3, "Instructions", 512);
    const char *titles[] = {"Connection", "Agent", "Instructions"};
    box(u, 0, 5, u->w, u->h - 7, titles[u->settings_section], 0);
    draw_text(u, 3, 6, u->w - 6, "Edit inline · Tab moves focus · Ctrl+S saves · Escape discards", MUTED, 0, 0);
    int bottom = u->h - 5, row = 8, start = 0;
    int focused = u->focus >= 520 && u->focus < 520 + m->count ? u->focus - 520 : m->focus;
    m->focus = focused;
    int visible = (bottom - row) / 4;
    if (visible < 1)
        visible = 1;
    if (focused >= visible)
        start = focused - visible + 1;
    for (int i = start; i < m->count; i++) {
        Field *f = &m->fields[i];
        int height = f->e.multiline ? bottom - row : 3;
        if (height < 3 || row + height > bottom)
            break;
        box(u, 2, row, u->w - 4, height, f->label, u->focus == 520 + i);
        if (f->kind == 4 || f->kind == 5)
            draw_text(u, 3, row + 1, u->w - 6,
                      f->kind == 4 ? (!strcmp(f->e.s, "true") ? "[x] On" : "[ ] Off") : f->e.s,
                      ACCENT, 0, 0);
        else
            editor_draw(u, &f->e, 3, row + 1, u->w - 6, height - 2, u->focus == 520 + i);
        hit(u, 2, row, u->w - 4, height, 520 + i, i);
        row += height + 1;
    }
    button(u, 3, u->h - 4, "Save", 530);
    button(u, 15, u->h - 4, "Discard", 531);
    if (u->settings_section == 0) {
        button(u, 32, u->h - 4, "Profile", 501);
        button(u, 47, u->h - 4, "Discover / Test", 503);
    }
    draw_text(u, 3, u->h - 3, u->w - 6,
              "Save applies this section. Switching sections or views keeps edits until discarded.", MUTED, 0, 0);
}
static const char *styles[] = {
    "\033[38;2;255;255;255m\033[48;2;34;34;38m", "\033[38;2;170;170;170m\033[48;2;34;34;38m",
    "\033[38;2;255;190;111m\033[48;2;34;34;38m", "\033[38;2;255;255;255m\033[48;2;56;56;60m",
    "\033[38;2;34;34;38m\033[48;2;255;190;111m", "\033[38;2;170;170;170m\033[48;2;34;34;38m",
    "\033[38;2;255;190;111m\033[48;2;34;34;38m", "\033[38;2;255;255;255m\033[48;2;56;56;60m",
    "\033[38;2;34;34;38m\033[48;2;255;163;72m",
    "\033[38;2;246;97;81m\033[48;2;34;34;38m"};
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
    const char *tabs[] = {"F1 Chat", "F2 MCP servers", "F3 Automation", "F4 Memory",
                         "F5 Settings", "F6 Help (?)"};
    const char *short_tabs[] = {"F1 Chat", "F2 MCP", "F3 Auto", "F4 Memory", "F5 Settings", "F6 Help"};
    int padding = u->w < 110 ? 1 : 2;
    int x = 2;
    for (int i = 0; i < 6; i++) {
        const char *label = u->w < 110 ? short_tabs[i] : tabs[i];
        int w = strlen(label) + 2 * padding;
        int color =
            ((u->modal && u->modal->action == 999) ? i == 5 : u->tab == i) ? SELECTED : SURFACE;
        fill(u, x, 1, w, 1, color);
        draw_text(u, x + padding, 1, w - 2 * padding, label, color, 0, 0);
        hit(u, x, 1, w, 1, 10 + i, 0);
        x += w + padding;
    }
    if (u->w < 80 || u->h < (u->tab == 4 ? 20 : 30))
        draw_text(u, 2, 5, u->w - 4, "Resize the terminal to at least 80 columns × 30 rows.",
                  ACCENT, 0, 0);
    else if (u->tab == 0)
        draw_chat(u);
    else if (u->tab == 1)
        draw_mcp(u);
    else if (u->tab == 2)
        draw_tasks(u);
    else if (u->tab == 3)
        draw_memories(u);
    else
        draw_settings(u);
    int working = u->busy && u->job != 9 && u->job != 10;
    char *status = fmt(
        "%s%s%s  %s: %s · %s · Context ≈ %zu / %.0f",
        working ? spinner() : "", working ? " " : "",
        working                   ? u->progress && *u->progress ? u->progress : "Working…"
        : u->notice && *u->notice ? u->notice
                                  : "Ready",
        gs(profile(u->config), "label"),
        *gs(profile(u->config), "model") ? gs(profile(u->config), "model") : "choose a model",
        gs(u->config, "permissions"), active_estimate(u->chat), gn(u->config, "contextWindow", 0));
    draw_text(u, 1, u->h - 1, u->w - 2, status, working ? ACCENT : u->notice && !strncmp(u->notice, "Error:", 6) ? ERROR : MUTED, 0, 0);
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
static void scroll_chat(UI *u, int change) {
    u->chat_scroll += change;
    if (u->chat_scroll < 0)
        u->chat_scroll = 0;
    if (u->chat_scroll > u->chat_max_scroll)
        u->chat_scroll = u->chat_max_scroll;
    u->following = u->chat_scroll == u->chat_max_scroll;
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
        } else if (u->tab == 4 && u->settings) {
            int i = u->settings->focus + (down ? 1 : -1);
            if (i < 0) i = 0;
            if (i >= u->settings->count) i = u->settings->count - 1;
            u->focus = 520 + i;
        } else if (u->tab == 0) {
            scroll_chat(u, down ? 3 : -3);
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
            if (h->id == 800) {
                m->focus = h->index;
                Field *f = &m->fields[m->focus];
                if (f->kind == 4)
                    es(&f->e, !strcmp(f->e.s, "true") ? "false" : "true");
            }
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
        if (id >= 3000 || id == 301 || id == 401 || id == 601 || id < 100)
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
    if (code == 1021) {
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
            if (m->focus < m->count && m->fields[m->focus].kind == 4) {
                Editor *e = &m->fields[m->focus].e;
                if (code == 13 || code == 32 || code == 1004 || code == 1005)
                    es(e, !strcmp(e->s, "true") ? "false" : "true");
                return;
            }
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
    if (code >= 1016 && code <= 1020) {
        dispatch(u, 10 + code - 1016, 0);
        return;
    }
    if (u->tab == 4 && u->settings) {
        if (code == 1012 || code == 1013) {
            int i = u->settings->focus + (code == 1012 ? -1 : 1);
            if (i < 0) i = 0;
            if (i >= u->settings->count) i = u->settings->count - 1;
            u->focus = 520 + i;
            return;
        }
        if (code == 19) {
            settings_save(u, 0);
            return;
        }
        if (code == 27) {
            settings_load(u, u->settings_section);
            notice_ui(u, "Unsaved edits discarded.");
            return;
        }
        int i = u->focus - 520;
        if (i >= 0 && i < u->settings->count) {
            Field *f = &u->settings->fields[i];
            if (code == 9 || code == 1011) {
                u->focus = code == 1011 ? (i ? 520 + i - 1 : 510) :
                           i + 1 < u->settings->count ? 520 + i + 1 : 530;
            } else if (f->kind == 4 || f->kind == 5) {
                if (code == 13 || code == 32 || code == 1004 || code == 1005)
                    settings_toggle(f, code == 1004);
            } else {
                if (f->e.secret == 1 && text && *text) {
                    es(&f->e, "");
                    /* Keep replacement keys masked too. */
                    f->e.secret = 2;
                }
                editor_key(&f->e, code == 13 ? 10 : code, text);
            }
            return;
        }
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
            scroll_chat(u, code == 1012 ? -10 : 10);
        } else
            u->detail_scroll += code == 1012 ? -10 : 10;
        return;
    }
    if (u->tab == 0 && u->focus == 100) {
        if (code == 22)
            paste_image(u);
        else if (code == 13)
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
        } else if (u->tab == 3 && u->focus == 600) {
            int i = u->memory_sel + change;
            if (i >= 0 && u->memories && i < (int)u->memories->len) {
                u->memory_sel = i;
                u->detail_scroll = 0;
            }
        } else if (u->tab == 0) {
            scroll_chat(u, change);
        } else
            u->detail_scroll += change;
    }
}
static int function_key(const char *seq) {
    size_t n = strlen(seq);
    if (!n)
        return 0;
    if (n == 2 && seq[0] == '[' && seq[1] >= 'A' && seq[1] <= 'E')
        return 1016 + seq[1] - 'A'; /* Linux console F1–F5. */
    const char *modifier = strchr(seq, ';');
    int mods = modifier ? atoi(modifier + 1) : 1;
    const char *event = modifier ? strchr(modifier + 1, ':') : NULL;
    if ((event && atoi(event + 1) == 3) || mods < 1 || ((mods - 1) & ~(64 | 128)))
        return 0;
    char final = seq[n - 1];
    int number = atoi(seq);
    if ((final == 'P' || final == 'Q' || final == 'R' || final == 'S') &&
        (n == 1 || number == 1))
        return 1016 + final - 'P';
    if (final == '~') {
        const int keys[] = {11, 12, 13, 14, 15, 17};
        for (int i = 0; i < 6; i++)
            if (number == keys[i])
                return 1016 + i;
    }
    if (final == 'u' && number >= 57364 && number <= 57369)
        return 1016 + number - 57364;
    return 0;
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
                                                             : settings_editor(u);
                if (u->modal && u->modal->kind == FORM && u->modal->focus < u->modal->count &&
                    u->modal->fields[u->modal->focus].kind == 4)
                    e = NULL;
                if (e && !(e == &u->composer && !u->pasted.n && paste_image(u))) {
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
                    if (e->secret == 1 && clean.n) {
                        es(e, "");
                        e->secret = 2;
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
                if (u->input.n > 2 && p[2] == '[')
                    end++;
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
                else if (function_key(seq))
                    code = function_key(seq);
                else if (seq[strlen(seq) - 1] == 'u') {
                    int val = atoi(seq);
                    if (val > 0) {
                        const char *modifier = strchr(seq, ';');
                        int mods = modifier ? atoi(modifier + 1) : 1;
                        const char *event = modifier ? strchr(modifier + 1, ':') : NULL;
                        if (event && atoi(event + 1) == 3)
                            val = 0;
                        int flags = mods - 1;
                        if (val == 27)
                            code = 27;
                        else if (val == 127)
                            code = 127;
                        else if (val == 13)
                            code = (flags & 1) ? 1010 : 13;
                        else if (val == 9)
                            code = (flags & 1) ? 1011 : 9;
                        else if (flags & 4) {
                            int letter = val < 128 ? tolower(val) : 0;
                            code = letter >= 'a' && letter <= 'z' ? letter - 'a' + 1 : 0;
                        }
                        else if (val >= 32 && val <= 0x10ffff && (val < 57344 || val > 63743)) {
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
    if (u->job == 9 && u->jobresult) {
        free(u->timer);
        u->timer = strdup(jstr(u->jobresult));
    } else if (u->job == 10) {
        free(u->memory_error);
        u->memory_error = strdup(u->result ? u->notice : "");
        if (u->jobresult) {
            char *id = strdup(u->memory_target ? u->memory_target
                                             : gs(ji(u->memories, u->memory_sel), "id"));
            jf(u->memories);
            u->memories = u->jobresult;
            u->jobresult = NULL;
            for (size_t i = 0; i < u->memories->len; i++)
                if (!strcmp(gs(u->memories->v[i], "id"), id))
                    u->memory_sel = (int)i;
            free(id);
            free(u->memory_target);
            u->memory_target = NULL;
        }
    } else if (u->job == 11) {
        if (u->result) {
            fail("%s", u->notice);
            error_ui(u);
        } else {
            int deleted = jg(u->jobresult, "deleted") != NULL;
            if (!deleted) {
                free(u->memory_target);
                u->memory_target = strdup(gs(u->jobresult, "id"));
            }
            if (u->modal && (u->modal->action == A_MEMORY_EDIT || u->modal->action == A_MEMORY_DELETE))
                close_modal(u);
            notice_ui(u, deleted ? "Memory deleted" : "Memory saved");
            u->memory_reload = 1;
        }
    }
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
    if (u->steering->len && !u->result && (u->job == 1 || u->job == 2 || u->job == 3)) {
        start_job(u, 2, "");
        return;
    }
    if (u->memory_reload && u->tab == 3 && !u->modal) {
        u->memory_reload = 0;
        start_job(u, 10, "");
    }
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
    u.steering = ja();
    u.config = jc(config);
    u.chat = new_chat(config);
    u.expanded = jo();
    u.memories = ja();
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
    u.mcp_config = jc(config);
    mcp_init(&u.mcp, u.mcp_config);
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
        if (u.busy)
            u.dirty = 1;
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
    for (int i = 0; i < 3; i++)
        modal_free(u.settings_drafts[i]);
    mcp_close(&u.mcp);
    jf(u.mcp_config);
    jf(u.config);
    jf(u.chat);
    jf(u.history);
    jf(u.tasklist);
    jf(u.memories);
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
    free(u.memory_error);
    free(u.memory_target);
    free(u.composer.s);
    jf(u.images);
    jf(u.jobimages);
    jf(u.steering);
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
