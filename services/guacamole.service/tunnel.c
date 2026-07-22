#define _GNU_SOURCE
#include "tunnel.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_WS_PAYLOAD (1024 * 1024)

typedef struct Ticket {
    char value[65];
    char username[256];
    char connection_id[128];
    time_t expires;
    struct Ticket *next;
} Ticket;

static Ticket *g_tickets;
static pthread_mutex_t g_ticket_lock = PTHREAD_MUTEX_INITIALIZER;
static guac_tunnel_protocol_start_fn g_start_protocol;

static int ticket_ttl(void)
{
    const char *value = getenv("KIN_GUACAMOLE_TICKET_TTL");
    int ttl = value && value[0] ? atoi(value) : 30;
    return ttl > 0 && ttl <= 300 ? ttl : 30;
}

static int random_hex(char *out, size_t cap)
{
    if (cap < 65) return -1;
    unsigned char bytes[32];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[i * 2] = hex[bytes[i] >> 4]; out[i * 2 + 1] = hex[bytes[i] & 15];
    }
    out[64] = 0;
    return 0;
}

static void prune_locked(time_t now)
{
    Ticket **p = &g_tickets;
    while (*p) {
        if ((*p)->expires <= now) { Ticket *old = *p; *p = old->next; free(old); }
        else p = &(*p)->next;
    }
}

int guac_tunnel_ticket_issue(const char *username, const char *connection_id,
                             char *ticket, size_t ticket_cap, long *expires)
{
    if (!username || !username[0] || !connection_id || !connection_id[0] ||
        strlen(username) >= 256 || strlen(connection_id) >= 128 || random_hex(ticket, ticket_cap) != 0)
        return -1;
    Ticket *item = calloc(1, sizeof(*item));
    if (!item) return -1;
    snprintf(item->value, sizeof(item->value), "%s", ticket);
    snprintf(item->username, sizeof(item->username), "%s", username);
    snprintf(item->connection_id, sizeof(item->connection_id), "%s", connection_id);
    item->expires = time(NULL) + ticket_ttl();
    pthread_mutex_lock(&g_ticket_lock);
    prune_locked(time(NULL));
    item->next = g_tickets; g_tickets = item;
    pthread_mutex_unlock(&g_ticket_lock);
    if (expires) *expires = (long)item->expires;
    return 0;
}

int guac_tunnel_ticket_consume(const char *value, char *username, size_t username_cap,
                               char *connection_id, size_t connection_cap)
{
    int found = -1;
    pthread_mutex_lock(&g_ticket_lock);
    prune_locked(time(NULL));
    Ticket **p = &g_tickets;
    while (*p) {
        if (strcmp((*p)->value, value) == 0) {
            Ticket *item = *p; *p = item->next;
            if (username && username_cap) snprintf(username, username_cap, "%s", item->username);
            if (connection_id && connection_cap) snprintf(connection_id, connection_cap, "%s", item->connection_id);
            free(item); found = 0; break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&g_ticket_lock);
    return found;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len) { ssize_t n = send(fd, p, len, MSG_NOSIGNAL); if (n <= 0) return -1; p += n; len -= (size_t)n; }
    return 0;
}

static int ws_send(int fd, unsigned opcode, const void *data, size_t len)
{
    unsigned char h[10]; size_t hn = 2;
    h[0] = 0x80 | (opcode & 15);
    if (len < 126) h[1] = (unsigned char)len;
    else if (len <= 65535) { h[1] = 126; h[2] = len >> 8; h[3] = len; hn = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (unsigned char)((uint64_t)len >> (56 - i * 8)); hn = 10; }
    return write_all(fd, h, hn) || write_all(fd, data, len) ? -1 : 0;
}

static int recv_exact(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len) { ssize_t n = recv(fd, p, len, 0); if (n <= 0) return -1; p += n; len -= (size_t)n; }
    return 0;
}

static int ws_receive_to_guac(int ws, int guac)
{
    unsigned char h[2];
    if (recv_exact(ws, h, 2) != 0) return -1;
    unsigned opcode = h[0] & 15;
    uint64_t len = h[1] & 127;
    if (!(h[1] & 0x80) || !(h[0] & 0x80)) return -1;
    if (len == 126) { unsigned char x[2]; if (recv_exact(ws,x,2)) return -1; len=((uint64_t)x[0]<<8)|x[1]; }
    else if (len == 127) { unsigned char x[8]; if (recv_exact(ws,x,8)) return -1; len=0; for(int i=0;i<8;i++) len=(len<<8)|x[i]; }
    if (len > MAX_WS_PAYLOAD) return -1;
    unsigned char mask[4]; if (recv_exact(ws,mask,4)) return -1;
    unsigned char *payload = malloc((size_t)len + 1); if (!payload) return -1;
    if (recv_exact(ws,payload,(size_t)len)) { free(payload); return -1; }
    for (uint64_t i=0;i<len;i++) payload[i] ^= mask[i&3];
    int rc = 0;
    if (opcode == 8) rc = -1;
    else if (opcode == 9) rc = ws_send(ws, 10, payload, (size_t)len);
    else if (opcode == 1 || opcode == 2) rc = write_all(guac, payload, (size_t)len);
    free(payload); return rc;
}

