/* Tasks: native UI and backend. Terminal controls from stored data are removed. */
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>

#define MAX 8192
#define TEXT 4096
#define BG "\033[48;2;34;34;38m\033[38;2;255;255;255m"
#define RAISED "\033[48;2;56;56;60m\033[38;2;255;255;255m"
#define ACCENT "\033[48;2;255;190;111m\033[38;2;34;34;38m"
#define MUTED "\033[38;2;170;170;170m"
typedef struct { char id[128],name[TEXT]; } Project;
typedef struct { char id[128],project[128],title[TEXT],description[TEXT],due[TEXT],labels[TEXT],date[32]; int priority,done; } Task;
static Project projects[MAX]; static Task tasks[MAX];
static int np,nt,visible[MAX],nv,project,selected,focus=1,completed,offset,poffset,view,nav;
static const char *views[]={"All Tasks","Today","Upcoming","Completed"};
static int rows=34,cols=110; static char backend[16],status[TEXT]="",search[TEXT];
static struct termios original; static volatile sig_atomic_t stopped;
static void copy(char *to,const char *from,size_t n) { size_t length=strnlen(from,n-1);memcpy(to,from,length);to[length]=0; }
static void restore(void) { tcsetattr(0,TCSAFLUSH,&original); printf("\033[?1000l\033[?1006l\033[?25h\033[0m\033[?1049l"); fflush(stdout); }
static void stop(int sig) { (void)sig; stopped=1; }
static void at(int y,int x) { printf("\033[%d;%dH",y,x); }
/* Count UTF-8 characters without cutting byte sequences. Controls are spaces. */
static void text(const char *s,int width) {
    int used=0;
    for (size_t i=0;s[i] && used<width;) {
        unsigned char c=(unsigned char)s[i];
        int bytes=c<128?1:(c<224?2:(c<240?3:4));
        if(c<32 || c==127) { putchar(' '); i++; }
        else { for(int j=0;j<bytes && s[i];j++) putchar(s[i++]); }
        used++;
    }
    while(used++<width) putchar(' ');
}
static void line(int y,int x,const char *s,int w,const char *color) { at(y,x); fputs(color,stdout); text(s,w); fputs(BG,stdout); }
/* Frames share the Agent-style labeled border; active focus uses orange. */
static void frame(int y,int x,int w,int h,const char *title,int active,int clear) {
    const char *border=active?"\033[38;2;255;190;111m":MUTED;
    if(w<4 || h<3)return;
    if(clear)for(int j=1;j<h-1;j++)line(y+j,x+1,"",w-2,BG);
    at(y,x);fputs(BG,stdout);fputs(border,stdout);fputs("┌",stdout);
    for(int i=0;i<w-2;i++)fputs("─",stdout);
    fputs("┐",stdout);
    for(int j=1;j<h-1;j++) {
        at(y+j,x);fputs(border,stdout);fputs("│",stdout);
        at(y+j,x+w-1);fputs("│",stdout);
    }
    at(y+h-1,x);fputs(border,stdout);fputs("└",stdout);
    for(int i=0;i<w-2;i++)fputs("─",stdout);
    fputs("┘",stdout);
    at(y,x+2);fputs(BG,stdout);fputs(border,stdout);putchar(' ');
    int title_width=(int)strlen(title);if(title_width>w-7)title_width=w-7;
    text(title,title_width);putchar(' ');fputs(BG,stdout);
}
static int run(char **args,const char *input,char **result) {
    return backend_dispatch(args,input,result);
}
static int call(char **args,const char *input) {
    char *output=NULL; copy(status,"Working...",sizeof status);
    line(rows-1,1,status,cols,RAISED);fflush(stdout);
    int code=run(args,input,&output);
    copy(status,code? (output && *output?output:"Operation failed") : "Saved",sizeof status);
    free(output);
    return code;
}
static void decode(char *s) {
    char *d=s;
    while(*s) { if(*s=='\\' && s[1]) { s++; *d++=*s=='n'?'\n':*s=='t'?'\t':*s=='r'?'\r':*s;s++; } else *d++=*s++; }
    *d=0;
}
static void today_date(char *date,size_t n) {
    time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);strftime(date,n,"%Y-%m-%d",&tm);
}
static int dated(const Task *t) {
    const char *d=t->date;
    if(strlen(d)<10 || d[4]!='-' || d[7]!='-')return 0;
    for(int i=0;i<10;i++)if(i!=4 && i!=7 && !isdigit((unsigned char)d[i]))return 0;
    return 1;
}
static void filter(void) {
    char today[32];today_date(today,sizeof today);nv=0;
    for(int i=0;i<nt;i++) {
        Task *t=&tasks[i];
        if(project && strcmp(t->project,projects[project-1].id))continue;
        if(!project && view==3) {if(!t->done)continue;}
        else if(t->done && !completed)continue;
        if(!project && (view==1 || view==2)) {
            if(t->done || !dated(t))continue;
            int cmp=strncmp(t->date,today,10);
            if((view==1 && cmp>0) || (view==2 && cmp<=0))continue;
        }
        if(*search && !strstr(t->title,search) && !strstr(t->description,search) && !strstr(t->labels,search))continue;
        visible[nv++]=i;
    }
    if(selected>=nv)selected=nv?nv-1:0;
    if(selected<0)selected=0;
}
static void select_nav(int index) {
    nav=index;project=index>=4?index-3:0;
    if(!project)view=index;
    selected=offset=0;
}
static void header(int loading) {
    char heading[TEXT];snprintf(heading,sizeof heading," Tasks   /   %s",backend);
    line(1,1,heading,cols,RAISED);
    if(loading && cols>=12)line(1,cols-10,"Loading...",10,RAISED);
}
static int refresh(void) {
    char *args[]={"tasks","snapshot",backend,NULL},*output=NULL;
    status[0]=0;header(1);fflush(stdout);
    if(run(args,NULL,&output)) { copy(status,output?output:"Connection failed",sizeof status);free(output);
    return 1; }
    char current[128]="",taskid[128]="";
    if(project && project<=np)copy(current,projects[project-1].id,sizeof current);
    if(nv && selected<nv)copy(taskid,tasks[visible[selected]].id,sizeof taskid);
    np=nt=0;
    char *save=NULL;
    for(char *row=strtok_r(output,"\n",&save);row;row=strtok_r(NULL,"\n",&save)) {
        char *fields[11];int nf=0; fields[nf++]=row;
        for(char *p=row;*p && nf<11;p++) if(*p=='\t') { *p=0;fields[nf++]=p+1; }
        for(int i=0;i<nf;i++)decode(fields[i]);
        if(nf==3 && !strcmp(fields[0],"P") && np<MAX) { copy(projects[np].id,fields[1],128);copy(projects[np++].name,fields[2],TEXT); }
        if(nf==10 && !strcmp(fields[0],"T") && nt<MAX) {
            Task *t=&tasks[nt++];copy(t->id,fields[1],128);copy(t->project,fields[2],128);copy(t->title,fields[3],TEXT);
            copy(t->description,fields[4],TEXT);copy(t->due,fields[5],TEXT);copy(t->labels,fields[6],TEXT);t->priority=atoi(fields[7]);t->done=atoi(fields[8]);copy(t->date,fields[9],sizeof t->date);
        }
    }
    free(output);project=0;
    for(int i=0;i<np;i++)if(!strcmp(current,projects[i].id))project=i+1;
    nav=project?project+3:view;
    filter();for(int i=0;i<nv;i++)if(!strcmp(taskid,tasks[visible[i]].id))selected=i;
    status[0]=0;
    return 0;
}
static void size(void) { struct winsize ws; if(!ioctl(0,TIOCGWINSZ,&ws) && ws.ws_row && ws.ws_col) {rows=ws.ws_row;cols=ws.ws_col;} }
static void draw(void) {
    size();printf(BG "\033[2J");
    if(cols<70 || rows<22) {line(1,1,"Tasks needs a window at least 70 columns by 22 rows.",cols,BG);fflush(stdout);return;}
    int sidebar=24,h=rows-14;
    char heading[TEXT];header(0);
    frame(3,1,24,6,"Tasks",focus==0 && nav<4,0);
    for(int i=0;i<4;i++)line(4+i,2,views[i],22,nav==i?ACCENT:BG);
    int ph=h-6;
    frame(9,1,24,ph+2,"Projects",focus==0 && nav>=4,0);
    int pi=project?project-1:0;
    if(pi<poffset)poffset=pi;
    if(pi>=poffset+ph)poffset=pi-ph+1;
    for(int j=0;j<ph && j+poffset<np;j++) {
        int i=j+poffset;line(10+j,2,projects[i].name,22,project==i+1?ACCENT:BG);
    }
    snprintf(heading,sizeof heading,"%.4000s  (%d)%s",project?projects[project-1].name:views[view],nv,completed && view!=3?" · including completed":"");
    frame(3,25,cols-25,h+2,heading,focus==1,0);
    if(selected<offset)offset=selected;
    if(selected>=offset+h)offset=selected-h+1;
    for(int j=0;j<h && j+offset<nv;j++) {
        Task *t=&tasks[visible[j+offset]];char prefix[32];snprintf(prefix,sizeof prefix," %s P%d ",t->done?"[x]":"[ ]",t->priority);
        at(4+j,sidebar+2);fputs(j+offset==selected?RAISED:BG,stdout);text(prefix,9);text(t->title,cols-sidebar-12);fputs(BG,stdout);
    }
    if(!nv)line(5,sidebar+3,!project && view==3 && !strcmp(backend,"todoist")?
        "Todoist completed history is not loaded.":"No tasks here. Press a to add a task.",cols-sidebar-4,MUTED);
    frame(rows-9,1,cols-1,5,"Details",0,0);
    if(nv) {
        Task *t=&tasks[visible[selected]];line(rows-8,2,t->title,cols-3,BG);
        if(*t->labels)snprintf(heading,sizeof heading,"Due: %.1000s   Labels: %.1000s",*t->due?t->due:"None",t->labels);
        else snprintf(heading,sizeof heading,"Due: %.1000s",*t->due?t->due:"None");
        line(rows-7,2,heading,cols-3,MUTED);
        line(rows-6,2,t->description,cols-3,BG);
    }

    line(rows-3,1," Tab pane | arrows navigate | a add | e edit | Space complete | / search",cols,RAISED);
    line(rows-2,1," n project | F2 rename | d delete | v completed | b backend | r refresh | q quit",cols,RAISED);
    line(rows-1,1,status,cols,MUTED);fflush(stdout);
}
enum { UP=1000,DOWN,LEFT,RIGHT,ESC,TAB,ENTER,BACK,CLICK,WHEELUP,WHEELDOWN,F2 };
static int mx,my;
static int byte(int timeout) { fd_set f;FD_ZERO(&f);FD_SET(0,&f);struct timeval t={timeout/1000,(timeout%1000)*1000};
    if(select(1,&f,NULL,NULL,&t)<=0)return -1;
    unsigned char c;
    return read(0,&c,1)==1?c:-1; }
