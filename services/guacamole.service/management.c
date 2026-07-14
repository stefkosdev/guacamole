#include "management.h"
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <ctype.h>

static GuacConnection g_connections[MAX_CONNECTIONS];
static int g_connection_count = 0;
static pthread_mutex_t g_connections_lock = PTHREAD_MUTEX_INITIALIZER;

static GuacSession g_sessions[MAX_SESSIONS];
static int g_session_count = 0;
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;

static const char* g_supported_protocols[] = {
    "vnc", "rdp", "ssh", "telnet", "kubernetes", NULL
};

static void generate_id(char* out, size_t cap)
{
    if (!out || cap < 16) return;
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned char b[16];
    ssize_t r = fd >= 0 ? read(fd, b, sizeof(b)) : -1;
    if (fd >= 0) close(fd);
    if (r != (ssize_t)sizeof(b))
    {
        unsigned int seed = (unsigned int)(time(NULL) ^ getpid());
        for (size_t i = 0; i < sizeof(b); i++)
        {
            seed = seed * 1103515245U + 12345U;
            b[i] = (unsigned char)(seed >> 16);
        }
    }
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(b); i++)
    {
        out[i * 2] = hx[(b[i] >> 4) & 15];
        out[i * 2 + 1] = hx[b[i] & 15];
    }
    out[32] = '\0';
}

static int protocol_supported(const char* protocol)
{
    if (!protocol) return 0;
    for (int i = 0; g_supported_protocols[i]; i++)
    {
        if (strcasecmp(protocol, g_supported_protocols[i]) == 0)
            return 1;
    }
    return 0;
}

