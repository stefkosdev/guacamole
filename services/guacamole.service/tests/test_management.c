/*
 * Unit / integration tests for the guacamole.service management layer.
 *
 * These drive the PUBLIC dispatcher (guac_mgmt_handle) exactly as service.c
 * does, plus guac_mgmt_init/find, so they exercise CRUD, defaults, JSON
 * (de)serialization, and the ".info" persistence round-trip.
 *
 * No external dependencies: compiled directly against management.c, so this
 * runs without libguac or kin.library. See tests/Makefile.
 */
#include "../management.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Open a loopback TCP listener so "connect" has a reachable target. Returns the
 * listening fd and writes the chosen ephemeral port to *port_out. */
static int open_listener(int* port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 1) != 0)
    {
        close(fd);
        return -1;
    }
    socklen_t sl = sizeof(a);
    getsockname(fd, (struct sockaddr*)&a, &sl);
    *port_out = ntohs(a.sin_port);
    return fd;
}

/* ---- tiny assertion framework ------------------------------------------ */

static int g_checks = 0;
static int g_fails = 0;
static const char* g_current = "";

#define CHECK(cond, msg) do {                                             \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            fprintf(stderr, "  FAIL [%s]: %s (%s:%d)\n",                  \
                    g_current, (msg), __FILE__, __LINE__);               \
        }                                                                 \
    } while (0)

#define CONTAINS(hay, needle) (strstr((hay), (needle)) != NULL)

static char g_store[512];

/* Fresh, empty store for a test, then (re)init the in-memory table. */
static void reset_store(void)
{
    unlink(g_store);
    guac_mgmt_init();
}

/* Convenience: call the dispatcher and return an owned string (caller frees). */
static char* call(const char* cmd, const char* body)
{
    size_t len = 0;
    const char* r = guac_mgmt_handle(cmd, body, &len);
    return r ? (char*)r : strdup("(null)");
}

/* Extract the connection_id value from an add response into `out`. */
static void extract_id(const char* json, char* out, size_t cap)
{
    out[0] = '\0';
    const char* p = strstr(json, "\"connection_id\":\"");
    if (!p) return;
    p += strlen("\"connection_id\":\"");
    size_t w = 0;
    while (*p && *p != '"' && w + 1 < cap) out[w++] = *p++;
    out[w] = '\0';
}

/* Count occurrences of `needle` in `hay`. */
static int count(const char* hay, const char* needle)
{
    int n = 0;
    const char* p = hay;
    size_t l = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { n++; p += l; }
    return n;
}

/* ---- tests ------------------------------------------------------------- */

static void test_empty_list(void)
{
    g_current = "empty_list";
    reset_store();
    char* r = call("connections", NULL);
    CHECK(CONTAINS(r, "\"response\":\"success\""), "empty list is success");
    CHECK(CONTAINS(r, "\"connections\":[]"), "empty list has empty array");
    free(r);
}

static void test_add_defaults(void)
{
    g_current = "add_defaults";
    reset_store();
    /* No port/width/height/dpi given → defaults applied. */
    char* r = call("connections",
        "{\"action\":\"add\",\"name\":\"RDP box\",\"protocol\":\"rdp\","
        "\"hostname\":\"host.example\"}");
    CHECK(CONTAINS(r, "\"response\":\"success\""), "add succeeds");
    char id[64];
    extract_id(r, id, sizeof(id));
    CHECK(id[0] != '\0', "add returns a connection_id");
    free(r);

    char body[128];
    snprintf(body, sizeof(body), "{\"action\":\"get\",\"id\":\"%s\"}", id);
    char* g = call("connections", body);
    CHECK(CONTAINS(g, "\"port\":3389"), "rdp default port 3389");
    CHECK(CONTAINS(g, "\"width\":1024"), "default width 1024");
    CHECK(CONTAINS(g, "\"height\":768"), "default height 768");
    CHECK(CONTAINS(g, "\"dpi\":96"), "default dpi 96");
    CHECK(CONTAINS(g, "\"color_depth\":\"32\""), "default color depth 32");
    free(g);
}

static void test_protocol_default_ports(void)
{
    g_current = "protocol_default_ports";
    struct { const char* proto; const char* port; } cases[] = {
        { "vnc", "\"port\":5900" }, { "ssh", "\"port\":22" },
        { "telnet", "\"port\":23" }, { NULL, NULL }
    };
    for (int i = 0; cases[i].proto; i++) {
        reset_store();
        char body[256];
        snprintf(body, sizeof(body),
            "{\"action\":\"add\",\"name\":\"n\",\"protocol\":\"%s\",\"hostname\":\"h\"}",
            cases[i].proto);
        char* a = call("connections", body);
        char id[64]; extract_id(a, id, sizeof(id)); free(a);
        snprintf(body, sizeof(body), "{\"action\":\"get\",\"id\":\"%s\"}", id);
        char* g = call("connections", body);
        CHECK(CONTAINS(g, cases[i].port), cases[i].proto);
        free(g);
    }
}

