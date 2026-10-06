#include "backend.h"
#include "json.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <ctype.h>
#include <math.h>

static char data[4096],config[4096],error[1024];
static int fail(const char *format,...) {va_list args;va_start(args,format);vsnprintf(error,sizeof error,format,args);va_end(args);return 1;}
static char *path(const char *dir,const char *name) {size_t n=strlen(dir)+strlen(name)+2;char *p=malloc(n);if(!p)exit(1);snprintf(p,n,"%s/%s",dir,name);return p;}
static int mkdirs(char *p) {
    for(char *s=p+1;*s;s++)if(*s=='/') {*s=0;if(mkdir(p,0700) && errno!=EEXIST){*s='/';return fail("Cannot create directory: %s",strerror(errno));}*s='/';}
    if(mkdir(p,0700) && errno!=EEXIST)return fail("Cannot create directory: %s",strerror(errno));
    return 0;
}
int backend_init(void) {
    umask(077);const char *home=getenv("HOME"),*d=getenv("XDG_DATA_HOME"),*c=getenv("XDG_CONFIG_HOME");
    if(!home)home=".";
    if(d)snprintf(data,sizeof data,"%s/tasks",d);else snprintf(data,sizeof data,"%s/.local/share/tasks",home);
    if(c)snprintf(config,sizeof config,"%s/tasks",c);else snprintf(config,sizeof config,"%s/.config/tasks",home);
    if(mkdirs(data)||mkdirs(config)) {fprintf(stderr,"%s\n",error);return 1;}
    return curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK;
}
static char *read_file(const char *p) {
    FILE *f=fopen(p,"rb");if(!f)return NULL;
    size_t len=0,cap=4096;char *s=malloc(cap);if(!s)exit(1);
    size_t n;while((n=fread(s+len,1,cap-len-1,f))) {len+=n;if(cap-len<1024){cap*=2;s=realloc(s,cap);if(!s)exit(1);}}
    if(ferror(f)){free(s);s=NULL;}else s[len]=0;fclose(f);return s;
}
static int atomic_write(const char *dir,const char *name,const char *value) {
    char *temp=path(dir,".write.XXXXXX"),*dest=path(dir,name);int fd=mkstemp(temp),rc=0;
    if(fd<0)rc=fail("Cannot create private file: %s",strerror(errno));
    else {
        const char *p=value;size_t left=strlen(value);
        while(left) {ssize_t n=write(fd,p,left);if(n<0 && errno==EINTR)continue;if(n<=0){rc=fail("Write failed: %s",strerror(errno));break;}p+=n;left-=(size_t)n;}
        if(!rc && fsync(fd))rc=fail("Cannot flush file: %s",strerror(errno));
        if(close(fd) && !rc)rc=fail("Cannot close file");
        if(!rc && rename(temp,dest))rc=fail("Cannot replace file: %s",strerror(errno));
        if(!rc){int parent=open(dir,O_RDONLY|O_DIRECTORY);if(parent>=0){fsync(parent);close(parent);}}
    }
    if(rc)unlink(temp);
    free(temp);free(dest);return rc;
}
static int lock_db(void) {char *p=path(data,"local.lock");int fd=open(p,O_CREAT|O_RDWR,0600);free(p);if(fd<0 || flock(fd,LOCK_EX)){if(fd>=0)close(fd);fail("Cannot lock local database");return -1;}return fd;}
static J *load_db(void) {
    char *p=path(data,"local.json"),*s=read_file(p);int saved_errno=errno;free(p);
    J *db=NULL;
    if(!s) {
        if(saved_errno!=ENOENT){fail("Cannot read local database");return NULL;}
        db=jo();J *projects=ja(),*inbox=jo();jset(inbox,"id",js("inbox"));jset(inbox,"name",js("Inbox"));jadd(projects,inbox);jset(db,"projects",projects);jset(db,"tasks",ja());
    }else {char *why=NULL;db=jp(s,&why);free(s);free(why);}
    if(!db || !jg(db,"projects") || jg(db,"projects")->type!=JARR || !jg(db,"tasks") || jg(db,"tasks")->type!=JARR){jf(db);fail("Invalid local database; file left unchanged");return NULL;}
    return db;
}
static J *find(J *array,const char *id) {for(size_t i=0;i<array->len;i++)if(!strcmp(gs(array->v[i],"id"),id))return array->v[i];return NULL;}
static char *new_id(void) {
    unsigned char bytes[16];int fd=open("/dev/urandom",O_RDONLY);if(fd<0)return NULL;size_t got=0;
    while(got<sizeof bytes){ssize_t n=read(fd,bytes+got,sizeof bytes-got);if(n<=0){close(fd);return NULL;}got+=(size_t)n;}close(fd);
    char *id=malloc(33);if(!id)exit(1);for(int i=0;i<16;i++)snprintf(id+i*2,3,"%02x",bytes[i]);return id;
}
static int valid_task(J *item) {
    if(!item || item->type!=JOBJ || !*gs(item,"content"))return fail("Task title is required");
    const char *strings[]={"content","description","project_id","due_string"};
    for(int i=0;i<4;i++){J *v=jg(item,strings[i]);if(v && v->type!=JSTR)return fail("Invalid task text field");}
    J *p=jg(item,"priority");if(p && (p->type!=JNUM || !isfinite(p->n) || p->n<1 || p->n>4 || floor(p->n)!=p->n))return fail("Priority must be 1–4");
    J *labels=jg(item,"labels");if(labels){if(labels->type!=JARR)return fail("Invalid labels");for(size_t i=0;i<labels->len;i++)if(labels->v[i]->type!=JSTR)return fail("Invalid label");}
    return 0;
}
typedef struct {char *s;size_t n;} Buffer;
static size_t receive(char *ptr,size_t size,size_t count,void *user) {
    Buffer *b=user;size_t n=size*count;if(n>32*1024*1024-b->n)return 0;
    char *s=realloc(b->s,b->n+n+1);if(!s)return 0;b->s=s;memcpy(s+b->n,ptr,n);b->n+=n;s[b->n]=0;return n;
}
static J *api(const char *method,const char *endpoint,J *payload) {
    const char *env=getenv("TODOIST_API_TOKEN");char *token=NULL;
    if(env && *env)token=strdup(env);else {char *p=path(config,"token");token=read_file(p);free(p);if(token)token[strcspn(token,"\r\n")]=0;}
    if(!token || !*token || strchr(token,'\n') || strchr(token,'\r')){free(token);fail("Connect Todoist in backend settings");return NULL;}
    CURL *curl=curl_easy_init();if(!curl){free(token);fail("Cannot initialize HTTPS");return NULL;}
    char *auth=malloc(strlen(token)+24);if(!auth)exit(1);sprintf(auth,"Authorization: Bearer %s",token);free(token);
    struct curl_slist *headers=NULL;headers=curl_slist_append(headers,auth);free(auth);headers=curl_slist_append(headers,"Content-Type: application/json");
    char url[8192];snprintf(url,sizeof url,"https://api.todoist.com/api/v1/%s",endpoint);
    Buffer body={0};char *json=payload?jd(payload,0):NULL;char curl_error[CURL_ERROR_SIZE]={0};
    curl_easy_setopt(curl,CURLOPT_URL,url);curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,method);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(curl,CURLOPT_TIMEOUT,30L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body);curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,curl_error);
    if(json)curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json);
    CURLcode code=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    curl_easy_cleanup(curl);curl_slist_free_all(headers);free(json);
    J *result=NULL;
    if(code!=CURLE_OK)fail("Todoist: %s. Refresh before retrying writes.",curl_error[0]?curl_error:curl_easy_strerror(code));
    else if(status<200 || status>=300)fail("Todoist HTTP %ld: %.600s",status,body.s?body.s:"");
    else if(!body.n)result=jnull();
    else {char *why=NULL;result=jp(body.s,&why);free(why);if(!result)fail("Invalid Todoist response");}
    free(body.s);return result;
}
static J *api_list(const char *endpoint) {
    J *all=ja();char *cursor=strdup("");
    for(int page=0;page<10000;page++) {
        CURL *curl=curl_easy_init();char *escaped=curl?curl_easy_escape(curl,cursor,0):NULL;
        if(!escaped){if(curl)curl_easy_cleanup(curl);jf(all);free(cursor);fail("Cannot encode pagination cursor");return NULL;}
        char url[8192];snprintf(url,sizeof url,"%s?limit=200%s%s",endpoint,*cursor?"&cursor=":"",escaped);curl_free(escaped);curl_easy_cleanup(curl);
        J *response=api("GET",url,NULL);if(!response){jf(all);free(cursor);return NULL;}J *items=jg(response,"results");
        if(!items || items->type!=JARR){jf(response);jf(all);free(cursor);fail("Invalid Todoist list response");return NULL;}
        for(size_t i=0;i<items->len;i++)jadd(all,jc(items->v[i]));
        const char *next=gs(response,"next_cursor");if(!*next){jf(response);free(cursor);return all;}
        if(!strcmp(next,cursor)){jf(response);break;}
        free(cursor);cursor=strdup(next);jf(response);
    }
    free(cursor);jf(all);fail("Todoist pagination limit reached");return NULL;
}
static J *list(const char *backend,const char *kind) {
    if(!strcmp(backend,"todoist"))return api_list(kind);
    int fd=lock_db();if(fd<0)return NULL;J *db=load_db(),*result=db?jc(jg(db,kind)):NULL;
    jf(db);close(fd);return result;
}
static int mutate(const char *backend,const char *action,const char *id,J *item) {
    int task=!strncmp(action,"task-",5);
    if(!strcmp(action,"task-save") && valid_task(item))return 1;
    if(!strcmp(backend,"todoist")) {
        char endpoint[512];J *result=NULL;
        for(const char *p=id;*p;p++)if(!isalnum((unsigned char)*p) && *p!='_' && *p!='-')return fail("Invalid Todoist ID");
        if(!strcmp(action,"task-save")) {
            J *payload=jc(item);if(jg(payload,"due_string") && !*gs(payload,"due_string"))jset(payload,"due_string",js("no date"));
            char *project=strdup(gs(payload,"project_id"));if(*id)jdel(payload,"project_id");
            snprintf(endpoint,sizeof endpoint,"tasks%s%s",*id?"/":"",id);result=api("POST",endpoint,payload);jf(payload);
            if(result && *id && *project) {jf(result);J *move=jo();jset(move,"project_id",js(project));snprintf(endpoint,sizeof endpoint,"tasks/%s/move",id);result=api("POST",endpoint,move);jf(move);}free(project);
        }else if(!strcmp(action,"task-complete")) {snprintf(endpoint,sizeof endpoint,"tasks/%s/%s",id,item->n?"close":"reopen");result=api("POST",endpoint,NULL);}
        else if(!strcmp(action,"project-save")){snprintf(endpoint,sizeof endpoint,"projects%s%s",*id?"/":"",id);result=api("POST",endpoint,item);}
        else {snprintf(endpoint,sizeof endpoint,"%s/%s",task?"tasks":"projects",id);result=api("DELETE",endpoint,NULL);}
        int rc=result?0:1;jf(result);return rc;
    }
    int fd=lock_db();if(fd<0)return 1;J *db=load_db();if(!db){close(fd);return 1;}
    J *array=jg(db,task?"tasks":"projects"),*existing=find(array,id);int rc=0;
    if(!strcmp(action,"task-save")) {
        const char *project=gs(item,"project_id");if(!*project)project=existing?gs(existing,"project_id"):"inbox";
        if(!find(jg(db,"projects"),project))rc=fail("Project not found");
        else if(*id && !existing)rc=fail("Task not found");
        else {
            if(!existing){char *new=new_id();if(!new)rc=fail("Cannot create task ID");else {existing=jo();jset(existing,"id",js(new));free(new);jset(existing,"is_completed",jb(0));jadd(array,existing);}}
            if(!rc){for(size_t i=0;i<item->len;i++){J *field=item->v[i];if(strcmp(field->key,"id") && strcmp(field->key,"is_completed"))jset(existing,field->key,jc(field));}if(!jg(existing,"project_id"))jset(existing,"project_id",js(project));}
        }
    }else if(!strcmp(action,"project-save")) {
        if(!*gs(item,"name"))rc=fail("Project name required");
        else if(*id && !existing)rc=fail("Project not found");
        else if(existing)jset(existing,"name",js(gs(item,"name")));
        else {char *new=new_id();if(!new)rc=fail("Cannot create project ID");else {existing=jc(item);jset(existing,"id",js(new));free(new);jadd(array,existing);}}
    }else if(!strcmp(action,"task-complete")) {if(!existing)rc=fail("Task not found");else jset(existing,"is_completed",jc(item));}
    else if(!task && !strcmp(id,"inbox"))rc=fail("Cannot delete Inbox");
    else {
        for(size_t i=0;i<array->len;i++)if(!strcmp(gs(array->v[i],"id"),id)){jremove(array,i);break;}
        if(!task){J *tasks=jg(db,"tasks");for(size_t i=0;i<tasks->len;i++)if(!strcmp(gs(tasks->v[i],"project_id"),id))jset(tasks->v[i],"project_id",js("inbox"));}
    }
    if(!rc){char *s=jd(db,0);rc=atomic_write(data,"local.json",s);free(s);}jf(db);close(fd);return rc;
}
static int task_order(const void *left,const void *right) {
    const J *a=*(J *const *)left,*b=*(J *const *)right;
    int done=gb(a,"is_completed",0)-gb(b,"is_completed",0);
    if(done)return done;
    int priority=(int)gn(b,"priority",1)-(int)gn(a,"priority",1);
    return priority?priority:strcmp(gs(a,"content"),gs(b,"content"));
}
static void tsv(FILE *f,const char *s) {for(;*s;s++){if(*s=='\\')fputs("\\\\",f);else if(*s=='\n')fputs("\\n",f);else if(*s=='\r')fputs("\\r",f);else if(*s=='\t')fputs("\\t",f);else fputc(*s,f);}}
static J *labels(const char *text) {J *array=ja();char *s=strdup(text),*save=NULL;for(char *p=strtok_r(s,",",&save);p;p=strtok_r(NULL,",",&save)){while(*p==' ')p++;char *end=p+strlen(p);while(end>p && end[-1]==' ')*--end=0;if(*p)jadd(array,js(p));}free(s);return array;}
int backend_dispatch(char **args,const char *input,char **output) {
    error[0]=0;int rc=0;const char *action=args[1],*backend=args[2];*output=NULL;
    if(!strcmp(action,"preference")){char *p=path(config,"backend");*output=read_file(p);free(p);if(!*output)*output=strdup("");return 0;}
    if(!strcmp(action,"configure")){if(strcmp(backend,"local") && strcmp(backend,"todoist"))rc=fail("Invalid backend");else rc=atomic_write(config,"backend",backend);}
    else if(!strcmp(action,"token")){if(!input || !*input || strchr(input,'\r') || strchr(input,'\n'))rc=fail("Invalid API token");else rc=atomic_write(config,"token",input);}
    else if(!strcmp(action,"snapshot")) {
        J *projects=NULL,*tasks=NULL;
        if(!strcmp(backend,"local")) {
            int fd=lock_db();
            if(fd>=0){J *db=load_db();if(db){projects=jc(jg(db,"projects"));tasks=jc(jg(db,"tasks"));}jf(db);close(fd);}
        }else {projects=list(backend,"projects");tasks=projects?list(backend,"tasks"):NULL;}
        if(!projects || !tasks)rc=1;
        else {
            qsort(tasks->v,tasks->len,sizeof *tasks->v,task_order);
            size_t length=0;FILE *f=open_memstream(output,&length);
            if(!f)rc=fail("Cannot allocate snapshot");else {
                for(size_t i=0;i<projects->len;i++){J *p=projects->v[i];fputs("P\t",f);tsv(f,gs(p,"id"));fputc('\t',f);tsv(f,gs(p,"name"));fputc('\n',f);}
                for(size_t i=0;i<tasks->len;i++){J *t=tasks->v[i],*due=jg(t,"due");const char *when=gs(t,"due_string");if(!*when)when=gs(due,"string");if(!*when)when=gs(due,"date");
                    const char *fields[]={gs(t,"id"),gs(t,"project_id"),gs(t,"content"),gs(t,"description"),when};fputc('T',f);for(int k=0;k<5;k++){fputc('\t',f);tsv(f,fields[k]);}
                    fputc('\t',f);J *ls=jg(t,"labels");if(ls)for(size_t k=0;k<ls->len;k++){if(k)fputc(',',f);tsv(f,jstr(ls->v[k]));}
                    fprintf(f,"\t%d\t%d\t",5-(int)gn(t,"priority",1),gb(t,"is_completed",0));tsv(f,*gs(due,"date")?gs(due,"date"):gs(t,"due_string"));fputc('\n',f);
                }fclose(f);
            }
        }jf(projects);jf(tasks);
    }else {
        J *item=NULL;const char *mapped=action;
        if(!strcmp(action,"save")){mapped="task-save";item=jo();jset(item,"content",js(args[4]));jset(item,"description",js(args[5]));jset(item,"project_id",js(args[6]));jset(item,"labels",labels(args[8]));jset(item,"priority",jnum(5-atoi(args[9])));if(!strcmp(args[10],"1"))jset(item,"due_string",js(args[7]));}
        else if(!strcmp(action,"complete")){mapped="task-complete";item=jb(!strcmp(args[4],"true"));}
        else if(!strcmp(action,"delete"))mapped="task-delete";
        else if(!strcmp(action,"project-save")){item=jo();jset(item,"name",js(args[4]));}
        rc=mutate(backend,mapped,args[3],item);jf(item);
    }
    if(rc){free(*output);*output=strdup(*error?error:"Backend operation failed");}else if(!*output)*output=strdup("");return rc;
}
int backend_cli(const char *backend,int argc,char **argv) {
    const char *action=argv[0];J *item=NULL;int rc=0;
    if(!strcmp(action,"list") || !strcmp(action,"projects")){J *result=list(backend,!strcmp(action,"list")?"tasks":"projects");if(!result)rc=1;else {char *s=jd(result,1);puts(s);free(s);jf(result);}}
    else {
        const char *mapped=NULL,*id="";
        if(!strcmp(action,"add") && argc==2){mapped="task-save";item=jp(argv[1],NULL);}
        else if(!strcmp(action,"edit") && argc==3){mapped="task-save";id=argv[1];item=jp(argv[2],NULL);}
        else if((!strcmp(action,"complete") || !strcmp(action,"reopen")) && argc==2){mapped="task-complete";id=argv[1];item=jb(!strcmp(action,"complete"));}
        else if(!strcmp(action,"project-add") && argc==2){mapped="project-save";item=jo();jset(item,"name",js(argv[1]));}
        else if(!strcmp(action,"project-rename") && argc==3){mapped="project-save";id=argv[1];item=jo();jset(item,"name",js(argv[2]));}
        else if((!strcmp(action,"delete") || !strcmp(action,"project-delete")) && argc==2){mapped=!strcmp(action,"delete")?"task-delete":"project-delete";id=argv[1];}
        if(!mapped)rc=fail("Invalid command or arguments");else rc=mutate(backend,mapped,id,item);jf(item);
    }
    if(rc)fprintf(stderr,"%s\n",*error?error:"Operation failed");
    return rc;
}