static void json_escape(const char* in, char* out, size_t cap)
{
    if (!in || !out || cap == 0) return;
    size_t w = 0;
    for (size_t i = 0; in[i]; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\')
        {
            if (w + 2 >= cap) break;
            out[w++] = '\\';
            out[w++] = (char)c;
        }
        else if (c < ' ')
        {
            if (w + 6 >= cap) break;
            snprintf(out + w, cap - w, "\\u%04x", c);
            w = strlen(out);
        }
        else
        {
            if (w + 1 >= cap) break;
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

static int json_field_str(const char* json, const char* field, char* out, size_t cap)
{
    if (!json || !field || !out || cap < 2) return -1;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    const char* p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    size_t w = 0;
    while (*p && *p != '"' && w + 1 < cap)
    {
        if (*p == '\\' && p[1])
        {
            if (p[1] == '"') { out[w++] = '"'; p += 2; continue; }
            if (p[1] == 'n') { out[w++] = '\n'; p += 2; continue; }
            if (p[1] == 'r') { out[w++] = '\r'; p += 2; continue; }
            if (p[1] == 't') { out[w++] = '\t'; p += 2; continue; }
            if (p[1] == '\\') { out[w++] = '\\'; p += 2; continue; }
        }
        out[w++] = *p++;
    }
    out[w] = '\0';
    return w > 0 ? 0 : -1;
}

static int json_field_int(const char* json, const char* field)
{
    if (!json || !field) return 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char* p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return atoi(p);
}

static int json_field_bool(const char* json, const char* field)
{
    if (!json || !field) return 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char* p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return (strncmp(p, "true", 4) == 0);
}

static long json_field_long(const char* json, const char* field)
{
    if (!json || !field) return 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char* p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return strtol(p, NULL, 10);
}

static char* json_error(const char* msg)
{
    char buf[512];
    char esc[256];
    json_escape(msg ? msg : "Unknown error", esc, sizeof(esc));
    snprintf(buf, sizeof(buf), "{\"response\":\"fail\",\"message\":\"%s\"}", esc);
    return strdup(buf);
}

/* ------------------------------------------------------------------------ *
 *  Persistent storage — Kin ".info" convention
 *
 *  Connections are the durable "settings" of the gateway: the service needs
 *  them to route protocol clients, so they must survive restarts. Following
 *  the Kin convention (e.g. Wallpaper.info, Calendar.info) they are stored as
 *  a JSON document in a "*.info" file. Sessions are live runtime state and are
 *  intentionally NOT persisted — they are meaningless once the service (and the
 *  remote desktop connections it holds) is gone.
 *
 *  Store location, first that resolves:
 *    $KIN_GUACAMOLE_STATE                              (explicit full path override)
 *    $XDG_DATA_HOME/kin/guacamole/Guacamole.info
 *    $HOME/.local/share/kin/guacamole/Guacamole.info
 *    /tmp/kin-guacamole-<uid>/Guacamole.info           (last resort)
 * ------------------------------------------------------------------------ */

static char g_store_path[1024] = {0};

static void resolve_store_path(void)
{
    const char* over = getenv("KIN_GUACAMOLE_STATE");
    if (over && over[0])
    {
        snprintf(g_store_path, sizeof(g_store_path), "%s", over);
        return;
    }
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0])
    {
        snprintf(g_store_path, sizeof(g_store_path), "%s/kin/guacamole/Guacamole.info", xdg);
        return;
    }
    const char* home = getenv("HOME");
    if (home && home[0])
    {
        snprintf(g_store_path, sizeof(g_store_path), "%s/.local/share/kin/guacamole/Guacamole.info", home);
        return;
    }
    snprintf(g_store_path, sizeof(g_store_path), "/tmp/kin-guacamole-%d/Guacamole.info", (int)getuid());
}

/* Create every parent directory of `path` (the "mkdir -p" of its dirname). */
static void mkdir_parents(const char* path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char* last = strrchr(tmp, '/');
    if (!last) return;
    *last = '\0';                       /* drop the filename, keep the directory */
    for (char* p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            (void)mkdir(tmp, 0700);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0700);
}

/* Serialize a single connection as a JSON object (no response wrapper). */
static size_t serialize_connection(const GuacConnection* c, char* buf, size_t cap)
{
    char name_esc[512], host_esc[512], user_esc[512], pass_esc[512];
    char pk_esc[2100], dom_esc[512], sec_esc[128], cd_esc[32];
    char rapp_esc[512], rdir_esc[1024], rargs_esc[1024];
    json_escape(c->name, name_esc, sizeof(name_esc));
    json_escape(c->hostname, host_esc, sizeof(host_esc));
    json_escape(c->username, user_esc, sizeof(user_esc));
    json_escape(c->password, pass_esc, sizeof(pass_esc));
    json_escape(c->private_key, pk_esc, sizeof(pk_esc));
    json_escape(c->domain, dom_esc, sizeof(dom_esc));
    json_escape(c->security, sec_esc, sizeof(sec_esc));
    json_escape(c->color_depth, cd_esc, sizeof(cd_esc));
    json_escape(c->remote_app, rapp_esc, sizeof(rapp_esc));
    json_escape(c->remote_app_dir, rdir_esc, sizeof(rdir_esc));
    json_escape(c->remote_app_args, rargs_esc, sizeof(rargs_esc));
    int n = snprintf(buf, cap,
        "{\"id\":\"%s\",\"name\":\"%s\",\"protocol\":\"%s\",\"hostname\":\"%s\","
        "\"port\":%d,\"username\":\"%s\",\"password\":\"%s\",\"private_key\":\"%s\","
        "\"domain\":\"%s\",\"security\":\"%s\",\"color_depth\":\"%s\","
        "\"remote_app\":\"%s\",\"remote_app_dir\":\"%s\",\"remote_app_args\":\"%s\","
        "\"enable_audio\":%s,\"enable_video\":%s,\"enable_printing\":%s,"
        "\"enable_file_transfer\":%s,\"enable_wallpaper\":%s,\"enable_theming\":%s,"
        "\"enable_font_smoothing\":%s,\"enable_full_window_drag\":%s,"
        "\"enable_menu_animation\":%s,\"disable_copy\":%s,\"disable_paste\":%s,"
        "\"ignore_cert\":%s,"
        "\"width\":%d,\"height\":%d,\"dpi\":%d,\"created\":%ld,\"last_used\":%ld}",
        c->id, name_esc, c->protocol, host_esc,
        c->port, user_esc, pass_esc, pk_esc,
        dom_esc, sec_esc, cd_esc,
        rapp_esc, rdir_esc, rargs_esc,
        c->enable_audio ? "true" : "false",
        c->enable_video ? "true" : "false",
        c->enable_printing ? "true" : "false",
        c->enable_file_transfer ? "true" : "false",
        c->enable_wallpaper ? "true" : "false",
        c->enable_theming ? "true" : "false",
        c->enable_font_smoothing ? "true" : "false",
        c->enable_full_window_drag ? "true" : "false",
        c->enable_menu_animation ? "true" : "false",
        c->disable_copy ? "true" : "false",
        c->disable_paste ? "true" : "false",
        c->ignore_cert ? "true" : "false",
        c->width, c->height, c->dpi, c->created, c->last_used);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

/* Write all connections to the .info store atomically (temp file + rename).
 * Acquires g_connections_lock itself — callers must NOT already hold it. */
static void persist_connections(void)
{
    if (!g_store_path[0]) return;

    size_t cap = 2u * 1024u * 1024u;
    char* buf = malloc(cap);
    if (!buf) return;

    size_t pos = 0;
    pos += snprintf(buf + pos, cap - pos,
                    "{\n  \"version\": 1,\n  \"connections\": [");

    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (cap - pos < 8192) break;    /* leave room for one object + closer */
        pos += snprintf(buf + pos, cap - pos, "%s\n    ", i ? "," : "");
        pos += serialize_connection(&g_connections[i], buf + pos, cap - pos);
    }
    pthread_mutex_unlock(&g_connections_lock);

    pos += snprintf(buf + pos, cap - pos, "\n  ]\n}\n");

    mkdir_parents(g_store_path);
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_store_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
    {
        ssize_t w = write(fd, buf, pos);
        close(fd);
        if (w == (ssize_t)pos)
            (void)rename(tmp, g_store_path);
        else
            (void)unlink(tmp);
    }
    free(buf);
}

/* Read an entire file into a malloc'd, NUL-terminated buffer. Caller frees. */
static char* read_file_alloc(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || sz > 8 * 1024 * 1024 || lseek(fd, 0, SEEK_SET) != 0)
    {
        close(fd);
        return NULL;
    }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t r = read(fd, buf, (size_t)sz);
    close(fd);
    if (r < 0) { free(buf); return NULL; }
    buf[r] = '\0';
    return buf;
}

/* Copy the next balanced {...} object at or after *pp into `out`. Advances *pp
 * past the closing brace. Returns 1 on success, 0 when the array ends. Respects
 * quoted strings and escapes; connection objects are flat so this is exact. */
