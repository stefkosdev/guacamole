#include <kin/polykernel_module.h>

#include <stdio.h>
#include <string.h>

static void json_string(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; in && in[i] && w + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[w++] = '\\'; out[w++] = (char)c; }
        else if (c >= 0x20) out[w++] = (char)c;
    }
    out[w] = 0;
}

static kin_polykernel_handler_result_v1 handle_guacamole(
    const kin_polykernel_host_v1 *host, const kin_polykernel_request_v1 *request)
{
    const char *command = request->path + strlen("api/guacamole/");
    if (!command[0] || strchr(command, '|')) return KIN_MODULE_HANDLER_REJECTED;
    char payload[530000];
    if (strcmp(command, "tunnel-ticket") == 0) {
        char user[768];
        json_string(request->username, user, sizeof(user));
        if (snprintf(payload, sizeof(payload),
                     "tunnel-ticket|{\"username\":\"%s\",\"request\":%s}",
                     user, request->json) >= (int)sizeof(payload))
            return KIN_MODULE_HANDLER_REJECTED;
    } else if (snprintf(payload, sizeof(payload), "%s|%s", command, request->json) >= (int)sizeof(payload))
        return KIN_MODULE_HANDLER_REJECTED;

    if (host->forward_ipc(request->response_context, "guacamole", payload) != 0) {
        host->respond_json(request->response_context,
            "{\"response\":\"fail\",\"message\":\"Guacamole service unavailable\"}");
        return KIN_MODULE_HANDLER_RESPONDED;
    }
    return KIN_MODULE_HANDLER_ASYNC;
}

static const kin_polykernel_route_v1 routes[] = {
    { "api/guacamole/" }
};
static const kin_polykernel_manual_page_v1 manual_pages[] = {
    { "Guacamole Remote Desktop", "/repository/Applications/Administration/kin_guacamole_admin/docs/manual.md" }
};

__attribute__((visibility("default")))
const kin_polykernel_module_descriptor_v1 kin_polykernel_module_v1 = {
    KIN_POLYKERNEL_MODULE_ABI_V1,
    sizeof(kin_polykernel_module_descriptor_v1),
    "guacamole", "Guacamole Remote Desktop", "1.1.0",
    routes, sizeof(routes) / sizeof(routes[0]),
    manual_pages, sizeof(manual_pages) / sizeof(manual_pages[0]),
    handle_guacamole
};
