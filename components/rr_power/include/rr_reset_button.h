#pragma once
// Local factory reset: hold BOOT (GPIO 9) for 10 s. See rr_reset_button.c for
// why this is the only recovery for a watch unlinked while out of range.
#include "esp_err.h"

esp_err_t rr_reset_button_init(void);