static int next_json_object(const char** pp, char* out, size_t cap)
{
    const char* p = *pp;
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') { *pp = p; return 0; }

    const char* start = p;
    int depth = 0, in_str = 0, esc = 0;
    for (; *p; p++)
    {
        char ch = *p;
        if (in_str)
        {
            if (esc)            esc = 0;
            else if (ch == '\\') esc = 1;
            else if (ch == '"')  in_str = 0;
            continue;
        }
        if (ch == '"')       in_str = 1;
        else if (ch == '{')  depth++;
        else if (ch == '}' && --depth == 0) { p++; break; }
    }

    size_t len = (size_t)(p - start);
    *pp = p;
    if (len == 0 || len >= cap) return 0;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

/* Load connections from the .info store into the in-memory table.
 * Acquires g_connections_lock itself — callers must NOT already hold it. */
static void load_connections(void)
{
    char* file = read_file_alloc(g_store_path);
    if (!file) return;

    const char* arr = strstr(file, "\"connections\"");
    const char* p = arr ? arr + strlen("\"connections\"") : file;
    char obj[8192];

    pthread_mutex_lock(&g_connections_lock);
    while (g_connection_count < MAX_CONNECTIONS && next_json_object(&p, obj, sizeof(obj)))
    {
        GuacConnection* c = &g_connections[g_connection_count];
        memset(c, 0, sizeof(*c));
        json_field_str(obj, "id", c->id, sizeof(c->id));
        if (!c->id[0]) generate_id(c->id, sizeof(c->id));
        json_field_str(obj, "name", c->name, sizeof(c->name));
        json_field_str(obj, "protocol", c->protocol, sizeof(c->protocol));
        json_field_str(obj, "hostname", c->hostname, sizeof(c->hostname));
        c->port = json_field_int(obj, "port");
        json_field_str(obj, "username", c->username, sizeof(c->username));
        json_field_str(obj, "password", c->password, sizeof(c->password));
        json_field_str(obj, "private_key", c->private_key, sizeof(c->private_key));
        json_field_str(obj, "domain", c->domain, sizeof(c->domain));
        json_field_str(obj, "security", c->security, sizeof(c->security));
        json_field_str(obj, "color_depth", c->color_depth, sizeof(c->color_depth));
        if (!c->color_depth[0]) snprintf(c->color_depth, sizeof(c->color_depth), "32");
        json_field_str(obj, "remote_app", c->remote_app, sizeof(c->remote_app));
        json_field_str(obj, "remote_app_dir", c->remote_app_dir, sizeof(c->remote_app_dir));
        json_field_str(obj, "remote_app_args", c->remote_app_args, sizeof(c->remote_app_args));
        c->enable_audio = json_field_bool(obj, "enable_audio");
        c->enable_video = json_field_bool(obj, "enable_video");
        c->enable_printing = json_field_bool(obj, "enable_printing");
        c->enable_file_transfer = json_field_bool(obj, "enable_file_transfer");
        c->enable_wallpaper = json_field_bool(obj, "enable_wallpaper");
        c->enable_theming = json_field_bool(obj, "enable_theming");
        c->enable_font_smoothing = json_field_bool(obj, "enable_font_smoothing");
        c->enable_full_window_drag = json_field_bool(obj, "enable_full_window_drag");
        c->enable_menu_animation = json_field_bool(obj, "enable_menu_animation");
        c->disable_copy = json_field_bool(obj, "disable_copy");
        c->disable_paste = json_field_bool(obj, "disable_paste");
        c->ignore_cert = json_field_bool(obj, "ignore_cert");
        c->width = json_field_int(obj, "width");
        c->height = json_field_int(obj, "height");
        c->dpi = json_field_int(obj, "dpi");
        if (c->width <= 0) c->width = 1024;
        if (c->height <= 0) c->height = 768;
        if (c->dpi <= 0) c->dpi = 96;
        c->created = json_field_long(obj, "created");
        c->last_used = json_field_long(obj, "last_used");
        c->active = 0;                  /* no live sessions after a restart */
        if (c->protocol[0] && c->hostname[0])
            g_connection_count++;       /* else: leave slot for the next object */
    }
    pthread_mutex_unlock(&g_connections_lock);
    free(file);
}

static char* handle_list_connections(void)
{
    char* buf = malloc(65536);
    if (!buf) return NULL;
    size_t pos = 0;
    pos += snprintf(buf + pos, 65536 - pos, "{\"response\":\"success\",\"connections\":[");
    pthread_mutex_lock(&g_connections_lock);
    int first = 1;
    for (int i = 0; i < g_connection_count; i++)
    {
        if (!first) pos += snprintf(buf + pos, 65536 - pos, ",");
        first = 0;
        char name_esc[512], host_esc[512], user_esc[512], dom_esc[512], sec_esc[512], cd_esc[64];
        char rapp_esc[512], rdir_esc[1024], rargs_esc[1024];
        json_escape(g_connections[i].name, name_esc, sizeof(name_esc));
        json_escape(g_connections[i].hostname, host_esc, sizeof(host_esc));
        json_escape(g_connections[i].username, user_esc, sizeof(user_esc));
        json_escape(g_connections[i].domain, dom_esc, sizeof(dom_esc));
        json_escape(g_connections[i].security, sec_esc, sizeof(sec_esc));
        json_escape(g_connections[i].color_depth, cd_esc, sizeof(cd_esc));
        json_escape(g_connections[i].remote_app, rapp_esc, sizeof(rapp_esc));
        json_escape(g_connections[i].remote_app_dir, rdir_esc, sizeof(rdir_esc));
        json_escape(g_connections[i].remote_app_args, rargs_esc, sizeof(rargs_esc));
        pos += snprintf(buf + pos, 65536 - pos,
            "{\"id\":\"%s\",\"name\":\"%s\",\"protocol\":\"%s\",\"hostname\":\"%s\","
            "\"port\":%d,\"username\":\"%s\",\"domain\":\"%s\",\"security\":\"%s\","
            "\"color_depth\":\"%s\","
            "\"remote_app\":\"%s\",\"remote_app_dir\":\"%s\",\"remote_app_args\":\"%s\","
            "\"enable_audio\":%s,\"enable_video\":%s,"
            "\"enable_printing\":%s,\"enable_file_transfer\":%s,"
            "\"enable_wallpaper\":%s,\"enable_theming\":%s,"
            "\"enable_font_smoothing\":%s,\"enable_full_window_drag\":%s,"
            "\"enable_menu_animation\":%s,\"disable_copy\":%s,\"disable_paste\":%s,"
            "\"ignore_cert\":%s,"
            "\"width\":%d,\"height\":%d,\"dpi\":%d,\"active\":%s,"
            "\"created\":%ld,\"last_used\":%ld}",
            g_connections[i].id, name_esc, g_connections[i].protocol, host_esc,
            g_connections[i].port, user_esc, dom_esc, sec_esc,
            cd_esc,
            rapp_esc, rdir_esc, rargs_esc,
            g_connections[i].enable_audio ? "true" : "false",
            g_connections[i].enable_video ? "true" : "false",
            g_connections[i].enable_printing ? "true" : "false",
            g_connections[i].enable_file_transfer ? "true" : "false",
            g_connections[i].enable_wallpaper ? "true" : "false",
            g_connections[i].enable_theming ? "true" : "false",
            g_connections[i].enable_font_smoothing ? "true" : "false",
            g_connections[i].enable_full_window_drag ? "true" : "false",
            g_connections[i].enable_menu_animation ? "true" : "false",
            g_connections[i].disable_copy ? "true" : "false",
            g_connections[i].disable_paste ? "true" : "false",
            g_connections[i].ignore_cert ? "true" : "false",
            g_connections[i].width, g_connections[i].height, g_connections[i].dpi,
            g_connections[i].active ? "true" : "false",
            g_connections[i].created, g_connections[i].last_used);
    }
    pthread_mutex_unlock(&g_connections_lock);
    pos += snprintf(buf + pos, 65536 - pos, "]}");
    return buf;
}

static char* handle_get_connection(const char* id)
{
    if (!id || !id[0]) return json_error("Missing connection id");
    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, id) == 0)
        {
            char buf[8192];
            char name_esc[512], host_esc[512], user_esc[512], pass_esc[512];
            char pk_esc[1024], dom_esc[512], sec_esc[512], cd_esc[64];
            char rapp_esc[512], rdir_esc[1024], rargs_esc[1024];
            json_escape(g_connections[i].name, name_esc, sizeof(name_esc));
            json_escape(g_connections[i].hostname, host_esc, sizeof(host_esc));
            json_escape(g_connections[i].username, user_esc, sizeof(user_esc));
            json_escape(g_connections[i].password, pass_esc, sizeof(pass_esc));
            json_escape(g_connections[i].private_key, pk_esc, sizeof(pk_esc));
            json_escape(g_connections[i].domain, dom_esc, sizeof(dom_esc));
            json_escape(g_connections[i].security, sec_esc, sizeof(sec_esc));
            json_escape(g_connections[i].color_depth, cd_esc, sizeof(cd_esc));
            json_escape(g_connections[i].remote_app, rapp_esc, sizeof(rapp_esc));
            json_escape(g_connections[i].remote_app_dir, rdir_esc, sizeof(rdir_esc));
            json_escape(g_connections[i].remote_app_args, rargs_esc, sizeof(rargs_esc));
            snprintf(buf, sizeof(buf),
                "{\"response\":\"success\",\"connection\":{\"id\":\"%s\",\"name\":\"%s\","
                "\"protocol\":\"%s\",\"hostname\":\"%s\",\"port\":%d,\"username\":\"%s\","
                "\"password\":\"%s\",\"private_key\":\"%s\",\"domain\":\"%s\","
                "\"security\":\"%s\",\"color_depth\":\"%s\","
                "\"remote_app\":\"%s\",\"remote_app_dir\":\"%s\",\"remote_app_args\":\"%s\","
                "\"enable_audio\":%s,\"enable_video\":%s,\"enable_printing\":%s,"
                "\"enable_file_transfer\":%s,\"enable_wallpaper\":%s,\"enable_theming\":%s,"
                "\"enable_font_smoothing\":%s,\"enable_full_window_drag\":%s,"
                "\"enable_menu_animation\":%s,\"disable_copy\":%s,\"disable_paste\":%s,"
                "\"ignore_cert\":%s,"
                "\"width\":%d,\"height\":%d,\"dpi\":%d,\"active\":%s,"
                "\"created\":%ld,\"last_used\":%ld}}",
                g_connections[i].id, name_esc,
                g_connections[i].protocol, host_esc,
                g_connections[i].port, user_esc,
                pass_esc, pk_esc, dom_esc,
                sec_esc, cd_esc,
                rapp_esc, rdir_esc, rargs_esc,
                g_connections[i].enable_audio ? "true" : "false",
                g_connections[i].enable_video ? "true" : "false",
                g_connections[i].enable_printing ? "true" : "false",
                g_connections[i].enable_file_transfer ? "true" : "false",
                g_connections[i].enable_wallpaper ? "true" : "false",
                g_connections[i].enable_theming ? "true" : "false",
                g_connections[i].enable_font_smoothing ? "true" : "false",
                g_connections[i].enable_full_window_drag ? "true" : "false",
                g_connections[i].enable_menu_animation ? "true" : "false",
                g_connections[i].disable_copy ? "true" : "false",
                g_connections[i].disable_paste ? "true" : "false",
                g_connections[i].ignore_cert ? "true" : "false",
                g_connections[i].width, g_connections[i].height, g_connections[i].dpi,
                g_connections[i].active ? "true" : "false",
                g_connections[i].created, g_connections[i].last_used);
            pthread_mutex_unlock(&g_connections_lock);
            return strdup(buf);
        }
    }
    pthread_mutex_unlock(&g_connections_lock);
    return json_error("Connection not found");
}

