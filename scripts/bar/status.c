#define _GNU_SOURCE
#include <systemd/sd-bus.h>
#include <pipewire/pipewire.h>
#include <json-c/json.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* No command execution: D-Bus and PipeWire connections survive each update. */
static sd_bus *system_bus, *session_bus;
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static const char *runtime, *mode, *part;
static char previous[8192];
static int dirty = 1;
static int changed(sd_bus_message *m, void *data, sd_bus_error *e) {
    (void)m; (void)data; (void)e; dirty = 1; return 0;
}
static sd_bus_message *property(const char *service, const char *path,
                               const char *iface, const char *key) {
    sd_bus_message *m = NULL;
    if (!system_bus || sd_bus_call_method(system_bus, service, path,
            "org.freedesktop.DBus.Properties", "Get", NULL, &m, "ss", iface, key) < 0) return NULL;
    /* Get returns a variant; expose its contained basic value to callers. */
    if (sd_bus_message_enter_container(m, 'v', NULL) < 0) {
        sd_bus_message_unref(m); return NULL;
    }
    return m;
}
static void string_property(const char *service, const char *path, const char *iface,
                            const char *key, char *out, size_t size) {
    sd_bus_message *m = property(service, path, iface, key);
    const char *s = NULL;
    if (m && sd_bus_message_read(m, "s", &s) > 0) snprintf(out, size, "%s", s);
    sd_bus_message_unref(m);
}
static unsigned number(const char *service, const char *path, const char *iface,
                       const char *key, char type) {
    sd_bus_message *m = property(service, path, iface, key);
    unsigned value = 0; uint8_t byte = 0; int boolean = 0;
    if (m) {
        if (type == 'b' && sd_bus_message_read(m, "b", &boolean) > 0) value = boolean;
        else if (type == 'y' && sd_bus_message_read(m, "y", &byte) > 0) value = byte;
        else if (type == 'u') sd_bus_message_read(m, "u", &value);
    }
    sd_bus_message_unref(m); return value;
}
#ifdef DOTFILES_BAR
static void bar_status(const char *text, const char *tooltip, const char *class);
#endif
static void emit(const char *text, const char *tooltip, const char *class) {
#ifdef DOTFILES_BAR
    bar_status(text, tooltip, class);
#else
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "text", json_object_new_string(text));
    json_object_object_add(o, "tooltip", json_object_new_string(tooltip));
    if (class) json_object_object_add(o, "class", json_object_new_string(class));
    const char *s = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
    if (strcmp(previous, s)) {
        puts(s); fflush(stdout); snprintf(previous, sizeof previous, "%s", s);
    }
    json_object_put(o);
