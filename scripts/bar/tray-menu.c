/* Render DBusMenu exports as grabbed xdg popups parented to the bar layer.
 * App-owned ContextMenu windows cannot be parented to our Wayland surface. */
static struct xdg_surface *menu_xdg;
static struct xdg_popup *menu_popup;
static struct wl_buffer *menu_buffer;
static void *menu_data;
static size_t menu_size;
static int menu_ready, menu_selected=-1, menu_parent, menu_rows[256], menu_count;
static int menu_width=320, menu_height;
static char menu_service[256], menu_path[256];
struct menu_entry { int id,parent,enabled,visible,separator,toggle,checked,submenu; char label[512]; };
static struct menu_entry menu_entries[256];
static int menu_entries_count;

static void close_tray_menu(void){
    if(menu_popup)xdg_popup_destroy(menu_popup);
    if(menu_xdg)xdg_surface_destroy(menu_xdg);
    if(menu_surface)wl_surface_destroy(menu_surface);
    if(menu_buffer)wl_buffer_destroy(menu_buffer);
    if(menu_data)munmap(menu_data,menu_size);
    menu_popup=NULL;menu_xdg=NULL;menu_surface=NULL;menu_buffer=NULL;menu_data=NULL;
    menu_ready=0;menu_selected=-1;menu_pointer=0;
}
static int menu_call(const char *method,sd_bus_message **reply,const char *types,...){
    sd_bus_message *m=NULL;int r=sd_bus_message_new_method_call(session_bus,&m,menu_service,menu_path,"com.canonical.dbusmenu",method);
    if(r>=0){va_list args;va_start(args,types);r=sd_bus_message_appendv(m,types,args);va_end(args);}
    if(r>=0)r=sd_bus_call(session_bus,m,1000000,NULL,reply);
    sd_bus_message_unref(m);return r;
}
static void menu_label(char *out,size_t size,const char *label){
    /* DBusMenu uses underscores for mnemonic markers, doubled for literals. */
    size_t n=0;for(const char *p=label;*p&&n+1<size;p++){
        if(*p=='_'){if(p[1]=='_')p++;else continue;}out[n++]=*p;
    }out[n]=0;
}
static int menu_parse(sd_bus_message *m,int parent,int depth){
    if(depth>16||menu_entries_count>=256)return -1;
    if(sd_bus_message_enter_container(m,'r',"ia{sv}av")<=0)return -1;
    struct menu_entry *e=&menu_entries[menu_entries_count++];memset(e,0,sizeof *e);
    e->parent=parent;e->enabled=e->visible=1;
    if(sd_bus_message_read(m,"i",&e->id)<0)return -1;
    if(sd_bus_message_enter_container(m,'a',"{sv}")<0)return -1;
    while(sd_bus_message_enter_container(m,'e',"sv")>0){
        const char *key,*signature;char type;
        if(sd_bus_message_read(m,"s",&key)<0||sd_bus_message_peek_type(m,&type,&signature)<=0)return -1;
        if(sd_bus_message_enter_container(m,'v',signature)<0)return -1;
        if(!strcmp(signature,"s")){
            const char *value;if(sd_bus_message_read(m,"s",&value)<0)return -1;
            if(!strcmp(key,"label"))menu_label(e->label,sizeof e->label,value);
            else if(!strcmp(key,"type"))e->separator=!strcmp(value,"separator");
            else if(!strcmp(key,"toggle-type"))e->toggle=1;
            else if(!strcmp(key,"children-display"))e->submenu=!strcmp(value,"submenu");
        }else if(!strcmp(signature,"b")){
            int value;if(sd_bus_message_read(m,"b",&value)<0)return -1;
            if(!strcmp(key,"enabled"))e->enabled=value;else if(!strcmp(key,"visible"))e->visible=value;
        }else if(!strcmp(signature,"i")){
            int value;if(sd_bus_message_read(m,"i",&value)<0)return -1;
            if(!strcmp(key,"toggle-state"))e->checked=value==1;
        }else if(sd_bus_message_skip(m,signature)<0)return -1;
        if(sd_bus_message_exit_container(m)<0||sd_bus_message_exit_container(m)<0)return -1;
    }
    if(sd_bus_message_exit_container(m)<0||sd_bus_message_enter_container(m,'a',"v")<0)return -1;
    while(sd_bus_message_enter_container(m,'v',"(ia{sv}av)")>0){
        if(menu_parse(m,e->id,depth+1)<0||sd_bus_message_exit_container(m)<0)return -1;
    }
    if(sd_bus_message_exit_container(m)<0||sd_bus_message_exit_container(m)<0)return -1;
    return 0;
}
static void menu_list(int parent){
    menu_parent=parent;menu_count=0;menu_selected=-1;
    if(parent!=0)menu_rows[menu_count++]=-1;
    for(int i=0;i<menu_entries_count;i++)if(menu_entries[i].parent==parent&&menu_entries[i].visible)
        menu_rows[menu_count++]=i;
    menu_height=16+menu_count*28;
}
static void menu_paint(void){
    if(!menu_surface||!menu_ready)return;
    if(menu_buffer){wl_buffer_destroy(menu_buffer);menu_buffer=NULL;}
    if(menu_data){munmap(menu_data,menu_size);menu_data=NULL;}
    int w=menu_width*scale,h=menu_height*scale;menu_size=(size_t)w*h*4;
    int fd=memfd_create("bar-tray-menu",MFD_CLOEXEC);
    if(fd<0)return;
    if(ftruncate(fd,menu_size)<0){close(fd);return;}
    menu_data=mmap(NULL,menu_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(menu_data==MAP_FAILED){menu_data=NULL;close(fd);return;}
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,menu_size);
    menu_buffer=wl_shm_pool_create_buffer(pool,0,w,h,w*4,WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);close(fd);
    cairo_surface_t *cs=cairo_image_surface_create_for_data(menu_data,CAIRO_FORMAT_ARGB32,w,h,w*4);
    cairo_t *c=cairo_create(cs);cairo_scale(c,scale,scale);rect(c,0,0,menu_width,menu_height,bg0);
    color(c,bg3);cairo_rectangle(c,.5,.5,menu_width-1,menu_height-1);cairo_stroke(c);
    PangoLayout *l=pango_cairo_create_layout(c);
    PangoFontDescription *font=pango_font_description_from_string("JetBrainsMono Nerd Font 10");
    pango_layout_set_font_description(l,font);pango_layout_set_width(l,(menu_width-64)*PANGO_SCALE);
    pango_layout_set_ellipsize(l,PANGO_ELLIPSIZE_END);
    for(int i=0;i<menu_count;i++){
        int index=menu_rows[i],y=8+i*28;struct menu_entry *e=index<0?NULL:&menu_entries[index];
        if(e&&e->separator){rect(c,12,y+13,menu_width-24,1,bg3);continue;}
        int active=i==menu_selected&&(!e||e->enabled);
        if(active)rect(c,4,y,menu_width-8,28,accent);
        unsigned fg=active?bg0:(!e||e->enabled?0xffffff:0xaaaaaa);
        pango_layout_set_text(l,e?e->label:"Back",-1);color(c,fg);cairo_move_to(c,30,y+5);pango_cairo_show_layout(c,l);
        if(e&&e->toggle&&e->checked){color(c,fg);cairo_arc(c,16,y+14,3,0,2*M_PI);cairo_fill(c);}
        if(e&&e->submenu){pango_layout_set_text(l,"›",-1);cairo_move_to(c,menu_width-22,y+5);pango_cairo_show_layout(c,l);}
    }
    pango_font_description_free(font);g_object_unref(l);cairo_destroy(c);cairo_surface_flush(cs);cairo_surface_destroy(cs);
    xdg_surface_set_window_geometry(menu_xdg,0,0,menu_width,menu_height);
    wl_surface_set_buffer_scale(menu_surface,scale);wl_surface_attach(menu_surface,menu_buffer,0,0);
    wl_surface_damage_buffer(menu_surface,0,0,w,h);wl_surface_commit(menu_surface);
}
static void menu_motion(double y){
    int selected=y<8?-1:(int)(y-8)/28;if(selected>=menu_count)selected=-1;
    if(selected!=menu_selected){menu_selected=selected;menu_paint();}
}
static void menu_activate(uint32_t time){
    if(menu_selected<0||menu_selected>=menu_count)return;
    int index=menu_rows[menu_selected];
    if(index<0){int parent=0;for(int i=0;i<menu_entries_count;i++)if(menu_entries[i].id==menu_parent){parent=menu_entries[i].parent;break;}menu_list(parent);menu_paint();return;}
    struct menu_entry *e=&menu_entries[index];if(!e->enabled||e->separator)return;
    if(e->submenu){menu_list(e->id);menu_paint();return;}
    sd_bus_message *m=NULL;
    if(sd_bus_message_new_method_call(session_bus,&m,menu_service,menu_path,"com.canonical.dbusmenu","Event")>=0){
        sd_bus_message_append(m,"isvu",e->id,"clicked","i",0,time);
        sd_bus_message_set_expect_reply(m,0);sd_bus_send(session_bus,m,NULL);
    }
    sd_bus_message_unref(m);close_tray_menu();
}
static void menu_configure(void *d,struct xdg_surface *s,uint32_t serial){(void)d;xdg_surface_ack_configure(s,serial);menu_ready=1;menu_paint();}
static const struct xdg_surface_listener menu_surface_listener={menu_configure};
static void menu_popup_configure(void *d,struct xdg_popup *p,int32_t x,int32_t y,int32_t w,int32_t h){(void)d;(void)p;(void)x;(void)y;(void)w;(void)h;}
static void menu_done(void *d,struct xdg_popup *p){(void)d;(void)p;close_tray_menu();}
static const struct xdg_popup_listener menu_popup_listener={.configure=menu_popup_configure,.popup_done=menu_done};
static void open_tray_menu(struct tray_item *t,uint32_t serial){
    close_tray_menu();hide_tooltip();hover_deadline=0;
    if(!wm_base||!seat)return;
    sd_bus_message *m=NULL;const char *path=NULL;
    if(sd_bus_get_property(session_bus,t->service,t->path,"org.kde.StatusNotifierItem","Menu",NULL,&m,"o")<0)return;
    if(sd_bus_message_read(m,"o",&path)<0||!path||!strcmp(path,"/")){sd_bus_message_unref(m);return;}
    snprintf(menu_service,sizeof menu_service,"%s",t->service);snprintf(menu_path,sizeof menu_path,"%s",path);sd_bus_message_unref(m);m=NULL;
    menu_call("AboutToShow",&m,"i",0);sd_bus_message_unref(m);m=NULL;
    menu_entries_count=0;
    if(menu_call("GetLayout",&m,"iias",0,-1,0)<0){sd_bus_message_unref(m);return;}
    uint32_t revision;int result=sd_bus_message_read(m,"u",&revision);
    if(result>=0)result=menu_parse(m,-1,0);
    sd_bus_message_unref(m);if(result<0)return;
    menu_list(0);if(!menu_count)return;
    menu_surface=wl_compositor_create_surface(compositor);
    menu_xdg=xdg_wm_base_get_xdg_surface(wm_base,menu_surface);
    xdg_surface_add_listener(menu_xdg,&menu_surface_listener,NULL);
    struct xdg_positioner *pos=xdg_wm_base_create_positioner(wm_base);
    xdg_positioner_set_size(pos,menu_width,menu_height);
    xdg_positioner_set_anchor_rect(pos,17+tray_hover*24,0,24,height);
    xdg_positioner_set_anchor(pos,bottom?XDG_POSITIONER_ANCHOR_TOP_LEFT:XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(pos,bottom?XDG_POSITIONER_GRAVITY_TOP_RIGHT:XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos,XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X|XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    menu_popup=xdg_surface_get_popup(menu_xdg,NULL,pos);xdg_positioner_destroy(pos);
    zwlr_layer_surface_v1_get_popup(layer,menu_popup);
    xdg_popup_add_listener(menu_popup,&menu_popup_listener,NULL);
    xdg_popup_grab(menu_popup,seat,serial);wl_surface_commit(menu_surface);
}