static char* handle_add_connection(const char* message)
{
    if (!message || !message[0]) return json_error("Missing connection data");
    char name[256] = "", protocol[32] = "", hostname[256] = "";
    char username[128] = "", password[256] = "", private_key[1024] = "";
    char domain[128] = "", security[32] = "", color_depth[8] = "32";
    char remote_app[256] = "", remote_app_dir[512] = "", remote_app_args[512] = "";
    int port = 0;

    json_field_str(message, "name", name, sizeof(name));
    json_field_str(message, "protocol", protocol, sizeof(protocol));
    json_field_str(message, "hostname", hostname, sizeof(hostname));
    json_field_str(message, "username", username, sizeof(username));
    json_field_str(message, "password", password, sizeof(password));
    json_field_str(message, "private_key", private_key, sizeof(private_key));
    json_field_str(message, "domain", domain, sizeof(domain));
    json_field_str(message, "security", security, sizeof(security));
    json_field_str(message, "color_depth", color_depth, sizeof(color_depth));
    json_field_str(message, "remote_app", remote_app, sizeof(remote_app));
    json_field_str(message, "remote_app_dir", remote_app_dir, sizeof(remote_app_dir));
    json_field_str(message, "remote_app_args", remote_app_args, sizeof(remote_app_args));
    port = json_field_int(message, "port");

    if (!name[0] || !protocol[0] || !hostname[0])
        return json_error("name, protocol, and hostname are required");
    if (!protocol_supported(protocol))
        return json_error("Unsupported protocol (supported: vnc, rdp, ssh, telnet, kubernetes)");

    if (port <= 0)
    {
        if (strcasecmp(protocol, "vnc") == 0) port = 5900;
        else if (strcasecmp(protocol, "rdp") == 0) port = 3389;
        else if (strcasecmp(protocol, "ssh") == 0) port = 22;
        else if (strcasecmp(protocol, "telnet") == 0) port = 23;
        else if (strcasecmp(protocol, "kubernetes") == 0) port = 0;
    }

    pthread_mutex_lock(&g_connections_lock);
    if (g_connection_count >= MAX_CONNECTIONS)
    {
        pthread_mutex_unlock(&g_connections_lock);
        return json_error("Maximum connection limit reached");
    }

    GuacConnection* c = &g_connections[g_connection_count];
    memset(c, 0, sizeof(GuacConnection));
    generate_id(c->id, sizeof(c->id));
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->protocol, sizeof(c->protocol), "%s", protocol);
    snprintf(c->hostname, sizeof(c->hostname), "%s", hostname);
    c->port = port;
    snprintf(c->username, sizeof(c->username), "%s", username);
    snprintf(c->password, sizeof(c->password), "%s", password);
    snprintf(c->private_key, sizeof(c->private_key), "%s", private_key);
    snprintf(c->domain, sizeof(c->domain), "%s", domain);
    snprintf(c->security, sizeof(c->security), "%s", security);
    snprintf(c->color_depth, sizeof(c->color_depth), "%s", color_depth);
    snprintf(c->remote_app, sizeof(c->remote_app), "%s", remote_app);
    snprintf(c->remote_app_dir, sizeof(c->remote_app_dir), "%s", remote_app_dir);
    snprintf(c->remote_app_args, sizeof(c->remote_app_args), "%s", remote_app_args);
    c->enable_audio = json_field_bool(message, "enable_audio");
    c->enable_video = json_field_bool(message, "enable_video");
    c->enable_printing = json_field_bool(message, "enable_printing");
    c->enable_file_transfer = json_field_bool(message, "enable_file_transfer");
    c->enable_wallpaper = json_field_bool(message, "enable_wallpaper");
    c->enable_theming = json_field_bool(message, "enable_theming");
    c->enable_font_smoothing = json_field_bool(message, "enable_font_smoothing");
    c->enable_full_window_drag = json_field_bool(message, "enable_full_window_drag");
    c->enable_menu_animation = json_field_bool(message, "enable_menu_animation");
    c->disable_copy = json_field_bool(message, "disable_copy");
    c->disable_paste = json_field_bool(message, "disable_paste");
    c->ignore_cert = json_field_bool(message, "ignore_cert");
    c->width = json_field_int(message, "width");
    c->height = json_field_int(message, "height");
    c->dpi = json_field_int(message, "dpi");
    if (c->width <= 0) c->width = 1024;
    if (c->height <= 0) c->height = 768;
    if (c->dpi <= 0) c->dpi = 96;
    c->active = 0;
    c->created = (long)time(NULL);
    c->last_used = 0;
    g_connection_count++;
    char new_id[64];
    snprintf(new_id, sizeof(new_id), "%s", c->id);
    pthread_mutex_unlock(&g_connections_lock);

    persist_connections();

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"response\":\"success\",\"message\":\"Connection added\",\"connection_id\":\"%s\"}", new_id);
    return strdup(buf);
}