#endif
}
#define UP "org.freedesktop.UPower"
#define DEV UP ".Device"
#define NM "org.freedesktop.NetworkManager"
static void power(void) {
    char profile[64] = "", text[256], tip[512];
    string_property(UP ".PowerProfiles", "/org/freedesktop/UPower/PowerProfiles",
                    UP ".PowerProfiles", "ActiveProfile", profile, sizeof profile);
    if (!*profile) string_property("net.hadess.PowerProfiles", "/net/hadess/PowerProfiles",
                                    "net.hadess.PowerProfiles", "ActiveProfile", profile, sizeof profile);
    const char *icon = "󰾅";
    if (!strcmp(profile, "performance")) icon = "󰓅";
    else if (!strcmp(profile, "power-saver")) icon = "󰌪";
    else strcpy(profile, "balanced");
    snprintf(text, sizeof text, "%s", icon);
    snprintf(tip, sizeof tip, "Power profile: %s", profile);
    /* UPower's aggregate battery handles machines with multiple batteries. */
    const char *path = "/org/freedesktop/UPower/devices/DisplayDevice";
    if (number(UP, path, DEV, "Type", 'u') == 2 && number(UP, path, DEV, "IsPresent", 'b')) {
        double percent = 0;
        sd_bus_message *m = property(UP, path, DEV, "Percentage");
        if (m) sd_bus_message_read(m, "d", &percent);
        sd_bus_message_unref(m);
        unsigned state = number(UP, path, DEV, "State", 'u');
        static const char *states[] = {"unknown", "charging", "discharging", "empty", "fully-charged", "pending-charge", "pending-discharge"};
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        const char *mark = state == 1 ? " " : (state == 4 && !number(UP, "/org/freedesktop/UPower", UP, "OnBattery", 'b') ? " " : "");
        snprintf(text, sizeof text, "%s  %s%d%%", icon, mark, (int)percent);
        snprintf(tip, sizeof tip, "Power profile: %s\nBattery: %d%% (%s)", profile, (int)percent, states[state < 7 ? state : 0]);
    }
    emit(text, tip, profile);
}
static unsigned notifications(const char *method) {
    sd_bus_message *m = NULL; unsigned count = 0;
    if (session_bus && sd_bus_call_method(session_bus, "org.freedesktop.Notifications", "/fr/emersion/Mako",
                                         "fr.emersion.Mako", method, NULL, &m, "") >= 0) {
        if (sd_bus_message_enter_container(m, 'a', "a{sv}") > 0)
            while (sd_bus_message_skip(m, "a{sv}") > 0) count++;
    }
    sd_bus_message_unref(m); return count;
}
static int dnd(void) {
    sd_bus_message *m = NULL; const char *s; int result = 0;
    if (session_bus && sd_bus_call_method(session_bus, "org.freedesktop.Notifications", "/fr/emersion/Mako",
                                         "fr.emersion.Mako", "ListModes", NULL, &m, "") >= 0) {
        if (sd_bus_message_enter_container(m, 'a', "s") > 0)
            while (sd_bus_message_read(m, "s", &s) > 0) if (!strcmp(s, "do-not-disturb")) result = 1;
    }
    sd_bus_message_unref(m); return result;
}
static void indicators(void) {
    const char *neticon = "󰤭"; char nettip[128] = "Offline";
    sd_bus_message *m = NULL;
    int ethernet = 0, wifi = 0; unsigned strength = 0;
    if (system_bus && sd_bus_call_method(system_bus, NM, "/org/freedesktop/NetworkManager", NM,
                                        "GetDevices", NULL, &m, "") >= 0) {
        const char *path;
        if (sd_bus_message_enter_container(m, 'a', "o") > 0)
            while (sd_bus_message_read(m, "o", &path) > 0) {
                if (number(NM, path, NM ".Device", "State", 'u') != 100) continue;
                unsigned type = number(NM, path, NM ".Device", "DeviceType", 'u');
                if (type == 1) ethernet = 1;
                if (type == 2) {
                    wifi = 1; sd_bus_message *ap = property(NM, path, NM ".Device.Wireless", "ActiveAccessPoint");
                    const char *apath;
                    if (ap && sd_bus_message_read(ap, "o", &apath) > 0)
                        strength = number(NM, apath, NM ".AccessPoint", "Strength", 'y');
                    sd_bus_message_unref(ap);
                }
            }
    }
    sd_bus_message_unref(m); m = NULL;
    if (wifi) { neticon = strength >= 75 ? "󰤨" : strength >= 50 ? "󰤥" : strength >= 25 ? "󰤢" : "󰤟";
        snprintf(nettip, sizeof nettip, "Wi-Fi (%u%%)", strength);
    } else if (ethernet) { neticon = "󰈀"; strcpy(nettip, "Ethernet connected"); }
    unsigned connected = 0; int powered = 0;
    if (system_bus && sd_bus_call_method(system_bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                                        "GetManagedObjects", NULL, &m, "") >= 0 &&
        sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") > 0) {
        while (sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
            const char *path, *iface;
            sd_bus_message_read(m, "o", &path);
            sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
            while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
                sd_bus_message_read(m, "s", &iface);
                sd_bus_message_enter_container(m, 'a', "{sv}");
                while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                    const char *key;
                    sd_bus_message_read(m, "s", &key);
                    if ((!strcmp(iface, "org.bluez.Adapter1") && !strcmp(key, "Powered")) ||
                        (!strcmp(iface, "org.bluez.Device1") && !strcmp(key, "Connected"))) {
                        int value = 0;
                        if (sd_bus_message_enter_container(m, 'v', "b") > 0) {
                            sd_bus_message_read(m, "b", &value); sd_bus_message_exit_container(m);
                            if (!strcmp(key, "Powered")) powered |= value; else connected += value;
                        }
                    } else sd_bus_message_skip(m, "v");
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m); sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m); sd_bus_message_exit_container(m);
        }
    }
    sd_bus_message_unref(m);
    char bttip[128] = "Bluetooth: off";
    const char *bticon = "󰂲";
    if (powered) { bticon = connected ? "󰂱" : "󰂯";
        if (connected) snprintf(bttip, sizeof bttip, "Bluetooth: %u connected", connected);
        else strcpy(bttip, "Bluetooth: on"); }
    unsigned count = notifications("ListNotifications") + notifications("ListHistory");
    if (count > 99) count = 99;
    const char *nicon = dnd() ? (count ? "󱏨" : "󰂛") : "󰂚";
    char text[512], tip[512], ntip[64];
    if (count) { snprintf(text, sizeof text, "%s %s <span foreground=\"#ffbe6f\">%s</span> %u", neticon, bticon, nicon, count);
        snprintf(ntip, sizeof ntip, "Notifications: %u", count);
    } else { snprintf(text, sizeof text, "%s %s %s 0", neticon, bticon, nicon); strcpy(ntip, "Notifications: none"); }
    snprintf(tip, sizeof tip, "%s\n%s\n%s", nettip, bttip, ntip);
    emit(text, tip, NULL);
}

