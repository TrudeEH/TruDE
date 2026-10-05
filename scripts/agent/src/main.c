#include "seth.h"
#include <locale.h>
#include <string.h>
#include <unistd.h>
static void signal_stop(int sig) {
    (void)sig;
    cancelled = 1;
}
static void print_event(const char *kind, const char *text, void *opaque) {
    (void)opaque;
    if (!strcmp(kind, "delta")) {
        fputs(text, stdout);
        fflush(stdout);
    } else if (!strcmp(kind, "notice"))
        fprintf(stderr, "%s\n", text);
}
int main(int argc, char **argv) {
    if (!setlocale(LC_ALL, ""))
        setlocale(LC_ALL, "C.UTF-8");
    setlocale(LC_NUMERIC, "C");
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (n > 0)
        executable[n] = 0;
    else if (!realpath(argv[0], executable)) {
        fputs("Cannot resolve executable\n", stderr);
        return 1;
    }
    const char *workspace = NULL, *taskid = NULL, *prompt = NULL, *server = NULL;
    int check = 0, due = 0, test = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            puts("Seth — native Debian AI agent\n\nUsage: dotfiles-agent-tui [--workspace DIR]\n   "
                 "    dotfiles-agent-tui --check\n       dotfiles-agent-tui --run-due | --run-task "
                 "ID\n       dotfiles-agent-tui --prompt TEXT [--workspace DIR]\n       "
                 "dotfiles-agent-tui --self-test\n\nEnter sends. Shift+Enter adds a line. F6 opens "
                 "chat help.\nSettings and history use the existing dotfiles-agent XDG "
                 "directories.");
            return 0;
        } else if (!strcmp(argv[i], "--workspace") && i + 1 < argc)
            workspace = argv[++i];
        else if (!strcmp(argv[i], "--run-task") && i + 1 < argc)
            taskid = argv[++i];
        else if (!strcmp(argv[i], "--prompt") && i + 1 < argc)
            prompt = argv[++i];
        else if (!strcmp(argv[i], "--check"))
            check = 1;
        else if (!strcmp(argv[i], "--run-due"))
            due = 1;
        else if (!strcmp(argv[i], "--self-test"))
            test = 1;
        else if (!strcmp(argv[i], "--mcp-filesystem"))
            server = "filesystem";
        else if (!strcmp(argv[i], "--mcp-web"))
            server = "web";
        else if (!strcmp(argv[i], "--mcp-shell"))
            server = "shell";
        else if (!strcmp(argv[i], "--mcp-memory"))
            server = "memory";
        else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            return 2;
        }
    }
    if (test)
        return selftest();
    if (store_init()) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (server)
        return server_main(server);
    if (taskid || due) {
        int r = due ? run_due() : run_task(taskid);
        if (r && *err)
            fprintf(stderr, "%s\n", err);
        return r ? 1 : 0;
    }
    J *config = load_config();
    if (!config) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (workspace) {
        char real[PATH_MAX];
        if (!realpath(workspace, real)) {
            fprintf(stderr, "Cannot open workspace: %s\n", workspace);
            jf(config);
            return 1;
        }
        jset(config, "workspace", js(real));
    }
    int r = 0;
    if (check || prompt) {
        MCP m;
        mcp_init(&m, config);
        int failed = mcp_connect(&m);
        if (check) {
            for (size_t i = 0; i < m.count; i++) {
                Server *s = m.servers[i];
                printf("%s: %s (%s)%s%s\n", s->name, s->state,
                       s->modern ? "MCP v2 / 2026" : "MCP v2 / legacy",
                       s->detail && *s->detail ? " — " : "", s->detail ? s->detail : "");
            }
            printf("%zu tools available\n", m.toolcount);
            r = failed ? 1 : 0;
        } else {
            J *chat = new_chat(config);
            Agent a = {.config = config, .chat = chat, .mcp = &m, .update = print_event};
            r = agent_run(&a, prompt, 0) ? 1 : 0;
            puts("");
            if (r)
                fprintf(stderr, "%s\n", err);
            fprintf(stderr, "Saved chat: %s\n", gs(chat, "id"));
            jf(chat);
        }
        mcp_close(&m);
    } else
        r = tui(config) ? 1 : 0;
    jf(config);
    if (r && *err)
        fprintf(stderr, "%s\n", err);
    return r;
}
