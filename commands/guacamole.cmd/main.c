/*
 * guacamole — KinDOS command for managing Guacamole remote-desktop connections.
 *
 * Everything the admin app's Sessions/Connections UI can do, from the shell:
 * list / get / add / update / delete / reload connections, list active sessions,
 * list supported protocols, and connect / disconnect a session. It talks to
 * guacamole.service over the exact same IPC surface the http.service uses
 * (event "guacamole", message "<command>|<json-body>"), and prints the service's
 * JSON response verbatim — so scripted callers get the same result the UI does.
 *
 * Usage:
 *   guacamole op=list
 *   guacamole op=get id=<id>
 *   guacamole op=add name=Office protocol=vnc hostname=10.0.0.5 port=5900 password=secret
 *   guacamole op=update id=<id> hostname=10.0.0.6 enable_audio=true
 *   guacamole op=delete id=<id>
 *   guacamole op=reload
 *   guacamole op=sessions
 *   guacamole op=protocols
 *   guacamole op=connect id=<id>
 *   guacamole op=disconnect id=<id>
 *
 * The Kin shell passes manager_pid=<pid> automatically; it also honours the
 * KIN_MANAGER_PID environment variable when run standalone.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kin.h"

#define RESP_CAP 262144
#define BODY_CAP 16384

/* ------------------------------------------------------------------ args --- */

static const char* get_arg_value(int argc, char* argv[], const char* key)
{
    size_t key_len = strlen(key);
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], key, key_len) == 0 && argv[i][key_len] == '=')
            return argv[i] + key_len + 1;
    return NULL;
}

static int arg_is_true(const char* v)
{
    return v && (!strcasecmp(v, "1") || !strcasecmp(v, "true")
                 || !strcasecmp(v, "yes") || !strcasecmp(v, "on"));
}

/* ------------------------------------------------------------------ json --- */

/* Append a JSON-escaped string (no surrounding quotes) into out[]. */
static void json_escape_into(char* out, size_t cap, size_t* len, const char* in)
{
    for (const unsigned char* p = (const unsigned char*)(in ? in : ""); *p; p++)
    {
        char esc[8];
        int n;
        if (*p == '"' || *p == '\\') n = snprintf(esc, sizeof(esc), "\\%c", *p);
        else if (*p == '\n')         n = snprintf(esc, sizeof(esc), "\\n");
        else if (*p == '\r')         n = snprintf(esc, sizeof(esc), "\\r");
        else if (*p == '\t')         n = snprintf(esc, sizeof(esc), "\\t");
        else if (*p < 32)            n = snprintf(esc, sizeof(esc), "\\u%04x", *p);
        else                         n = snprintf(esc, sizeof(esc), "%c", *p);
        if (n < 0 || *len + (size_t)n + 1 >= cap) return;
        memcpy(out + *len, esc, (size_t)n);
        *len += (size_t)n;
        out[*len] = '\0';
    }
}

static void buf_add(char* out, size_t cap, size_t* len, const char* s)
{
    size_t n = strlen(s);
    if (*len + n + 1 >= cap) return;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
}

static void print_fail(const char* msg)
{
    char out[1024];
    size_t len = 0;
    out[0] = '\0';
    buf_add(out, sizeof(out), &len, "{\"response\":\"fail\",\"message\":\"");
    json_escape_into(out, sizeof(out), &len, msg);
    buf_add(out, sizeof(out), &len, "\"}");
    printf("%s", out);
}

/* Connection fields, matching guacamole.service's add/update JSON parser. */
typedef enum { F_STR, F_INT, F_BOOL } FieldType;
static const struct { const char* name; FieldType type; } FIELDS[] = {
    { "name",                    F_STR  },
    { "protocol",                F_STR  },
    { "hostname",                F_STR  },
    { "port",                    F_INT  },
    { "username",                F_STR  },
    { "password",                F_STR  },
    { "private_key",             F_STR  },
    { "domain",                  F_STR  },
    { "security",                F_STR  },
    { "color_depth",             F_STR  },
    { "remote_app",              F_STR  },
    { "remote_app_dir",          F_STR  },
    { "remote_app_args",         F_STR  },
    { "width",                   F_INT  },
    { "height",                  F_INT  },
    { "dpi",                     F_INT  },
    { "enable_audio",            F_BOOL },
    { "enable_video",            F_BOOL },
    { "enable_printing",         F_BOOL },
    { "enable_file_transfer",    F_BOOL },
    { "enable_wallpaper",        F_BOOL },
    { "enable_theming",          F_BOOL },
    { "enable_font_smoothing",   F_BOOL },
    { "enable_full_window_drag", F_BOOL },
    { "enable_menu_animation",   F_BOOL },
    { "disable_copy",            F_BOOL },
    { "disable_paste",           F_BOOL },
    { "ignore_cert",             F_BOOL },
};