static int header_value(const char *request, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    for (const char *p = request; p && *p;) {
        const char *next = strstr(p, "\r\n"); if (!next) break;
        if (strncasecmp(p,name,n)==0 && p[n]==':') {
            p += n+1; while(*p==' '||*p=='\t')p++;
            size_t len=(size_t)(next-p); while(len && (p[len-1]==' '||p[len-1]=='\t'))len--;
            if (len >= cap) return 0;
            memcpy(out, p, len);
            out[len] = 0;
            return 1;
        }
        p = next + 2;
    }
    return 0;
}

static int header_has_token(const char *value, const char *wanted)
{
    size_t wn = strlen(wanted);
    for (const char *p = value; p && *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        if (n == wn && strncasecmp(p, wanted, wn) == 0) return 1;
        p = end ? end + 1 : p + n;
        if (!end) break;
    }
    return 0;
}

static void reject_http(int fd, const char *status)
{
    char response[256];
    int n = snprintf(response,sizeof(response),"HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",status);
    (void)write_all(fd,response,(size_t)n);
}

typedef struct { int fd; } TunnelClient;
static void *tunnel_client(void *opaque)
{
    TunnelClient *client = opaque; int ws = client->fd; free(client);
    char request[16385]; size_t used=0;
    while (used + 1 < sizeof(request)) {
        ssize_t n=recv(ws,request+used,sizeof(request)-used-1,0); if(n<=0){close(ws);return NULL;} used+=(size_t)n; request[used]=0;
        if (strstr(request,"\r\n\r\n")) break;
    }
    char method[16], target[2048];
    if (sscanf(request,"%15s %2047s",method,target)!=2 || strcmp(method,"GET") || strncmp(target,"/guacamole/tunnel-ws?",21)) {
        reject_http(ws,"404 Not Found"); close(ws); return NULL;
    }
    char key[256], protocol[256], upgrade[64];
    if (!header_value(request,"Sec-WebSocket-Key",key,sizeof(key)) ||
        !header_value(request,"Sec-WebSocket-Protocol",protocol,sizeof(protocol)) ||
        !header_has_token(protocol,"guacamole") ||
        !header_value(request,"Upgrade",upgrade,sizeof(upgrade)) || strcasecmp(upgrade,"websocket")) {
        reject_http(ws,"400 Bad Request"); close(ws); return NULL;
    }
    const char *ticket_arg = strstr(target,"ticket=");
    char ticket[65]={0}, connection[128]={0};
    if (!ticket_arg || sscanf(ticket_arg+7,"%64[0-9a-fA-F]",ticket)!=1 || strlen(ticket)!=64 ||
        guac_tunnel_ticket_consume(ticket,NULL,0,connection,sizeof(connection))) {
        reject_http(ws,"403 Forbidden"); close(ws); return NULL;
    }
    char combined[512]; snprintf(combined,sizeof(combined),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    unsigned char digest[SHA_DIGEST_LENGTH], accept[64]; SHA1((unsigned char*)combined,strlen(combined),digest);
    EVP_EncodeBlock(accept,digest,SHA_DIGEST_LENGTH);
    char response[512]; int rn=snprintf(response,sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: guacamole\r\n\r\n",accept);
    if (write_all(ws,response,(size_t)rn)) { close(ws); return NULL; }
    int pair[2]; if (socketpair(AF_UNIX,SOCK_STREAM,0,pair)) { close(ws); return NULL; }
    char peer[96]; snprintf(peer,sizeof(peer),"websocket:%.84s",connection);
    if (!g_start_protocol || g_start_protocol(pair[1],peer)) { close(pair[0]);close(pair[1]);close(ws);return NULL; }
    char select[300]; int sn=snprintf(select,sizeof(select),"6.select,%zu.$%s;",strlen(connection)+1,connection);
    if (write_all(pair[0],select,(size_t)sn)) { close(pair[0]);close(ws);return NULL; }
    struct pollfd fds[2]={{ws,POLLIN,0},{pair[0],POLLIN,0}};
    char buf[65536];
    while (poll(fds,2,-1)>0) {
        if (fds[0].revents & POLLIN) { if (ws_receive_to_guac(ws,pair[0])) break; }
        if (fds[1].revents & POLLIN) { ssize_t n=recv(pair[0],buf,sizeof(buf),0); if(n<=0||ws_send(ws,1,buf,(size_t)n)) break; }
        if (fds[0].revents&(POLLERR|POLLHUP|POLLNVAL) || fds[1].revents&(POLLERR|POLLHUP|POLLNVAL)) break;
    }
    (void)ws_send(ws,8,"",0); close(pair[0]); close(ws); return NULL;
}

int guac_tunnel_listen(guac_tunnel_protocol_start_fn start_protocol)
{
    g_start_protocol = start_protocol;
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); if(fd<0)return -1;
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(19131); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(fd,(struct sockaddr*)&a,sizeof(a))||listen(fd,32)){close(fd);return -1;} return fd;
}

void guac_tunnel_accept(int listen_fd)
{
    int fd=accept4(listen_fd,NULL,NULL,SOCK_CLOEXEC); if(fd<0)return;
    TunnelClient *c=malloc(sizeof(*c)); if(!c){close(fd);return;} c->fd=fd;
    pthread_t tid; if(pthread_create(&tid,NULL,tunnel_client,c)){free(c);close(fd);} else pthread_detach(tid);
}

void guac_tunnel_cleanup(void)
{
    pthread_mutex_lock(&g_ticket_lock); Ticket *p=g_tickets; g_tickets=NULL; pthread_mutex_unlock(&g_ticket_lock);
    while(p){Ticket *next=p->next;free(p);p=next;}
}
