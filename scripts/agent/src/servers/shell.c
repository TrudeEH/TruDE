#include "servers.h"
#include <errno.h>
#include <string.h>
J *shell_tools(void) {
    J *a = ja();
    server_tool(a, "shell",
                "Run /bin/sh in the workspace with the user's OS permissions; "
                "commands are not sandboxed.",
                "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
                "\"minLength\":1},\"timeout\":{\"type\":\"integer\",\"minimum\":1,"
                "\"maximum\":300}},\"required\":[\"command\"]}", 0);
    return a;
}
J *shell_call(const char *name, J *a) {
    err[0] = 0;
    char root[PATH_MAX];
    const char *workspace = getenv("AGENT_WORKSPACE"), *cmd = gs(a, "command");
    int timeout = gn(a, "timeout", 60), code = 0;
    char *text = NULL, *output = NULL;
    if (strcmp(name, "shell"))
        fail("Unknown shell tool");
    else if (!*cmd || timeout < 1 || timeout > 300)
        fail("Command required; timeout must be 1–300 seconds");
    else if (!realpath(workspace ? workspace : ".", root))
        fail("Workspace: %s", strerror(errno));
    else {
        char *args[] = {"/bin/sh", "-c", (char *)cmd, NULL};
        output = command(args, root, timeout, 24000, &code);
        if (output)
            text = fmt("Exit %d\n%s", code, output);
    }
    J *r = server_result(*err ? err : text ? text : "No output", !!*err);
    free(output);
    free(text);
    return r;
}