static void test_required_fields_and_unsupported(void)
{
    g_current = "validation";
    reset_store();
    char* r1 = call("connections", "{\"action\":\"add\",\"protocol\":\"rdp\"}");
    CHECK(CONTAINS(r1, "\"response\":\"fail\""), "missing name/hostname rejected");
    free(r1);

    char* r2 = call("connections",
        "{\"action\":\"add\",\"name\":\"n\",\"protocol\":\"gopher\",\"hostname\":\"h\"}");
    CHECK(CONTAINS(r2, "\"response\":\"fail\""), "unsupported protocol rejected");
    free(r2);

    char* list = call("connections", NULL);
    CHECK(CONTAINS(list, "\"connections\":[]"), "no bad connection stored");
    free(list);
}

static void test_update(void)
{
    g_current = "update";
    reset_store();
    char* a = call("connections",
        "{\"action\":\"add\",\"name\":\"old\",\"protocol\":\"vnc\",\"hostname\":\"h1\"}");
    char id[64]; extract_id(a, id, sizeof(id)); free(a);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"action\":\"update\",\"id\":\"%s\",\"name\":\"new\",\"hostname\":\"h2\",\"port\":5901}", id);
    char* u = call("connections", body);
    CHECK(CONTAINS(u, "\"response\":\"success\""), "update succeeds");
    free(u);

    snprintf(body, sizeof(body), "{\"action\":\"get\",\"id\":\"%s\"}", id);
    char* g = call("connections", body);
    CHECK(CONTAINS(g, "\"name\":\"new\""), "name updated");
    CHECK(CONTAINS(g, "\"hostname\":\"h2\""), "hostname updated");
    CHECK(CONTAINS(g, "\"port\":5901"), "port updated");
    free(g);
}

static void test_delete_swap_remove(void)
{
    g_current = "delete";
    reset_store();
    char idA[64], idB[64];
    char* a = call("connections",
        "{\"action\":\"add\",\"name\":\"A\",\"protocol\":\"ssh\",\"hostname\":\"a\"}");
    extract_id(a, idA, sizeof(idA)); free(a);
    char* b = call("connections",
        "{\"action\":\"add\",\"name\":\"B\",\"protocol\":\"ssh\",\"hostname\":\"b\"}");
    extract_id(b, idB, sizeof(idB)); free(b);

    char body[128];
    snprintf(body, sizeof(body), "{\"action\":\"delete\",\"id\":\"%s\"}", idA);
    char* d = call("connections", body);
    CHECK(CONTAINS(d, "\"response\":\"success\""), "delete succeeds");
    free(d);

    char* list = call("connections", NULL);
    CHECK(!CONTAINS(list, "\"name\":\"A\""), "A removed");
    CHECK(CONTAINS(list, "\"name\":\"B\""), "B survives swap-remove");
    free(list);

    /* deleting a non-existent id fails cleanly */
    char* d2 = call("connections", "{\"action\":\"delete\",\"id\":\"deadbeef\"}");
    CHECK(CONTAINS(d2, "\"response\":\"fail\""), "delete unknown id fails");
    free(d2);
}

static void test_json_escaping(void)
{
    g_current = "escaping";
    reset_store();
    /* Password contains an embedded quote and backslash. */
    char* a = call("connections",
        "{\"action\":\"add\",\"name\":\"Quote \\\"Test\\\"\",\"protocol\":\"rdp\","
        "\"hostname\":\"h\",\"password\":\"p\\\"w\\\\d\"}");
    char id[64]; extract_id(a, id, sizeof(id)); free(a);

    char body[128];
    snprintf(body, sizeof(body), "{\"action\":\"get\",\"id\":\"%s\"}", id);
    char* g = call("connections", body);
    CHECK(CONTAINS(g, "Quote \\\"Test\\\""), "name quotes escaped in output");
    CHECK(CONTAINS(g, "p\\\"w\\\\d"), "password quote+backslash escaped");
    free(g);
}

