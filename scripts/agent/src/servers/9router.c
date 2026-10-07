#include "servers.h"
#include <ctype.h>
#include <string.h>

J *router_tools(void) {
    J *tools = ja();
    server_tool(tools, "list_models", "List available 9Router web search/fetch models and combos.",
                "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}", 1);
    server_tool(tools, "search", "Search the web through 9Router. Use a discovered webSearch model "
                "or combo, or the configured default. Provider charges may apply.",
                "{\"type\":\"object\",\"properties\":{"
                "\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":2000},"
                "\"model\":{\"type\":\"string\",\"minLength\":1},"
                "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20},"
                "\"search_type\":{\"type\":\"string\",\"enum\":[\"web\",\"news\",\"x\"]}},"
                "\"required\":[\"query\"],\"additionalProperties\":false}", 1);
    return tools;
}
J *router_call(const char *name, J *args) {
    err[0] = 0;
    const char *base = getenv("NINEROUTER_URL"), *key = getenv("NINEROUTER_KEY");
    if (!base || !*base || !key || !*key)
        return server_result("Configure NINEROUTER_URL and NINEROUTER_KEY in the 9router MCP server environment.", 1);
    if (strpbrk(base, "\r\n?#") || valid_url(base, 1))
        return server_result("NINEROUTER_URL must be an HTTP(S) gateway URL without query or fragment.", 1);
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        if (iscntrl(*p))
            return server_result("NINEROUTER_KEY must not contain control characters.", 1);
    J *body = NULL;
    const char *path, *method;
    if (!strcmp(name, "list_models")) {
        path = "/v1/models/web";
        method = "GET";
    } else if (!strcmp(name, "search")) {
        const char *model = gs(args, "model");
        if (!*model) {
            model = getenv("NINEROUTER_SEARCH_MODEL");
            if (!model || !*model)
                return server_result("Choose a webSearch model or combo from list_models, or configure NINEROUTER_SEARCH_MODEL.", 1);
        }
        body = jc(args);
        jset(body, "model", js(model));
        if (!jg(body, "max_results"))
            jset(body, "max_results", jnum(5));
        path = "/v1/search";
        method = "POST";
    } else
        return server_result("Unknown 9Router tool", 1);
    size_t n = strlen(base);
    while (n && base[n - 1] == '/')
        n--;
    char *url = fmt("%.*s%s", (int)n, base, path);
    J *headers = jo();
    char *auth = fmt("Bearer %s", key);
    jset(headers, "Authorization", js(auth));
    jset(headers, "Content-Type", js("application/json"));
    free(auth);
    Http h = {0};
    J *result;
    if (http(url, method, headers, body, 30, 2000000, NULL, NULL, &h)) {
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
