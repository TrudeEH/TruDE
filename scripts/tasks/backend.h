#ifndef TASKS_BACKEND_H
#define TASKS_BACKEND_H
int backend_init(void);
int backend_dispatch(char **args,const char *input,char **output);
int backend_cli(const char *backend,int argc,char **argv);
#endif