/* Build the "<command>|<json-body>" for an add/update from the named args that
 * are actually present (omitted fields keep the service's defaults). */
static void build_conn_body(int argc, char* argv[], const char* action,
                            const char* id, char* out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    buf_add(out, cap, &len, "connections|{\"action\":\"");
    buf_add(out, cap, &len, action);
    buf_add(out, cap, &len, "\"");
    if (id && id[0])
    {
        buf_add(out, cap, &len, ",\"id\":\"");
        json_escape_into(out, cap, &len, id);
        buf_add(out, cap, &len, "\"");
    }
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++)
    {
        const char* v = get_arg_value(argc, argv, FIELDS[i].name);
        if (!v) continue;
        buf_add(out, cap, &len, ",\"");
        buf_add(out, cap, &len, FIELDS[i].name);
        buf_add(out, cap, &len, "\":");
        if (FIELDS[i].type == F_STR)
        {
            buf_add(out, cap, &len, "\"");
            json_escape_into(out, cap, &len, v);
            buf_add(out, cap, &len, "\"");
        }
        else if (FIELDS[i].type == F_INT)
        {
            char nb[32];
            snprintf(nb, sizeof(nb), "%ld", strtol(v, NULL, 10));
            buf_add(out, cap, &len, nb);
        }
        else /* F_BOOL */
        {
            buf_add(out, cap, &len, arg_is_true(v) ? "true" : "false");
        }
    }
    buf_add(out, cap, &len, "}");
}

/* --------------------------------------------------------------- ipc ------- */

typedef struct { int done; char* message; } wait_ctx_t;

static void on_response(const char* event, const char* message, void* user_data,
                        long timestamp, unsigned long message_id, unsigned long callback_id)
{
    (void)event; (void)timestamp; (void)message_id; (void)callback_id;
    wait_ctx_t* ctx = (wait_ctx_t*)user_data;
    if (!ctx) return;
    free(ctx->message);
    ctx->message = NULL;
    if (message && message[0])
    {
        size_t n = strlen(message);
        ctx->message = (char*)malloc(n + 1);
        if (ctx->message) memcpy(ctx->message, message, n + 1);
    }
    ctx->done = 1;
}

/* Send one IPC request to guacamole.service and wait (up to ~10s) for its reply. */
static int send_ipc_and_wait(const char* message, char* out, size_t out_size)
{
    wait_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    unsigned long mid = kin_message_write_callback("guacamole", message, on_response, &ctx);
    if (mid == 0) return -1;

    int elapsed = 0;
    while (!ctx.done && elapsed < 10000)
    {
        kin_wait_messages();
        usleep(10000);
        elapsed += 10;
    }
    if (!ctx.done) { free(ctx.message); kin_cancel_response_callback(&ctx); return -1; }

    int rc = -1;
    if (ctx.message && ctx.message[0])
    {
        snprintf(out, out_size, "%s", ctx.message);
        rc = 0;
    }
    free(ctx.message);
    return rc;
}

/* --------------------------------------------------------------- usage ----- */