static int key(void) {
    int c=byte(150);
    if(c<0)return -1;
    if(c==27) {
        int n=byte(40);
    if(n<0)return ESC;
        char sequence[80];int len=0;sequence[len++]=(char)n;
        while(len<79) {n=byte(40);
    if(n<0)break;
        sequence[len++]=(char)n;
    if(isalpha(n)||n=='~')break;}sequence[len]=0;
        if(!strcmp(sequence,"[A"))return UP;
    if(!strcmp(sequence,"[B"))return DOWN;
        if(!strcmp(sequence,"[C"))return RIGHT;
    if(!strcmp(sequence,"[D"))return LEFT;
        if(!strcmp(sequence,"OQ") || !strcmp(sequence,"[12~"))return F2;
        int button;char end;
        if(sscanf(sequence,"[<%d;%d;%d%c",&button,&mx,&my,&end)==4 && end=='M') {
            if(button==64)return WHEELUP;
    if(button==65)return WHEELDOWN;
    if(button==0)return CLICK;
        }
        return -1;
    }
    if(c==9)return TAB;
    if(c==10 || c==13)return ENTER;
    if(c==127 || c==8)return BACK;
    return c;
}
/* Single modal form: all fields visible, no nested selectors. */
static int form(const char *title,char fields[][TEXT],const char **labels,int count,int secret) {
    int field=0;
    while(!stopped) {
        draw();
        int height=count*2+6,width=cols-8;
        if(width>96)width=96;
        int left=(cols-width)/2+1,top=(rows-height)/2+1;
        if(top<2)top=2;
        frame(top,left,width,height,title,1,1);
        int x=left+2,y=top,w=width-4;
        for(int i=0;i<count;i++) {
            line(y+2+i*2,x,labels[i],w,MUTED);
            const char *value=secret?"[hidden]":fields[i];
            if(count==6 && i==5) {int p=atoi(fields[i])-1;if(p>=0 && p<np)value=projects[p].name;}
            line(y+3+i*2,x,value,w,i==field?RAISED:BG);
        }
        line(y+4+count*2,x,"Tab / arrows: field   Enter: save   Esc: cancel   Ctrl-u: clear",w,RAISED);
        fflush(stdout);int k;
        do {k=key();} while(k<0 && !stopped);
        if(k==ESC)return 0;
    if(k==ENTER)return 1;
        if(k==TAB || k==DOWN)field=(field+1)%count;
        else if(k==UP)field=(field+count-1)%count;
        else if(k==CLICK) {int f=(my-y-3)/2;
    if(my>=y+3 && f<count)field=f;}
        else if(count==6 && field==5) {
            int p=atoi(fields[5])-1;
            if(k==LEFT)p=(p+np-1)%np;
            else if(k==RIGHT)p=(p+1)%np;
            snprintf(fields[5],TEXT,"%d",p+1);
        }
        else if(k==21)fields[field][0]=0;
        else if(k==BACK) {size_t n=strlen(fields[field]);
    if(n) {n--;while(n && ((unsigned char)fields[field][n]&192)==128)n--;fields[field][n]=0;}}
        else if(k>=32 && k<256) {size_t n=strlen(fields[field]);
    if(n<TEXT-1){fields[field][n]=(char)k;fields[field][n+1]=0;}}
    }
    return 0;
}
static int confirm(const char *message) {char f[1][TEXT]={{0}};const char *l[]={"Type yes to confirm permanent deletion"};
    return form(message,f,l,1,0) && !strcmp(f[0],"yes");}
