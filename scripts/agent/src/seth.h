#ifndef SETH_H
#define SETH_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "json.h"
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#define VERSION "1.0.0"
#define MCP_VERSION "2026-07-28"
#define LIMIT (2 * 1024 * 1024)
typedef struct {
    char *s;
    size_t n, cap;
} Buf;
void bput(Buf *, const char *, size_t), bs(Buf *, const char *), bf(Buf *, const char *, ...),
    bfree(Buf *);
char *fmt(const char *, ...), *readfile(const char *, size_t), *now(void), *uuid(void),
    *expand(const char *), *urlencode(const char *), *text_html(const char *);
int writeall(int, const void *, size_t), mkdirs(const char *, mode_t),
    atomic_json(const char *, J *), private_text(const char *, const char *),
    valid_url(const char *, int), contains(J *, const char *);
J *readjson(const char *, J *);
double mono(void);
void alias(char[64], const char *, const char *);
void trim_utf8(char *, size_t);
extern _Thread_local char err[2048];
int fail(const char *, ...);
extern atomic_int cancelled;
typedef struct {
    pid_t pid;
    int in, out, error;
    Buf pending, errors;
} Proc;
int spawn(Proc *, char *const[], char *const[], const char *);
void stopproc(Proc *), drain_error(Proc *), cancel_command(void);
char *proc_line(Proc *, int);
char *command(char *const[], const char *, int, size_t, int *);
typedef void (*Chunk)(const char *, size_t, void *);
typedef struct {
    char *body, *headers, *effective;
    int status;
} Http;
int http(const char *, const char *, J *, J *, int, size_t, Chunk, void *, Http *);
void http_free(Http *);
int http_notify(const char *, J *, J *);
int http_close_session(const char *, J *);
int http_stream(Proc *, const char *, J *, int);
int http_listen(Proc *, const char *, J *, J *, int);
typedef struct Server {
    char *name, *url, *post, *session, *version, *instructions, *detail;
    _Atomic(const char *) state;
    J *config, *caps, *headers, *listed;
    Proc proc;
    int kind, modern, seq, listen;
    atomic_int changed;
    Buf sse;
} Server;
typedef struct {
    char *name, *server;
    char public[64];
    J *definition;
    int safe;
    Server *conn;
} Tool;
typedef struct {
    J *config;
    Server **servers;
    size_t count;
    Tool **tools;
    size_t toolcount;
    J *(*elicit)(const char *, J *, void *);
    void *opaque;
    pthread_mutex_t *mutex;
} MCP;
void mcp_init(MCP *, J *), mcp_close(MCP *);
int mcp_connect(MCP *), mcp_refresh(MCP *), mcp_poll(MCP *);
J *mcp_definitions(MCP *), *mcp_request(MCP *, Server *, const char *, J *, int);
Tool *mcp_tool(MCP *, const char *);
char *mcp_call(MCP *, const char *, J *), *mcp_guidance(MCP *);
J *parse_search_html(const char *, int);
int server_main(const char *);
extern char executable[PATH_MAX], config_dir[PATH_MAX], data_dir[PATH_MAX], state_dir[PATH_MAX];
int store_init(void), save_config(J *), validate_config(J *), validate_server(const char *, J *),
    save_chat(J *), lock_store(const char *);
void unlock_store(const char *), repair_chat(J *);
J *load_config(void), *new_chat(J *), *chats(void), *load_chat(const char *), *tasks(void);
int save_tasks(J *), delete_chat(const char *);
void event(J *, const char *);
J *profile(J *), *models(J *);
int model_vision(J *);
char *clipboard_image(void);
J *image_content(const char *, J *);
J *completion(J *, J *, J *, int, int, Chunk, void *, J **, char **);
typedef struct Agent {
    J *config, *chat, *background, *images, *steering;
    MCP *mcp;
    pthread_mutex_t *mutex;
    int (*approve)(Tool *, J *, void *);
    void (*update)(const char *, const char *, void *);
    void *opaque;
} Agent;
int agent_run(Agent *, const char *, int), agent_compact(Agent *, int);
J *native_tools(int), *context(Agent *);
size_t estimate(J *);
char *native_call(const char *, J *, J *, MCP *);
char *next_run(const char *, const char *, time_t);
int task_save(J *, J *, MCP *, const char *), task_delete(const char *), run_task(const char *),
    run_due(void), timer_change(int);
J *task_patch(const char *, J *);
char *timer_status(void), *export_chat(J *);
int tui(J *), selftest(void);
int validate_schema(J *, J *);
#endif