static char* handle_update_connection(const char* message)
{
    if (!message || !message[0]) return json_error("Missing update data");
    char id[64] = "";
    json_field_str(message, "id", id, sizeof(id));
    if (!id[0]) return json_error("Missing connection id");

    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, id) == 0)
        {
            char val[256];
            if (json_field_str(message, "name", val, sizeof(val)) == 0)
                snprintf(g_connections[i].name, sizeof(g_connections[i].name), "%s", val);
            if (json_field_str(message, "protocol", val, sizeof(val)) == 0)
                snprintf(g_connections[i].protocol, sizeof(g_connections[i].protocol), "%s", val);
            if (json_field_str(message, "hostname", val, sizeof(val)) == 0)
                snprintf(g_connections[i].hostname, sizeof(g_connections[i].hostname), "%s", val);
            int p = json_field_int(message, "port");
            if (p > 0) g_connections[i].port = p;
            if (json_field_str(message, "username", val, sizeof(val)) == 0)
                snprintf(g_connections[i].username, sizeof(g_connections[i].username), "%s", val);
            if (json_field_str(message, "password", val, sizeof(val)) == 0)
                snprintf(g_connections[i].password, sizeof(g_connections[i].password), "%s", val);
            if (json_field_str(message, "private_key", val, sizeof(val)) == 0)
                snprintf(g_connections[i].private_key, sizeof(g_connections[i].private_key), "%s", val);
            if (json_field_str(message, "domain", val, sizeof(val)) == 0)
                snprintf(g_connections[i].domain, sizeof(g_connections[i].domain), "%s", val);
            if (json_field_str(message, "security", val, sizeof(val)) == 0)
                snprintf(g_connections[i].security, sizeof(g_connections[i].security), "%s", val);
            if (json_field_str(message, "color_depth", val, sizeof(val)) == 0)
                snprintf(g_connections[i].color_depth, sizeof(g_connections[i].color_depth), "%s", val);
            {
                char rbuf[512];
                if (json_field_str(message, "remote_app", rbuf, sizeof(rbuf)) == 0)
                    snprintf(g_connections[i].remote_app, sizeof(g_connections[i].remote_app), "%s", rbuf);
                if (json_field_str(message, "remote_app_dir", rbuf, sizeof(rbuf)) == 0)
                    snprintf(g_connections[i].remote_app_dir, sizeof(g_connections[i].remote_app_dir), "%s", rbuf);
                if (json_field_str(message, "remote_app_args", rbuf, sizeof(rbuf)) == 0)
                    snprintf(g_connections[i].remote_app_args, sizeof(g_connections[i].remote_app_args), "%s", rbuf);
            }

            pthread_mutex_unlock(&g_connections_lock);
            persist_connections();
            return strdup("{\"response\":\"success\",\"message\":\"Connection updated\"}");
        }
    }
    pthread_mutex_unlock(&g_connections_lock);
    return json_error("Connection not found");
}

