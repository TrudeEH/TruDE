#define TRUDE_BAR
#include "status.c"
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <glob.h>
#include <math.h>
#include "layer-shell.h"
#include "xdg-shell.h"

/* A single process, single status backend, fixed launcher anchor. */
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_subcompositor *subcompositor;
static struct zwlr_layer_shell_v1 *shell;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_cursor_theme *cursor_theme;
static struct wl_cursor *hand_cursor;
static struct wl_surface *cursor_surface;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer;
static struct wl_output *bar_output;
struct output { struct wl_output *object; char name[128]; int scale; };
static struct output outputs[16]; static int output_count;
static int width = 1920, height = 33, scale = 1, configured, redraw = 1, bottom;
static char target[128];
struct buffer { struct wl_buffer *object; void *data; size_t size; int busy, w, h; };
static struct buffer buffers[2];
struct item { char text[4096], tip[1024], class[64]; double x,w; unsigned bg,fg; const char *command; };
enum { TEMP, MEM, CPU, LAUNCH, CLOCK, DATE, INDICATORS, VOLUME, MAINT, POWER, MENU, ITEM_COUNT };
static struct item items[ITEM_COUNT];
static int selected_mode, hover = -1, workspace_hover, active_workspace = 1, visible[6], occupied[6];
static double mouse_x;
static unsigned long long old_total, old_idle;
static unsigned frames;
static struct wl_callback *frame_callback;
static int frame_pending;
static void show_tooltip(void);
static void hide_tooltip(void);
static int64_t hover_deadline;
static int64_t monotonic_ms(void){
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}
static void draw_tray(cairo_t *c);
static void click_tray(uint32_t button,uint32_t serial);
static struct xdg_wm_base *wm_base;
static struct wl_surface *menu_surface;
static int menu_pointer;
static void menu_motion(double y);
static void menu_activate(uint32_t time);
static void close_tray_menu(void);
static void adjust_volume(int steps);
static int tray_hover = -1;
static int tray_count;
static double workspace_x = 85;
static volatile sig_atomic_t refresh_requested;
static void refresh_status(int sig) { (void)sig; refresh_requested=1; }
static const unsigned bg0=0x222226, bg1=0x28282c, bg2=0x2e2e32, bg3=0x38383c, accent=0xffbe6f;
static void bar_status(const char *text, const char *tooltip, const char *class) {
    struct item *i = &items[selected_mode];
    if (strcmp(i->text,text) || strcmp(i->tip,tooltip) || strcmp(i->class,class ? class : "")) redraw=1;
    snprintf(i->text,sizeof i->text,"%s",text); snprintf(i->tip,sizeof i->tip,"%s",tooltip);
    snprintf(i->class,sizeof i->class,"%s",class ? class : "");
}
static void plain(int index,const char *text) { if(strcmp(items[index].text,text)) {snprintf(items[index].text,sizeof items[index].text,"%s",text);redraw=1;} }
static void color(cairo_t *c,unsigned rgb) { cairo_set_source_rgb(c,((rgb>>16)&255)/255.,((rgb>>8)&255)/255.,(rgb&255)/255.); }
static void rect(cairo_t *c,double x,double y,double w,double h,unsigned rgb) {color(c,rgb);cairo_rectangle(c,x,y,w,h);cairo_fill(c);}
/* Bounded layout cache: avoid font setup and markup parsing on every frame. */
struct text_cache { char markup[4096]; int size; PangoLayout *layout; };
static struct text_cache text_cache[32];
static unsigned text_cache_next;
static PangoLayout *text_layout(cairo_t *c,const char *markup,int size) {
    struct text_cache *entry=NULL;
    for(unsigned i=0;i<32;i++)if(text_cache[i].layout&&text_cache[i].size==size&&!strcmp(text_cache[i].markup,markup)){entry=&text_cache[i];break;}
    if(!entry){
        entry=&text_cache[text_cache_next++%32];
        if(entry->layout)g_object_unref(entry->layout);
        entry->layout=pango_cairo_create_layout(c);entry->size=size;
        snprintf(entry->markup,sizeof entry->markup,"%s",markup);
        PangoFontDescription *font=pango_font_description_new();
        pango_font_description_set_family(font,"JetBrainsMono Nerd Font");
        pango_font_description_set_weight(font,PANGO_WEIGHT_MEDIUM);
        pango_font_description_set_absolute_size(font,size*PANGO_SCALE);
        pango_layout_set_font_description(entry->layout,font);pango_font_description_free(font);
        GError *err=NULL;PangoAttrList *attrs=NULL;char *parsed=NULL;
        if(pango_parse_markup(markup,-1,0,&attrs,&parsed,NULL,&err)){
            pango_layout_set_text(entry->layout,parsed,-1);pango_layout_set_attributes(entry->layout,attrs);
            pango_attr_list_unref(attrs);g_free(parsed);
        }else{g_clear_error(&err);pango_layout_set_text(entry->layout,markup,-1);}
    }
    pango_cairo_update_layout(c,entry->layout);
    return entry->layout;
}
static void draw_text(cairo_t *c,const char *markup,double x,double y,double w,int size,unsigned rgb,int center_ink) {
    PangoLayout *layout=text_layout(c,markup,size);
    int tw,th;pango_layout_get_pixel_size(layout,&tw,&th);
    double tx=x+(w-tw)/2;
    if(center_ink){
        PangoRectangle ink;pango_layout_get_pixel_extents(layout,&ink,NULL);
        tx=x+(w-ink.width)/2-ink.x;
    }
    color(c,rgb);cairo_move_to(c,tx,y+(27-th)/2);pango_cairo_show_layout(c,layout);
}
static void text(cairo_t *c,const char *markup,double x,double y,double w,int size,unsigned rgb) {
    draw_text(c,markup,x,y,w,size,rgb,0);
}
static void wedge(cairo_t *c,double x,int direction,unsigned rgb) {
    color(c,rgb);cairo_move_to(c,x+(direction>0?0:12),3);cairo_line_to(c,x+(direction>0?12:0),16.5);cairo_line_to(c,x+(direction>0?0:12),30);cairo_close_path(c);cairo_fill(c);
}
static void release(void *data,struct wl_buffer *b) {(void)b;((struct buffer*)data)->busy=0;}
static const struct wl_buffer_listener buffer_listener={release};
static struct buffer *getbuffer(void) {
    struct buffer *b=NULL;for(int i=0;i<2;i++)if(!buffers[i].busy){b=&buffers[i];break;}if(!b)return NULL;
    int w=width*scale,h=height*scale;
    if(b->object && (b->w!=w||b->h!=h)){wl_buffer_destroy(b->object);munmap(b->data,b->size);memset(b,0,sizeof *b);}
    if(!b->object){b->w=w;b->h=h;b->size=(size_t)w*h*4;int fd=memfd_create("TruDE-bar",MFD_CLOEXEC);if(fd<0||ftruncate(fd,b->size)<0)exit(1);
        b->data=mmap(NULL,b->size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(b->data==MAP_FAILED)exit(1);
        struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,b->size);b->object=wl_shm_pool_create_buffer(pool,0,w,h,w*4,WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);close(fd);wl_buffer_add_listener(b->object,&buffer_listener,b);}
    return b;
}
static void frame_done(void *data,struct wl_callback *callback,uint32_t time){(void)data;(void)time;wl_callback_destroy(callback);frame_callback=NULL;frame_pending=0;}
static const struct wl_callback_listener frame_listener={frame_done};
static void render(void) {
    if(!configured||!redraw||frame_pending)return;
    struct buffer *b=getbuffer();if(!b)return;
    cairo_surface_t *cs=cairo_image_surface_create_for_data(b->data,CAIRO_FORMAT_ARGB32,b->w,b->h,b->w*4);cairo_t *c=cairo_create(cs);cairo_scale(c,scale,scale);
    rect(c,0,0,width,height,0x1d1d20);rect(c,3,3,width-6,27,bg0);
    unsigned launch_bg=!strcmp(items[LAUNCH].class,"screensharing")?0xf66151:(!strcmp(items[LAUNCH].class,"recording")||!strcmp(items[LAUNCH].class,"transcribing"))?0x99c1f1:accent;
    double center=width/2.;items[LAUNCH].x=center-21;items[LAUNCH].w=42;
    const double module=78, divider=12;
    double x=center-21-2*divider;
    for(int id=CPU;id>=TEMP;id--){x-=module;items[id].x=x;items[id].w=module;x-=divider;}
    x=center+21+2*divider;
    for(int id=CLOCK;id<=INDICATORS;id++){items[id].x=x;items[id].w=module;x+=items[id].w+divider;}
    /* Fit the right modules to their rendered content, keeping the edge anchored. */
    x=width-3;
    for(int id=MENU;id>=VOLUME;id--){
        PangoRectangle ink;pango_layout_get_pixel_extents(text_layout(c,items[id].text,id==MENU?16:12),&ink,NULL);
        double min_width=32;
        if(id==MAINT||id==POWER){
            /* Keep icon-only breathing room as a floor, rather than extra label padding. */
            PangoRectangle icon_ink;
            pango_layout_get_pixel_extents(text_layout(c,id==MAINT?"":"󰾅",12),&icon_ink,NULL);
            min_width=icon_ink.width+40;
        }
        items[id].w=fmax(min_width,ink.width+24);x-=items[id].w;items[id].x=x;x-=divider;
    }
    for(int id=0;id<ITEM_COUNT;id++){
        struct item *i=&items[id];if(id>=VOLUME&&id<=POWER&&i->x<items[INDICATORS].x+items[INDICATORS].w+24)continue;unsigned bg=i->bg,fg=i->fg;
        if(id==LAUNCH){bg=launch_bg;fg=bg0;}
        if((id==CPU||id==MEM)&&!strcmp(i->class,"warning"))fg=0xf8e45c;
        if((id==CPU||id==MEM||id==TEMP)&&!strcmp(i->class,"critical"))fg=0xf66151;
        if(id==MAINT&&!strcmp(i->class,"updated"))fg=0xbfbfc0;
        if(id==MAINT&&!strcmp(i->class,"updates"))fg=0xf8e45c;
        if(id==MAINT&&!strcmp(i->class,"trude-updates"))fg=0xf66151;
        if(id==POWER&&!strcmp(i->class,"performance"))fg=accent;
        if(id==POWER&&!strcmp(i->class,"power-saver"))fg=0x57e389;
        if(id==hover)fg=id==LAUNCH?0x595953:id==MENU?0xffa348:0xbfbfc0;
        rect(c,i->x,3,i->w,27,bg);
        if(id!=LAUNCH)draw_text(c,i->text,i->x,3,i->w,id==MENU?16:12,fg,id>=VOLUME);
        if(id>=TEMP&&id<=CPU){rect(c,i->x-12,3,12,27,id==TEMP?bg0:items[id-1].bg);wedge(c,i->x-12,-1,bg);}
        if(id>=CLOCK&&id<=INDICATORS){rect(c,i->x+i->w,3,12,27,id==INDICATORS?bg0:items[id+1].bg);wedge(c,i->x+i->w,1,bg);}
        if(id>=VOLUME&&id<=POWER){rect(c,i->x-12,3,12,27,id==VOLUME?bg0:items[id-1].bg);wedge(c,i->x-12,-1,bg);}
    }
    /* Nested powerline chevrons: half the 27-pixel height gives 45-degree edges. */
    const double launch_tip=13.5;
    rect(c,center-51,3,102,27,bg3);
    color(c,bg0);cairo_move_to(c,center-33,3);cairo_line_to(c,center+33,3);
    cairo_line_to(c,center+33+launch_tip,16.5);cairo_line_to(c,center+33,30);
    cairo_line_to(c,center-33,30);cairo_line_to(c,center-33-launch_tip,16.5);cairo_close_path(c);cairo_fill(c);
    color(c,launch_bg);cairo_move_to(c,center-21,3);cairo_line_to(c,center+21,3);
    cairo_line_to(c,center+21+launch_tip,16.5);cairo_line_to(c,center+21,30);
    cairo_line_to(c,center-21,30);cairo_line_to(c,center-21-launch_tip,16.5);cairo_close_path(c);cairo_fill(c);
    text(c,items[LAUNCH].text,center-21,3,42,20,hover==LAUNCH?0x595953:bg0);
    rect(c,items[MENU].x-12,3,12,27,bg3);
    wedge(c,items[MENU].x-12,-1,bg0);
    /* Workspaces remain separate from the independently centered launcher. */
    double wx=workspace_x=25+tray_count*24+12;wedge(c,wx-12,-1,bg1);rect(c,wx,3,160,27,bg1);
    for(int i=1;i<=5;i++)text(c,active_workspace==i||visible[i]?"":"",wx+(i-1)*32,3,32,active_workspace==i||visible[i]?18:12,workspace_hover==i?0xffa348:active_workspace==i?accent:visible[i]?0xffffff:occupied[i]?accent:0xffffff);
    wedge(c,wx+160,1,bg1);
    draw_tray(c);
    cairo_destroy(c);cairo_surface_flush(cs);cairo_surface_destroy(cs);
    wl_surface_set_buffer_scale(surface,scale);wl_surface_attach(surface,b->object,0,0);wl_surface_damage_buffer(surface,0,0,b->w,b->h);frame_callback=wl_surface_frame(surface);wl_callback_add_listener(frame_callback,&frame_listener,NULL);frame_pending=1;wl_surface_commit(surface);
    b->busy=1;redraw=0;frames++;
}
static void configure(void *data,struct zwlr_layer_surface_v1 *l,uint32_t serial,uint32_t w,uint32_t h){(void)data;zwlr_layer_surface_v1_ack_configure(l,serial);if(w)width=w;if(h)height=h;configured=1;redraw=1;}
static void closed(void *data,struct zwlr_layer_surface_v1 *l){(void)data;(void)l;running=0;}
static const struct zwlr_layer_surface_v1_listener layer_listener={configure,closed};
static void output_geometry(void*d,struct wl_output*o,int32_t x,int32_t y,int32_t pw,int32_t ph,int32_t sub,const char*make,const char*model,int32_t transform){(void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)model;(void)transform;}
static void output_mode(void*d,struct wl_output*o,uint32_t flags,int32_t w,int32_t h,int32_t refresh){(void)d;(void)o;(void)flags;(void)w;(void)h;(void)refresh;}
static void output_done(void*d,struct wl_output*o){(void)d;(void)o;}
static void output_scale(void*d,struct wl_output*o,int32_t s){(void)o;((struct output*)d)->scale=s;}
static void output_name(void*d,struct wl_output*o,const char*n){(void)o;snprintf(((struct output*)d)->name,128,"%s",n);}
static void output_description(void*d,struct wl_output*o,const char*n){(void)d;(void)o;(void)n;}
static const struct wl_output_listener output_listener={output_geometry,output_mode,output_done,output_scale,output_name,output_description};
static char *hypr(const char *request,int event) {
    static char response[131072];char path[PATH_MAX];const char *sig=getenv("HYPRLAND_INSTANCE_SIGNATURE");if(!sig)return NULL;
    snprintf(path,sizeof path,"%s/hypr/%s/.socket%s.sock",runtime,sig,event?"2":"");
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);struct sockaddr_un address={.sun_family=AF_UNIX};
    if(strlen(path)>=sizeof address.sun_path){close(fd);return NULL;}strcpy(address.sun_path,path);
    if(connect(fd,(struct sockaddr*)&address,sizeof address)<0){close(fd);return NULL;}
    if(event){fcntl(fd,F_SETFL,O_NONBLOCK);return (char*)(intptr_t)fd;}
    struct timeval timeout={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    if(write(fd,request,strlen(request))<0){close(fd);return NULL;}size_t used=0;ssize_t n;
    while(used<sizeof response-1&&(n=read(fd,response+used,sizeof response-1-used))>0)used+=n;
    response[used]=0;close(fd);return response;
}
static void workspaces(void) {
    char *s=hypr("j/workspaces",0);struct json_object *a=s?json_tokener_parse(s):NULL;
    memset(occupied,0,sizeof occupied);memset(visible,0,sizeof visible);
    if(a){for(size_t i=0;i<json_object_array_length(a);i++){struct json_object *w=json_object_array_get_idx(a,i),*v;
        int id=json_object_object_get_ex(w,"id",&v)?json_object_get_int(v):0;if(id>0&&id<=5)occupied[id]=json_object_object_get_ex(w,"windows",&v)&&json_object_get_int(v)>0;}json_object_put(a);}
    s=hypr("j/monitors",0);a=s?json_tokener_parse(s):NULL;
    if(a){for(size_t i=0;i<json_object_array_length(a);i++){struct json_object *w=json_object_array_get_idx(a,i),*v,*id;
        if(json_object_object_get_ex(w,"activeWorkspace",&v)&&json_object_object_get_ex(v,"id",&id)){int n=json_object_get_int(id);if(n>0&&n<=5)visible[n]=1;
            if(json_object_object_get_ex(w,"focused",&v)&&json_object_get_boolean(v))active_workspace=n;}}
        json_object_put(a);}redraw=1;
}
static void spawn(const char *command) {if(!command)return;pid_t pid=fork();if(pid==0){setsid();execl("/bin/sh","sh","-c",command,(char*)NULL);_exit(127);}}
static void hover_at(double x){tray_hover=x>=17&&x<17+tray_count*24?(int)(x-17)/24:-1;int h=-1;for(int i=0;i<ITEM_COUNT;i++)if(!(i>=VOLUME&&i<=POWER&&items[i].x<items[INDICATORS].x+items[INDICATORS].w+24)&&x>=items[i].x-((i==LAUNCH||i<=CPU||(i>=VOLUME&&i<=POWER))?12:0)&&x<items[i].x+items[i].w+((i==LAUNCH||(i>=CLOCK&&i<=INDICATORS))?12:0)){h=i;break;}int ws=x>=workspace_x&&x<workspace_x+160?1+(int)(x-workspace_x)/32:0;if(h!=hover||ws!=workspace_hover){hide_tooltip();hover_deadline=monotonic_ms()+250;hover=h;workspace_hover=ws;redraw=1;}}
static void pointer_enter(void*d,struct wl_pointer*p,uint32_t serial,struct wl_surface*s,wl_fixed_t x,wl_fixed_t y){
    (void)d;(void)s;(void)y;
    if(hand_cursor){
        struct wl_cursor_image *image=hand_cursor->images[0];
        struct wl_buffer *buffer=wl_cursor_image_get_buffer(image);
        if(buffer){
            wl_surface_set_buffer_scale(cursor_surface,scale);
            wl_pointer_set_cursor(p,serial,cursor_surface,image->hotspot_x/scale,image->hotspot_y/scale);
            wl_surface_attach(cursor_surface,buffer,0,0);
            wl_surface_damage(cursor_surface,0,0,image->width/scale,image->height/scale);
            wl_surface_commit(cursor_surface);
        }
    }
    menu_pointer=s==menu_surface;
    if(menu_pointer){menu_motion(wl_fixed_to_double(y));return;}
    mouse_x=wl_fixed_to_double(x);hover_at(mouse_x);
}
static void pointer_leave(void*d,struct wl_pointer*p,uint32_t serial,struct wl_surface*s){(void)d;(void)p;(void)serial;(void)s;menu_pointer=0;hide_tooltip();hover=-1;tray_hover=-1;workspace_hover=0;redraw=1;}
static void pointer_motion(void*d,struct wl_pointer*p,uint32_t t,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)t;if(menu_pointer){menu_motion(wl_fixed_to_double(y));return;}mouse_x=wl_fixed_to_double(x);hover_at(mouse_x);}
static void pointer_button(void*d,struct wl_pointer*p,uint32_t serial,uint32_t time,uint32_t button,uint32_t state){(void)d;(void)p;(void)serial;(void)time;if(state==WL_POINTER_BUTTON_STATE_PRESSED){
        if(!menu_pointer&&tray_hover>=0&&button==0x111)click_tray(button,serial);
        return;
    }
    if(menu_pointer){if(button==0x110)menu_activate(time);return;}
    if(tray_hover>=0){if(button!=0x111)click_tray(button,serial);}
    else if(workspace_hover&&button==0x110){char command[64];snprintf(command,sizeof command,"dispatch hl.dsp.focus({ workspace = %d })",workspace_hover);hypr(command,0);workspaces();}
    else if(hover==INDICATORS&&button==0x111)spawn("makoctl mode -t do-not-disturb");
    else if(hover>=0&&button==0x110)spawn(items[hover].command);
}
static double scroll_distance;
static int scroll_steps;
static void pointer_axis(void*d,struct wl_pointer*p,uint32_t t,uint32_t axis,wl_fixed_t value){
    (void)d;(void)t;
    if(axis==WL_POINTER_AXIS_VERTICAL_SCROLL&&hover==VOLUME){
        scroll_distance+=wl_fixed_to_double(value);
        if(wl_proxy_get_version((struct wl_proxy*)p)<5){
            if(scroll_distance!=0)adjust_volume(scroll_distance<0?1:-1);
            scroll_distance=0;
        }
    }
}
static void pointer_frame(void*d,struct wl_pointer*p){
    (void)d;(void)p;
    if(hover==VOLUME){
        if(scroll_steps)adjust_volume(-scroll_steps);
        else if(scroll_distance!=0)adjust_volume(scroll_distance<0?1:-1);
    }
    scroll_steps=0;scroll_distance=0;
}
static void pointer_axis_source(void*d,struct wl_pointer*p,uint32_t source){(void)d;(void)p;(void)source;}
static void pointer_axis_stop(void*d,struct wl_pointer*p,uint32_t time,uint32_t axis){(void)d;(void)p;(void)time;(void)axis;}
static void pointer_axis_discrete(void*d,struct wl_pointer*p,uint32_t axis,int32_t discrete){
    (void)d;(void)p;if(axis==WL_POINTER_AXIS_VERTICAL_SCROLL&&hover==VOLUME)scroll_steps+=discrete;
}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=pointer_motion,.button=pointer_button,.axis=pointer_axis,.frame=pointer_frame,.axis_source=pointer_axis_source,.axis_stop=pointer_axis_stop,.axis_discrete=pointer_axis_discrete};
static void capabilities(void*d,struct wl_seat*s,uint32_t caps){(void)d;if((caps&WL_SEAT_CAPABILITY_POINTER)&&!pointer){pointer=wl_seat_get_pointer(s);wl_pointer_add_listener(pointer,&pointer_listener,NULL);}}
static void seat_name(void*d,struct wl_seat*s,const char*n){(void)d;(void)s;(void)n;}
static const struct wl_seat_listener seat_listener={capabilities,seat_name};
static void wm_ping(void*d,struct xdg_wm_base*w,uint32_t serial){(void)d;xdg_wm_base_pong(w,serial);}
static const struct xdg_wm_base_listener wm_listener={wm_ping};
static void global_registry(void*d,struct wl_registry*r,uint32_t name,const char*interface,uint32_t version){(void)d;
    if(!strcmp(interface,"wl_compositor"))compositor=wl_registry_bind(r,name,&wl_compositor_interface,version<4?version:4);
    else if(!strcmp(interface,"xdg_wm_base")){wm_base=wl_registry_bind(r,name,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(wm_base,&wm_listener,NULL);}
    else if(!strcmp(interface,"wl_subcompositor"))subcompositor=wl_registry_bind(r,name,&wl_subcompositor_interface,1);
    else if(!strcmp(interface,"wl_shm"))shm=wl_registry_bind(r,name,&wl_shm_interface,1);
    else if(!strcmp(interface,"zwlr_layer_shell_v1"))shell=wl_registry_bind(r,name,&zwlr_layer_shell_v1_interface,1);
    else if(!strcmp(interface,"wl_output")&&output_count<16){struct output *o=&outputs[output_count++];o->scale=1;o->object=wl_registry_bind(r,name,&wl_output_interface,version<4?version:4);wl_output_add_listener(o->object,&output_listener,o);}
    else if(!strcmp(interface,"wl_seat")){seat=wl_registry_bind(r,name,&wl_seat_interface,version<5?version:5);wl_seat_add_listener(seat,&seat_listener,NULL);}}
static void global_removed(void*d,struct wl_registry*r,uint32_t n){(void)d;(void)r;(void)n;}
static const struct wl_registry_listener registry_listener={global_registry,global_removed};
/* Sensor topology rarely changes. Retain paths, not file descriptors, so
 * hot-unplug remains safe; failed reads trigger discovery on the next sample. */
static glob_t sensor_paths;
static int64_t sensor_discovery;
static void temperatures(void) {
    int64_t now = monotonic_ms();
    if (!sensor_discovery || now - sensor_discovery >= 60000) {
        globfree(&sensor_paths); memset(&sensor_paths, 0, sizeof sensor_paths);
        glob("/sys/class/hwmon/hwmon*/temp*_input", 0, NULL, &sensor_paths);
        glob("/sys/class/thermal/thermal_zone*/temp", GLOB_APPEND, NULL, &sensor_paths);
        sensor_discovery = now;
    }
    long max = 0;
    for (size_t i = 0; i < sensor_paths.gl_pathc; i++) {
        long temp = 0; FILE *f = fopen(sensor_paths.gl_pathv[i], "r");
        if (f) { if (fscanf(f, "%ld", &temp) == 1 && temp > max) max = temp; fclose(f); }
        else sensor_discovery = 0;
    }
    char out[128]; snprintf(out, sizeof out, " %ld°C", max / 1000); plain(TEMP, out);
    snprintf(items[TEMP].tip, sizeof items[TEMP].tip, "Maximum temperature: %ld°C", max / 1000);
    strcpy(items[TEMP].class, max >= 90000 ? "critical" : "normal");
}
static void statistics(void) {
    char out[128];unsigned long long total=0,idle=0,v[10]={0};FILE*f=fopen("/proc/stat","r");
    if(f){char line[256];if(fgets(line,sizeof line,f)){sscanf(line,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]);for(int i=0;i<8;i++)total+=v[i];idle=v[3]+v[4];}fclose(f);}
    int usage=total>old_total?100*(total-old_total-(idle-old_idle))/(total-old_total):0;old_total=total;old_idle=idle;
    snprintf(out,sizeof out,"󰍛 %d%%",usage);plain(CPU,out);snprintf(items[CPU].tip,sizeof items[CPU].tip,"CPU: %d%%",usage);strcpy(items[CPU].class,usage>=90?"critical":usage>=75?"warning":"normal");
    unsigned long long mt=0,ma=0;f=fopen("/proc/meminfo","r");if(f){char line[256];while(fgets(line,sizeof line,f)){sscanf(line,"MemTotal: %llu",&mt);sscanf(line,"MemAvailable: %llu",&ma);}fclose(f);}
    int percent=mt?(int)(100*(mt-ma)/mt):0;snprintf(out,sizeof out,"󰘚 %d%%",percent);plain(MEM,out);snprintf(items[MEM].tip,sizeof items[MEM].tip,"RAM: %.1f GiB / %.1f GiB (%d%%)",(mt-ma)/1048576.,mt/1048576.,percent);unsigned long long swap_total=0,swap_used=0,zram_total=0,zram_used=0;
    f=fopen("/proc/swaps","r");if(f){char line[512],device[256];unsigned long long cap,used;
        while(fgets(line,sizeof line,f))if(sscanf(line,"%255s %*s %llu %llu",device,&cap,&used)==3){if(strstr(device,"/zram")){zram_total+=cap;zram_used+=used;}else{swap_total+=cap;swap_used+=used;}}
        fclose(f);}
    size_t used_tip=strlen(items[MEM].tip);snprintf(items[MEM].tip+used_tip,sizeof items[MEM].tip-used_tip,"\nzRAM: %.1f GiB / %.1f GiB\nSWAP: %.1f GiB / %.1f GiB",zram_used/1048576.,zram_total/1048576.,swap_used/1048576.,swap_total/1048576.);
    strcpy(items[MEM].class,percent>=90?"critical":percent>=75?"warning":"normal");
}
static int maintenance_watch = -1;
static int maintenance_dirty = 1;
static void watch_maintenance(void) {
    maintenance_watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (maintenance_watch >= 0 && inotify_add_watch(maintenance_watch, runtime,
            IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF) < 0) {
        close(maintenance_watch); maintenance_watch = -1;
    }
}
static void maintenance_events(void) {
    union { struct inotify_event align; char data[4096]; } buf;
    ssize_t n;
    while ((n = read(maintenance_watch, buf.data, sizeof buf.data)) > 0) {
        for (size_t off = 0; off < (size_t)n;) {
            struct inotify_event *e = (struct inotify_event *)(buf.data + off);
            if ((e->mask & IN_Q_OVERFLOW) || (e->len && !strcmp(e->name, "TruDE-bar-maintenance.json")))
                maintenance_dirty = 1;
            if (e->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                close(maintenance_watch); maintenance_watch = -1; maintenance_dirty = 1; return;
            }
            off += sizeof *e + e->len;
        }
    }
}
static void maintenance(void) {char path[PATH_MAX];snprintf(path,sizeof path,"%s/TruDE-bar-maintenance.json",runtime);struct json_object *o=json_object_from_file(path),*v;
    if(o){if(json_object_object_get_ex(o,"class",&v)){const char *cls=json_object_get_string(v);if(strcmp(items[MAINT].class,cls))redraw=1;snprintf(items[MAINT].class,sizeof items[MAINT].class,"%s",cls);}if(json_object_object_get_ex(o,"text",&v))plain(MAINT,json_object_get_string(v));if(json_object_object_get_ex(o,"tooltip",&v))snprintf(items[MAINT].tip,sizeof items[MAINT].tip,"%s",json_object_get_string(v));json_object_put(o);}}
