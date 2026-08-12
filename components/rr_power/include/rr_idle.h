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

/**
 * Register the predicate that decides whether the watch face may be drawn AT
 * ALL. It must answer "is this watch genuinely paired, with a child record" —
 * NOT "is there anything in the cache".
 *
 * Without it rr_idle renders the face on boot and on every wake regardless of
 * state, which is what painted the face over the pairing QR ~1 s into a
 * factory-reset boot. When the gate is false, rr_idle leaves whatever screen
 * the caller put up (the QR) alone.
 *
 * Set this BEFORE rr_idle_init(). Absent, the gate is closed and no face is
 * ever drawn — the safe default: a watch stuck on the QR is recoverable, a
 * reset watch showing a stranger's face is not.
 *
 * rr_idle also POLLS this to notice pairing: the gate going false->true is the
 * pairing event, and rr_idle promotes the "Paired" confirmation to the live
 * face when it does. It is polled rather than pushed from rr_ble because
 * rr_power already depends on rr_ble.
 */
void rr_idle_set_face_gate(bool (*fn)(void));

/**
 * The queue depth changed — refresh the face's "N to upload" badge.
 *
 * Deliberately only SETS A FLAG. This is called from whichever task changed the
 * queue, which for an ack is the NimBLE host task, and rebuilding the face
 * streams a 280px avatar off littlefs — doing that inline would stall the BLE
 * host for the duration of a flash read. idle_tick picks the flag up on its next
 * 500 ms pass instead, which is immediate to a human and costs the host nothing.
 *
 * No-op unless the face is actually the screen on display: there is nothing to
 * refresh behind a routine step screen or a dark panel.
 */
void rr_idle_notify_queue_changed(void);