static char* handle_delete_connection(const char* message)
{
    if (!message || !message[0]) return json_error("Missing connection id");
    char id[64] = "";
    json_field_str(message, "id", id, sizeof(id));
    if (!id[0]) return json_error("Missing connection id");

    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, id) == 0)
        {
            g_connections[i] = g_connections[g_connection_count - 1];
            g_connection_count--;
            pthread_mutex_unlock(&g_connections_lock);
            persist_connections();
            return strdup("{\"response\":\"success\",\"message\":\"Connection deleted\"}");
        }
    }
    pthread_mutex_unlock(&g_connections_lock);
    return json_error("Connection not found");
}

static char* handle_list_sessions(void)
{
    char* buf = malloc(65536);
    if (!buf) return NULL;
    size_t pos = 0;
    pos += snprintf(buf + pos, 65536 - pos, "{\"response\":\"success\",\"sessions\":[");
    pthread_mutex_lock(&g_sessions_lock);
    int first = 1;
    for (int i = 0; i < g_session_count; i++)
    {
        if (!first) pos += snprintf(buf + pos, 65536 - pos, ",");
        first = 0;
        char user_esc[512], sess_esc[512], proto_esc[64], host_esc[512];
        json_escape(g_sessions[i].username, user_esc, sizeof(user_esc));
        json_escape(g_sessions[i].session_id, sess_esc, sizeof(sess_esc));
        json_escape(g_sessions[i].protocol, proto_esc, sizeof(proto_esc));
        json_escape(g_sessions[i].hostname, host_esc, sizeof(host_esc));
        pos += snprintf(buf + pos, 65536 - pos,
            "{\"id\":\"%s\",\"connection_id\":\"%s\",\"username\":\"%s\","
            "\"session_id\":\"%s\",\"protocol\":\"%s\",\"hostname\":\"%s\","
            "\"port\":%d,\"started\":%ld,\"last_active\":%ld,\"active\":%s}",
            g_sessions[i].id, g_sessions[i].connection_id,
            user_esc, sess_esc, proto_esc, host_esc,
            g_sessions[i].port, g_sessions[i].started,
            g_sessions[i].last_active,
            g_sessions[i].active ? "true" : "false");
    }
    pthread_mutex_unlock(&g_sessions_lock);
    pos += snprintf(buf + pos, 65536 - pos, "]}");
    return buf;
}

