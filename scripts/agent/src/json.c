#include "json.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *mem(size_t n) {
    void *p = calloc(1, n);
    if (!p) {
        fputs("Out of memory\n", stderr);
        exit(1);
    }
    return p;
}
static J *newj(int t) {
    J *j = mem(sizeof *j);
    j->type = t;
    return j;
}
J *jnull(void) {
    return newj(JNULL);
}
J *jb(int b) {
    J *j = newj(JBOOL);
    j->n = !!b;
    return j;
}
J *jnum(double n) {
    J *j = newj(JNUM);
    j->n = n;
    return j;
}
J *js(const char *s) {
    J *j = newj(JSTR);
    j->s = strdup(s ? s : "");
    return j;
}
J *ja(void) {
    return newj(JARR);
}
J *jo(void) {
    return newj(JOBJ);
}
void jf(J *j) {
    if (!j)
        return;
    for (size_t i = 0; i < j->len; i++)
        jf(j->v[i]);
    free(j->v);
    free(j->key);
    free(j->s);
    free(j);
}
void jadd(J *j, J *v) {
    if (!j || !v)
        return;
    j->v = realloc(j->v, (j->len + 1) * sizeof *j->v);
    if (!j->v)
        exit(1);
    j->v[j->len++] = v;
}
J *jg(const J *j, const char *k) {
    if (!j || j->type != JOBJ)
        return NULL;
    for (size_t i = 0; i < j->len; i++)
        if (!strcmp(j->v[i]->key, k))
            return j->v[i];
    return NULL;
}
J *ji(const J *j, size_t i) {
    return j && i < j->len ? j->v[i] : NULL;
}
void jremove(J *j, size_t i) {
    if (!j || i >= j->len)
        return;
    jf(j->v[i]);
    memmove(j->v + i, j->v + i + 1, (j->len - i - 1) * sizeof *j->v);
    j->len--;
}
void jdel(J *j, const char *k) {
    if (!j)
        return;
    for (size_t i = 0; i < j->len; i++)
        if (j->v[i]->key && !strcmp(j->v[i]->key, k)) {
            jremove(j, i);
            return;
        }
}
void jset(J *j, const char *k, J *v) {
    char *key = strdup(k);
    jdel(j, k);
    free(v->key);
    v->key = key;
    jadd(j, v);
}
const char *jstr(const J *j) {
    return j && j->type == JSTR ? j->s : "";
}
const char *gs(const J *j, const char *k) {
    return jstr(jg(j, k));
}
double gn(const J *j, const char *k, double d) {
    J *v = jg(j, k);
    return v && v->type == JNUM ? v->n : d;
}
int gb(const J *j, const char *k, int d) {
    J *v = jg(j, k);
    return v && v->type == JBOOL ? v->n != 0 : d;
}
J *jc(const J *j) {
    if (!j)
        return jnull();
    J *v = newj(j->type);
    v->n = j->n;
    if (j->s)
        v->s = strdup(j->s);
    for (size_t i = 0; i < j->len; i++) {
        J *c = jc(j->v[i]);
        if (j->v[i]->key)
            c->key = strdup(j->v[i]->key);
        jadd(v, c);
    }
    return v;
}
typedef struct {
    char *p;
    size_t n, cap;
} B;
static void put(B *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 128;
        b->p = realloc(b->p, b->cap);
        if (!b->p)
            exit(1);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void ch(B *b, char c) {
    put(b, &c, 1);
}
static size_t utf8_length(const unsigned char *s) {
    unsigned u;
    size_t n;
    if (*s < 128)
        return 1;
    if (*s >= 0xc2 && *s <= 0xdf) {
        n = 2;
        u = *s & 31;
    } else if (*s >= 0xe0 && *s <= 0xef) {
        n = 3;
        u = *s & 15;
    } else if (*s >= 0xf0 && *s <= 0xf4) {
        n = 4;
        u = *s & 7;
    } else
        return 0;
    for (size_t i = 1; i < n; i++) {
        if (!s[i] || (s[i] & 192) != 128)
            return 0;
        u = (u << 6) | (s[i] & 63);
    }
    if ((n == 3 && u < 2048) || (n == 4 && u < 65536) || (u >= 0xd800 && u <= 0xdfff) ||
        u > 0x10ffff)
        return 0;
    return n;
}
static void utf(B *b, unsigned u) {
    char s[4];
    int n;
    if (u < 128) {
        s[0] = u;
        n = 1;
    } else if (u < 2048) {
        s[0] = 192 | (u >> 6);
        s[1] = 128 | (u & 63);
        n = 2;
    } else if (u < 65536) {
        s[0] = 224 | (u >> 12);
        s[1] = 128 | ((u >> 6) & 63);
        s[2] = 128 | (u & 63);
        n = 3;
    } else {
        s[0] = 240 | (u >> 18);
        s[1] = 128 | ((u >> 12) & 63);
        s[2] = 128 | ((u >> 6) & 63);
        s[3] = 128 | (u & 63);
        n = 4;
    }
    put(b, s, n);
}
typedef struct {
    const char *p;
    char *err;
    int depth;
} P;
static void ws(P *p) {
    while (isspace((unsigned char)*p->p))
        p->p++;
}
static void bad(P *p, const char *s) {
    if (!p->err)
        p->err = strdup(s);
}
static unsigned hex4(P *p) {
    unsigned u = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = *p->p;
        if (!isxdigit(c)) {
            bad(p, "Invalid Unicode escape");
            return 0;
        }
        p->p++;
        u = u * 16 + (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    return u;
}
static char *str(P *p) {
    B b = {0};
    p->p++;
    while (*p->p && *p->p != '"' && !p->err) {
        unsigned char c = *p->p++;
        if (c < 32) {
            bad(p, "Control byte in JSON string");
            break;
        }
        if (c == '\\') {
            if (!*p->p) {
                bad(p, "Unterminated escape");
                break;
            }
            c = *p->p++;
            switch (c) {
            case '"':
            case '\\':
            case '/':
                ch(&b, c);
                break;
            case 'b':
                ch(&b, '\b');
                break;
            case 'f':
                ch(&b, '\f');
                break;
            case 'n':
                ch(&b, '\n');
                break;
            case 'r':
                ch(&b, '\r');
                break;
            case 't':
                ch(&b, '\t');
                break;
            case 'u': {
                unsigned u = hex4(p);
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (p->p[0] != '\\' || p->p[1] != 'u') {
                        bad(p, "Missing low surrogate");
                        break;
                    }
                    p->p += 2;
                    unsigned l = hex4(p);
                    if (l < 0xdc00 || l > 0xdfff) {
                        bad(p, "Invalid low surrogate");
                        break;
                    }
                    u = 65536 + ((u - 0xd800) << 10) + (l - 0xdc00);
                } else if (u >= 0xdc00 && u <= 0xdfff) {
                    bad(p, "Unexpected low surrogate");
                    break;
                }
                if (!u) {
                    bad(p, "NUL strings are unsupported");
                    break;
                }
                utf(&b, u);
                break;
            }
            default:
                bad(p, "Invalid string escape");
            }
        } else if (c >= 128) {
            const unsigned char *start = (const unsigned char *)p->p - 1;
            size_t n = utf8_length(start);
            if (!n) {
                bad(p, "Invalid UTF-8 in JSON string");
                break;
            }
            put(&b, (const char *)start, n);
            p->p += n - 1;
        } else
            ch(&b, c);
    }
    if (*p->p != '"')
        bad(p, "Unterminated string");
    else
        p->p++;
    return b.p ? b.p : strdup("");
}
static J *value(P *p) {
    ws(p);
    if (++p->depth > 128) {
        bad(p, "JSON nesting too deep");
        p->depth--;
        return NULL;
    }
    J *j = NULL;
    char c = *p->p;
    if (c == '"') {
        j = newj(JSTR);
        j->s = str(p);
    } else if (c == '{' || c == '[') {
        p->p++;
        j = c == '{' ? jo() : ja();
        ws(p);
        char end = c == '{' ? '}' : ']';
        if (*p->p != end)
            for (;;) {
                char *key = NULL;
                if (c == '{') {
                    if (*p->p != '"') {
                        bad(p, "Expected object key");
                        break;
                    }
                    key = str(p);
                    ws(p);
                    if (*p->p != ':') {
                        free(key);
                        bad(p, "Expected colon");
                        break;
                    }
                    p->p++;
                }
                J *v = value(p);
                if (!v) {
                    free(key);
                    break;
                }
                if (key) {
                    if (jg(j, key)) {
                        free(key);
                        jf(v);
                        bad(p, "Duplicate object key");
                        break;
                    }
                    v->key = key;
                }
                jadd(j, v);
                ws(p);
                if (*p->p != ',')
                    break;
                p->p++;
                ws(p);
            }
        if (*p->p != end)
            bad(p, "Expected closing bracket");
        else
            p->p++;
    } else if (!strncmp(p->p, "true", 4)) {
        j = jb(1);
        p->p += 4;
    } else if (!strncmp(p->p, "false", 5)) {
        j = jb(0);
        p->p += 5;
    } else if (!strncmp(p->p, "null", 4)) {
        j = jnull();
        p->p += 4;
    } else if (c == '-' || isdigit((unsigned char)c)) {
        const char *start = p->p;
        if (*p->p == '-')
            p->p++;
        if (*p->p == '0')
            p->p++;
        else if (isdigit((unsigned char)*p->p))
            while (isdigit((unsigned char)*p->p))
                p->p++;
        else
            bad(p, "Invalid number");
        if (*p->p == '.') {
            p->p++;
            if (!isdigit((unsigned char)*p->p))
                bad(p, "Invalid fraction");
            while (isdigit((unsigned char)*p->p))
                p->p++;
        }
        if (*p->p == 'e' || *p->p == 'E') {
            p->p++;
            if (*p->p == '+' || *p->p == '-')
                p->p++;
            if (!isdigit((unsigned char)*p->p))
                bad(p, "Invalid exponent");
            while (isdigit((unsigned char)*p->p))
                p->p++;
        }
        double n = strtod(start, NULL);
        if (!isfinite(n))
            bad(p, "Number out of range");
        j = jnum(n);
    } else
        bad(p, "Unexpected JSON token");
    p->depth--;
    if (p->err) {
        jf(j);
        return NULL;
    }
    return j;
}
J *jp(const char *s, char **err) {
    P p = {.p = s ? s : ""};
    J *j = value(&p);
    if (!p.err)
        ws(&p);
    if (!p.err && *p.p) {
        jf(j);
        j = NULL;
        bad(&p, "Trailing JSON data");
    }
    if (err)
        *err = p.err;
    else
        free(p.err);
    return j;
}
static void quote(B *b, const char *s) {
    ch(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char buf[8];
        switch (*p) {
        case '"':
            put(b, "\\\"", 2);
            break;
        case '\\':
            put(b, "\\\\", 2);
            break;
        case '\n':
            put(b, "\\n", 2);
            break;
        case '\r':
            put(b, "\\r", 2);
            break;
        case '\t':
            put(b, "\\t", 2);
            break;
        default:
            if (*p < 32) {
                snprintf(buf, sizeof buf, "\\u%04x", *p);
                put(b, buf, 6);
            } else if (*p >= 128) {
                size_t n = utf8_length(p);
                if (!n)
                    put(b, "\\ufffd", 6);
                else {
                    put(b, (const char *)p, n);
                    p += n - 1;
                }
            } else
                ch(b, *p);
        }
    }
    ch(b, '"');
}
static void dump(B *b, const J *j, int pretty, int depth) {
    if (!j) {
        put(b, "null", 4);
        return;
    }
    char n[64];
    switch (j->type) {
    case JNULL:
        put(b, "null", 4);
        break;
    case JBOOL:
        put(b, j->n ? "true" : "false", j->n ? 4 : 5);
        break;
    case JNUM:
        snprintf(n, sizeof n, "%.17g", j->n);
        put(b, n, strlen(n));
        break;
    case JSTR:
        quote(b, j->s);
        break;
    default: {
        int obj = j->type == JOBJ;
        ch(b, obj ? '{' : '[');
        for (size_t i = 0; i < j->len; i++) {
            if (i)
                ch(b, ',');
            if (pretty) {
                ch(b, '\n');
                for (int k = 0; k < (depth + 1) * 2; k++)
                    ch(b, ' ');
            }
            if (obj) {
                quote(b, j->v[i]->key);
                ch(b, ':');
                if (pretty)
                    ch(b, ' ');
            }
            dump(b, j->v[i], pretty, depth + 1);
        }
        if (pretty && j->len) {
            ch(b, '\n');
            for (int k = 0; k < depth * 2; k++)
                ch(b, ' ');
        }
        ch(b, obj ? '}' : ']');
    }
    }
}
char *jd(const J *j, int pretty) {
    B b = {0};
    dump(&b, j, pretty, 0);
    return b.p;
}
int jeq(const J *a, const J *b) {
    char *x = jd(a, 0), *y = jd(b, 0);
    int r = !strcmp(x, y);
    free(x);
    free(y);
    return r;
}
