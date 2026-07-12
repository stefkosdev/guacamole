#define _GNU_SOURCE
#include "../../libraries/kin/kin.library.h"
#include "management.h"

#include <guacamole/client.h>
#include <guacamole/error.h>
#include <guacamole/parser.h>
#include <guacamole/plugin.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>
#include <guacamole/user.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define GUAC_LINE_MAX 4096
#define GUAC_SOCK_BACKLOG 16

static volatile sig_atomic_t g_running = 1;
static int g_listen_fd = -1;
static int g_wake_pipe[2] = { -1, -1 };
static int g_manager_pid;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_wake_pipe[1] >= 0)
    {
        char b = 1;
        (void)write(g_wake_pipe[1], &b, 1);
    }
}

static int guac_sock_path(char* out, size_t cap)
{
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0])
    {
        char ddir[512];
        snprintf(ddir, sizeof(ddir), "%s/kin", rt);
        (void)mkdir(ddir, 0700);
        return snprintf(out, cap, "%s/guacamole.sock", ddir) < (int)cap ? 0 : -1;
    }
    return snprintf(out, cap, "/tmp/kin-guacamole-%d.sock", (int)getuid()) < (int)cap ? 0 : -1;
}

/* IPC handler thread: receives management API calls via Kin messaging */
static void ipc_handler(const char* event, const char* message, void* user_data,
                        long timestamp, unsigned long message_id, unsigned long callback_id)
{
    (void)user_data; (void)timestamp; (void)message_id;

    kin_log_info("guacamole", "IPC: event=%s message=%s", event ? event : "", message ? message : "");

    if (!event) return;

    char cmd_buf[256] = "";
    const char* msg_body = NULL;

    if (message && message[0])
    {
        char msg_copy[65536];
        snprintf(msg_copy, sizeof(msg_copy), "%s", message);

        char* pipe = strchr(msg_copy, '|');
        if (pipe)
        {
            *pipe = '\0';
            msg_body = pipe + 1;
        }

        if (strncmp(msg_copy, "guacamole/", 10) == 0)
            snprintf(cmd_buf, sizeof(cmd_buf), "%s", msg_copy + 10);
        else
            snprintf(cmd_buf, sizeof(cmd_buf), "%s", msg_copy);
    }
    else
    {
        if (strncmp(event, "guacamole/", 10) == 0)
            snprintf(cmd_buf, sizeof(cmd_buf), "%s", event + 10);
        else
            snprintf(cmd_buf, sizeof(cmd_buf), "%s", event);
    }

    if (!cmd_buf[0])
    {
        if (callback_id > 0)
            kin_write_response(callback_id, "response",
                "{\"response\":\"fail\",\"message\":\"Empty command\"}");
        return;
    }

    size_t out_len = 0;
    const char* result = guac_mgmt_handle(cmd_buf, msg_body, &out_len);
    if (!result)
        result = "{\"response\":\"fail\",\"message\":\"Command not handled\"}";

    if (callback_id > 0)
        kin_write_response(callback_id, "response", result);

    free((void*)result);
}

/* Connection thread: handles a Guacamole protocol client */
typedef struct {
    int client_fd;
    char peer_str[64];
} GuacClientThreadParams;

/*
 * Per-connection injection of a stored connection's parameters.
 *
 * A libguac protocol plugin receives its parameters as the argv of the
 * client's "connect" instruction (guac_*_parse_args reads argv, never the
 * environment). For a stored connection (`select $<id>`) the parameters must
 * come from the server, not from whatever the remote client sends. We do this
 * by overriding the plugin's join_handler for THIS connection only and
 * rebuilding argv from the stored connection before the plugin parses it.
 *
 * State is thread-local: each connection runs on its own thread, the join
 * handler runs synchronously on that same thread inside
 * guac_user_handle_connection(), and guac_client is per-connection. So there
 * are no process-global mutations — concurrent sessions cannot clobber each
 * other's host/credentials (the previous setenv() approach could, and was
 * ignored by the plugin anyway).
 */
static __thread GuacConnection t_stored_conn;
static __thread guac_user_join_handler* t_orig_join_handler;
static __thread int t_use_stored_conn;

