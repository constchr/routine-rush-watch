#pragma once
// rr_idle — display sleep/wake + the idle watch face (§9B.2, §10).
#include <stdbool.h>
#include "esp_err.h"

/** Show the face and start the idle/sleep cycle. Call after rr_imu_init(). */
esp_err_t rr_idle_init(void);

/** Called from the IMU interrupt task on a wrist raise. */
void rr_idle_notify_wake(void);

/** Any interaction — resets the sleep timeout. */
void rr_idle_notify_activity(void);

/** Manual wake (BOOT tap — the PWR fallback of §9B.2). */
void rr_idle_wake_manual(void);

bool rr_idle_is_awake(void);

/**
 * Register a predicate that suspends idle-sleep while it returns true.
 * Used so a running routine is never blanked mid-step — a child reading a
 * countdown is interacting even when they are not tapping.
 */
void rr_idle_set_suspend_check(bool (*fn)(void));
bool rr_idle_is_suspended(void);
