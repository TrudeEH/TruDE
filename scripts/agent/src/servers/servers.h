#ifndef SETH_SERVERS_H
#define SETH_SERVERS_H
#include "seth.h"
J *fs_tools(void), *web_tools(void), *shell_tools(void), *memory_tools(void);
J *fs_call(const char *, J *), *web_call(const char *, J *), *shell_call(const char *, J *),
    *memory_call(const char *, J *);
J *server_result(const char *, int);
void server_tool(J *, const char *, const char *, const char *, int);
#endif