static const char* guac_bool(int v) { return v ? "true" : "false"; }

static int stored_conn_join_handler(guac_user* user, int argc, char** argv)
{
    guac_user_join_handler* orig = t_orig_join_handler;
    if (!t_use_stored_conn || !orig)
        return orig ? orig(user, argc, argv) : 1;

    const GuacConnection* c = &t_stored_conn;
    const char** names = user->client->args;

    char** ov = malloc((size_t)argc * sizeof(char*));
    if (!ov)
        return orig(user, argc, argv);   /* fall back rather than fail */

    char portb[16], wb[16], hb[16], db[16];
    snprintf(portb, sizeof(portb), "%d", c->port);
    snprintf(wb, sizeof(wb), "%d", c->width);
    snprintf(hb, sizeof(hb), "%d", c->height);
    snprintf(db, sizeof(db), "%d", c->dpi);

    for (int i = 0; i < argc; i++)
    {
        const char* n = names[i] ? names[i] : "";
        const char* v = argv[i];          /* default: keep client-sent value */

        if      (!strcmp(n, "hostname"))                v = c->hostname;
        else if (!strcmp(n, "port"))                    v = c->port > 0 ? portb : argv[i];
        else if (!strcmp(n, "username"))                v = c->username;
        else if (!strcmp(n, "password"))                v = c->password;
        else if (!strcmp(n, "domain"))                  v = c->domain;
        else if (!strcmp(n, "private-key"))             v = c->private_key;
        else if (!strcmp(n, "security"))                v = c->security;
        else if (!strcmp(n, "color-depth"))             v = c->color_depth[0] ? c->color_depth : argv[i];
        else if (!strcmp(n, "width"))                   v = c->width > 0 ? wb : argv[i];
        else if (!strcmp(n, "height"))                  v = c->height > 0 ? hb : argv[i];
        else if (!strcmp(n, "dpi"))                     v = c->dpi > 0 ? db : argv[i];
        else if (!strcmp(n, "enable-audio"))            v = guac_bool(c->enable_audio);
        else if (!strcmp(n, "enable-printing"))         v = guac_bool(c->enable_printing);
        else if (!strcmp(n, "enable-drive"))            v = guac_bool(c->enable_file_transfer);
        else if (!strcmp(n, "enable-wallpaper"))        v = guac_bool(c->enable_wallpaper);
        else if (!strcmp(n, "enable-theming"))          v = guac_bool(c->enable_theming);
        else if (!strcmp(n, "enable-font-smoothing"))   v = guac_bool(c->enable_font_smoothing);
        else if (!strcmp(n, "enable-full-window-drag")) v = guac_bool(c->enable_full_window_drag);
        else if (!strcmp(n, "enable-menu-animations"))  v = guac_bool(c->enable_menu_animation);
        else if (!strcmp(n, "disable-copy"))            v = guac_bool(c->disable_copy);
        else if (!strcmp(n, "disable-paste"))           v = guac_bool(c->disable_paste);

        ov[i] = (char*)(v ? v : "");
    }

    int rc = orig(user, argc, ov);
    free(ov);
    return rc;
}