static void test_persistence_roundtrip(void)
{
    g_current = "persistence_roundtrip";
    reset_store();
    char* a = call("connections",
        "{\"action\":\"add\",\"name\":\"Persist\",\"protocol\":\"rdp\","
        "\"hostname\":\"10.1.2.3\",\"port\":3390,\"username\":\"u\","
        "\"password\":\"secret\",\"width\":1920,\"height\":1080,\"dpi\":120,"
        "\"enable_audio\":true,\"disable_copy\":true}");
    char id[64]; extract_id(a, id, sizeof(id)); free(a);

    /* File must exist and hold the data. */
    FILE* f = fopen(g_store, "r");
    CHECK(f != NULL, "store file created on disk");
    if (f) fclose(f);

    /* Simulate a service restart: re-init reads from disk. */
    guac_mgmt_init();
    char* list = call("connections", NULL);
    CHECK(CONTAINS(list, "\"name\":\"Persist\""), "name survives restart");
    CHECK(CONTAINS(list, "\"hostname\":\"10.1.2.3\""), "hostname survives restart");
    CHECK(CONTAINS(list, "\"port\":3390"), "port survives restart");
    CHECK(CONTAINS(list, "\"width\":1920"), "width survives restart");
    CHECK(CONTAINS(list, "\"dpi\":120"), "dpi survives restart");
    CHECK(CONTAINS(list, "\"enable_audio\":true"), "bool true survives restart");
    CHECK(CONTAINS(list, "\"disable_copy\":true"), "bool true survives restart");
    CHECK(CONTAINS(list, "\"active\":false"), "active reset to false on load");
    free(list);

    /* Credentials survive too (checked via get, which includes password). */
    char body[128];
    snprintf(body, sizeof(body), "{\"action\":\"get\",\"id\":\"%s\"}", id);
    char* g = call("connections", body);
    CHECK(CONTAINS(g, "\"password\":\"secret\""), "password survives restart");
    free(g);
}

static void test_reload_action(void)
{
    g_current = "reload_action";
    reset_store();
    char* a = call("connections",
        "{\"action\":\"add\",\"name\":\"R\",\"protocol\":\"vnc\",\"hostname\":\"h\"}");
    free(a);

    char* rl = call("connections", "{\"action\":\"reload\"}");
    CHECK(CONTAINS(rl, "\"response\":\"success\""), "reload succeeds");
    CHECK(CONTAINS(rl, "\"connection_count\":1"), "reload reports 1 connection");
    free(rl);

    /* Reload must not duplicate. */
    char* list = call("connections", NULL);
    CHECK(count(list, "\"name\":\"R\"") == 1, "reload does not duplicate connections");
    free(list);
}

static void test_settings_report_storage(void)
{
    g_current = "settings";
    reset_store();
    char* s = call("settings", NULL);
    CHECK(CONTAINS(s, "\"persistent\":true"), "settings report persistent:true");
    CHECK(CONTAINS(s, "\"storage_path\":"), "settings report storage_path");
    CHECK(CONTAINS(s, g_store), "settings report the actual store path");
    free(s);
}

static void test_corrupt_file_tolerated(void)
{
    g_current = "corrupt_file";
    unlink(g_store);
    FILE* f = fopen(g_store, "w");
    assert(f);
    fputs("this is not valid json at all {{{ ]][", f);
    fclose(f);

    guac_mgmt_init();   /* must not crash */
    char* list = call("connections", NULL);
    CHECK(CONTAINS(list, "\"response\":\"success\""), "corrupt file does not break service");
    CHECK(CONTAINS(list, "\"connections\":[]"), "corrupt file yields empty list");
    free(list);
}

static void test_skip_incomplete_objects(void)
{
    g_current = "skip_incomplete";
    unlink(g_store);
    /* One valid object and one missing protocol+hostname → only 1 loaded. */
    FILE* f = fopen(g_store, "w");
    assert(f);
    fputs("{\"version\":1,\"connections\":[",  f);
    fputs("{\"id\":\"aaa\",\"name\":\"Good\",\"protocol\":\"rdp\",\"hostname\":\"h\",\"port\":3389},", f);
    fputs("{\"id\":\"bbb\",\"name\":\"NoHostNoProto\"}", f);
    fputs("]}", f);
    fclose(f);

    guac_mgmt_init();
    char* list = call("connections", NULL);
    CHECK(CONTAINS(list, "\"name\":\"Good\""), "valid object loaded");
    CHECK(!CONTAINS(list, "NoHostNoProto"), "object without protocol/hostname skipped");
    free(list);
}

static void test_remote_app(void)
{
    g_current = "remote_app";
    reset_store();
    char* a = call("connections",
        "{\"action\":\"add\",\"name\":\"App\",\"protocol\":\"rdp\",\"hostname\":\"h\","
        "\"remote_app\":\"notepad\",\"remote_app_args\":\"/x\",\"remote_app_dir\":\"C:\"}");
    char id[64]; extract_id(a, id, sizeof(id)); free(a);

    char body[128];
    snprintf(body, sizeof(body), "{\"action\":\"get\",\"id\":\"%s\"}", id);
    char* g = call("connections", body);
    CHECK(CONTAINS(g, "\"remote_app\":\"notepad\""), "remote_app stored");
    CHECK(CONTAINS(g, "\"remote_app_args\":\"/x\""), "remote_app_args stored");
    CHECK(CONTAINS(g, "\"remote_app_dir\":\"C:\""), "remote_app_dir stored");
    free(g);

    char* list = call("connections", NULL);
    CHECK(CONTAINS(list, "\"remote_app\":\"notepad\""), "remote_app in list");
    free(list);

    /* Survives a restart (persisted to .info). */
    guac_mgmt_init();
    char* g2 = call("connections", body);
    CHECK(CONTAINS(g2, "\"remote_app\":\"notepad\""), "remote_app survives restart");
    free(g2);
}

