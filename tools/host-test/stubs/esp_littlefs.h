#pragma once
#include "esp_err.h"
typedef struct { const char *base_path; const char *partition_label; int format_if_mount_failed; int dont_mount; } esp_vfs_littlefs_conf_t;
static inline esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *c){(void)c;return 0;}
static inline esp_err_t esp_littlefs_info(const char *l, size_t *t, size_t *u){(void)l;*t=0;*u=0;return 0;}
static inline esp_err_t esp_littlefs_format(const char *l){(void)l;return 0;}