#include <pulse/pulseaudio.h>
static pa_mainloop *audio_loop;
static pa_context *audio;
/* Mirror PulseAudio descriptors into the outer poll so audio stays event-driven. */
static struct pollfd *audio_fds;
static unsigned audio_nfds,audio_capacity;
static int audio_poll_failed;
static struct pollfd *poll_fds;
static unsigned poll_capacity;
static int audio_poll(struct pollfd *fds,unsigned long n,int timeout,void *data){
    (void)timeout;(void)data;
    struct pollfd *copy=n>audio_capacity?realloc(audio_fds,n*sizeof *copy):audio_fds;
    audio_poll_failed=n&&!copy;
    if(!audio_poll_failed){
        audio_fds=copy;audio_nfds=(unsigned)n;
        if(n>audio_capacity)audio_capacity=(unsigned)n;
        for(unsigned i=0;i<audio_nfds;i++){audio_fds[i]=fds[i];audio_fds[i].revents=0;}
    }else audio_nfds=0;
    return poll(fds,n,0);
}
static char default_sink[256];
static void server_info(pa_context *c,const pa_server_info *info,void *data);
static pa_cvolume sink_volume;
static uint32_t sink_index=PA_INVALID_INDEX;
static int volume_requests;
static void volume_done(pa_context*c,int success,void*d){
    (void)d;if(volume_requests>0)volume_requests--;
    if(!volume_requests){pa_operation*o=pa_context_get_server_info(c,server_info,NULL);if(o)pa_operation_unref(o);}
    if(!success)fprintf(stderr,"TruDE-bar: volume change failed\n");
}
static void adjust_volume(int steps){
    if(!steps||!audio||pa_context_get_state(audio)!=PA_CONTEXT_READY||sink_index==PA_INVALID_INDEX||!pa_cvolume_valid(&sink_volume))return;
    pa_volume_t current=pa_cvolume_max(&sink_volume);
    int64_t next=(int64_t)current+(int64_t)steps*PA_VOLUME_NORM/20;
    if(next<0)next=0;
    if(next>PA_VOLUME_NORM)next=PA_VOLUME_NORM;
    pa_cvolume_scale(&sink_volume,(pa_volume_t)next);
    pa_operation*o=pa_context_set_sink_volume_by_index(audio,sink_index,&sink_volume,volume_done,NULL);
    if(o){volume_requests++;pa_operation_unref(o);}
    char out[128];unsigned percent=(unsigned)round(100.*pa_cvolume_avg(&sink_volume)/PA_VOLUME_NORM);
    snprintf(out,sizeof out,"%s %u%%",percent<34?"󰕿":percent<67?"󰖀":"󰕾",percent);plain(VOLUME,out);
}

