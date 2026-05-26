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

static void* connection_thread(void* arg)
{
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

        /* Set connection args from stored connection */
        /* The plugin's guac_client_init will parse these from the environment */
        setenv("GUAC_HOSTNAME", conn->hostname, 1);
        setenv("GUAC_PORT", "", 1);
        {
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%d", conn->port);
            setenv("GUAC_PORT", port_str, 1);
        }
        if (conn->username[0])
            setenv("GUAC_USERNAME", conn->username, 1);
        if (conn->password[0])
            setenv("GUAC_PASSWORD", conn->password, 1);
        if (conn->private_key[0])
            setenv("GUAC_PRIVATE_KEY", conn->private_key, 1);
        if (conn->domain[0])
            setenv("GUAC_DOMAIN", conn->domain, 1);
        if (conn->security[0])
            setenv("GUAC_SECURITY", conn->security, 1);
        if (conn->color_depth[0])
            setenv("GUAC_COLOR_DEPTH", conn->color_depth, 1);
        if (conn->width > 0)
        {
            char wstr[16];
            snprintf(wstr, sizeof(wstr), "%d", conn->width);
            setenv("GUAC_WIDTH", wstr, 1);
        }
        if (conn->height > 0)
        {
            char hstr[16];
            snprintf(hstr, sizeof(hstr), "%d", conn->height);
            setenv("GUAC_HEIGHT", hstr, 1);
        }
        if (conn->dpi > 0)
        {
            char dstr[16];
            snprintf(dstr, sizeof(dstr), "%d", conn->dpi);
            setenv("GUAC_DPI", dstr, 1);
        }

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
