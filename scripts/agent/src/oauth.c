#include "seth.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

/* Public-client OAuth, scoped to the exact MCP URL. No SDK or shell interpolation. */
static int secure_url(const char *url) {
    if (valid_url(url, 0)) return -1;
    if (!strncmp(url, "https://", 8)) return 0;
    if (!strncmp(url, "http://127.0.0.1:", 17) || !strncmp(url, "http://localhost:", 17)) return 0;
    return fail("OAuth requires HTTPS (HTTP is allowed only on loopback)");
}
static char *hash(const char *text) {
    Proc p;
    char *argv[] = {"sha256sum", NULL};
    if (spawn(&p, argv, NULL, NULL)) return NULL;
    if (writeall(p.in, text, strlen(text))) { stopproc(&p); return NULL; }
    close(p.in); p.in = -1;
    char *line = proc_line(&p, 5000);
    stopproc(&p);
    if (!line || strlen(line) < 64) { free(line); fail("OAuth needs sha256sum"); return NULL; }
    line[64] = 0;
    return line;
}
static char *b64(const unsigned char *data, size_t n) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    Buf out = {0};
    unsigned value = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        value = (value << 8) | data[i]; bits += 8;
        while (bits >= 6) { bits -= 6; char c = alphabet[(value >> bits) & 63]; bput(&out, &c, 1); }
    }
    if (bits) { char c = alphabet[(value << (6 - bits)) & 63]; bput(&out, &c, 1); }
    return out.s;
}
static char *random_string(void) {
    unsigned char bytes[32];
    if (getrandom(bytes, sizeof bytes, 0) != sizeof bytes) { fail("OAuth secure randomness unavailable"); return NULL; }
    return b64(bytes, sizeof bytes);
}
static char *challenge(const char *verifier) {
    char *hex = hash(verifier);
    if (!hex) return NULL;
    unsigned char bytes[32];
    for (int i = 0; i < 32; i++) {
        unsigned value;
        if (sscanf(hex + i * 2, "%2x", &value) != 1) { free(hex); return NULL; }
        bytes[i] = value;
    }
    free(hex); return b64(bytes, sizeof bytes);
}
static void param(Buf *b, const char *key, const char *value) {
    char *encoded = urlencode(value);
    bf(b, "%s%s=%s", b->n ? "&" : "", key, encoded);
    free(encoded);
}
static J *get_json(const char *url) {
    if (secure_url(url)) return NULL;
    Http h = {0};
    int r = http_oauth(url, "GET", NULL, &h);
    J *j = r ? NULL : jp(h.body, NULL);
    http_free(&h);
    return j;
}
static char *origin(const char *url) {
    const char *p = strstr(url, "://");
    if (!p) return NULL;
    const char *end = strchr(p + 3, '/');
    return end ? strndup(url, end - url) : strdup(url);
}
static char *resource_metadata(const char *headers, const char *url) {
    /* Accept the quoted resource_metadata parameter only from a Bearer challenge. */
    const char *p = headers;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        if (!strncasecmp(p, "WWW-Authenticate:", 17)) {
            char *line = strndup(p + 17, end - p - 17);
            char *v = line; while (isspace((unsigned char)*v)) v++;
            char *q = !strncasecmp(v, "Bearer", 6) ? strstr(v, "resource_metadata=\"") : NULL;
            if (q) {
                q += 19; char *last = strchr(q, '"');
                char *result = last ? strndup(q, last - q) : NULL;
                free(line); if (result) return result;
            } else free(line);
        }
        p = *end ? end + 1 : end;
    }
    char *base = origin(url);
    const char *path = strstr(url, "://"); path = path ? strchr(path + 3, '/') : NULL;
    char *result = fmt("%s/.well-known/oauth-protected-resource%s", base, path ? path : "");
    free(base); return result;
}
static J *token_request(const char *endpoint, Buf *form) {
    if (secure_url(endpoint)) return NULL;
    Http h = {0};
    int r = http_form(endpoint, form->s, &h);
    J *j = r ? NULL : jp(h.body, NULL);
    http_free(&h);
    if (!j || !*gs(j, "access_token") || strcasecmp(gs(j, "token_type"), "Bearer")) {
        jf(j); fail("OAuth token exchange failed; check provider configuration"); return NULL;
    }
    return j;
}
static int install_token(Server *s, J *record, J *token, const char *path) {
    const char *access = gs(token, "access_token");
    if (strchr(access, '\r') || strchr(access, '\n')) return fail("Invalid OAuth token");
    jset(record, "access_token", js(access));
    if (*gs(token, "refresh_token")) jset(record, "refresh_token", js(gs(token, "refresh_token")));
    jset(record, "expires_at", jnum(time(NULL) + gn(token, "expires_in", 3600)));
    if (atomic_json(path, record)) return -1;
    char *header = fmt("Bearer %s", access);
    jset(s->headers, "Authorization", js(header)); free(header);
    return 0;
}
static char *decode(const char *s, size_t n) {
    Buf b = {0};
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n) {
            char h[3] = {s[i+1], s[i+2], 0};
            if (!isxdigit(h[0]) || !isxdigit(h[1])) { bfree(&b); return NULL; }
            c = (char)strtol(h, NULL, 16); i += 2;
            if (!c) { bfree(&b); return NULL; }
        } else if (c == '+') c = ' ';
        bput(&b, &c, 1);
    }
    return b.s ? b.s : strdup("");
}
static char *query_value(const char *query, const char *name) {
    size_t len = strlen(name);
    for (const char *p = query; p && *p;) {
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > len && !strncmp(p, name, len) && p[len] == '=')
            return decode(p + len + 1, end - p - len - 1);
        p = *end ? end + 1 : end;
    }
    return NULL;
}
static char *callback(int listener, const char *state, int timeout) {
    double end = mono() + timeout;
    while (!cancelled && mono() < end) {
        struct pollfd fd = {listener, POLLIN, 0};
        if (poll(&fd, 1, 100) <= 0) continue;
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) continue;
        char request[8192]; size_t used = 0;
        double deadline = mono() + 2;
        while (used < sizeof request - 1 && mono() < deadline && !cancelled) {
            struct pollfd cfd = {client, POLLIN, 0};
            if (poll(&cfd, 1, 100) <= 0) continue;
            ssize_t n = read(client, request + used, sizeof request - used - 1);
            if (n <= 0) break;
            used += n; request[used] = 0;
            if (strstr(request, "\r\n\r\n")) break;
        }
        request[used] = 0;
        char *code = NULL;
        if (!strncmp(request, "GET /callback?", 14)) {
            char *space = strchr(request + 14, ' ');
            if (space) {
                *space = 0;
                char *got = query_value(request + 14, "state");
                if (got && !strcmp(got, state)) {
                    char *error = query_value(request + 14, "error");
                    if (!error) code = query_value(request + 14, "code");
                    free(error);
                    if (code && !*code) { free(code); code = NULL; }
                }
                free(got);
            }
        }
        const char *response = code ? "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nSign-in complete. Return to Seth.\n"
                                    : "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid OAuth callback.\n";
        send(client, response, strlen(response), MSG_NOSIGNAL); close(client);
        if (code) return code;
    }
    fail(cancelled ? "OAuth cancelled" : "OAuth sign-in timed out"); return NULL;
}
int oauth_authorize(MCP *m, Server *s, const char *headers, int force) {
    int rc = -1, listener = -1, lock = -1;
    char *key = hash(s->url), *path = NULL, *lockpath = NULL;
    J *record = NULL, *resource = NULL, *metadata = NULL, *registered = NULL, *token = NULL;
    char *rm = NULL, *issuer = NULL, *discovery = NULL, *redirect = NULL, *verifier = NULL,
         *pkce = NULL, *state = NULL, *authurl = NULL, *code = NULL;
    Buf form = {0}, query = {0};
    if (!key) goto done;
    path = fmt("%s/oauth-%s.json", config_dir, key);
    lockpath = fmt("%s/oauth-%s.lock", config_dir, key);
    lock = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) { fail("Cannot lock OAuth token storage"); goto done; }
    double lock_end = mono() + 200;
    while (flock(lock, LOCK_EX | LOCK_NB)) {
        if (errno != EWOULDBLOCK || cancelled || mono() > lock_end) {
            fail("OAuth storage is busy or sign-in was cancelled"); goto done;
        }
        struct timespec wait = {0, 100000000}; nanosleep(&wait, NULL);
    }
    record = readjson(path, NULL);
    if (!record || record->type != JOBJ) { jf(record); record = jo(); }
    if (!headers && !*gs(record, "access_token") && !*gs(record, "refresh_token")) {
        rc = 0; goto done;
    }
    if (secure_url(s->url)) goto done;
    const char *configured = gs(jg(s->config, "oauth"), "clientId");
    if (*configured && strcmp(configured, gs(record, "client_id"))) {
        jf(record); record = jo();
    }
    if (strchr(gs(record, "access_token"), '\r') || strchr(gs(record, "access_token"), '\n')) {
        fail("Invalid stored OAuth token"); goto done;
    }
    if (*gs(record, "access_token") && !force && gn(record, "expires_at", 0) > time(NULL) + 60) {
        char *h = fmt("Bearer %s", gs(record, "access_token"));
        jset(s->headers, "Authorization", js(h)); free(h); rc = 0; goto done;
    }
    if (*gs(record, "refresh_token") && *gs(record, "token_endpoint")) {
        param(&form, "grant_type", "refresh_token");
        param(&form, "refresh_token", gs(record, "refresh_token"));
        param(&form, "client_id", gs(record, "client_id"));
        param(&form, "resource", s->url);
        token = token_request(gs(record, "token_endpoint"), &form);
        bfree(&form);
        if (token) { rc = install_token(s, record, token, path); goto done; }
        err[0] = 0;
    }
    /* A cached miss must not launch a browser before the server challenges us. */
    if (!headers) { rc = 0; goto done; }
    if (!m->elicit && !m->interactive) {
        fail("OAuth sign-in required; reconnect this server interactively first"); goto done;
    }
    rm = resource_metadata(headers, s->url);
    resource = get_json(rm);
    if (!resource || strcmp(gs(resource, "resource"), s->url)) {
        fail("OAuth protected-resource metadata is missing or does not match the MCP URL"); goto done;
    }
    issuer = strdup(jstr(ji(jg(resource, "authorization_servers"), 0)));
    if (!*issuer || secure_url(issuer)) goto done;
    char *base = origin(issuer);
    const char *ipath = strstr(issuer, "://"); ipath = ipath ? strchr(ipath + 3, '/') : NULL;
    discovery = fmt("%s/.well-known/oauth-authorization-server%s", base, ipath ? ipath : "");
    free(base);
    metadata = get_json(discovery);
    if (!metadata) {
        free(discovery); discovery = fmt("%s%s.well-known/openid-configuration", issuer,
                                           issuer[strlen(issuer)-1] == '/' ? "" : "/");
        metadata = get_json(discovery);
    }
    if (!metadata || strcmp(gs(metadata, "issuer"), issuer) ||
        secure_url(gs(metadata, "authorization_endpoint")) || secure_url(gs(metadata, "token_endpoint"))) {
        fail("Invalid OAuth authorization-server metadata"); goto done;
    }
    int supports_pkce = 0;
    J *methods = jg(metadata, "code_challenge_methods_supported");
    for (size_t i = 0; methods && i < methods->len; i++)
        if (!strcmp(jstr(methods->v[i]), "S256")) supports_pkce = 1;
    if (!supports_pkce) { fail("OAuth provider must advertise S256 PKCE"); goto done; }
    J *cfg = jg(s->config, "oauth");
    int port = (int)gn(cfg, "callbackPort", 0);
    listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof addr;
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof addr) ||
        listen(listener, 4) || getsockname(listener, (struct sockaddr *)&addr, &length)) {
        fail("Cannot create OAuth loopback callback"); goto done;
    }
    redirect = fmt("http://127.0.0.1:%u/callback", ntohs(addr.sin_port));
    const char *client = gs(cfg, "clientId");
    if (!*client) {
        const char *registration = gs(metadata, "registration_endpoint");
        if (!*registration || secure_url(registration)) {
            fail("OAuth provider requires a registered client; set oauth.clientId"); goto done;
        }
        J *body = jo(), *uris = ja(), *grants = ja(), *responses = ja();
        jadd(uris, js(redirect)); jset(body, "redirect_uris", uris);
        jadd(grants, js("authorization_code")); jadd(grants, js("refresh_token"));
        jset(body, "grant_types", grants); jadd(responses, js("code")); jset(body, "response_types", responses);
        jset(body, "client_name", js("Seth")); jset(body, "token_endpoint_auth_method", js("none"));
        Http response = {0};
        int r = http_oauth(registration, "POST", body, &response);
        registered = r ? NULL : jp(response.body, NULL);
        http_free(&response); jf(body);
        if (!registered || !*gs(registered, "client_id") ||
            (*gs(registered, "token_endpoint_auth_method") && strcmp(gs(registered, "token_endpoint_auth_method"), "none"))) {
            fail("OAuth public-client registration failed; configure oauth.clientId"); goto done;
        }
        client = gs(registered, "client_id");
    }
    jset(record, "client_id", js(client));
    jset(record, "token_endpoint", js(gs(metadata, "token_endpoint")));
    verifier = random_string(); state = random_string();
    if (!verifier || !state) goto done;
    pkce = challenge(verifier); if (!pkce) goto done;
    param(&query, "response_type", "code"); param(&query, "client_id", client);
    param(&query, "redirect_uri", redirect); param(&query, "state", state);
    param(&query, "code_challenge", pkce); param(&query, "code_challenge_method", "S256");
    param(&query, "resource", s->url);
    const char *scope = gs(cfg, "scope");
    Buf scopes = {0};
    if (!*scope) {
        J *list = jg(resource, "scopes_supported");
        for (size_t i = 0; list && i < list->len; i++) bf(&scopes, "%s%s", i ? " " : "", jstr(list->v[i]));
        scope = scopes.s ? scopes.s : "";
    }
    if (*scope) param(&query, "scope", scope);
    bfree(&scopes);
    const char *auth = gs(metadata, "authorization_endpoint");
    authurl = fmt("%s%s%s", auth, strchr(auth, '?') ? "&" : "?", query.s);
    if (m->elicit) {
        J *p = jo(); jset(p, "mode", js("url")); jset(p, "url", js(authurl));
        jset(p, "message", js("Copy the sign-in URL, select Confirm, then open it in your browser and sign in. Tokens are stored privately."));
        J *answer = m->elicit(s->name, p, m->opaque);
        int accepted = answer && !strcmp(gs(answer, "action"), "accept");
        jf(answer); jf(p);
        if (!accepted) { fail("OAuth sign-in declined"); goto done; }
    } else fprintf(stderr, "OAuth sign-in for %s: open this URL in your browser:\n%s\n", s->name, authurl);
    code = callback(listener, state, 180); if (!code) goto done;
    param(&form, "grant_type", "authorization_code"); param(&form, "code", code);
    param(&form, "client_id", client); param(&form, "redirect_uri", redirect);
    param(&form, "code_verifier", verifier); param(&form, "resource", s->url);
    token = token_request(gs(metadata, "token_endpoint"), &form);
    if (token) rc = install_token(s, record, token, path);
done:
    if (listener >= 0) close(listener);
    if (lock >= 0) close(lock);
    free(key); free(path); free(lockpath); free(rm); free(issuer); free(discovery);
    free(redirect); free(verifier); free(pkce); free(state); free(authurl); free(code);
    jf(record); jf(resource); jf(metadata); jf(registered); jf(token);
    bfree(&form); bfree(&query);
    return rc;
}