struct node {
    struct node *next; uint32_t id; struct pw_node *proxy;
    struct spa_hook hook; int screen, video;
};
static struct node *nodes;
static struct pw_main_loop *pwloop;
static struct pw_context *context;
static struct pw_core *core;
static struct pw_registry *registry;
static struct spa_hook registry_hook, core_hook;
static int pw_failed;
static void core_error(void *data, uint32_t id, int seq, int res, const char *message) {
    (void)data; (void)seq; (void)message;
    if (id == PW_ID_CORE && (res == -EPIPE || res == -ECONNRESET)) pw_failed = 1;
}
static const struct pw_core_events core_events = { PW_VERSION_CORE_EVENTS, .error = core_error };
static void node_info(void *data, const struct pw_node_info *info) {
    struct node *n = data;
    if (!info->props) return;
    const char *role = spa_dict_lookup(info->props, "media.role");
    const char *class = spa_dict_lookup(info->props, "media.class");
    n->screen = role && !strcmp(role, "Screen");
    n->video = class && (!strcmp(class, "Stream/Input/Video") || !strcmp(class, "Stream/Output/Video") || !strcmp(class, "Video/Source"));
    dirty = 1;
}
static const struct pw_node_events node_events = { PW_VERSION_NODE_EVENTS, .info = node_info };
static void global(void *data, uint32_t id, uint32_t permissions, const char *type,
                   uint32_t version, const struct spa_dict *props) {
    (void)data; (void)permissions; (void)props;
    if (strcmp(type, PW_TYPE_INTERFACE_Node)) return;
    struct node *n = calloc(1, sizeof *n); if (!n) return;
    n->id = id; n->proxy = pw_registry_bind(registry, id, type, version < PW_VERSION_NODE ? version : PW_VERSION_NODE, 0);
    if (!n->proxy) { free(n); return; }
    n->next = nodes; nodes = n;
    pw_node_add_listener(n->proxy, &n->hook, &node_events, n);
}
static void removed(void *data, uint32_t id) {
    (void)data;
    struct node **p = &nodes;
    while (*p) {
        struct node *n = *p;
        if (n->id == id) { *p = n->next; spa_hook_remove(&n->hook); pw_proxy_destroy((struct pw_proxy *)n->proxy); free(n); dirty = 1; return; }
        p = &n->next;
    }
}
static const struct pw_registry_events registry_events = { PW_VERSION_REGISTRY_EVENTS, .global = global, .global_remove = removed };
static void disconnect_pipewire(void) {
    while (nodes) removed(NULL, nodes->id);
    if (registry) { spa_hook_remove(&registry_hook); pw_proxy_destroy((struct pw_proxy *)registry); registry = NULL; }
    if (core) { spa_hook_remove(&core_hook); pw_core_disconnect(core); core = NULL; }
    pw_failed = 0;
}
static void connect_pipewire(void) {
    if (core || !context) return;
    core = pw_context_connect(context, NULL, 0);
    if (core) {
        pw_core_add_listener(core, &core_hook, &core_events, NULL);
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(registry, &registry_hook, &registry_events, NULL);
    }
}
static void launcher(void) {
    const char *class = "launcher", *icon = "󰀻", *tip = "";
    for (struct node *n = nodes; n; n = n->next) if (n->screen && n->video) class = "screensharing";
    char path[PATH_MAX], state[64] = "";
    snprintf(path, sizeof path, "%s/dotfiles-dictation/state", runtime);
    FILE *f = fopen(path, "r");
    if (f) { if (fgets(state, sizeof state, f)) state[strcspn(state, "\r\n")] = 0; fclose(f); }
    if (!strcmp(state, "recording") || !strcmp(state, "transcribing")) {
        snprintf(path, sizeof path, "%s/dotfiles-dictation/worker.lock", runtime);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { if (flock(fd, LOCK_EX | LOCK_NB) < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) class = state; close(fd); }
    }
    if (!strcmp(class, "recording")) { icon = "󰍬"; tip = "Dictation recording — Super+D to stop and paste"; }
    else if (!strcmp(class, "transcribing")) { icon = "󰔟"; tip = "Transcribing locally — text will paste into the focused field"; }
    else if (!strcmp(class, "screensharing")) icon = "󰍹";
    static struct json_object *settings;
    static struct stat saved;
    struct json_object *bar = NULL, *custom = NULL;
    if (!strcmp(class, "launcher") && !strcmp(part, "center")) {
        const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        if (config) snprintf(path, sizeof path, "%s/dotfiles/settings.json", config);
        else snprintf(path, sizeof path, "%s/.config/dotfiles/settings.json", home ? home : "");
        struct stat st = {0};
        if (stat(path, &st) < 0) { json_object_put(settings); settings = NULL; memset(&saved, 0, sizeof saved); }
        else if (!settings || st.st_ino != saved.st_ino || st.st_size != saved.st_size ||
                 st.st_mtim.tv_sec != saved.st_mtim.tv_sec || st.st_mtim.tv_nsec != saved.st_mtim.tv_nsec) {
            json_object_put(settings); settings = json_object_from_file(path); saved = st;
        }
        if (settings && json_object_object_get_ex(settings, "waybar", &bar) &&
            json_object_object_get_ex(bar, "launcher_icon", &custom) &&
            json_object_is_type(custom, json_type_string) && *json_object_get_string(custom)) icon = json_object_get_string(custom);
    }
    char text[4096];
    if (!strcmp(part, "left")) strcpy(text, "");
    else if (!strcmp(part, "right")) strcpy(text, "");
    else snprintf(text, sizeof text, "<span font_family='JetBrainsMono Nerd Font Propo' stretch='condensed'>%s</span>", icon);
    emit(text, tip, class);
}
static void connect_buses(void) {
    if (!system_bus && sd_bus_open_system(&system_bus) >= 0) {
        sd_bus_set_method_call_timeout(system_bus, 1000000);
        if (!strcmp(mode, "power")) {
            sd_bus_add_match(system_bus, NULL, "type='signal',sender='org.freedesktop.UPower',interface='org.freedesktop.DBus.Properties'", changed, NULL);
            sd_bus_add_match(system_bus, NULL, "type='signal',sender='org.freedesktop.UPower.PowerProfiles',interface='org.freedesktop.DBus.Properties'", changed, NULL);
            sd_bus_add_match(system_bus, NULL, "type='signal',sender='net.hadess.PowerProfiles',interface='org.freedesktop.DBus.Properties'", changed, NULL);
        } else {
            sd_bus_add_match(system_bus, NULL, "type='signal',sender='org.freedesktop.NetworkManager',interface='org.freedesktop.DBus.Properties'", changed, NULL);
            sd_bus_add_match(system_bus, NULL, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties'", changed, NULL);
            sd_bus_add_match(system_bus, NULL, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'", changed, NULL);
        }
        sd_bus_add_match(system_bus, NULL, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'", changed, NULL);
        dirty = 1;
    }
    if (!strcmp(mode, "indicators") && !session_bus && sd_bus_open_user(&session_bus) >= 0) {
        sd_bus_set_method_call_timeout(session_bus, 1000000);
        sd_bus_add_match(session_bus, NULL, "type='signal',sender='org.freedesktop.Notifications',interface='org.freedesktop.DBus.Properties'", changed, NULL);
        sd_bus_add_match(session_bus, NULL, "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'", changed, NULL);
        dirty = 1;
    }
}
static void drain(sd_bus **bus) {
    if (!*bus) return;
    int r; while ((r = sd_bus_process(*bus, NULL)) > 0) {}
    if (r < 0) { *bus = sd_bus_flush_close_unref(*bus); dirty = 1; }
}
#ifndef DOTFILES_BAR
int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "launcher") && strcmp(argv[1], "power") && strcmp(argv[1], "indicators"))) {
        fprintf(stderr, "usage: %s launcher [center|left|right] | power | indicators [--once]\n", argv[0]); return 2;
    }
    mode = argv[1]; part = argc > 2 && strcmp(argv[2], "--once") ? argv[2] : "center";
    int once = !strcmp(argv[argc-1], "--once");
    runtime = getenv("XDG_RUNTIME_DIR"); if (!runtime) runtime = "/tmp";
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGPIPE, stop);
    if (!strcmp(mode, "launcher")) {
        pw_init(NULL, NULL); pwloop = pw_main_loop_new(NULL);
        if (!pwloop) return 1;
        context = pw_context_new(pw_main_loop_get_loop(pwloop), NULL, 0);
        connect_pipewire();
        /* Receive the initial registry before producing the first output. */
        for (int i = 0; i < 5; i++) pw_loop_iterate(pw_main_loop_get_loop(pwloop), 20);
    } else connect_buses();
    /* Watch directories, not files: writers use atomic rename, and workers create
     * their state directory lazily. The 1s fallback covers missed watch setup. */
    int notify = -1;
    if (pwloop) {
        notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (notify >= 0) {
            inotify_add_watch(notify, runtime, IN_CREATE | IN_MOVED_TO | IN_DELETE);
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/dotfiles-dictation", runtime);
            inotify_add_watch(notify, path, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
        }
    }
    time_t last = 0, retry = 0;
    while (running) {
        time_t now = time(NULL);
        if (!strcmp(mode, "launcher")) {
            if (pw_failed) disconnect_pipewire();
            if (!core && now - retry >= 5) { connect_pipewire(); retry = now; }
            if (dirty || now != last) { launcher(); dirty = 0; last = now; }
        } else {
            if (now - retry >= 5) { connect_buses(); retry = now; }
            drain(&system_bus); drain(&session_bus);
            /* Mako exposes no notification-list change signal: query it every 2s.
             * Power has change signals; 30s refresh also recovers missed events. */
            if (dirty || now - last >= (!strcmp(mode, "power") ? 30 : 2)) {
                if (!strcmp(mode, "power")) power(); else indicators();
                dirty = 0; last = now;
            }
        }
        if (once) break;
        if (pwloop) {
            struct pollfd fds[2] = {
                { pw_loop_get_fd(pw_main_loop_get_loop(pwloop)), POLLIN, 0 },
                { notify, POLLIN, 0 }
            };
            poll(fds, 2, 1000);
            if (pw_loop_iterate(pw_main_loop_get_loop(pwloop), 0) < 0) disconnect_pipewire();
            if (notify >= 0 && fds[1].revents) {
                char events[4096]; while (read(notify, events, sizeof events) > 0) {}
                char path[PATH_MAX]; snprintf(path, sizeof path, "%s/dotfiles-dictation", runtime);
                inotify_add_watch(notify, path, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
                dirty = 1;
            }
        } else {
            struct pollfd fds[2]; nfds_t n = 0;
            if (system_bus) fds[n++] = (struct pollfd){sd_bus_get_fd(system_bus), POLLIN, 0};
            if (session_bus) fds[n++] = (struct pollfd){sd_bus_get_fd(session_bus), POLLIN, 0};
            poll(fds, n, 1000);
        }
    }
    if (notify >= 0) close(notify);
    disconnect_pipewire();
    if (context) pw_context_destroy(context);
    if (pwloop) { pw_main_loop_destroy(pwloop); pw_deinit(); }
    sd_bus_flush_close_unref(system_bus); sd_bus_flush_close_unref(session_bus);
    return 0;
}

#endif
