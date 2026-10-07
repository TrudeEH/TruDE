#include "servers.h"
#include <ctype.h>
#include <string.h>

J *router_tools(void) {
    J *tools = ja();
    server_tool(tools, "search", "Search the web through the user-configured 9Router search provider "
                "or combo (default search-combo). Provider charges may apply.",
                "{\"type\":\"object\",\"properties\":{"
                "\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":2000},"
                "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20},"
                "\"search_type\":{\"type\":\"string\",\"enum\":[\"web\",\"news\",\"x\"]}},"
                "\"required\":[\"query\"],\"additionalProperties\":false}", 1);
    server_tool(tools, "fetch_page", "Fetch a page through the user-configured 9Router fetch provider "
                "or combo (default fetch-combo). Provider charges may apply.",
                "{\"type\":\"object\",\"properties\":{"
                "\"url\":{\"type\":\"string\",\"minLength\":1},"
                "\"format\":{\"type\":\"string\",\"enum\":[\"markdown\",\"text\",\"html\"]},"
                "\"max_characters\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100000}},"
                "\"required\":[\"url\"],\"additionalProperties\":false}", 1);
    return tools;
}
J *router_call(const char *name, J *args) {
    err[0] = 0;
    if (jg(args, "model") || jg(args, "provider"))
        return server_result("Provider selection belongs in the 9Router MCP settings, not tool arguments.", 1);
    const char *base = getenv("NINEROUTER_URL"), *key = getenv("NINEROUTER_KEY");
    if (!base || !*base)
        return server_result("Configure the 9Router gateway URL in the MCP settings or 9router profile.", 1);
    if (strpbrk(base, "\r\n?#") || valid_url(base, 1))
        return server_result("NINEROUTER_URL must be an HTTP(S) gateway URL without query or fragment.", 1);
    if (!key) key = "";
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        if (iscntrl(*p))
            return server_result("NINEROUTER_KEY must not contain control characters.", 1);
    int search = !strcmp(name, "search"), fetch = !strcmp(name, "fetch_page");
    if (!search && !fetch)
        return server_result("Unknown 9Router tool", 1);
    if (fetch && valid_url(gs(args, "url"), 0))
        return server_result("Page URL must be HTTP(S).", 1);
    const char *model = getenv(search ? "NINEROUTER_SEARCH_MODEL" : "NINEROUTER_FETCH_MODEL");
    if (!model || !*model) model = search ? "search-combo" : "fetch-combo";
    J *body = jc(args);
    jset(body, "model", js(model));
    if (search && !jg(body, "max_results"))
        jset(body, "max_results", jnum(5));
    if (fetch) {
        if (!jg(body, "format")) jset(body, "format", js("markdown"));
        if (!jg(body, "max_characters")) jset(body, "max_characters", jnum(14000));
    }
    const char *path = search ? "/v1/search" : "/v1/web/fetch";
    size_t n = strlen(base);
    while (n && base[n - 1] == '/')
        n--;
    char *url = fmt("%.*s%s", (int)n, base, path);
    J *headers = jo();
    char *auth = fmt("Bearer %s", key);
    if (*key) jset(headers, "Authorization", js(auth));
    jset(headers, "Content-Type", js("application/json"));
    free(auth);
    Http h = {0};
    J *result;
    if (http(url, "POST", headers, body, 30, 2000000, NULL, NULL, &h)) {
        /* Do not echo upstream bodies or transport details that might contain credentials. */
        char *message = h.status ? fmt("9Router HTTP %d. Check gateway credentials, provider configuration and availability.", h.status)
                                 : strdup("9Router request failed. Check gateway URL, connectivity and timeout.");
        result = server_result(message, 1);
        free(message);
    } else {
        J *data = jp(h.body, NULL);
        if (!data || data->type != JOBJ)
            result = server_result("9Router returned an invalid JSON object.", 1);
        else {
            char *text = jd(data, 1);
            result = server_result(text, 0);
            free(text);
        }
        jf(data);
    }
    http_free(&h);
    jf(headers);
    jf(body);
    free(url);
    return result;
}
