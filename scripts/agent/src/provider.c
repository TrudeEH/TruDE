#include "seth.h"
#include <ctype.h>
#include <string.h>
static J *headers(J *config) {
    J *h = jo();
    jset(h, "Content-Type", js("application/json"));
    J *p = profile(config);
    const char *env = gs(p, "apiKeyEnv"), *key = *env ? getenv(env) : gs(p, "apiKey");
    if (*env && (!key || !*key)) {
        jf(h);
        fail("Environment variable %s is empty", env);
        return NULL;
    }
    if (key && *key) {
        if (strchr(key, '\n') || strchr(key, '\r')) {
            jf(h);
            fail("Invalid API key");
            return NULL;
        }
        char *s = fmt("Bearer %s", key);
        jset(h, "Authorization", js(s));
        free(s);
    }
    return h;
}
static char *endpoint(J *c, const char *route) {
    const char *url = gs(profile(c), "endpoint");
    size_t n = strlen(url);
    while (n && url[n - 1] == '/')
        n--;
    return fmt("%.*s/%s", (int)n, url, route);
}
J *models(J *config) {
    J *h = headers(config);
    if (!h)
        return NULL;
    char *url = endpoint(config, "models");
    Http r = {0};
    J *a = NULL;
    if (!http(url, "GET", h, NULL, (int)gn(config, "timeout", 180), LIMIT, NULL, NULL, &r)) {
        J *j = jp(r.body, NULL), *data = jg(j, "data");
        if (!data || data->type != JARR)
            fail("Provider returned no model list");
        else {
            a = ja();
            J *vision = jo();
            for (size_t i = 0; i < data->len; i++)
                if (*gs(data->v[i], "id")) {
                    J *v = data->v[i];
                    const char *id = gs(v, "id");
                    jadd(a, js(id));
                    J *inputs = jg(v, "input_modalities");
                    if (!inputs)
                        inputs = jg(jg(v, "architecture"), "input_modalities");
                    jset(vision, id, jb(contains(inputs, "image") ||
                                      gb(jg(v, "capabilities"), "vision", 0)));
                }
            jset(profile(config), "discoveredVision", vision);
        }
        jf(j);
    }
    free(url);
    jf(h);
    http_free(&r);
    return a;
}
static void append(J *o, const char *key, const char *s) {
    char *v = fmt("%s%s", gs(o, key), s);
    jset(o, key, js(v));
    free(v);
}
typedef struct {
    Buf pending;
    J *message, *calls, *usage;
    char *finish;
    int done, invalid, sse;
    Chunk delta;
    void *arg;
} Stream;
static void consume(Stream *s, const char *line) {
    if (strncmp(line, "data:", 5))
        return;
    s->sse = 1;
    const char *raw = line + 5;
    while (isspace((unsigned char)*raw))
        raw++;
    if (!strcmp(raw, "[DONE]")) {
        s->done = 1;
        return;
    }
    if (!*raw)
        return;
    char *e = NULL;
    J *j = jp(raw, &e);
    if (!j) {
        s->invalid = 1;
        fail("Invalid provider stream: %s", e ? e : "");
        free(e);
        return;
    }
    if (jg(j, "error")) {
        s->invalid = 1;
        fail("Provider: %s", gs(jg(j, "error"), "message"));
        jf(j);
        return;
    }
    if (jg(j, "usage")) {
        jf(s->usage);
        s->usage = jc(jg(j, "usage"));
    }
    J *choice = ji(jg(j, "choices"), 0), *d = jg(choice, "delta");
    if (*gs(choice, "finish_reason")) {
        free(s->finish);
        s->finish = strdup(gs(choice, "finish_reason"));
    }
    if (*gs(d, "content")) {
        append(s->message, "content", gs(d, "content"));
        if (s->delta)
            s->delta(gs(d, "content"), strlen(gs(d, "content")), s->arg);
    }
    J *calls = jg(d, "tool_calls");
    for (size_t i = 0; calls && i < calls->len; i++) {
        J *part = calls->v[i];
        int index = (int)gn(part, "index", 0);
        if (index < 0 || index > 128) {
            s->invalid = 1;
            fail("Invalid tool index");
            break;
        }
        while (s->calls->len <= (size_t)index) {
            J *v = jo(), *f = jo();
            jset(v, "id", js(""));
            jset(v, "type", js("function"));
            jset(f, "name", js(""));
            jset(f, "arguments", js(""));
            jset(v, "function", f);
            jadd(s->calls, v);
        }
        J *v = s->calls->v[index];
        if (*gs(part, "id"))
            jset(v, "id", js(gs(part, "id")));
        J *f = jg(v, "function"), *pf = jg(part, "function");
        if (*gs(pf, "name"))
            append(f, "name", gs(pf, "name"));
        if (*gs(pf, "arguments"))
            append(f, "arguments", gs(pf, "arguments"));
    }
    jf(j);
}
static void chunks(const char *p, size_t n, void *arg) {
    Stream *s = arg;
    bput(&s->pending, p, n);
    char *nl;
    while (s->pending.s && (nl = strchr(s->pending.s, '\n'))) {
        size_t len = nl - s->pending.s;
        char *line = strndup(s->pending.s, len);
        if (len && line[len - 1] == '\r')
            line[len - 1] = 0;
        consume(s, line);
        free(line);
        memmove(s->pending.s, nl + 1, s->pending.n - len);
        s->pending.n -= len + 1;
    }
}
J *completion(J *config, J *messages, J *tools, int stream, int max, Chunk delta, void *arg,
              J **usage, char **finish) {
    if (!*gs(profile(config), "model")) {
        fail("Choose a model in Settings first");
        return NULL;
    }
    for (size_t i = 0; messages && i < messages->len; i++) {
        J *parts = jg(messages->v[i], "content");
        for (size_t k = 0; parts && parts->type == JARR && k < parts->len; k++)
            if (!strcmp(gs(parts->v[k], "type"), "image_url") && !model_vision(config)) {
                fail("Selected model has no confirmed vision support for images in this chat");
                return NULL;
            }
    }
    J *h = headers(config);
    if (!h)
        return NULL;
    J *body = jo();
    jset(body, "model", js(gs(profile(config), "model")));
    jset(body, "messages", jc(messages));
    jset(body, "stream", jb(stream));
    if (stream) {
        J *options = jo();
        jset(options, "include_usage", jb(1));
        jset(body, "stream_options", options);
    }
    jset(body, "max_tokens", jnum(max));
    if (tools && tools->len) {
        jset(body, "tools", jc(tools));
        jset(body, "tool_choice", js("auto"));
    }
    Stream s = {0};
    s.message = jo();
    jset(s.message, "role", js("assistant"));
    jset(s.message, "content", js(""));
    s.calls = ja();
    s.usage = jo();
    s.delta = delta;
    s.arg = arg;
    char *url = endpoint(config, "chat/completions");
    Http r = {0};
    int code = http(url, "POST", h, body, (int)gn(config, "timeout", 180), 8 * LIMIT,
                    stream ? chunks : NULL, &s, &r);
    free(url);
    jf(h);
    jf(body);
    if (!code) {
        if (s.pending.n)
            consume(&s, s.pending.s);
        if (!s.sse) {
            J *j = jp(r.body, NULL), *choice = ji(jg(j, "choices"), 0),
              *msg = jg(choice, "message");
            if (!msg) {
                code = fail("Provider response contained no assistant message");
            } else {
                jf(s.message);
                s.message = jc(msg);
                jset(s.message, "role", js("assistant"));
                if (!jg(s.message, "content") || jg(s.message, "content")->type != JSTR)
                    jset(s.message, "content", js(""));
                jf(s.usage);
                s.usage = jg(j, "usage") ? jc(jg(j, "usage")) : jo();
                free(s.finish);
                s.finish = strdup(gs(choice, "finish_reason"));
            }
            jf(j);
        } else {
            if (s.invalid)
                code = -1;
            else if (!s.done && (!s.finish || !*s.finish))
                code = fail("Provider stream ended early; retry the request");
            if (s.calls->len)
                jset(s.message, "tool_calls", jc(s.calls));
        }
        J *calls = jg(s.message, "tool_calls");
        for (size_t i = 0; !code && calls && i < calls->len; i++) {
            if (!*gs(calls->v[i], "id") || !*gs(jg(calls->v[i], "function"), "name"))
                code = fail("Provider returned an incomplete tool call");
            else {
                J *function = jg(calls->v[i], "function");
                J *args = jg(function, "arguments");
                if (args && args->type != JSTR && args->type != JNULL)
                    code = fail("Provider tool arguments must be a JSON string");
                else if (!*gs(function, "arguments"))
                    jset(function, "arguments", js("{}"));
                jset(calls->v[i], "type", js("function"));
            }
        }
    }
    http_free(&r);
    bfree(&s.pending);
    jf(s.calls);
    if (code) {
        jf(s.message);
        jf(s.usage);
        free(s.finish);
        return NULL;
    }
    if (usage)
        *usage = s.usage;
    else
        jf(s.usage);
    if (finish)
        *finish = s.finish;
    else
        free(s.finish);
    return s.message;
}

int model_vision(J *config) {
    J *p = profile(config);
    const char *model = gs(p, "model");
    J *override = jg(jg(p, "visionModels"), model);
    if (override && override->type == JBOOL)
        return override->n != 0;
    return gb(jg(p, "discoveredVision"), model, 0);
}
J *image_content(const char *text, J *images) {
    J *parts = ja(), *part = jo();
    jset(part, "type", js("text"));
    jset(part, "text", js(text));
    jadd(parts, part);
    for (size_t i = 0; images && i < images->len; i++) {
        part = jo();
        J *url = jo();
        jset(url, "url", js(gs(images->v[i], "url")));
        jset(part, "type", js("image_url"));
        jset(part, "image_url", url);
        jadd(parts, part);
    }
    return parts;
}
