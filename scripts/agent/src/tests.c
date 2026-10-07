#include "seth.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int failures;
static void check(int ok, const char *name) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        failures++;
}
int selftest(void) {
    char sandbox[] = "/tmp/seth-selftest-XXXXXX";
    if (!mkdtemp(sandbox))
        return 1;
    const char *variables[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"};
    const char *folders[] = {"config", "data", "state"};
    for (int i = 0; i < 3; i++) {
        char *p = fmt("%s/%s", sandbox, folders[i]);
        setenv(variables[i], p, 1);
        free(p);
    }
    if (store_init())
        return 1;
    J *j = jp("{\"text\":\"héllo \\ud83d\\ude42\\n\",\"n\":42,\"v\":[true,null]}", NULL);
    check(j && !strcmp(gs(j, "text"), "héllo 🙂\n"), "JSON Unicode and escapes");
    char *s = jd(j, 0);
    J *copy = jp(s, NULL);
    check(jeq(j, copy), "JSON round trip");
    free(s);
    jf(j);
    jf(copy);
    check(!jp("{\"x\":1,\"x\":2}", NULL) && !jp("[01]", NULL) && !jp("\"\\ud800\"", NULL),
          "Malformed JSON rejected");
    check(!jp("\"\xc0\xaf\"", NULL), "Invalid UTF-8 rejected");
    J *vision_config = jo(), *profiles = jo(), *provider = jo();
    jset(vision_config, "profile", js("test"));
    jset(vision_config, "profiles", profiles);
    jset(profiles, "test", provider);
    jset(provider, "model", js("test-model"));
    check(!model_vision(vision_config), "Unknown model rejects image pastes");
    J *flags = jo();
    jset(flags, "test-model", jb(1));
    jset(profile(vision_config), "visionModels", flags);
    check(model_vision(vision_config), "Per-model vision capability enabled");
    jset(flags, "test-model", jb(0));
    check(!model_vision(vision_config), "Per-model vision capability disabled");
    jf(vision_config);
    J *images = ja(), *image = jo();
    jset(image, "url", js("data:image/png;base64,aGVsbG8="));
    jadd(images, image);
    J *parts = image_content("Describe [Image 1]", images);
    check(parts->len == 2 && !strcmp(gs(parts->v[0], "text"), "Describe [Image 1]") &&
          !strcmp(gs(jg(parts->v[1], "image_url"), "url"), gs(image, "url")),
          "Image attachment serialized as multimodal content");
    check(estimate(parts) < 5000, "Image context estimate excludes base64 bytes");
    jf(parts);
    jf(images);
    char name[64];
    alias(name, "web", "search");
    check(!strcmp(name, "web__search_458186c80118"), "Stable MCP tool aliases");
    char *next = next_run("0 9 * * 1-5", "Etc/UTC", 0);
    check(next && !strcmp(next, "1970-01-01T09:00:00.000Z"), "Cron weekday schedule and timezone");
    free(next);
    next = next_run("0 9 */2 * *", "Etc/UTC", 32400);
    check(next && !strcmp(next, "1970-01-03T09:00:00.000Z"), "Cron day steps respected");
    free(next);
    check(!next_run("61 * * * *", "Europe/Lisbon", 0) && !next_run("* * * * *", "../etc/passwd", 0),
          "Invalid cron and timezone rejected");
    char *plain = text_html("<p>Hello &amp; <b>world</b></p><script>bad()</script>");
    check(!strcmp(plain, "Hello & world"), "Web HTML extraction");
    free(plain);
    J *results = parse_search_html(
        "<a class=\"result__a\" "
        "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2F\">Example &amp; title</a><a "
        "class=\"result__snippet\">A <b>useful</b> snippet.</a>",
        5);
    check(results->len == 1 && !strcmp(gs(results->v[0], "url"), "https://example.org/") &&
              !strcmp(gs(results->v[0], "snippet"), "A useful snippet."),
          "DuckDuckGo redirects and formatted snippets");
    jf(results);
    char tmp[] = "/tmp/seth-test-XXXXXX";
    char *root = mkdtemp(tmp);
    if (!root)
        return 1;
    char *path = fmt("%s/seed.txt", root);
    private_text(path, "first\nsecond\n");
    J *config = load_config();
    jset(config, "workspace", js(root));
    J *rp = jg(jg(config, "profiles"), "9router");
    jset(rp, "endpoint", js("http://127.0.0.1:20128/v1/"));
    jset(rp, "apiKeyEnv", js("ROUTER_TEST_KEY"));
    J *rs = jo(), *re = router_env(config, rs);
    check(!strcmp(gs(re, "NINEROUTER_URL"), "http://127.0.0.1:20128") &&
          !strcmp(gs(re, "NINEROUTER_KEY"), "${ROUTER_TEST_KEY}"), "9Router profile URL and key reuse");
    jf(re);
    J *overrides = jo();
    jset(overrides, "NINEROUTER_URL", js("https://example.org"));
    jset(overrides, "NINEROUTER_KEY", js("override"));
    jset(rs, "env", overrides);
    re = router_env(config, rs);
    check(!strcmp(gs(re, "NINEROUTER_KEY"), "override") &&
          !strcmp(gs(re, "NINEROUTER_URL"), "https://example.org"), "9Router MCP overrides preserved");
    jf(re); jf(rs);
    MCP m;
    mcp_init(&m, config);
    int rc = mcp_connect(&m);
    check(!rc && m.count == 5 && m.toolcount == 16, "Real MCP v2 subprocess discovery");
    Tool *read = NULL, *write = NULL, *edit = NULL, *restore = NULL;
    for (size_t i = 0; i < m.toolcount; i++) {
        Tool *t = m.tools[i];
        if (!strcmp(t->name, "read_file"))
            read = t;
        else if (!strcmp(t->name, "write_file"))
            write = t;
        else if (!strcmp(t->name, "edit_file"))
            edit = t;
        else if (!strcmp(t->name, "restore_checkpoint"))
            restore = t;
    }
    J *args = jo();
    jset(args, "path", js("seed.txt"));
    s = read ? mcp_call(&m, read->public, args) : NULL;
    check(s && strstr(s, "1: first"), "Filesystem read through MCP");
    free(s);
    jset(args, "path", js("../outside"));
    s = read ? mcp_call(&m, read->public, args) : NULL;
    check(s && strstr(s, "Tool error:"), "Workspace escape rejected");
    free(s);
    jset(args, "path", js("seed.txt"));
    jset(args, "content", js("changed\n"));
    s = write ? mcp_call(&m, write->public, args) : NULL;
    check(s && strstr(s, "Checkpoint:"), "Write creates checkpoint");
    char *checkpoint =
        s && strstr(s, "Checkpoint: ") ? strdup(strstr(s, "Checkpoint: ") + 12) : NULL;
    free(s);
    jdel(args, "content");
    jset(args, "old_text", js("changed"));
    jset(args, "new_text", js("edited"));
    s = edit ? mcp_call(&m, edit->public, args) : NULL;
    check(s && strstr(s, "Edited"), "Exact text edit");
    free(s);
    J *a = jo();
    if (checkpoint)
        jset(a, "id", js(checkpoint));
    s = restore ? mcp_call(&m, restore->public, a) : NULL;
    char *file = readfile(path, LIMIT);
    check(s && file && !strcmp(file, "first\nsecond\n"), "Checkpoint restores original text");
    free(s);
    free(file);
    free(checkpoint);
    jf(a);
    jf(args);
    mcp_close(&m);
    jf(config);
    unlink(path);
    rmdir(root);
    free(path);
    char *cleanup[] = {"/bin/rm", "-rf", "--", sandbox, NULL};
    int code = 0;
    char *output = command(cleanup, NULL, 5, 2000, &code);
    free(output);
    return failures ? 1 : 0;
}