static void sink_info(pa_context *c,const pa_sink_info *info,int eol,void *data){(void)c;(void)data;if(eol||!info)return;
    if(volume_requests)return;
    sink_volume=info->volume;sink_index=info->index;
    unsigned volume=(unsigned)round(100.*pa_cvolume_avg(&info->volume)/PA_VOLUME_NORM);char out[128];
    snprintf(out,sizeof out,"%s %u%%",info->mute?"󰖁":volume<34?"󰕿":volume<67?"󰖀":"󰕾",volume);plain(VOLUME,out);
    snprintf(items[VOLUME].tip,sizeof items[VOLUME].tip,"Output: %s (%u%%)",info->description,volume);
}
static void server_info(pa_context *c,const pa_server_info *info,void *data){(void)data;if(!info)return;snprintf(default_sink,sizeof default_sink,"%s",info->default_sink_name?info->default_sink_name:"");pa_operation *o=pa_context_get_sink_info_by_name(c,default_sink,sink_info,NULL);if(o)pa_operation_unref(o);}
static void audio_event(pa_context *c,pa_subscription_event_type_t t,uint32_t idx,void *data){(void)t;(void)idx;(void)data;pa_operation *o=pa_context_get_server_info(c,server_info,NULL);if(o)pa_operation_unref(o);}
static void audio_state(pa_context *c,void *data){(void)data;if(pa_context_get_state(c)==PA_CONTEXT_READY){pa_context_set_subscribe_callback(c,audio_event,NULL);pa_operation *o=pa_context_subscribe(c,PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SERVER,NULL,NULL);if(o)pa_operation_unref(o);o=pa_context_get_server_info(c,server_info,NULL);if(o)pa_operation_unref(o);}}
static void init_audio(void){sink_index=PA_INVALID_INDEX;volume_requests=0;audio_loop=pa_mainloop_new();pa_mainloop_set_poll_func(audio_loop,audio_poll,NULL);audio=pa_context_new(pa_mainloop_get_api(audio_loop),"TruDE-bar");pa_context_set_state_callback(audio,audio_state,NULL);pa_context_connect(audio,NULL,PA_CONTEXT_NOFLAGS,NULL);}

