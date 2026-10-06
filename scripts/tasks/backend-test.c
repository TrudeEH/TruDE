#include "backend.h"
#include "json.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    assert(!backend_init());char *out=NULL;
    char *pref[]={"tasks","configure","todoist",NULL};assert(!backend_dispatch(pref,NULL,&out));free(out);
    char *read[]={"tasks","preference",NULL};assert(!backend_dispatch(read,NULL,&out));assert(!strcmp(out,"todoist"));free(out);
    char *token[]={"tasks","token",NULL};assert(!backend_dispatch(token,"fake-token",&out));free(out);
    assert(backend_dispatch(token,"invalid\nheader",&out));free(out);
    char *config[]={"tasks","configure","local",NULL};assert(!backend_dispatch(config,NULL,&out));free(out);
    char *save[]={"tasks","save","local","","A\tB\nC\\D","Description","inbox","2026-10-08","work, home","1","1",NULL};
    assert(!backend_dispatch(save,NULL,&out));free(out);
    char *snapshot[]={"tasks","snapshot","local",NULL};assert(!backend_dispatch(snapshot,NULL,&out));
    assert(strstr(out,"A\\tB\\nC\\\\D"));assert(strstr(out,"\twork,home\t1\t0\t2026-10-08"));free(out);
    char *bad[]={"add","not json"};assert(backend_cli("local",2,bad));
    puts("PASS: native settings, token validation, snapshot escaping and task serialization");return 0;
}