static void print_usage(void)
{
    puts("Manage Guacamole remote-desktop connections (same actions as the admin app).");
    puts("");
    puts("Usage: guacamole op=<operation> [args...]");
    puts("");
    puts("Operations:");
    puts("  list                      list all stored connections");
    puts("  get id=<id>               show one connection");
    puts("  add <fields...>           create a connection");
    puts("  update id=<id> <fields>   modify a connection (only given fields change)");
    puts("  delete id=<id>            remove a connection");
    puts("  reload                    reload connections from disk");
    puts("  sessions                  list active sessions");
    puts("  protocols                 list supported protocols");
    puts("  connect id=<id>           start a session");
    puts("  disconnect id=<id>        end a session");
    puts("");
    puts("Fields (add/update): name protocol hostname port username password");
    puts("  private_key domain security color_depth remote_app remote_app_dir");
    puts("  remote_app_args width height dpi enable_audio enable_video enable_printing");
    puts("  enable_file_transfer enable_wallpaper enable_theming enable_font_smoothing");
    puts("  enable_full_window_drag enable_menu_animation disable_copy disable_paste");
    puts("");
    puts("Booleans accept 1/true/yes/on. Output is the service's JSON response.");
    puts("Example: guacamole op=add name=Office protocol=vnc hostname=10.0.0.5 port=5900 password=secret");
}

/* --------------------------------------------------------------- main ------ */

int main(int argc, char* argv[])
{
    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
    { print_usage(); return 0; }

    const char* op = get_arg_value(argc, argv, "op");
    const char* id = get_arg_value(argc, argv, "id");
    if (!op || !op[0]) { print_fail("Missing op (try: guacamole --help)."); return 1; }

    /* Resolve the manager pid the same way other KinDOS commands do. */
    const char* mp_arg = get_arg_value(argc, argv, "manager_pid");
    const char* mp_env = getenv("KIN_MANAGER_PID");
    int manager_pid = 0;
    if (mp_arg && *mp_arg) manager_pid = atoi(mp_arg);
    else if (mp_env && *mp_env) manager_pid = atoi(mp_env);
    if (manager_pid <= 0) { print_fail("Missing manager_pid (run inside Kin or set KIN_MANAGER_PID)."); return 1; }

    /* Register under a distinct process identity — the service already owns the
     * "guacamole" name; we only send to its "guacamole" event, below. */
    if (kin_init("guacamole_cli", manager_pid) < 0)
    { print_fail("Could not connect to the Kin message bus."); return 1; }

    char message[BODY_CAP];
    int needs_id = 0;

    if (!strcmp(op, "list"))            snprintf(message, sizeof(message), "connections");
    else if (!strcmp(op, "reload"))     snprintf(message, sizeof(message), "connections|{\"action\":\"reload\"}");
    else if (!strcmp(op, "sessions") || !strcmp(op, "active"))
                                        snprintf(message, sizeof(message), "active");
    else if (!strcmp(op, "protocols"))  snprintf(message, sizeof(message), "protocols");
    else if (!strcmp(op, "get"))      { needs_id = 1; snprintf(message, sizeof(message), "connections|{\"action\":\"get\",\"id\":\"%s\"}", id ? id : ""); }
    else if (!strcmp(op, "delete"))   { needs_id = 1; snprintf(message, sizeof(message), "connections|{\"action\":\"delete\",\"id\":\"%s\"}", id ? id : ""); }
    else if (!strcmp(op, "connect"))  { needs_id = 1; snprintf(message, sizeof(message), "connection|{\"action\":\"connect\",\"id\":\"%s\"}", id ? id : ""); }
    else if (!strcmp(op, "disconnect")){ needs_id = 1; snprintf(message, sizeof(message), "connection|{\"action\":\"disconnect\",\"id\":\"%s\"}", id ? id : ""); }
    else if (!strcmp(op, "add"))        build_conn_body(argc, argv, "add", NULL, message, sizeof(message));
    else if (!strcmp(op, "update"))   { needs_id = 1; build_conn_body(argc, argv, "update", id, message, sizeof(message)); }
    else { print_fail("Unknown op (try: guacamole --help)."); kin_cleanup(); return 1; }

    if (needs_id && (!id || !id[0]))
    { print_fail("This op requires id=<connection-id>."); kin_cleanup(); return 1; }

    char resp[RESP_CAP];
    int rc = send_ipc_and_wait(message, resp, sizeof(resp));
    kin_cleanup();

    if (rc != 0) { print_fail("No response from guacamole.service (is it running?)."); return 1; }
    printf("%s", resp);
    return 0;
}