/* StatusNotifier watcher: clients register their own D-Bus name/object path.
 * IconPixmap works without loading a GTK icon theme; common IconName clients
 * use application-provided paths and installed hicolor PNGs. Exported menus are rendered by the bar. */
struct tray_item { char service[256], path[256]; cairo_surface_t *icon; int dirty; };
static struct tray_item tray_items[16];

/* Reject failed PNG surfaces so later search locations can still win. */
static cairo_surface_t *tray_png(const char *path){
    cairo_surface_t *icon=cairo_image_surface_create_from_png(path);
    if(cairo_surface_status(icon)==CAIRO_STATUS_SUCCESS)return icon;
    cairo_surface_destroy(icon);return NULL;
}
static cairo_surface_t *tray_icon_glob(const char *root,const char *name){
    glob_t paths={0};char pattern[PATH_MAX];cairo_surface_t *icon=NULL;
    snprintf(pattern,sizeof pattern,"%s/hicolor/*/apps/%s.png",root,name);
    glob(pattern,0,NULL,&paths);
    for(size_t i=0;i<paths.gl_pathc&&!icon;i++)icon=tray_png(paths.gl_pathv[i]);
    globfree(&paths);return icon;
}

static void tray_refresh(struct tray_item *t) {
    if(t->icon){cairo_surface_destroy(t->icon);t->icon=NULL;}
    sd_bus_message *m=NULL;
    if(sd_bus_call_method(session_bus,t->service,t->path,"org.freedesktop.DBus.Properties","Get",NULL,&m,"ss","org.kde.StatusNotifierItem","IconPixmap")>=0 &&
       sd_bus_message_enter_container(m,'v',"a(iiay)")>0 && sd_bus_message_enter_container(m,'a',"(iiay)")>0){
        while(sd_bus_message_enter_container(m,'r',"iiay")>0){int w,h;const void *pixels;size_t bytes;sd_bus_message_read(m,"ii",&w,&h);
            if(sd_bus_message_read_array(m,'y',&pixels,&bytes)>0&&w>0&&h>0&&w<=256&&h<=256&&bytes==(size_t)w*h*4&&!t->icon){
                t->icon=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);unsigned char *out=cairo_image_surface_get_data(t->icon);int stride=cairo_image_surface_get_stride(t->icon);const unsigned char *in=pixels;
                for(int y=0;y<h;y++)for(int x=0;x<w;x++){size_t p=((size_t)y*w+x)*4;uint32_t a=in[p],r=in[p+1],g=in[p+2],b=in[p+3];((uint32_t*)(out+y*stride))[x]=(a<<24)|((r*a/255)<<16)|((g*a/255)<<8)|(b*a/255);}
                cairo_surface_mark_dirty(t->icon);
            }sd_bus_message_exit_container(m);
        }
    }sd_bus_message_unref(m);m=NULL;
    if(!t->icon && sd_bus_call_method(session_bus,t->service,t->path,"org.freedesktop.DBus.Properties","Get",NULL,&m,"ss","org.kde.StatusNotifierItem","IconName")>=0){
        const char *name;if(sd_bus_message_enter_container(m,'v',"s")>0&&sd_bus_message_read(m,"s",&name)>0&&*name){
            if(name[0]=='/')t->icon=tray_png(name);
            else if(!strchr(name,'/')&&!strstr(name,"..")){
                /* Ayatana clients such as Steam keep icons outside system themes. */
                sd_bus_message *theme=NULL;const char *root;char file[PATH_MAX];
                if(sd_bus_call_method(session_bus,t->service,t->path,"org.freedesktop.DBus.Properties","Get",NULL,&theme,"ss","org.kde.StatusNotifierItem","IconThemePath")>=0&&
                   sd_bus_message_enter_container(theme,'v',"s")>0&&sd_bus_message_read(theme,"s",&root)>0&&root[0]=='/'){
                    snprintf(file,sizeof file,"%s/%s.png",root,name);t->icon=tray_png(file);
                    if(!t->icon)t->icon=tray_icon_glob(root,name);
                }
                sd_bus_message_unref(theme);
                if(!t->icon){
                    const char *data=getenv("XDG_DATA_HOME"),*home=getenv("HOME");
                    if(data&&*data)snprintf(file,sizeof file,"%s/icons",data);
                    else if(home)snprintf(file,sizeof file,"%s/.local/share/icons",home);
                    else file[0]=0;
                    if(*file)t->icon=tray_icon_glob(file,name);
                }
                if(!t->icon)t->icon=tray_icon_glob("/usr/local/share/icons",name);
                if(!t->icon)t->icon=tray_icon_glob("/usr/share/icons",name);
            }
        }
    }sd_bus_message_unref(m);redraw=1;
}
static void tray_update(void){
    for(int i=0;i<tray_count;i++)if(tray_items[i].dirty){
        struct tray_item snapshot=tray_items[i];
        tray_items[i].icon=NULL;tray_items[i].dirty=0;
        tray_refresh(&snapshot);
        int found=0;
        for(int j=0;j<tray_count;j++)if(!strcmp(snapshot.service,tray_items[j].service)&&!strcmp(snapshot.path,tray_items[j].path)){
            if(tray_items[j].icon)cairo_surface_destroy(tray_items[j].icon);
            tray_items[j].icon=snapshot.icon;found=1;break;
        }
        if(!found&&snapshot.icon)cairo_surface_destroy(snapshot.icon);
    }
}
static int tray_register(sd_bus_message *m,void *data,sd_bus_error *error){(void)data;(void)error;const char *name;
    if(sd_bus_message_read(m,"s",&name)<=0)return -EINVAL;
    const char *service=name[0]=='/'?sd_bus_message_get_sender(m):name,*path=name[0]=='/'?name:"/StatusNotifierItem";
    for(int i=0;i<tray_count;i++)if(!strcmp(service,tray_items[i].service)&&!strcmp(path,tray_items[i].path))return sd_bus_reply_method_return(m,"");
    if(tray_count<16){struct tray_item *t=&tray_items[tray_count++];memset(t,0,sizeof *t);t->dirty=1;snprintf(t->service,sizeof t->service,"%s",service);snprintf(t->path,sizeof t->path,"%s",path);
        char full[512];snprintf(full,sizeof full,"%s%s",service,path);sd_bus_emit_signal(session_bus,"/StatusNotifierWatcher","org.kde.StatusNotifierWatcher","StatusNotifierItemRegistered","s",full);redraw=1;}
    return sd_bus_reply_method_return(m,"");
}
static int tray_host(sd_bus_message*m,void*d,sd_bus_error*e){(void)d;(void)e;return sd_bus_reply_method_return(m,"");}
static int tray_get(sd_bus*b,const char*p,const char*i,const char*k,sd_bus_message*reply,void*d,sd_bus_error*e){(void)b;(void)p;(void)i;(void)d;(void)e;
    if(!strcmp(k,"IsStatusNotifierHostRegistered"))return sd_bus_message_append(reply,"b",1);
    if(!strcmp(k,"ProtocolVersion"))return sd_bus_message_append(reply,"i",0);
    sd_bus_message_open_container(reply,'a',"s");for(int n=0;n<tray_count;n++){char full[512];snprintf(full,sizeof full,"%.255s%.255s",tray_items[n].service,tray_items[n].path);sd_bus_message_append(reply,"s",full);}return sd_bus_message_close_container(reply);
}
static const sd_bus_vtable tray_vtable[]={SD_BUS_VTABLE_START(0),SD_BUS_METHOD("RegisterStatusNotifierItem","s","",tray_register,SD_BUS_VTABLE_UNPRIVILEGED),SD_BUS_METHOD("RegisterStatusNotifierHost","s","",tray_host,SD_BUS_VTABLE_UNPRIVILEGED),SD_BUS_PROPERTY("RegisteredStatusNotifierItems","as",tray_get,0,0),SD_BUS_PROPERTY("IsStatusNotifierHostRegistered","b",tray_get,0,0),SD_BUS_PROPERTY("ProtocolVersion","i",tray_get,0,0),SD_BUS_SIGNAL("StatusNotifierItemRegistered","s",0),SD_BUS_SIGNAL("StatusNotifierItemUnregistered","s",0),SD_BUS_SIGNAL("StatusNotifierHostRegistered","",0),SD_BUS_VTABLE_END};
static int tray_signal(sd_bus_message*m,void*d,sd_bus_error*e){(void)d;(void)e;const char *sender=sd_bus_message_get_sender(m);const char *member=sd_bus_message_get_member(m);
    if(member&&!strcmp(member,"NameOwnerChanged")){const char *name,*old,*now;if(sd_bus_message_read(m,"sss",&name,&old,&now)>0&&!*now){for(int i=0;i<tray_count;i++)if(!strcmp(tray_items[i].service,name)){if(tray_items[i].icon)cairo_surface_destroy(tray_items[i].icon);memmove(&tray_items[i],&tray_items[i+1],(tray_count-i-1)*sizeof tray_items[0]);tray_count--;memset(&tray_items[tray_count],0,sizeof tray_items[0]);redraw=1;break;}}}
    else if(sender)for(int i=0;i<tray_count;i++)if(!strcmp(sender,tray_items[i].service))tray_items[i].dirty=1;
    return 0;
}
/* Some clients retain their exported item but do not register again when
 * the watcher restarts. Probe existing connections asynchronously. */