static void* connection_thread(void* arg)
{
    t_use_stored_conn = 0;

    GuacClientThreadParams* params = (GuacClientThreadParams*)arg;
    int client_fd = params->client_fd;
    char peer_str[64];
    snprintf(peer_str, sizeof(peer_str), "%s", params->peer_str);
    free(params);

    kin_log_info("guacamole", "New connection from %s", peer_str);

    /* Wrap FD in guac_socket */
    guac_socket* socket = guac_socket_open(client_fd);
    if (!socket)
    {
        kin_log_info("guacamole", "Failed to create guac_socket for %s", peer_str);
        close(client_fd);
        return NULL;
    }

    /* Create parser and read handshake */
    guac_parser* parser = guac_parser_alloc();
    if (!parser)
    {
        guac_socket_free(socket);
        close(client_fd);
        return NULL;
    }

    /* Expect "select" instruction with protocol or connection ID */
    if (guac_parser_expect(parser, socket, 30000000, "select"))
    {
        kin_log_info("guacamole", "Handshake failed for %s: expected 'select'", peer_str);
        guac_parser_free(parser);
        guac_socket_free(socket);
        close(client_fd);
        return NULL;
    }

    if (parser->argc != 1)
    {
        kin_log_info("guacamole", "Bad 'select' arg count from %s", peer_str);
        guac_parser_free(parser);
        guac_socket_free(socket);
        close(client_fd);
        return NULL;
    }

    const char* identifier = parser->argv[0];
    kin_log_info("guacamole", "Client %s selected '%s'", peer_str, identifier);

    /* Create client and load plugin */
    guac_client* client = guac_client_alloc();
    if (!client)
    {
        guac_parser_free(parser);
        guac_socket_free(socket);
        close(client_fd);
        return NULL;
    }

    int is_connection_id = (identifier[0] == '$');
    if (!is_connection_id)
    {
        /* Protocol name - load plugin directly */
        if (guac_client_load_plugin(client, identifier))
        {
            const char* reason = (guac_error == GUAC_STATUS_NOT_FOUND)
                ? "Protocol not supported"
                : "Failed to load protocol plugin";
            kin_log_info("guacamole", "%s for %s: '%s'", reason, peer_str, identifier);
            guac_protocol_send_error(socket, reason, GUAC_PROTOCOL_STATUS_RESOURCE_NOT_FOUND);
            guac_socket_flush(socket);
            guac_client_free(client);
            guac_parser_free(parser);
            guac_socket_free(socket);
            close(client_fd);
            return NULL;
        }
    }
    else
    {
        /* Connection ID - find stored connection */
        GuacConnection* conn = guac_mgmt_find_connection(identifier + 1);
        if (!conn)
        {
            kin_log_info("guacamole", "Connection '%s' not found for %s", identifier, peer_str);
            guac_protocol_send_error(socket, "Connection not found",
                GUAC_PROTOCOL_STATUS_RESOURCE_NOT_FOUND);
            guac_socket_flush(socket);
            guac_client_free(client);
            guac_parser_free(parser);
            guac_socket_free(socket);
            close(client_fd);
            return NULL;
        }

        /* Load plugin by protocol from stored connection */
        if (guac_client_load_plugin(client, conn->protocol))
        {
            kin_log_info("guacamole", "Failed to load plugin '%s' for %s", conn->protocol, peer_str);
            guac_client_free(client);
            guac_parser_free(parser);
            guac_socket_free(socket);
            close(client_fd);
            return NULL;
        }

        /* Inject the stored connection's parameters into the plugin per this
         * connection only (thread-local; no process-global state), so the
         * plugin connects to the stored host with the stored credentials
         * regardless of what the remote client supplies. See
         * stored_conn_join_handler above. */
        t_stored_conn = *conn;
        t_orig_join_handler = client->join_handler;
        t_use_stored_conn = 1;
        client->join_handler = stored_conn_join_handler;

        /* Mark session active */
        guac_mgmt_mark_session_start(conn->id, "");
    }

    /* Logging handler */
    client->log_handler = NULL; /* use default */

    /* Create user (owner) */
    guac_user* user = guac_user_alloc();
    if (!user)
    {
        guac_client_free(client);
        guac_parser_free(parser);
        guac_socket_free(socket);
        close(client_fd);
        return NULL;
    }

    user->socket = socket;
    user->client = client;
    user->owner = 1;

    /* Handle the connection (blocks until disconnect) */
    guac_user_handle_connection(user, 30000000);

    /* Clean up */
    kin_log_info("guacamole", "Client %s disconnected", peer_str);

    if (client->connected_users == 0)
        guac_client_stop(client);

    /* Find connection ID from identifier and mark session end */
    if (identifier[0] == '$')
        guac_mgmt_mark_session_end(identifier + 1);

    guac_socket_free(socket);
    guac_user_free(user);
    guac_parser_free(parser);
    guac_client_free(client);
    close(client_fd);

    return NULL;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <manager_pid>\n", argv[0]);
        return 1;
    }

    g_manager_pid = atoi(argv[1]);

    if (kin_init("guacamole", g_manager_pid) == -1)
    {
        fprintf(stderr, "guacamole.service: kin_init failed\n");
        return 1;
    }
    kin_log_info("guacamole", "Kin Guacamole service initialized");

    guac_mgmt_init();

    /* Register IPC handler */
    if (kin_message_callback("guacamole", ipc_handler, NULL) == -1 &&
        kin_message_callback("api", ipc_handler, NULL) == -1)
    {
        kin_message_callback("guacamole", ipc_handler, NULL);
    }

    /* Signal handling */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Create Unix socket */
    char sockpath[512];
    if (guac_sock_path(sockpath, sizeof(sockpath)) != 0)
    {
        kin_log_info("guacamole", "Socket path too long");
        guac_mgmt_cleanup();
        kin_cleanup();
        return 1;
    }

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
    {
        kin_log_info("guacamole", "socket() failed: %s", strerror(errno));
        guac_mgmt_cleanup();
        kin_cleanup();
        return 1;
    }
    g_listen_fd = lfd;
    unlink(sockpath);

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, sockpath, sizeof(sun.sun_path) - 1);
    if (bind(lfd, (struct sockaddr*)&sun, sizeof(sun)) != 0)
    {
        kin_log_info("guacamole", "bind(%s) failed: %s", sockpath, strerror(errno));
        close(lfd);
        guac_mgmt_cleanup();
        kin_cleanup();
        return 1;
    }
    chmod(sockpath, 0600);

    if (listen(lfd, GUAC_SOCK_BACKLOG) != 0)
    {
        kin_log_info("guacamole", "listen() failed: %s", strerror(errno));
        close(lfd);
        unlink(sockpath);
        guac_mgmt_cleanup();
        kin_cleanup();
        return 1;
    }
    kin_log_info("guacamole", "Listening on %s", sockpath);

    /* Wake pipe for clean shutdown */
    if (pipe(g_wake_pipe) != 0)
    {
        kin_log_info("guacamole", "pipe() failed: %s", strerror(errno));
        close(lfd);
        unlink(sockpath);
        guac_mgmt_cleanup();
        kin_cleanup();
        return 1;
    }
    fcntl(g_wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_wake_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(g_wake_pipe[1], F_SETFL, O_NONBLOCK);

    /* Main poll loop (like proxy.service) */
    while (g_running)
    {
        struct pollfd fds[2];
        fds[0].fd = lfd;
        fds[0].events = POLLIN;
        fds[1].fd = g_wake_pipe[0];
        fds[1].events = POLLIN;

        int pr = poll(fds, 2, -1);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            kin_log_info("guacamole", "poll() failed: %s", strerror(errno));
            break;
        }
        if (!g_running) break;

        if (fds[1].revents & POLLIN)
        {
            char drain[32];
            while (read(g_wake_pipe[0], drain, sizeof(drain)) > 0) {}
            break;
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        if (!(fds[0].revents & POLLIN))
            continue;

        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int cfd = accept(lfd, (struct sockaddr*)&client_addr, &addr_len);
        if (cfd < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EBADF || errno == EINVAL) break;
            kin_log_info("guacamole", "accept() failed: %s", strerror(errno));
            continue;
        }

        /* Spawn connection thread (detached, like proxy.service) */
        GuacClientThreadParams* p = malloc(sizeof(GuacClientThreadParams));
        if (p)
        {
            p->client_fd = cfd;
            snprintf(p->peer_str, sizeof(p->peer_str), "fd:%d", cfd);

            pthread_t tid;
            if (pthread_create(&tid, NULL, connection_thread, p) != 0)
            {
                close(cfd);
                free(p);
            }
            else
                pthread_detach(tid);
        }
        else
            close(cfd);
    }

    /* Cleanup */
    if (g_wake_pipe[0] >= 0) { close(g_wake_pipe[0]); g_wake_pipe[0] = -1; }
    if (g_wake_pipe[1] >= 0) { close(g_wake_pipe[1]); g_wake_pipe[1] = -1; }
    if (g_listen_fd >= 0) close(lfd);
    g_listen_fd = -1;
    unlink(sockpath);

    guac_mgmt_cleanup();
    kin_cleanup();
    return 0;
}