static void test_connect_reachable(void)
{
    g_current = "connect_reachable";
    reset_store();

    /* A live loopback listener makes the target genuinely reachable. */
    int port = 0;
    int lfd = open_listener(&port);
    CHECK(lfd >= 0, "opened loopback listener");

    char add[256];
    snprintf(add, sizeof(add),
        "{\"action\":\"add\",\"name\":\"C\",\"protocol\":\"vnc\","
        "\"hostname\":\"127.0.0.1\",\"port\":%d}", port);
    char* a = call("connections", add);
    char id[64]; extract_id(a, id, sizeof(id)); free(a);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"action\":\"connect\",\"id\":\"%s\",\"username\":\"u\",\"session_id\":\"web-1\"}", id);
    char* c = call("connection", body);
    CHECK(CONTAINS(c, "\"response\":\"success\""), "connect to reachable host succeeds");
    free(c);

    char* active = call("active", NULL);
    CHECK(CONTAINS(active, "\"protocol\":\"vnc\""), "session listed as active");
    free(active);

    if (lfd >= 0) close(lfd);

    /* last_used should now be persisted (non-zero) after restart. */
    guac_mgmt_init();
    snprintf(body, sizeof(body), "{\"action\":\"get\",\"id\":\"%s\"}", id);
    char* g = call("connections", body);
    CHECK(!CONTAINS(g, "\"last_used\":0"), "last_used persisted after connect");
    free(g);
}

static void test_connect_unreachable_fails(void)
{
    g_current = "connect_unreachable";
    reset_store();

    /* Bind a listener, capture its port, then close it so nothing is listening. */
    int port = 0;
    int lfd = open_listener(&port);
    if (lfd >= 0) close(lfd);

    char add[256];
    snprintf(add, sizeof(add),
        "{\"action\":\"add\",\"name\":\"Dead\",\"protocol\":\"vnc\","
        "\"hostname\":\"127.0.0.1\",\"port\":%d}", port);
    char* a = call("connections", add);
    char id[64]; extract_id(a, id, sizeof(id)); free(a);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"action\":\"connect\",\"id\":\"%s\",\"username\":\"u\",\"session_id\":\"web-x\"}", id);
    char* c = call("connection", body);
    CHECK(CONTAINS(c, "\"response\":\"fail\""), "connect to a dead port fails");
    free(c);

    /* A failed connect must not leave an active session behind. */
    char* active = call("active", NULL);
    CHECK(CONTAINS(active, "\"sessions\":[]"), "no session created for a failed connect");
    free(active);
}

static void test_connect_bad_hostname_fails(void)
{
    g_current = "connect_bad_hostname";
    reset_store();
    char* a = call("connections",
        "{\"action\":\"add\",\"name\":\"Bad\",\"protocol\":\"rdp\","
        "\"hostname\":\"no.such.host.invalid.\",\"port\":3389}");
    char id[64]; extract_id(a, id, sizeof(id)); free(a);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"action\":\"connect\",\"id\":\"%s\",\"username\":\"u\",\"session_id\":\"web-y\"}", id);
    char* c = call("connection", body);
    CHECK(CONTAINS(c, "\"response\":\"fail\""), "connect to an unresolvable host fails");
    free(c);
}

int main(void)
{
    /* Isolate the store in a temp path for the whole run. */
    const char* tmp = getenv("TMPDIR");
    snprintf(g_store, sizeof(g_store), "%s/guac_test_%d.info",
             (tmp && tmp[0]) ? tmp : "/tmp", (int)getpid());
    setenv("KIN_GUACAMOLE_STATE", g_store, 1);

    printf("Running management/persistence tests (store=%s)\n", g_store);

    test_empty_list();
    test_add_defaults();
    test_protocol_default_ports();
    test_required_fields_and_unsupported();
    test_update();
    test_delete_swap_remove();
    test_json_escaping();
    test_persistence_roundtrip();
    test_reload_action();
    test_settings_report_storage();
    test_corrupt_file_tolerated();
    test_skip_incomplete_objects();
    test_remote_app();
    test_connect_reachable();
    test_connect_unreachable_fails();
    test_connect_bad_hostname_fails();

    unlink(g_store);
    guac_mgmt_cleanup();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("TESTS FAILED\n");
    return 1;
}
