/* Build alongside no libraries; exercise the same native filtering logic. */
#define main tasks_application_main
#include "tasks.c"
#undef main
#include <assert.h>
int main(void) {
    char today[32];today_date(today,sizeof today);
    np=1;copy(projects[0].id,"inbox",128);nt=5;
    for(int i=0;i<nt;i++) {copy(tasks[i].project,"inbox",128);copy(tasks[i].title,"Task",TEXT);}
    copy(tasks[0].date,"2000-01-01",32);
    copy(tasks[1].date,today,32);
    copy(tasks[2].date,"9999-12-31",32);
    tasks[4].done=1;copy(tasks[4].date,"2000-01-01",32);
    select_nav(0);filter();assert(nv==4);
    select_nav(1);filter();assert(nv==2 && visible[0]==0 && visible[1]==1);
    select_nav(2);filter();assert(nv==1 && visible[0]==2);
    select_nav(3);filter();assert(nv==1 && visible[0]==4);
    select_nav(4);filter();assert(project==1 && nv==4);
    copy(search,"missing",TEXT);filter();assert(nv==0);
    puts("PASS: All Tasks, Today with overdue, Upcoming, Completed, project and search filters");
    return 0;
}