/* Best-effort TCP reachability probe: can we open a connection to host:port?
 * Returns 1 if reachable, 0 otherwise (with a human-readable reason in `err`).
 * This is what turns "Connect" into an honest operation — a bad hostname or a
 * dead port now fails instead of silently reporting a session as connected. */
static int tcp_reachable(const char* host, int port, int timeout_ms, char* err, size_t errcap)
{
    if (err && errcap) err[0] = '\0';
    if (!host || !host[0] || port <= 0)
    {
        if (err) snprintf(err, errcap, "Invalid hostname or port");
        return 0;
    }

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0)
    {
        if (err) snprintf(err, errcap, "Cannot resolve host '%s': %s", host, gai_strerror(gai));
        return 0;
    }

    int ok = 0;
    for (rp = res; rp && !ok; rp = rp->ai_next)
    {
        int fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            ok = 1;
        else if (errno == EINPROGRESS)
        {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT))
            {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0)
                    ok = 1;
            }
        }
        close(fd);
    }

    freeaddrinfo(res);
    if (!ok && err && !err[0])
        snprintf(err, errcap, "Cannot reach %s:%d (host down, wrong port, or blocked)", host, port);
    return ok;
}

static char* handle_connect(const char* message)
{
    if (!message || !message[0]) return json_error("Missing connection data");
    char id[64] = "", username[256] = "", session_id[256] = "";
    json_field_str(message, "id", id, sizeof(id));
    json_field_str(message, "username", username, sizeof(username));
    json_field_str(message, "session_id", session_id, sizeof(session_id));
    if (!id[0]) return json_error("Missing connection id");

    /* Snapshot the target under lock; the reachability probe below may block for
     * up to a few seconds, so it must not hold g_connections_lock. */
    char thost[256] = "", tproto[32] = "", tname[256] = "";
    int tport = 0;
    int found = 0;
    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, id) == 0)
        {
            snprintf(thost, sizeof(thost), "%s", g_connections[i].hostname);
            snprintf(tproto, sizeof(tproto), "%s", g_connections[i].protocol);
            snprintf(tname, sizeof(tname), "%s", g_connections[i].name);
            tport = g_connections[i].port;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_connections_lock);

    if (!found) return json_error("Connection not found");

    /* Verify the backend actually accepts a TCP connection before we report a
     * session as connected. Kubernetes has no single TCP endpoint here, so it
     * is skipped. */
    if (strcasecmp(tproto, "kubernetes") != 0)
    {
        char reason[256];
        if (!tcp_reachable(thost, tport, 5000, reason, sizeof(reason)))
            return json_error(reason);
    }

    GuacSession session;
    memset(&session, 0, sizeof(session));
    generate_id(session.id, sizeof(session.id));
    snprintf(session.connection_id, sizeof(session.connection_id), "%s", id);
    snprintf(session.username, sizeof(session.username), "%s", username);
    snprintf(session.session_id, sizeof(session.session_id), "%s", session_id);
    snprintf(session.protocol, sizeof(session.protocol), "%s", tproto);
    snprintf(session.hostname, sizeof(session.hostname), "%s", thost);
    session.port = tport;
    session.started = (long)time(NULL);
    session.last_active = session.started;
    session.active = 1;

    /* Re-acquire the lock to mark the connection active (it may have been
     * deleted while we probed). */
    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, id) == 0)
        {
            g_connections[i].active = 1;
            g_connections[i].last_used = session.started;
            break;
        }
    }
    pthread_mutex_unlock(&g_connections_lock);

    pthread_mutex_lock(&g_sessions_lock);
    if (g_session_count < MAX_SESSIONS)
        g_sessions[g_session_count++] = session;
    pthread_mutex_unlock(&g_sessions_lock);

    persist_connections();          /* last_used changed — keep the store fresh */

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"response\":\"success\",\"session_id\":\"%s\",\"protocol\":\"%s\","
        "\"hostname\":\"%s\",\"port\":%d,\"connection_name\":\"%s\"}",
        session.id, session.protocol, session.hostname,
        session.port, tname);
    return strdup(buf);
}

