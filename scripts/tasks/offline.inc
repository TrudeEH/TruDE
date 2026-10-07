/* Included by backend.c: account-isolated cache, durable FIFO, background sync. */
static pthread_mutex_t sync_mutex=PTHREAD_MUTEX_INITIALIZER;
static int syncing,finished;
static char sync_message[1024];
static char *account_dir(void) {
    char *token=get_token();if(!token)return NULL;
    unsigned char digest[SHA256_DIGEST_LENGTH];SHA256((unsigned char *)token,strlen(token),digest);free(token);
    char name[80]="todoist-";for(int i=0;i<SHA256_DIGEST_LENGTH;i++)snprintf(name+8+i*2,3,"%02x",digest[i]);
    char *dir=path(data,name);if(mkdirs(dir)){free(dir);return NULL;}return dir;
}
static int cache_lock(const char *dir,const char *name,int nonblock) {
    char *p=path(dir,name);int fd=open(p,O_CREAT|O_RDWR,0600);free(p);
    if(fd<0)return -1;
    if(flock(fd,LOCK_EX|(nonblock?LOCK_NB:0))){close(fd);return -1;}return fd;
}
static J *cache_load(const char *dir) {
    char *p=path(dir,"cache.json"),*s=read_file(p);int saved=errno;free(p);J *db=NULL;
    if(!s && saved==ENOENT){db=jo();jset(db,"projects",ja());jset(db,"tasks",ja());jset(db,"queue",ja());}
    else if(s){db=jp(s,NULL);free(s);}
    const char *keys[]={"projects","tasks","queue"};
    for(int i=0;i<3;i++)if(!db || !jg(db,keys[i]) || jg(db,keys[i])->type!=JARR){jf(db);fail("Invalid Todoist cache; file left unchanged");return NULL;}
    J *queue=jg(db,"queue");
    for(size_t i=0;i<queue->len;i++) {
        J *op=queue->v[i];const char *action=gs(op,"action");J *item=jg(op,"item");
        int valid=!strcmp(action,"task-save") || !strcmp(action,"task-complete") || !strcmp(action,"task-delete") || !strcmp(action,"project-save") || !strcmp(action,"project-delete");
        if(!valid || !*gs(op,"id") || strlen(gs(op,"request"))!=32 || !item ||
           (!strcmp(action,"task-save") && valid_task(item)) ||
           (!strcmp(action,"task-complete") && item->type!=JBOOL) ||
           (!strcmp(action,"project-save") && (item->type!=JOBJ || !*gs(item,"name")))) {
            jf(db);fail("Invalid Todoist queue; file left unchanged");return NULL;
        }
    }
    return db;
}
static int cache_save(const char *dir,J *db) {char *s=jd(db,0);int rc=atomic_write(dir,"cache.json",s);free(s);return rc;}
static J *remote_cache(void) {
    char *dir=account_dir();if(!dir)return NULL;int fd=cache_lock(dir,"cache.lock",0);
    J *db=fd>=0?cache_load(dir):NULL;if(fd>=0)close(fd);else fail("Cannot lock Todoist cache");free(dir);return db;
}
static void remote_delete_project(J *db,const char *id) {
    J *tasks=jg(db,"tasks");for(size_t i=tasks->len;i>0;i--)if(!strcmp(gs(tasks->v[i-1],"project_id"),id))jremove(tasks,i-1);
}
static int queued_mutation(const char *action,const char *id,J *item) {
    char *dir=account_dir();if(!dir)return 1;int fd=cache_lock(dir,"cache.lock",0);
    if(fd<0){free(dir);return fail("Cannot lock Todoist cache");}
    J *db=cache_load(dir);int rc=1;char *uuid=new_id(),*temporary=NULL;
    if(db && uuid) {
        if(!strcmp(action,"task-save") && !*id && !*gs(item,"project_id")) {
            J *projects=jg(db,"projects");const char *inbox=NULL;
            for(size_t i=0;i<projects->len;i++)if(gb(projects->v[i],"is_inbox_project",0) || !strcmp(gs(projects->v[i],"id"),"inbox"))inbox=gs(projects->v[i],"id");
            if(inbox)jset(item,"project_id",js(inbox));
        }
        if(!*id){temporary=malloc(40);if(!temporary)exit(1);snprintf(temporary,40,"pending-%s",uuid);}
        J *project=find(jg(db,"projects"),id);
        if(!strcmp(action,"project-delete") && project && gb(project,"is_inbox_project",0)) {
            free(uuid);jf(db);close(fd);free(dir);return fail("Cannot delete Inbox");
        }
        if(!strcmp(action,"project-delete"))remote_delete_project(db,id);
        if(!strcmp(action,"task-complete") && !find(jg(db,"tasks"),id))rc=0;
        else rc=apply(db,action,id,item,temporary);
        if(!rc){J *op=jo();jset(op,"action",js(action));jset(op,"id",js(*id?id:temporary));jset(op,"create",jb(!*id));jset(op,"item",item?jc(item):jnull());
            jset(op,"request",js(uuid));jset(op,"phase",jnum(0));jadd(jg(db,"queue"),op);rc=cache_save(dir,db);}
    }else if(!uuid)fail("Cannot create sync request ID");
    free(uuid);free(temporary);jf(db);close(fd);free(dir);return rc;
}
static void remap(J *db,const char *old,const char *id) {
    const char *arrays[]={"projects","tasks","queue"};
    for(int a=0;a<3;a++){J *array=jg(db,arrays[a]);for(size_t i=0;i<array->len;i++){
        J *obj=array->v[i];if(!strcmp(gs(obj,"id"),old))jset(obj,"id",js(id));
        if(!strcmp(gs(obj,"project_id"),old))jset(obj,"project_id",js(id));
        J *item=jg(obj,"item");if(item && !strcmp(gs(item,"project_id"),old))jset(item,"project_id",js(id));
    }}
}
static J *send_op(J *op) {
    const char *action=gs(op,"action"),*id=gs(op,"id");J *item=jg(op,"item"),*payload=NULL;
    int create=gb(op,"create",0),phase=(int)gn(op,"phase",0);char endpoint[512],key[100];
    for(const char *p=id;*p;p++)if(!isalnum((unsigned char)*p) && *p!='_' && *p!='-'){fail("Invalid queued ID");return NULL;}
    const char *raw=gs(op,"request");
    if(strlen(raw)!=32){fail("Invalid queued request ID");return NULL;}
    snprintf(key,sizeof key,"%.8s-%.4s-%.4s-%.4s-%.12s",raw,raw+8,raw+12,raw+16,raw+20);
    /* A separate stable UUID for the move half of an edit. */
    if(phase)key[35]=key[35]=='0'?'1':'0';
    request_id=key;
    const char *method="POST";
    if(!strcmp(action,"task-save")) {
        if(phase==1){snprintf(endpoint,sizeof endpoint,"tasks/%s/move",id);payload=jo();jset(payload,"project_id",js(gs(item,"project_id")));}
        else {snprintf(endpoint,sizeof endpoint,"tasks%s%s",create?"":"/",create?"":id);payload=jc(item);if(!create)jdel(payload,"project_id");
            if(jg(payload,"due_string") && !*gs(payload,"due_string"))jset(payload,"due_string",js("no date"));}
    }else if(!strcmp(action,"task-complete"))snprintf(endpoint,sizeof endpoint,"tasks/%s/%s",id,item->n?"close":"reopen");
    else if(!strcmp(action,"project-save")){snprintf(endpoint,sizeof endpoint,"projects%s%s",create?"":"/",create?"":id);payload=jc(item);}
    else {snprintf(endpoint,sizeof endpoint,"%s/%s",!strncmp(action,"task-",5)?"tasks":"projects",id);method="DELETE";}
    http_status=0;J *response=api(method,endpoint,payload);request_id=NULL;jf(payload);
    /* Deleting an already deleted object is the desired state. */
    if(!response && http_status==404 && !strcmp(method,"DELETE"))response=jnull();
    return response;
}
static int sync_account(const char *dir) {
    int worker=cache_lock(dir,"sync.lock",1);if(worker<0)return 0;
    int rc=0;
    for(;;) {
        int fd=cache_lock(dir,"cache.lock",0);if(fd<0){rc=fail("Cannot lock cache");break;}
        J *db=cache_load(dir),*op=NULL;int valid=db!=NULL;if(db && jg(db,"queue")->len)op=jc(jg(db,"queue")->v[0]);jf(db);close(fd);
        if(!valid){rc=1;break;}if(!op)break;
        J *response=send_op(op);
        if(!response){jf(op);rc=1;break;}
        fd=cache_lock(dir,"cache.lock",0);db=fd>=0?cache_load(dir):NULL;
        if(!db){if(fd>=0)close(fd);jf(response);jf(op);rc=1;break;}
        J *queue=jg(db,"queue"),*head=queue->len?queue->v[0]:NULL;
        if(!head || strcmp(gs(head,"request"),gs(op,"request"))){rc=fail("Sync queue changed unexpectedly");}
        else if(gb(op,"create",0)) {
            const char *id=gs(response,"id");if(!*id)rc=fail("Create response has no ID; change remains queued");
            else {remap(db,gs(op,"id"),id);jremove(queue,0);}
        }else if(!strcmp(gs(op,"action"),"task-save") && gn(op,"phase",0)==0 && *gs(jg(op,"item"),"project_id"))jset(head,"phase",jnum(1));
        else jremove(queue,0);
        if(!rc)rc=cache_save(dir,db);
        jf(db);close(fd);jf(response);jf(op);if(rc)break;
    }
    if(!rc) {
        J *projects=api_list("projects"),*tasks=projects?api_list("tasks"):NULL;
        if(!projects || !tasks)rc=1;
        else {
            int fd=cache_lock(dir,"cache.lock",0);J *db=fd>=0?cache_load(dir):NULL;
            if(!db)rc=fail("Cannot reload cache");
            else {
                jset(db,"projects",jc(projects));jset(db,"tasks",jc(tasks));
                J *queue=jg(db,"queue");
                /* Edits made during the download remain visible and queued. */
                for(size_t i=0;i<queue->len && !rc;i++){J *op=queue->v[i];if(!strcmp(gs(op,"action"),"project-delete"))remote_delete_project(db,gs(op,"id"));if(!strcmp(gs(op,"action"),"task-complete") && !find(jg(db,"tasks"),gs(op,"id")))continue;
                    rc=apply(db,gs(op,"action"),gb(op,"create",0)?"":gs(op,"id"),jg(op,"item"),gs(op,"id"));}
                if(!rc){jset(db,"last_sync",jnum((double)time(NULL)));rc=cache_save(dir,db);}
            }
            jf(db);if(fd>=0)close(fd);
        }
        jf(projects);jf(tasks);
    }
    close(worker);return rc;
}
static void *sync_thread(void *argument) {
    char **settings=argument;char *dir=settings[0];sync_token=settings[1];free(settings);error[0]=0;int rc=sync_account(dir);free(dir);free(sync_token);sync_token=NULL;
    pthread_mutex_lock(&sync_mutex);snprintf(sync_message,sizeof sync_message,"%s",rc?(*error?error:"Sync failed; changes remain queued"):"");syncing=0;finished=1;pthread_mutex_unlock(&sync_mutex);return NULL;
}
void backend_sync_start(void) {
    pthread_mutex_lock(&sync_mutex);
    if(!syncing){char *dir=account_dir();if(dir){pthread_t thread;syncing=1;finished=0;sync_message[0]=0;
        char **settings=malloc(2*sizeof *settings);if(!settings)exit(1);settings[0]=dir;settings[1]=get_token();
        if(pthread_create(&thread,NULL,sync_thread,settings)){syncing=0;free(dir);free(settings[1]);free(settings);snprintf(sync_message,sizeof sync_message,"Cannot start sync");finished=1;}else pthread_detach(thread);
    }else {snprintf(sync_message,sizeof sync_message,"%s",*error?error:"Cannot load account");finished=1;}}
    pthread_mutex_unlock(&sync_mutex);
}
int backend_sync_poll(char *message,size_t length) {
    pthread_mutex_lock(&sync_mutex);int result=syncing?1:(finished?2:0);
    if(result==2){snprintf(message,length,"%s",sync_message);finished=0;}
    pthread_mutex_unlock(&sync_mutex);return result;
}
static int sync_now(void) {char *dir=account_dir();if(!dir)return 1;int rc=sync_account(dir);free(dir);return rc;}
