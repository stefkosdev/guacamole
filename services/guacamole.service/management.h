#ifndef GUAC_MANAGEMENT_H
#define GUAC_MANAGEMENT_H

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    char id[64];
    char name[256];
    char protocol[32];
    char hostname[256];
    int port;
    char username[128];
    char password[256];
    char private_key[1024];
    char domain[128];
    char security[32];
    char color_depth[8];
    int enable_audio;
    int enable_video;
    int enable_printing;
    int enable_file_transfer;
    int enable_wallpaper;
    int enable_theming;
    int enable_font_smoothing;
    int enable_full_window_drag;
    int enable_menu_animation;
    int disable_copy;
    int disable_paste;
    int width;
    int height;
    int dpi;
    int active;
    long created;
    long last_used;
} GuacConnection;

typedef struct {
    char id[64];
    char connection_id[64];
    char username[256];
    char session_id[256];
    char protocol[32];
    char hostname[256];
    int port;
    long started;
    long last_active;
    int active;
} GuacSession;

#define MAX_CONNECTIONS 256
#define MAX_SESSIONS 256

int guac_mgmt_init(void);
void guac_mgmt_cleanup(void);

const char* guac_mgmt_handle(const char* command, const char* body, size_t* out_len);

GuacConnection* guac_mgmt_find_connection(const char* id);
int guac_mgmt_mark_session_start(const char* conn_id, const char* session_id);
int guac_mgmt_mark_session_end(const char* conn_id);

#endif