static char* handle_disconnect(const char* message)
{
    if (!message || !message[0]) return json_error("Missing session id");
    char id[64] = "";
    json_field_str(message, "id", id, sizeof(id));
    if (!id[0]) return json_error("Missing session id");

    pthread_mutex_lock(&g_sessions_lock);
    for (int i = 0; i < g_session_count; i++)
    {
        if (strcmp(g_sessions[i].id, id) == 0)
        {
            g_sessions[i].active = 0;
            g_sessions[i].last_active = (long)time(NULL);

            pthread_mutex_lock(&g_connections_lock);
            for (int j = 0; j < g_connection_count; j++)
            {
                if (strcmp(g_connections[j].id, g_sessions[i].connection_id) == 0)
                {
                    g_connections[j].active = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&g_connections_lock);

            g_sessions[i] = g_sessions[g_session_count - 1];
            g_session_count--;
            pthread_mutex_unlock(&g_sessions_lock);
            return strdup("{\"response\":\"success\",\"message\":\"Session disconnected\"}");
        }
    }
    pthread_mutex_unlock(&g_sessions_lock);
    return json_error("Session not found");
}

static char* handle_list_protocols(void)
{
    char buf[512] = "{\"response\":\"success\",\"protocols\":[";
    int first = 1;
    for (int i = 0; g_supported_protocols[i]; i++)
    {
        if (!first) strcat(buf, ",");
        first = 0;
        strcat(buf, "\"");
        strcat(buf, g_supported_protocols[i]);
        strcat(buf, "\"");
    }
    strcat(buf, "]}");
    return strdup(buf);
}

/* Discard the in-memory table and reload it from the .info store. */
static char* handle_reload_connections(void)
{
    pthread_mutex_lock(&g_connections_lock);
    g_connection_count = 0;
    pthread_mutex_unlock(&g_connections_lock);

    load_connections();

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"response\":\"success\",\"message\":\"Connections reloaded\",\"connection_count\":%d}",
        g_connection_count);
    return strdup(buf);
}

static char* handle_get_settings(void)
{
    char buf[1536];
    char path_esc[1200];
    json_escape(g_store_path, path_esc, sizeof(path_esc));
    snprintf(buf, sizeof(buf),
        "{\"response\":\"success\",\"settings\":{"
        "\"connection_count\":%d,"
        "\"session_count\":%d,\"max_connections\":%d,\"max_sessions\":%d,"
        "\"persistent\":true,\"storage_path\":\"%s\"}}",
        g_connection_count, g_session_count,
        MAX_CONNECTIONS, MAX_SESSIONS, path_esc);
    return strdup(buf);
}

const char* guac_mgmt_handle(const char* command, const char* body, size_t* out_len)
{
    if (!command)
    {
        if (out_len) *out_len = 0;
        return NULL;
    }

    char* result = NULL;

    if (strcmp(command, "connections") == 0)
    {
        if (!body || body[0] == '\0')
            result = handle_list_connections();
        else
        {
            char action[32] = "";
            json_field_str(body, "action", action, sizeof(action));
            if (strcmp(action, "get") == 0)
            {
                char gid[64] = "";
                json_field_str(body, "id", gid, sizeof(gid));
                result = handle_get_connection(gid);
            }
            else if (strcmp(action, "add") == 0)
                result = handle_add_connection(body);
            else if (strcmp(action, "update") == 0)
                result = handle_update_connection(body);
            else if (strcmp(action, "delete") == 0)
                result = handle_delete_connection(body);
            else if (strcmp(action, "reload") == 0)
                result = handle_reload_connections();
            else
                result = json_error("Unknown action (use: get, add, update, delete, reload)");
        }
    }
    else if (strcmp(command, "users") == 0)
    {
        result = strdup("{\"response\":\"success\",\"message\":\"User management via Kin auth\"}");
    }
    else if (strcmp(command, "active") == 0)
    {
        result = handle_list_sessions();
    }
    else if (strcmp(command, "connection") == 0)
    {
        char action[32] = "";
        json_field_str(body, "action", action, sizeof(action));
        if (strcmp(action, "connect") == 0)
            result = handle_connect(body);
        else if (strcmp(action, "disconnect") == 0)
            result = handle_disconnect(body);
        else
            result = json_error("Unknown action (use: connect, disconnect)");
    }
    else if (strcmp(command, "protocols") == 0)
    {
        result = handle_list_protocols();
    }
    else if (strcmp(command, "settings") == 0)
    {
        result = handle_get_settings();
    }
    else
    {
        result = json_error("Unknown API command");
    }

    if (out_len)
        *out_len = result ? strlen(result) : 0;
    return result;
}

GuacConnection* guac_mgmt_find_connection(const char* id)
{
    if (!id || !id[0]) return NULL;
    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, id) == 0)
        {
            pthread_mutex_unlock(&g_connections_lock);
            return &g_connections[i];
        }
    }
    pthread_mutex_unlock(&g_connections_lock);
    return NULL;
}

int guac_mgmt_mark_session_start(const char* conn_id, const char* session_id)
{
    if (!conn_id || !session_id) return -1;
    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, conn_id) == 0)
        {
            g_connections[i].active = 1;
            g_connections[i].last_used = (long)time(NULL);
            pthread_mutex_unlock(&g_connections_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_connections_lock);
    return -1;
}

int guac_mgmt_mark_session_end(const char* conn_id)
{
    if (!conn_id) return -1;
    pthread_mutex_lock(&g_connections_lock);
    for (int i = 0; i < g_connection_count; i++)
    {
        if (strcmp(g_connections[i].id, conn_id) == 0)
        {
            g_connections[i].active = 0;
            pthread_mutex_unlock(&g_connections_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_connections_lock);
    return -1;
}

int guac_mgmt_init(void)
{
    memset(g_connections, 0, sizeof(g_connections));
    memset(g_sessions, 0, sizeof(g_sessions));
    g_connection_count = 0;
    g_session_count = 0;
    resolve_store_path();
    load_connections();     /* restore persisted connections from the .info store */
    return 0;
}

void guac_mgmt_cleanup(void)
{
}
