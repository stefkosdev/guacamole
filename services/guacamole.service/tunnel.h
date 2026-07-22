#ifndef KIN_GUACAMOLE_TUNNEL_H
#define KIN_GUACAMOLE_TUNNEL_H

#include <stddef.h>

typedef int (*guac_tunnel_protocol_start_fn)(int fd, const char *peer);

int guac_tunnel_ticket_issue(const char *username, const char *connection_id,
                             char *ticket, size_t ticket_cap, long *expires);
int guac_tunnel_ticket_consume(const char *ticket, char *username, size_t username_cap,
                               char *connection_id, size_t connection_cap);
int guac_tunnel_listen(guac_tunnel_protocol_start_fn start_protocol);
void guac_tunnel_accept(int listen_fd);
void guac_tunnel_cleanup(void);

#endif