static void edit(int existing) {
    if(!np) {copy(status,"No projects loaded. Refresh or change backend first.",TEXT);return;}
    if(existing && !nv)return;
    Task empty={0},*t=existing?&tasks[visible[selected]]:&empty;
    char fields[6][TEXT]={{0}},old_due[TEXT];
    copy(fields[0],t->title,TEXT);copy(fields[1],t->description,TEXT);copy(fields[2],t->due,TEXT);copy(old_due,t->due,TEXT);copy(fields[3],t->labels,TEXT);
    snprintf(fields[4],TEXT,"%d",existing?t->priority:4);
    int p=project?project-1:0;for(int i=0;i<np;i++)if(!strcmp(t->project,projects[i].id))p=i;
    snprintf(fields[5],TEXT,"%d",p+1);
    const char *labels[]={"Title","Description","Due date (Todoist accepts natural language)","Labels (comma-separated)","Priority (1 highest, 4 lowest)","Project number (see list below)"};
    labels[5]="Project (Left / Right to choose)";
    while(form(existing?"Edit task":"New task",fields,labels,6,0)) {
        int priority=atoi(fields[4]),proj=atoi(fields[5]);
        if(!*fields[0] || priority<1 || priority>4 || proj<1 || proj>np) {copy(status,"Title, priority 1-4 and a valid project number are required",TEXT);continue;}
        char *args[]={"tasks","save",backend,existing?t->id:"",fields[0],fields[1],projects[proj-1].id,fields[2],fields[3],fields[4],strcmp(old_due,fields[2])?"1":"0",NULL};
        if(!call(args,NULL))refresh();
        return;
    }
}
static int setup(void) {
    const char *override=getenv("TASKS_BACKEND");
    if(override && (!strcmp(override,"local") || !strcmp(override,"todoist"))) {copy(backend,override,sizeof backend);return 1;}
    char *args[]={"tasks","preference",NULL},*out=NULL;
    run(args,NULL,&out);
    if(out){out[strcspn(out,"\r\n")]=0;copy(backend,out,sizeof backend);}free(out);
    return !strcmp(backend,"local") || !strcmp(backend,"todoist");
}
static int backend_picker(void) {
    int choice=!strcmp(backend,"todoist");
    while(!stopped) {
        draw();
        int width=cols-8;if(width>96)width=96;
        int left=(cols-width)/2+1,top=(rows-10)/2+1;
        frame(top,left,width,10,"Tasks settings",1,1);
        line(top+2,left+2,"Choose where Tasks stores your tasks",width-4,MUTED);
        line(top+4,left+2,"Local    — tasks on this computer",width-4,choice==0?ACCENT:BG);
        line(top+5,left+2,"Todoist  — tasks in your Todoist account",width-4,choice==1?ACCENT:BG);
        line(top+8,left+2,"Arrows / Tab: choose   Enter: apply   Esc: cancel",width-4,RAISED);
        fflush(stdout);
        int k=key();
        if(k==ESC || k==3)return -1;
        if(k==ENTER)return choice;
        if(k==UP || k==DOWN || k==LEFT || k==RIGHT || k==TAB || k==' ')choice=!choice;
        else if(k==CLICK && mx>=left+2 && mx<left+width-2 && (my==top+4 || my==top+5)) {
            return my==top+5;
        }
    }
    return -1;
}
static void choose_backend(void) {
    int choice=backend_picker();if(choice<0)return;
    const char *next=choice?"todoist":"local";
    if(choice) {
        char token[1][TEXT]={{0}};
        const char *labels[]={"API token: paste to connect; leave empty to use the saved token"};
        if(!form("Connect Todoist — token saved privately on this computer",token,labels,1,1))return;
        if(*token[0]) {
            char *args[]={"tasks","token",NULL};
            int failed=call(args,token[0]);memset(token,0,sizeof token);
            if(failed)return;
        }
    }
    char *args[]={"tasks","configure",(char *)next,NULL};
    if(call(args,NULL))return;
    copy(backend,next,sizeof backend);
    np=nt=nv=project=selected=offset=poffset=view=nav=0;
    search[0]=0;completed=0;
    refresh();
}
int main(int argc,char **argv) {
    if(backend_init())return 1;
    int first=1;
    const char *mode=getenv("TASKS_BACKEND");
    if(argc>1 && (!strcmp(argv[1],"--local") || !strcmp(argv[1],"--todoist"))) {
        mode=argv[1]+2;setenv("TASKS_BACKEND",mode,1);first++;
    }
    if(argc>first && (!strcmp(argv[first],"--help") || !strcmp(argv[first],"-h"))) {
        puts("Tasks [--local | --todoist] [list | projects | add JSON | edit ID JSON | complete ID | reopen ID | project-add NAME]\nLaunch without commands for the interface. Tab switches panes; b selects backend.");return 0;
    }
    if(mode && strcmp(mode,"local") && strcmp(mode,"todoist")){fprintf(stderr,"Invalid backend\n");return 1;}
    if(argc>first)return backend_cli(mode?mode:"local",argc-first,argv+first);
    if(!isatty(0) || !isatty(1)){fprintf(stderr,"Tasks needs an interactive terminal.\n");return 1;}
    if(tcgetattr(0,&original))return 1;
    struct termios raw=original;raw.c_lflag&=(tcflag_t)~(ICANON|ECHO|ISIG);raw.c_iflag&=(tcflag_t)~(IXON|ICRNL);raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
    if(tcsetattr(0,TCSAFLUSH,&raw))return 1;
    atexit(restore);signal(SIGTERM,stop);signal(SIGHUP,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    printf("\033[?1049h\033[?25l\033[?1000h\033[?1006h");size();
    if(!setup())choose_backend();else refresh();
    if(!*backend) {
        copy(backend,"local",sizeof backend);
        char *args[]={"tasks","configure",backend,NULL};call(args,NULL);refresh();
    }
    while(!stopped) {
        filter();draw();int k;
        do {
            k=key();int previous_rows=rows,previous_cols=cols;size();
            if(rows!=previous_rows || cols!=previous_cols)draw();
        } while(k<0 && !stopped);
        if(k=='q' || k==3)break;
        if(cols<70 || rows<22)continue;
        if(k==TAB || k==LEFT || k==RIGHT)focus=!focus;
        else if(k==UP || k==DOWN || k==WHEELUP || k==WHEELDOWN) {
            int delta=(k==UP || k==WHEELUP)?-1:1;
            if(focus) {selected+=delta;
    if(selected<0)selected=0;
    if(selected>=nv)selected=nv?nv-1:0;}
            else {
                int next=nav+delta;if(next<0)next=0;if(next>np+3)next=np+3;
                select_nav(next);
            }
        } else if(k==CLICK) {
            int h=rows-14;
            if(mx<=24) {
                if(my>=4 && my<=7) {select_nav(my-4);focus=0;}
                else if(my>=10 && my<rows-10) {
                    int p=my-10+poffset;
                    if(p<np) {select_nav(p+4);focus=0;}
                }
            }
            else if(my>=4 && my<4+h) {int s=my-4+offset;
    if(s<nv){selected=s;focus=1;}if(mx>=26 && mx<=30 && s<nv)k=' ';}
        }
        if(k=='a')edit(0);
        else if(k=='e' || k==ENTER)edit(1);
        else if(k==' ' && nv) {Task *t=&tasks[visible[selected]];char *a[]={"tasks","complete",backend,t->id,t->done?"false":"true",NULL};
    if(!call(a,NULL))refresh();
    }
        else if(k=='v') {completed=!completed;copy(status,!strcmp(backend,"todoist")?"Todoist returns active tasks only":"Completed visibility changed",TEXT);}
        else if(k=='r')refresh();
        else if(k=='b')choose_backend();
        else if(k=='/') {char f[1][TEXT];copy(f[0],search,TEXT);const char *l[]={"Search titles, descriptions and labels (Ctrl-u clears)"};
    if(form("Search",f,l,1,0)){copy(search,f[0],TEXT);selected=offset=0;}}
        else if(k==ESC)search[0]=0;
        else if(k=='n' || (k==F2 && project)) {
            char f[1][TEXT]={{0}};
    if(k==F2)copy(f[0],projects[project-1].name,TEXT);
    const char *l[]={"Project name"};
            if(form(k==F2?"Rename project":"New project",f,l,1,0) && *f[0]) {char *a[]={"tasks","project-save",backend,k==F2?projects[project-1].id:"",f[0],NULL};
    if(!call(a,NULL))refresh();
    }
        } else if(k=='d') {
            if(!focus && project && confirm(!strcmp(backend,"todoist")?"Delete project AND its tasks permanently?":"Delete project? Tasks will move to Inbox.")) {
                char *a[]={"tasks","project-delete",backend,projects[project-1].id,NULL};
    if(!call(a,NULL)){project=0;refresh();}
            } else if(focus && nv && confirm("Delete selected task permanently?")) {char *a[]={"tasks","delete",backend,tasks[visible[selected]].id,NULL};
    if(!call(a,NULL))refresh();
    }
        }
    }
    return 0;
}
