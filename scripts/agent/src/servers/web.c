#include "servers.h"
#include <ctype.h>
#include <string.h>
static void utf(Buf *b, unsigned u) {
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
    bput(b, s, n);
}
char *text_html(const char *html) {
    Buf b = {0};
    const char *p = html;
    int space = 0;
    while (*p) {
        if (*p == '<') {
            const char *tags[] = {"script", "style", "noscript", NULL};
            int skip = 0;
            for (int i = 0; tags[i]; i++) {
                size_t n = strlen(tags[i]);
                if (!strncasecmp(p + 1, tags[i], n) && !isalnum((unsigned char)p[1 + n])) {
                    char *endtag = fmt("</%s", tags[i]);
                    const char *end = strcasestr(p, endtag);
                    free(endtag);
                    if (end) {
                        const char *close = strchr(end, '>');
                        p = close ? close + 1 : end + strlen(end);
                        skip = 1;
                        break;
                    }
                }
            }
            if (skip)
                continue;
            const char *end = strchr(p, '>');
            if (end) {
                p = end + 1;
                space = 1;
                continue;
            }
        }
        if (isspace((unsigned char)*p)) {
            space = 1;
            p++;
            continue;
        }
        if (space && b.n)
            bs(&b, " ");
        space = 0;
        if (*p == '&') {
            const char *end = strchr(p, ';');
            if (end && end - p < 16) {
                char *entity = strndup(p + 1, end - p - 1);
                unsigned u = 0;
                const char *names[] = {"amp",  "lt",    "gt",    "quot",   "apos",
                                       "nbsp", "ndash", "mdash", "hellip", NULL};
                unsigned codes[] = {38, 60, 62, 34, 39, 32, 8211, 8212, 8230};
                if (*entity == '#')
                    u = strtoul(entity + 1 + (entity[1] == 'x' || entity[1] == 'X'), NULL,
                                entity[1] == 'x' || entity[1] == 'X' ? 16 : 10);
                else
                    for (int i = 0; names[i]; i++)
                        if (!strcmp(entity, names[i]))
                            u = codes[i];
                free(entity);
                if (u && u <= 0x10ffff && !(u >= 0xd800 && u <= 0xdfff)) {
                    utf(&b, u);
                    p = end + 1;
                    continue;
                }
            }
        }
        bput(&b, p++, 1);
    }
    return b.s ? b.s : strdup("");
}
static char *attribute(const char *tag, const char *key) {
    const char *end = strchr(tag, '>');
    if (!end)
        return NULL;
    const char *p = tag;
    size_t n = strlen(key);
    while ((p = strcasestr(p, key)) && p < end) {
        if (p != tag && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '-')) {
            p += n;
            continue;
        }
        p += n;
        while (isspace((unsigned char)*p))
            p++;
        if (*p++ != '=')
            continue;
        while (isspace((unsigned char)*p))
            p++;
        char quote = *p;
        if (quote != '\'' && quote != '"')
            continue;
        p++;
        const char *q = strchr(p, quote);
        if (!q || q > end)
            return NULL;
        char *raw = strndup(p, q - p), *s = text_html(raw);
        free(raw);
        return s;
    }
    return NULL;
}
static char *decode(const char *s) {
    Buf b = {0};
    for (const char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char v[3] = {p[1], p[2], 0};
            char c = strtol(v, NULL, 16);
            if (c)
                bput(&b, &c, 1);
            p += 2;
        } else if (*p == '+')
            bs(&b, " ");
        else
            bput(&b, p, 1);
    }
    return b.s ? b.s : strdup("");
}
J *parse_search_html(const char *html, int limit) {
    J *rows = ja();
    const char *p = html;
    while ((p = strcasestr(p, "<a")) && rows->len < (size_t)limit) {
        char *cls = attribute(p, "class");
        if (!cls || !strstr(cls, "result__a")) {
            free(cls);
            p += 2;
            continue;
        }
        free(cls);
        char *href = attribute(p, "href");
        const char *start = strchr(p, '>'), *end = start ? strcasestr(start, "</a>") : NULL;
        if (!href || !end) {
            free(href);
            break;
        }
        char *raw = strndup(start + 1, end - start - 1), *title = text_html(raw);
        free(raw);
        char *target = NULL, *redir = strstr(href, "uddg=");
        if (redir) {
            const char *stop = strchr(redir + 5, '&');
            raw = stop ? strndup(redir + 5, stop - redir - 5) : strdup(redir + 5);
            target = decode(raw);
            free(raw);
        } else
            target = !strncmp(href, "//", 2) ? fmt("https:%s", href) : strdup(href);
        free(href);
        if (!valid_url(target, 0)) {
            J *j = jo();
            jset(j, "title", js(title));
            jset(j, "url", js(target));
            const char *snippet = strstr(end, "result__snippet"), *next = strstr(end, "result__a");
            char *snip = NULL;
            if (snippet && (!next || snippet < next)) {
                const char *tag = snippet;
                while (tag > end && tag[-1] != '<')
                    tag--;
                size_t namelen = 0;
                while (isalpha((unsigned char)tag[namelen]) && namelen < 16)
                    namelen++;
                char *closing = fmt("</%.*s", (int)namelen, tag);
                const char *s = strchr(snippet, '>'), *e = s ? strcasestr(s, closing) : NULL;
                free(closing);
                if (s && e && (!next || e < next)) {
                    raw = strndup(s + 1, e - s - 1);
                    snip = text_html(raw);
                    free(raw);
                }
            }
            jset(j, "snippet", js(snip ? snip : ""));
            free(snip);
            jadd(rows, j);
        }
        free(title);
        free(target);
        p = end + 4;
    }
    return rows;
}
J *web_tools(void) {
    return jp(
        "[{\"name\":\"search\",\"description\":\"Free DuckDuckGo web search with titles, URLs and "
        "snippets. No API "
        "key.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":"
        "\"string\",\"minLength\":1,\"maxLength\":500},\"limit\":{\"type\":\"integer\",\"minimum\":"
        "1,\"maximum\":10}},\"required\":[\"query\"]},\"annotations\":{\"readOnlyHint\":true}},{"
        "\"name\":\"fetch_page\",\"description\":\"Fetch and extract text from a source "
        "URL.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}}"
        ",\"required\":[\"url\"]},\"annotations\":{\"readOnlyHint\":true}}]",
        NULL);
}
J *web_call(const char *name, J *a) {
    err[0] = 0;
    char *url = NULL, *text = NULL;
    int search = !strcmp(name, "search");
    if (search) {
        const char *query = gs(a, "query");
        int limit = gn(a, "limit", 5);
        if (!*query || strlen(query) > 500 || limit < 1 || limit > 10)
            fail("Query must be 1–500 bytes; limit 1–10");
        else {
            char *q = urlencode(query);
            url = fmt("https://html.duckduckgo.com/html/?q=%s", q);
            free(q);
        }
    } else if (!strcmp(name, "fetch_page"))
        url = strdup(gs(a, "url"));
    else
        fail("Unknown web tool");
    Http h = {0};
    if (!*err && url && !http(url, "GET", NULL, NULL, 20, 1000000, NULL, NULL, &h)) {
        if (!h.headers || (!strcasestr(h.headers, "text/") && !strcasestr(h.headers, "json") &&
                           !strcasestr(h.headers, "xml")))
            fail("This tool reads text pages only");
        else if (search) {
            J *rows = parse_search_html(h.body, (int)gn(a, "limit", 5));
            if (!rows->len)
                fail("DuckDuckGo returned no results or a challenge page. Retry later or add "
                     "another search MCP server.");
            else
                text = jd(rows, 1);
            jf(rows);
        } else {
            char *plain = text_html(h.body);
            text = fmt("Source: %s\n%.14000s", h.effective, plain);
            free(plain);
        }
    }
    J *r = jo(), *content = ja(), *v = jo();
    jset(v, "type", js("text"));
    jset(v, "text", js(*err ? err : text ? text : "No result"));
    jadd(content, v);
    jset(r, "content", content);
    if (*err)
        jset(r, "isError", jb(1));
    http_free(&h);
    free(url);
    free(text);
    return r;
}
