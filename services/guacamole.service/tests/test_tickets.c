#include "../tunnel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(expr, msg) do { if (!(expr)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    char ticket[65], user[256], connection[128];
    long expires = 0;
    CHECK(guac_tunnel_ticket_issue("alice", "connection-1", ticket, sizeof(ticket), &expires) == 0,
          "issue ticket");
    CHECK(strlen(ticket) == 64 && expires > 0, "ticket shape");
    CHECK(guac_tunnel_ticket_consume(ticket, user, sizeof(user), connection, sizeof(connection)) == 0,
          "consume ticket");
    CHECK(strcmp(user, "alice") == 0, "ticket user binding");
    CHECK(strcmp(connection, "connection-1") == 0, "ticket connection binding");
    CHECK(guac_tunnel_ticket_consume(ticket, user, sizeof(user), connection, sizeof(connection)) != 0,
          "ticket is single use");
    CHECK(guac_tunnel_ticket_consume("not-a-ticket", user, sizeof(user), connection, sizeof(connection)) != 0,
          "invalid ticket rejected");
    setenv("KIN_GUACAMOLE_TICKET_TTL", "1", 1);
    CHECK(guac_tunnel_ticket_issue("bob", "connection-2", ticket, sizeof(ticket), &expires) == 0,
          "issue expiring ticket");
    sleep(2);
    CHECK(guac_tunnel_ticket_consume(ticket, user, sizeof(user), connection, sizeof(connection)) != 0,
          "expired ticket rejected");
    guac_tunnel_cleanup();
    printf("ticket tests: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