static int tray_discovered(sd_bus_message *m,void *d,sd_bus_error *e){
    (void)e;char path[256];
    snprintf(path,sizeof path,"%s",d?(char*)d:"/StatusNotifierItem");free(d);
    if(sd_bus_message_is_method_error(m,NULL))return 0;
    /* GetAll can succeed with an empty dictionary for a missing interface. */
    if(sd_bus_message_enter_container(m,'a',"{sv}")<=0||sd_bus_message_at_end(m,0)>0)return 0;
    const char *service=sd_bus_message_get_sender(m);
    if(!service)return 0;
    for(int i=0;i<tray_count;i++)if(!strcmp(service,tray_items[i].service)&&!strcmp(tray_items[i].path,path))return 0;
    if(tray_count>=16)return 0;
    struct tray_item *t=&tray_items[tray_count++];memset(t,0,sizeof *t);
    snprintf(t->service,sizeof t->service,"%s",service);
    snprintf(t->path,sizeof t->path,"%s",path);t->dirty=1;redraw=1;
    char full[512];snprintf(full,sizeof full,"%s%s",t->service,t->path);
    sd_bus_emit_signal(session_bus,"/StatusNotifierWatcher","org.kde.StatusNotifierWatcher","StatusNotifierItemRegistered","s",full);
    return 0;
}
struct tray_probe { const char *service; };
static void tray_probe_node(GMarkupParseContext *ctx,const gchar *element,const gchar **keys,const gchar **values,gpointer data,GError **error){
    (void)ctx;(void)error;struct tray_probe *probe=data;
    if(strcmp(element,"node"))return;
    for(int i=0;keys[i];i++)if(!strcmp(keys[i],"name")&&*values[i]&&!strchr(values[i],'/')){
        char *path=g_strdup_printf("/org/ayatana/NotificationItem/%s",values[i]);
        if(sd_bus_call_method_async(session_bus,NULL,probe->service,path,"org.freedesktop.DBus.Properties","GetAll",tray_discovered,path,"s","org.kde.StatusNotifierItem")<0)g_free(path);
    }
}
static int tray_ayatana_discovered(sd_bus_message *m,void *d,sd_bus_error *e){
    (void)d;(void)e;const char *xml;
    if(sd_bus_message_is_method_error(m,NULL)||sd_bus_message_read(m,"s",&xml)<=0)return 0;
    struct tray_probe probe={sd_bus_message_get_sender(m)};
    if(!probe.service)return 0;
    GMarkupParser parser={.start_element=tray_probe_node};
    GMarkupParseContext *ctx=g_markup_parse_context_new(&parser,0,&probe,NULL);
    if(g_markup_parse_context_parse(ctx,xml,strlen(xml),NULL))g_markup_parse_context_end_parse(ctx,NULL);
    g_markup_parse_context_free(ctx);return 0;
}
static void tray_discover(void){
    char **names=NULL;
    if(sd_bus_list_names(session_bus,&names,NULL)<0)return;
    for(char **n=names;n&&*n;n++){
        if((*n)[0]==':'){
            sd_bus_call_method_async(session_bus,NULL,*n,"/StatusNotifierItem","org.freedesktop.DBus.Properties","GetAll",tray_discovered,NULL,"s","org.kde.StatusNotifierItem");
            sd_bus_call_method_async(session_bus,NULL,*n,"/org/ayatana/NotificationItem","org.freedesktop.DBus.Introspectable","Introspect",tray_ayatana_discovered,NULL,"");
        }
        free(*n);
    }
    free(names);
}
static void init_tray(void){if(!session_bus)return;sd_bus_add_object_vtable(session_bus,NULL,"/StatusNotifierWatcher","org.kde.StatusNotifierWatcher",tray_vtable,NULL);
    if(sd_bus_request_name(session_bus,"org.kde.StatusNotifierWatcher",0)<0)return;
    sd_bus_request_name(session_bus,"org.freedesktop.StatusNotifierWatcher",0);
    sd_bus_add_match(session_bus,NULL,"type='signal',interface='org.kde.StatusNotifierItem'",tray_signal,NULL);
    sd_bus_add_match(session_bus,NULL,"type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'",tray_signal,NULL);
    sd_bus_emit_signal(session_bus,"/StatusNotifierWatcher","org.kde.StatusNotifierWatcher","StatusNotifierHostRegistered","");
    tray_discover();
}
static void draw_tray(cairo_t*c){for(int i=0;i<tray_count;i++){double x=17+i*24;
    if(tray_items[i].icon){cairo_save(c);cairo_translate(c,x,7);cairo_scale(c,18./cairo_image_surface_get_width(tray_items[i].icon),18./cairo_image_surface_get_height(tray_items[i].icon));cairo_set_source_surface(c,tray_items[i].icon,0,0);cairo_paint(c);cairo_restore(c);}
    else text(c,"•",x,3,18,12,0xaaaaaa);
}}
#include "tray-menu.c"
static void click_tray(uint32_t button,uint32_t serial){
    if(tray_hover<0||tray_hover>=tray_count)return;
    struct tray_item *t=&tray_items[tray_hover];
    if(button==0x111){open_tray_menu(t,serial);return;}
    if(button!=0x110&&button!=0x112)return;
    sd_bus_message *m=NULL;
    sd_bus_call_method(session_bus,t->service,t->path,"org.kde.StatusNotifierItem",button==0x112?"SecondaryActivate":"Activate",NULL,&m,"ii",(int)mouse_x,16);
    sd_bus_message_unref(m);
}
static void init_items(void) {
    for(int i=0;i<ITEM_COUNT;i++){items[i].bg=bg0;items[i].fg=0xffffff;}
    items[TEMP].bg=items[INDICATORS].bg=items[VOLUME].bg=bg1;
    items[MEM].bg=items[DATE].bg=items[MAINT].bg=bg2;
    items[CPU].bg=items[CLOCK].bg=items[POWER].bg=bg3;
    items[MENU].fg=accent;
    items[TEMP].command="footclient --app-id=temperature-tui --title='Temperature Monitor' --override=colors.regular0=222226 --window-size-chars=104x32 TruDE-temperature-tui";
    items[MEM].command=items[CPU].command="footclient --app-id=btop --title='System Monitor' --window-size-chars=120x36 TruDE-system-monitor-tui";
    items[LAUNCH].command="TruDE-app-launcher-toggle";
    items[CLOCK].command=items[DATE].command="footclient --app-id=notification-tui --title='Notifications' --override=colors.regular0=222226 --window-size-chars=94x28 TruDE-notification-tui";
    items[INDICATORS].command="footclient --app-id=status-menu --title='Quick Settings' --override=colors.regular0=222226 --window-size-chars=42x10 TruDE-bar-status-menu";
    items[VOLUME].command="footclient --app-id=pulsemixer --title='Volume Mixer' --window-size-chars=94x28 pulsemixer";
    items[MAINT].command="footclient --app-id=maintenance-tui --title='System Maintenance' --override=colors.regular0=222226 --window-size-chars=104x32 TruDE-maintenance-tui";
    items[POWER].command="footclient --app-id=power-profiles-tui --title='Power Profile' --override=colors.regular0=222226 --window-size-chars=82x24 TruDE-power-profiles-tui";
    items[MENU].command="footclient --app-id=power-menu-tui --title='Power' --override=colors.regular0=222226 --window-size-chars=88x23 TruDE-power-menu-tui";
    plain(MENU,"󰐥");plain(MAINT,"");plain(VOLUME,"󰕾 —");
}
static void preferences(void) {
    char path[PATH_MAX];const char *config=getenv("XDG_CONFIG_HOME");if(config)snprintf(path,sizeof path,"%s/TruDE/settings.json",config);else snprintf(path,sizeof path,"%s/.config/TruDE/settings.json",getenv("HOME"));
    struct json_object *o=json_object_from_file(path),*bar,*v;
    if(o&&json_object_object_get_ex(o,"waybar",&bar)){
        if(json_object_object_get_ex(bar,"enabled",&v)&&!json_object_get_boolean(v)){json_object_put(o);exit(0);}
        if(json_object_object_get_ex(bar,"position",&v))bottom=!strcmp(json_object_get_string(v),"bottom");
    }
    json_object_put(o);
}

static struct wl_surface *tip_surface;
static struct zwlr_layer_surface_v1 *tip_layer;
static int tip_width,tip_height;
static struct wl_buffer *tip_buffer;
static void *tip_data;
static size_t tip_size;
static int tip_released;
static char tip_last[1024];
static void tip_release(void*d,struct wl_buffer*b){(void)d;(void)b;tip_released=1;}
static const struct wl_buffer_listener tip_listener={tip_release};
static void hide_tooltip(void){
    if(tip_surface){zwlr_layer_surface_v1_destroy(tip_layer);wl_surface_destroy(tip_surface);tip_surface=NULL;tip_layer=NULL;}
    if(tip_buffer){wl_buffer_destroy(tip_buffer);tip_buffer=NULL;}
    if(tip_data){munmap(tip_data,tip_size);tip_data=NULL;}
    tip_last[0]=0;
}
static void tip_configure(void*d,struct zwlr_layer_surface_v1*l,uint32_t serial,uint32_t w,uint32_t h){
    (void)d;(void)w;(void)h;zwlr_layer_surface_v1_ack_configure(l,serial);
    wl_surface_set_buffer_scale(tip_surface,scale);wl_surface_attach(tip_surface,tip_buffer,0,0);
    wl_surface_damage_buffer(tip_surface,0,0,tip_width*scale,tip_height*scale);wl_surface_commit(tip_surface);
}
static void tip_closed(void*d,struct zwlr_layer_surface_v1*l){(void)d;(void)l;hide_tooltip();}
static const struct zwlr_layer_surface_v1_listener tip_layer_listener={tip_configure,tip_closed};
static void show_tooltip(void){
    if(hover<0||menu_surface)return;
    char content[1024];snprintf(content,sizeof content,"%s",items[hover].tip);
    if(hover==CLOCK||hover==DATE){
        time_t t=time(NULL);struct tm today;localtime_r(&t,&today);
        size_t n=strftime(content,sizeof content,"%B %Y\n",&today);
        n+=snprintf(content+n,sizeof content-n,"Mo Tu We Th Fr Sa Su\n");
        struct tm first=today;first.tm_mday=1;first.tm_hour=12;first.tm_isdst=-1;mktime(&first);
        int offset=(first.tm_wday+6)%7;
        for(int k=0;k<offset;k++)n+=snprintf(content+n,sizeof content-n,"   ");
        for(int day=1;day<=31;day++){
            struct tm date=first;date.tm_mday=day;mktime(&date);if(date.tm_mon!=today.tm_mon)break;
            n+=snprintf(content+n,sizeof content-n,"%2d%s",day,(offset+day)%7==0?"\n":" ");
        }
        snprintf(content+n,sizeof content-n,"\nToday: %d",today.tm_mday);
    }
    if(!*content)return;
    if(tip_surface&&!strcmp(content,tip_last))return;
    hide_tooltip();snprintf(tip_last,sizeof tip_last,"%s",content);
    /* Measure the same wrapped layout that will be painted, plus eight-pixel padding. */
    cairo_surface_t *measure=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1,1);
    cairo_t *mc=cairo_create(measure);
    PangoLayout *l=pango_cairo_create_layout(mc);
    PangoFontDescription *f=pango_font_description_from_string("JetBrainsMono Nerd Font 9");
    pango_layout_set_font_description(l,f);pango_layout_set_text(l,content,-1);
    pango_layout_set_wrap(l,PANGO_WRAP_WORD_CHAR);
    int tw,th;pango_layout_get_pixel_size(l,&tw,&th);
    int max_width=width-22;if(max_width>404)max_width=404;
    if(tw>max_width){pango_layout_set_width(l,max_width*PANGO_SCALE);pango_layout_get_pixel_size(l,&tw,&th);}
    int w=tw+16,h=th+16;
    g_object_unref(l);pango_font_description_free(f);cairo_destroy(mc);cairo_surface_destroy(measure);
    tip_size=(size_t)w*h*4*scale*scale;
    int fd=memfd_create("bar-tooltip",MFD_CLOEXEC);if(fd<0)return;if(ftruncate(fd,tip_size)<0){close(fd);return;}
    tip_data=mmap(NULL,tip_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(tip_data==MAP_FAILED){tip_data=NULL;close(fd);return;}
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,tip_size);
    tip_buffer=wl_shm_pool_create_buffer(pool,0,w*scale,h*scale,w*scale*4,WL_SHM_FORMAT_ARGB8888);wl_shm_pool_destroy(pool);close(fd);wl_buffer_add_listener(tip_buffer,&tip_listener,NULL);tip_released=0;
    tip_surface=wl_compositor_create_surface(compositor);tip_width=w;tip_height=h;
    int x=(int)(items[hover].x+items[hover].w/2)-w/2;if(x<3)x=3;if(x+w>width-3)x=width-3-w;
    tip_layer=zwlr_layer_shell_v1_get_layer_surface(shell,tip_surface,bar_output,ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,"TruDE-bar-tooltip");
    zwlr_layer_surface_v1_add_listener(tip_layer,&tip_layer_listener,NULL);
    zwlr_layer_surface_v1_set_size(tip_layer,w,h);
    zwlr_layer_surface_v1_set_anchor(tip_layer,ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|(bottom?ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP));
    zwlr_layer_surface_v1_set_exclusive_zone(tip_layer,-1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(tip_layer,0);
    zwlr_layer_surface_v1_set_margin(tip_layer,bottom?0:height+4,0,bottom?height+4:0,x);
    struct wl_region *empty=wl_compositor_create_region(compositor);wl_surface_set_input_region(tip_surface,empty);wl_region_destroy(empty);
    cairo_surface_t *cs=cairo_image_surface_create_for_data(tip_data,CAIRO_FORMAT_ARGB32,w*scale,h*scale,w*scale*4);cairo_t*c=cairo_create(cs);cairo_scale(c,scale,scale);
    rect(c,0,0,w,h,bg0);color(c,0x77767b);cairo_rectangle(c,.5,.5,w-1,h-1);cairo_stroke(c);
    l=pango_cairo_create_layout(c);f=pango_font_description_from_string("JetBrainsMono Nerd Font 9");pango_layout_set_font_description(l,f);pango_layout_set_text(l,content,-1);pango_layout_set_width(l,(w-16)*PANGO_SCALE);pango_layout_set_wrap(l,PANGO_WRAP_WORD_CHAR);color(c,0xffffff);cairo_move_to(c,8,8);pango_cairo_show_layout(c,l);g_object_unref(l);pango_font_description_free(f);
    cairo_destroy(c);cairo_surface_flush(cs);cairo_surface_destroy(cs);
    /* Initial commit must be bufferless; attach only after configure/ack. */
    wl_surface_commit(tip_surface);
}
int main(int argc,char **argv){
    runtime=getenv("XDG_RUNTIME_DIR");if(!runtime)runtime="/tmp";mode="indicators";part="center";
    signal(SIGUSR1,refresh_status);signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);signal(SIGCHLD,SIG_IGN);
    preferences();init_items();
    char *s=hypr("j/monitors",0);struct json_object *monitors=s?json_tokener_parse(s):NULL;
    if(monitors){for(size_t i=0;i<json_object_array_length(monitors);i++){struct json_object *m=json_object_array_get_idx(monitors,i),*x,*y,*n;
        if(json_object_object_get_ex(m,"x",&x)&&json_object_object_get_ex(m,"y",&y)&&json_object_get_int(x)==0&&json_object_get_int(y)==0&&json_object_object_get_ex(m,"name",&n))snprintf(target,sizeof target,"%s",json_object_get_string(n));}json_object_put(monitors);}
    if(argc>1)snprintf(target,sizeof target,"%s",argv[1]);
    display=wl_display_connect(NULL);if(!display){fprintf(stderr,"Cannot connect to Wayland\n");return 1;}
    struct wl_registry *reg=wl_display_get_registry(display);wl_registry_add_listener(reg,&registry_listener,NULL);wl_display_roundtrip(display);wl_display_roundtrip(display);
    if(!compositor||!shm||!shell){fprintf(stderr,"Compositor lacks layer-shell or shared-memory support\n");return 1;}
    struct wl_output *output=NULL;for(int i=0;i<output_count;i++)if(!strcmp(outputs[i].name,target)){output=outputs[i].object;scale=outputs[i].scale;break;}
    if(*target&&!output){fprintf(stderr,"Output %s unavailable\n",target);return 1;}
    const char *cursor_size=getenv("XCURSOR_SIZE");int size=cursor_size?atoi(cursor_size):24;if(size<1)size=24;
    cursor_theme=wl_cursor_theme_load(getenv("XCURSOR_THEME"),size*scale,shm);
    if(cursor_theme){
        hand_cursor=wl_cursor_theme_get_cursor(cursor_theme,"pointer");
        if(!hand_cursor)hand_cursor=wl_cursor_theme_get_cursor(cursor_theme,"hand2");
        if(hand_cursor)cursor_surface=wl_compositor_create_surface(compositor);
    }
    bar_output=output;surface=wl_compositor_create_surface(compositor);layer=zwlr_layer_shell_v1_get_layer_surface(shell,surface,output,ZWLR_LAYER_SHELL_V1_LAYER_TOP,"TruDE-bar");
    zwlr_layer_surface_v1_add_listener(layer,&layer_listener,NULL);zwlr_layer_surface_v1_set_size(layer,0,height);
    zwlr_layer_surface_v1_set_anchor(layer,ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT|(bottom?ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP));
    zwlr_layer_surface_v1_set_exclusive_zone(layer,height);zwlr_layer_surface_v1_set_keyboard_interactivity(layer,0);wl_surface_commit(surface);wl_display_roundtrip(display);
    connect_buses();init_tray();pw_init(NULL,NULL);pwloop=pw_main_loop_new(NULL);
    /* Embedded loops must enter their dispatch thread before binding new nodes. */
    pw_loop_enter(pw_main_loop_get_loop(pwloop));context=pw_context_new(pw_main_loop_get_loop(pwloop),NULL,0);connect_pipewire();init_audio();
    int event_fd=(int)(intptr_t)hypr(NULL,1);if(!event_fd)event_fd=-1;workspaces();statistics();temperatures();watch_maintenance();maintenance();maintenance_dirty=0;
    int64_t last_stats=0,last_temp=0,last_indicator=0,last_power=0,last_second=0,last_retry=0,last_maintenance=0,last_cache=0;
    while(running){
        while(wl_display_prepare_read(display)!=0)if(wl_display_dispatch_pending(display)<0){running=0;break;}
        if(!running)break;
        wl_display_flush(display);
        /* Backend events wake us early; a pending tooltip bounds the status timeout. */
        if(poll_capacity<6+audio_nfds){
            struct pollfd *next=realloc(poll_fds,(6+audio_nfds)*sizeof *next);
            if(!next){wl_display_cancel_read(display);running=0;break;}
            poll_fds=next;poll_capacity=6+audio_nfds;
        }
        struct pollfd *fds=poll_fds;
        fds[0]=(struct pollfd){wl_display_get_fd(display),POLLIN,0};
        fds[1]=(struct pollfd){system_bus?sd_bus_get_fd(system_bus):-1,POLLIN,0};
        fds[2]=(struct pollfd){session_bus?sd_bus_get_fd(session_bus):-1,POLLIN,0};
        fds[3]=(struct pollfd){pw_loop_get_fd(pw_main_loop_get_loop(pwloop)),POLLIN,0};
        fds[4]=(struct pollfd){event_fd,POLLIN,0};
        fds[5]=(struct pollfd){maintenance_watch,POLLIN,0};
        for(unsigned i=0;i<audio_nfds;i++)fds[6+i]=audio_fds[i];
        int timeout=audio_poll_failed?16:1000;
        sd_bus *buses[2]={system_bus,session_bus};
        for(int i=0;i<2;i++)if(buses[i]){
            fds[1+i].events=sd_bus_get_events(buses[i]);
            uint64_t deadline;
            if(sd_bus_get_timeout(buses[i],&deadline)>=0&&deadline!=UINT64_MAX){
                int64_t remaining=(int64_t)(deadline/1000)-monotonic_ms();
                if(remaining<timeout)timeout=remaining>0?(int)remaining:0;
            }
        }
        if(hover>=0&&hover_deadline){
            int64_t remaining=hover_deadline-monotonic_ms();
            if(remaining<timeout)timeout=remaining>0?(int)remaining:0;
        }
        poll(fds,6+audio_nfds,timeout);
        if(fds[0].revents&POLLIN){if(wl_display_read_events(display)<0)running=0;}else wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);
        short event_ready=fds[4].revents;
        if(fds[5].revents)maintenance_events();
        drain(&system_bus);drain(&session_bus);tray_update();if(pw_loop_iterate(pw_main_loop_get_loop(pwloop),0)<0)pw_failed=1;pa_mainloop_iterate(audio_loop,0,NULL);
        if(event_fd>=0&&event_ready){char events[4096];ssize_t n=read(event_fd,events,sizeof events);if(n>0){events[(size_t)n<sizeof events?(size_t)n:sizeof events-1]=0;if(strstr(events,"workspace")||strstr(events,"focusedmon")||strstr(events,"openwindow")||strstr(events,"closewindow")||strstr(events,"movewindow"))workspaces();}else if(n==0){close(event_fd);event_fd=-1;}}
        int64_t now=monotonic_ms()/1000;
        if(refresh_requested){refresh_requested=0;dirty=1;query_generation++;maintenance_dirty=1;last_maintenance=0;last_power=0;}
        if(dirty||now!=last_second){char out[128];struct tm tm;time_t wall=time(NULL);localtime_r(&wall,&tm);strftime(out,sizeof out,"%I:%M %p",&tm);plain(CLOCK,out);strftime(out,sizeof out,"%d %b",&tm);plain(DATE,out);selected_mode=LAUNCH;launcher();last_second=now;}
        if(now-last_maintenance>=3600){spawn("TruDE-bar-maintenance-status >/dev/null 2>&1");last_maintenance=now;}
        if(now-last_stats>=2){statistics();last_stats=now;}
        if(now-last_temp>=5){temperatures();last_temp=now;}
        if(maintenance_dirty||now-last_cache>=60){maintenance();maintenance_dirty=0;last_cache=now;}
        if(dirty||now-last_indicator>=5){selected_mode=INDICATORS;indicators();last_indicator=now;}
        if(dirty||now-last_power>=30){selected_mode=POWER;power();last_power=now;}dirty=0;
        if(now-last_retry>=5){status_cache_prune();if(pa_context_get_state(audio)==PA_CONTEXT_FAILED){pa_context_unref(audio);pa_mainloop_free(audio_loop);init_audio();}connect_buses();if(pw_failed)disconnect_pipewire();if(!core)connect_pipewire();if(event_fd<0){event_fd=(int)(intptr_t)hypr(NULL,1);if(!event_fd)event_fd=-1;}if(maintenance_watch<0)watch_maintenance();last_retry=now;}
        render();
        if(hover>=0&&(!hover_deadline||monotonic_ms()>=hover_deadline)){
            hover_deadline=0;show_tooltip();
        }
    }
    close_tray_menu();hide_tooltip();
    free(audio_fds);free(poll_fds);
    globfree(&sensor_paths);status_cache_clear(NULL);
    if(maintenance_watch>=0)close(maintenance_watch);
    for(unsigned i=0;i<32;i++)if(text_cache[i].layout)g_object_unref(text_cache[i].layout);
    fprintf(stderr,"TruDE-bar: %u frames rendered\n",frames);
    if(event_fd>=0)close(event_fd);
    pa_context_disconnect(audio);pa_context_unref(audio);pa_mainloop_free(audio_loop);disconnect_pipewire();pw_context_destroy(context);pw_loop_leave(pw_main_loop_get_loop(pwloop));pw_main_loop_destroy(pwloop);pw_deinit();
    for(int i=0;i<2;i++)if(buffers[i].object){wl_buffer_destroy(buffers[i].object);munmap(buffers[i].data,buffers[i].size);}
    zwlr_layer_surface_v1_destroy(layer);wl_surface_destroy(surface);sd_bus_flush_close_unref(system_bus);sd_bus_flush_close_unref(session_bus);wl_display_disconnect(display);return 0;
}
