// rr_ble — RR_SYNC GATT server (NimBLE, peripheral role).
//
// CONTRACT v2: TIME_SYNC + ROUTINE_PUSH + RR_CONTROL. QUEUE_STATUS /
// QUEUE_PULL / RUN_ACK belong to the completion queue and land in Phase 5.
//
// v2 moved command traffic (nonce_auth, factory_reset) off the ROUTINE_PUSH
// envelope onto RR_CONTROL. ROUTINE_PUSH is once again purely routine data.
//
// Every UUID and byte layout comes from the generated ble_contract.h. Nothing
// in this file hand-rolls a wire format.

#include "rr_ble.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cJSON.h"

#include "ble_contract.h"
#include "rr_rtc.h"
#include "rr_identity.h"
#include "rr_store.h"
#include "rr_ui.h"

static const char *TAG = "rr_ble";

#define RR_BLE_DEVICE_NAME "RoutineRush Watch"

static uint8_t s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// ─────────────────────────────────────────────────────────────────────────────
// UUID byte order.
//
// ble_contract.h emits UUIDs in CANONICAL order — the order you read the hex
// string, left to right. NimBLE's ble_uuid128_t stores the 128-bit value in
// LITTLE-ENDIAN (on-air) order, i.e. reversed.
//
// Rather than duplicate reversed literals here — which would silently rot the
// moment the contract changes — we reverse the generated arrays at init. The
// generated header stays the single source of truth.
// ─────────────────────────────────────────────────────────────────────────────
static ble_uuid128_t s_svc_uuid;
static ble_uuid128_t s_time_sync_uuid;
static ble_uuid128_t s_routine_push_uuid;
static ble_uuid128_t s_control_uuid;
static ble_uuid128_t s_queue_status_uuid;
static ble_uuid128_t s_queue_pull_uuid;
static ble_uuid128_t s_run_ack_uuid;
static uint16_t s_queue_status_handle;

// ROUTINE_PUSH reassembly. The contract sends [u32 len][JSON] chunked across
// MTU-sized writes; we accumulate until 4+len bytes have arrived.
//
// The cap is a denial-of-service guard, not a protocol limit: a peer that has
// bonded (which Just Works lets anyone do) could otherwise declare a 4 GB
// length and exhaust the ~180 KiB we have. Real routine sets are a few KB.
#define RR_ROUTINE_PUSH_MAX 32768
#define RR_CONTROL_MAX 1024
#define RR_QUEUE_PULL_MAX 4096
#define RR_FW_VERSION 5      /**< packed firmware version reported in QUEUE_STATUS */

static uint8_t *s_rx_buf;
static size_t   s_rx_len;
static size_t   s_rx_cap;

// Separate reassembly for RR_CONTROL: commands and routine pushes can be in
// flight on the same connection, and mixing them into one buffer would splice
// a command into the middle of a routine document.
static uint8_t *s_ctl_buf;
static size_t   s_ctl_len;
static size_t   s_ctl_cap;

// ── Per-connection authorisation ─────────────────────────────────────────────
// Set by a successful RR_CONTROL nonce_auth, and CLEARED on both connect and
// disconnect. Binding it to the conn_handle matters: NimBLE can hold more than
// one connection, and a flag that outlived its connection would hand a later
// (possibly different) peer the previous peer's privileges.
static uint16_t s_authed_conn = BLE_HS_CONN_HANDLE_NONE;

static void ctl_reset(void)
{
    free(s_ctl_buf);
    s_ctl_buf = NULL;
    s_ctl_len = 0;
    s_ctl_cap = 0;
}

static void rx_reset(void)
{
    free(s_rx_buf);
    s_rx_buf = NULL;
    s_rx_len = 0;
    s_rx_cap = 0;
}

static void uuid128_from_canonical(ble_uuid128_t *out, const uint8_t canonical[16])
{
    out->u.type = BLE_UUID_TYPE_128;
    for (int i = 0; i < 16; i++) {
        out->value[i] = canonical[15 - i];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TIME_SYNC write handler
// ─────────────────────────────────────────────────────────────────────────────
static int time_sync_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) conn_handle; (void) attr_handle; (void) arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != RR_TIME_SYNC_SIZE) {
        ESP_LOGW(TAG, "TIME_SYNC: expected %d bytes, got %u — rejecting",
                 RR_TIME_SYNC_SIZE, (unsigned) len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    rr_time_sync_t msg;
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &msg, sizeof(msg), &copied);
    if (rc != 0 || copied != RR_TIME_SYNC_SIZE) {
        ESP_LOGE(TAG, "TIME_SYNC: mbuf flatten failed (rc=%d, copied=%u)", rc, (unsigned) copied);
        return BLE_ATT_ERR_UNLIKELY;
    }

    // epoch_utc is little-endian per the contract; the C6 is little-endian, so
    // the packed struct field is already correct with no byte swapping.
    uint32_t epoch = msg.epoch_utc;

    rr_rtc_time_t before, after;
    char sbefore[24] = "<unreadable>", safter[24] = "<unreadable>";
    if (rr_rtc_get(&before) == ESP_OK) rr_rtc_format(&before, sbefore, sizeof(sbefore));

    ESP_LOGI(TAG, "TIME_SYNC received: epoch=%" PRIu32, epoch);
    ESP_LOGI(TAG, "  RTC before: %s (osc_ok=%d)", sbefore, (int) before.osc_ok);

    esp_err_t err = rr_rtc_set_epoch(epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  RTC set FAILED: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (rr_rtc_get(&after) == ESP_OK) rr_rtc_format(&after, safter, sizeof(safter));
    ESP_LOGI(TAG, "  RTC after:  %s (osc_ok=%d)  <<< TIME SYNCED", safter, (int) after.osc_ok);

    return 0;
}



// ─────────────────────────────────────────────────────────────────────────────
// Factory reset — the mirror of pairing.
//
// Wipes everything that makes this watch "someone's watch": the paired flag,
// the paired-peer anchor, the device_id, the BLE bonds, the routine cache, the
// cached child, and the completion queue. Then reboots, because a fresh
// device_id must be re-derived into a fresh BLE address and the QR path
// re-entered from a clean state — far safer than trying to unwind live
// LVGL/NimBLE state in place.
//
// Store wipe FIRST, identity second: if power is lost mid-reset, a watch with
// orphaned files but no paired flag re-pairs cleanly (the next push overwrites
// them), whereas a watch that kept its files but lost its identity would boot
// paired-looking with another child's data. Fail toward the QR.
// ─────────────────────────────────────────────────────────────────────────────
void rr_ble_factory_reset(const char *reason)
{
    ESP_LOGW(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGW(TAG, "║ FACTORY RESET — %s", reason);
    ESP_LOGW(TAG, "╚══════════════════════════════════════════════════");

    rr_store_factory_reset();
    rr_identity_factory_reset();

    // Drop every bond, so the phone that unlinked us cannot silently reconnect
    // to what is now a different device.
    int rc = ble_store_clear();
    ESP_LOGW(TAG, "bond store cleared (rc=%d)", rc);

    ESP_LOGW(TAG, "rebooting into first-boot state...");
    vTaskDelay(pdMS_TO_TICKS(400));   // let the log drain over USB-JTAG
    esp_restart();
}

// Is this connection both encrypted AND from the peer that paired us?
static bool peer_is_trusted(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;

    if (!desc.sec_state.encrypted) {
        ESP_LOGW(TAG, "reset refused: link is not encrypted");
        return false;
    }
    if (!rr_identity_is_paired_peer(desc.peer_id_addr.val, desc.peer_id_addr.type)) {
        ESP_LOGW(TAG, "reset refused: peer %02x:%02x:%02x:%02x:%02x:%02x is bonded "
                      "but is NOT the peer that paired this watch",
                 desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
                 desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
        return false;
    }
    return true;
}



static bool conn_is_authorised(uint16_t conn_handle);   // defined below

// ═════════════════════════════════════════════════════════════════════════════
// Phase 5 — the completion queue characteristics (frozen contract v2, §6B.3)
//
// All three are gated on the same authorisation as ROUTINE_PUSH: a queued run
// is a record of a child's day, so it does not go to an arbitrary bonded
// central.
// ═════════════════════════════════════════════════════════════════════════════

// QUEUE_STATUS — read | notify — 9 bytes LE (see rr_queue_status_t).
static int queue_status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) attr_handle; (void) arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (!conn_is_authorised(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    rr_queue_status_t st = {
        .queued_count = (uint16_t) rr_queue_count(),
        .oldest_ts = rr_queue_oldest_ts(),
        .fw_version = RR_FW_VERSION,
        .batt = 0,   // AXP2101 wiring is Phase 10; 0 is honest, not a guess
    };
    ESP_LOGI(TAG, "QUEUE_STATUS read: %u queued, oldest_ts=%" PRIu32,
             (unsigned) st.queued_count, st.oldest_ts);

    return os_mbuf_append(ctxt->om, &st, sizeof(st)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// QUEUE_PULL — read (multi-packet) — a stream of [u16 len][JSON] frames.
//
// NimBLE appends the WHOLE value here and handles ATT Read Blob fragmentation
// itself, so this callback runs once per logical read regardless of MTU.
static int queue_pull_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) attr_handle; (void) arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (!conn_is_authorised(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    int need = rr_queue_read_unacked(NULL, 0);
    if (need <= 0) {
        ESP_LOGI(TAG, "QUEUE_PULL: queue empty");
        return 0;   // zero-length value == nothing queued
    }
    if (need > RR_QUEUE_PULL_MAX) {
        ESP_LOGW(TAG, "QUEUE_PULL: %d bytes queued, capping the read at %d",
                 need, RR_QUEUE_PULL_MAX);
        need = RR_QUEUE_PULL_MAX;
    }

    char *ndjson = malloc((size_t) need + 1);
    if (ndjson == NULL) return BLE_ATT_ERR_INSUFFICIENT_RES;
    int got = rr_queue_read_unacked(ndjson, (size_t) need);
    if (got <= 0) { free(ndjson); return 0; }
    ndjson[got] = '\0';

    // The queue is stored NDJSON but the contract frames each record as
    // [u16 len][json] (§6B.3 QUEUE_PULL), so convert line-by-line here rather
    // than changing the on-flash format — the log stays greppable and
    // append-only, and the wire stays exactly what the phone decodes.
    int rc = 0, frames = 0;
    char *line = ndjson;
    while (line != NULL && *line != '\0') {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t) (nl - line) : strlen(line);
        if (len > 0 && len <= 0xFFFF) {
            uint16_t l16 = (uint16_t) len;
            if (os_mbuf_append(ctxt->om, &l16, sizeof(l16)) != 0 ||
                os_mbuf_append(ctxt->om, line, len) != 0) {
                rc = BLE_ATT_ERR_INSUFFICIENT_RES;
                break;
            }
            frames++;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(ndjson);

    ESP_LOGI(TAG, "QUEUE_PULL: streamed %d frame(s), %d bytes", frames, got);
    return rc;
}

// RUN_ACK — write — 38 bytes LE (see rr_run_ack_t).
static int run_ack_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) attr_handle; (void) arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (!conn_is_authorised(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    if (OS_MBUF_PKTLEN(ctxt->om) != RR_RUN_ACK_SIZE) {
        ESP_LOGW(TAG, "RUN_ACK: expected %d bytes, got %u",
                 RR_RUN_ACK_SIZE, (unsigned) OS_MBUF_PKTLEN(ctxt->om));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    rr_run_ack_t ack;
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, &ack, sizeof(ack), &copied) != 0 || copied != sizeof(ack)) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    // local_id is 16 raw bytes in CANONICAL order (§6B.3) — render it back to
    // the string form the queue stores.
    char local_id[37];
    const uint8_t *b = ack.local_id;
    snprintf(local_id, sizeof(local_id),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);

    ESP_LOGI(TAG, "RUN_ACK: local_id=%s xp=%" PRIu32 " streak=%u",
             local_id, ack.authoritative_xp, (unsigned) ack.authoritative_streak);

    esp_err_t err = rr_queue_ack(local_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RUN_ACK: queue ack failed: %s", esp_err_to_name(err));
    }
    rr_ble_notify_queue_status();
    return 0;
}

void rr_ble_notify_queue_status(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_queue_status_handle == 0) return;

    rr_queue_status_t st = {
        .queued_count = (uint16_t) rr_queue_count(),
        .oldest_ts = rr_queue_oldest_ts(),
        .fw_version = RR_FW_VERSION,
        .batt = 0,
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&st, sizeof(st));
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn_handle, s_queue_status_handle, om);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Authorisation model (contract v2)
//
// A connection may issue privileged writes if EITHER:
//   (a) it presented the correct pairing nonce via RR_CONTROL nonce_auth —
//       the bootstrap path, valid only while the QR is on screen; or
//   (b) its peer identity matches the peer recorded when this watch paired —
//       the steady-state path.
//
// (b) is not a convenience. Without it, a paired watch could never receive a
// routine UPDATE: the nonce only exists while the QR is displayed, and a
// paired watch does not display one. v1 had exactly that hole — every push
// after pairing would have been rejected.
//
// Encryption is a precondition for both: every characteristic is WRITE_ENC.
// It is necessary but NOT sufficient, because Just Works lets any central bond.
// ─────────────────────────────────────────────────────────────────────────────
static bool conn_is_authorised(uint16_t conn_handle)
{
    if (s_authed_conn == conn_handle) return true;

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;
    if (!desc.sec_state.encrypted) return false;

    return rr_identity_is_paired_peer(desc.peer_id_addr.val, desc.peer_id_addr.type);
}

// ─────────────────────────────────────────────────────────────────────────────
// RR_CONTROL write handler (contract v2) — the command channel.
//
// Envelope: [u32 len][UTF-8 JSON]  where JSON = { "cmd": "...", ... }
// Unknown commands are rejected individually so a newer phone talking to an
// older watch degrades one command at a time rather than wedging the link.
// ─────────────────────────────────────────────────────────────────────────────
static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) attr_handle; (void) arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    uint16_t chunk_len = OS_MBUF_PKTLEN(ctxt->om);
    if (chunk_len == 0) return 0;

    size_t need = s_ctl_len + chunk_len;
    if (need > RR_CONTROL_MAX) {
        ESP_LOGE(TAG, "RR_CONTROL: %u bytes exceeds the %d-byte cap — dropping",
                 (unsigned) need, RR_CONTROL_MAX);
        ctl_reset();
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (need > s_ctl_cap) {
        size_t newcap = s_ctl_cap ? s_ctl_cap * 2 : 256;
        while (newcap < need) newcap *= 2;
        uint8_t *grown = realloc(s_ctl_buf, newcap);
        if (grown == NULL) { ctl_reset(); return BLE_ATT_ERR_INSUFFICIENT_RES; }
        s_ctl_buf = grown;
        s_ctl_cap = newcap;
    }

    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, s_ctl_buf + s_ctl_len, chunk_len, &copied) != 0) {
        ctl_reset();
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_ctl_len += copied;

    if (s_ctl_len < RR_CONTROL_LEN_PREFIX_BYTES) return 0;

    uint32_t declared;
    memcpy(&declared, s_ctl_buf, sizeof(declared));
    size_t total = RR_CONTROL_LEN_PREFIX_BYTES + (size_t) declared;
    if (declared > RR_CONTROL_MAX) { ctl_reset(); return BLE_ATT_ERR_INSUFFICIENT_RES; }
    if (s_ctl_len < total) return 0;   // more packets inbound

    cJSON *env = cJSON_ParseWithLength((const char *) (s_ctl_buf + RR_CONTROL_LEN_PREFIX_BYTES), declared);
    ctl_reset();
    if (env == NULL) {
        ESP_LOGE(TAG, "RR_CONTROL: payload is not valid JSON");
        return BLE_ATT_ERR_UNLIKELY;
    }

    const cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(env, "cmd");
    if (!cJSON_IsString(jcmd)) {
        ESP_LOGE(TAG, "RR_CONTROL: envelope has no \"cmd\" field");
        cJSON_Delete(env);
        return BLE_ATT_ERR_UNLIKELY;
    }
    const char *cmd = jcmd->valuestring;
    ESP_LOGI(TAG, "RR_CONTROL: cmd=%s", cmd);

    // ── nonce_auth: bootstrap authorisation from the QR ──────────────────────
    if (strcmp(cmd, "nonce_auth") == 0) {
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(env, "nonce");
        bool ok = cJSON_IsString(jn) && rr_identity_check_nonce(jn->valuestring);
        if (!ok) {
            ESP_LOGW(TAG, "╔══════════════════════════════════════════════════");
            ESP_LOGW(TAG, "║ nonce_auth REJECTED");
            ESP_LOGW(TAG, "║ presented: %s", cJSON_IsString(jn) ? jn->valuestring : "(absent)");
            ESP_LOGW(TAG, "║ The peer is bonded but did not see the QR.");
            ESP_LOGW(TAG, "╚══════════════════════════════════════════════════");
            cJSON_Delete(env);
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        cJSON_Delete(env);

        s_authed_conn = conn_handle;
        ESP_LOGI(TAG, "nonce OK — connection %u authorised", (unsigned) conn_handle);

        // Anchor the trust relationship: this peer proved it saw the QR, so it
        // is the one allowed to send destructive commands later, and the one
        // allowed to push routine updates once the QR is gone.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(conn_handle, &desc) == 0) {
            rr_identity_set_paired_peer(desc.peer_id_addr.val, desc.peer_id_addr.type);
        }
        if (!rr_identity_is_paired()) rr_identity_set_paired(true);
        return 0;
    }

    // ── factory_reset: destructive, paired-peer only ─────────────────────────
    if (strcmp(cmd, "factory_reset") == 0) {
        cJSON_Delete(env);
        // Deliberately NOT gated on nonce_auth: after an unlink the watch shows
        // no QR, so there is no nonce to present. The gate is the paired-peer
        // identity, which survives the QR going away.
        if (!peer_is_trusted(conn_handle)) {
            ESP_LOGW(TAG, "FACTORY RESET REJECTED — untrusted peer");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        rr_ble_factory_reset("unlinked by the paired phone over BLE");
        return 0;   // not reached — esp_restart()
    }

    ESP_LOGW(TAG, "RR_CONTROL: unknown cmd \"%s\" — rejecting this command only", cmd);
    cJSON_Delete(env);
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

// ─────────────────────────────────────────────────────────────────────────────
// ROUTINE_PUSH write handler — reassembly + NONCE GATE
//
// v2: PURE ROUTINE DATA. The payload is the routines array itself —
//     [ { "assignment_id": ..., "steps": [...], "schedules": [...] }, ... ]
// No nonce, no command; those moved to RR_CONTROL.
//
// ⚠️ AUTHORISATION IS NOT THE BOND. Phase 2 proved on hardware that pairing
// here is Just Works: a Mac bonded to this watch unprompted, with no
// confirmation on either device. "Bonded" means the link is encrypted, NOT
// that the peer is the parent's phone.
//
// So this handler refuses to reassemble anything until conn_is_authorised()
// — the connection either presented the QR nonce over RR_CONTROL, or is the
// peer this watch recorded when it paired. An unauthorised push is rejected
// before a single byte is buffered, and the routine cache is left untouched.
// ─────────────────────────────────────────────────────────────────────────────
static int routine_push_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) conn_handle; (void) attr_handle; (void) arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    if (!conn_is_authorised(conn_handle)) {
        ESP_LOGW(TAG, "╔══════════════════════════════════════════════════");
        ESP_LOGW(TAG, "║ ROUTINE_PUSH REJECTED — connection not authorised");
        ESP_LOGW(TAG, "║ Send RR_CONTROL nonce_auth first, or connect as the");
        ESP_LOGW(TAG, "║ peer that paired this watch. Cache left untouched.");
        ESP_LOGW(TAG, "╚══════════════════════════════════════════════════");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    uint16_t chunk_len = OS_MBUF_PKTLEN(ctxt->om);
    if (chunk_len == 0) return 0;

    // Grow the reassembly buffer.
    size_t need = s_rx_len + chunk_len;
    if (need > RR_ROUTINE_PUSH_MAX) {
        ESP_LOGE(TAG, "ROUTINE_PUSH: %u bytes exceeds the %d-byte cap — dropping",
                 (unsigned) need, RR_ROUTINE_PUSH_MAX);
        rx_reset();
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (need > s_rx_cap) {
        size_t newcap = s_rx_cap ? s_rx_cap * 2 : 512;
        while (newcap < need) newcap *= 2;
        uint8_t *grown = realloc(s_rx_buf, newcap);
        if (grown == NULL) {
            ESP_LOGE(TAG, "ROUTINE_PUSH: out of memory growing to %u bytes", (unsigned) newcap);
            rx_reset();
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        s_rx_buf = grown;
        s_rx_cap = newcap;
    }

    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf + s_rx_len, chunk_len, &copied);
    if (rc != 0) {
        ESP_LOGE(TAG, "ROUTINE_PUSH: mbuf flatten failed (rc=%d)", rc);
        rx_reset();
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_rx_len += copied;

    // Need the u32 length prefix before we can know how much is coming.
    if (s_rx_len < RR_ROUTINE_PUSH_LEN_PREFIX_BYTES) return 0;

    uint32_t declared;
    memcpy(&declared, s_rx_buf, sizeof(declared));   // little-endian per contract
    size_t total = RR_ROUTINE_PUSH_LEN_PREFIX_BYTES + (size_t) declared;

    if (declared > RR_ROUTINE_PUSH_MAX) {
        ESP_LOGE(TAG, "ROUTINE_PUSH: declared length %" PRIu32 " exceeds cap — dropping", declared);
        rx_reset();
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (s_rx_len < total) {
        ESP_LOGI(TAG, "ROUTINE_PUSH: %u/%u bytes reassembled", (unsigned) s_rx_len, (unsigned) total);
        return 0;   // more packets inbound
    }

    ESP_LOGI(TAG, "ROUTINE_PUSH: complete — %" PRIu32 " byte payload", declared);

    const char *json = (const char *) (s_rx_buf + RR_ROUTINE_PUSH_LEN_PREFIX_BYTES);
    cJSON *env = cJSON_ParseWithLength(json, declared);
    if (env == NULL) {
        ESP_LOGE(TAG, "ROUTINE_PUSH: payload is not valid JSON — rejecting");
        rx_reset();
        return BLE_ATT_ERR_UNLIKELY;
    }

    // v2: PURE routine DATA — never a command. Two data shapes are accepted:
    //   [ ...routines ]                — the original document
    //   { child: {...}, routines: [] } — the same, plus the child it belongs
    //                                    to (§5 caches child.json too)
    // Both are data, so this does not reopen the v2 problem of commands riding
    // a data characteristic. rr_store does the real validation and splits the
    // document into its two cache files.
    if (!cJSON_IsArray(env) && !cJSON_IsObject(env)) {
        ESP_LOGE(TAG, "ROUTINE_PUSH: payload must be a routines array or {child,routines} — rejecting");
        cJSON_Delete(env);
        rx_reset();
        return BLE_ATT_ERR_UNLIKELY;
    }

    char *routines_json = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    rx_reset();

    if (routines_json == NULL) {
        ESP_LOGE(TAG, "ROUTINE_PUSH: could not re-serialise routines");
        return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t err = rr_store_put_routines(routines_json, strlen(routines_json));
    free(routines_json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ROUTINE_PUSH: caching failed: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    // Prove the round trip by reading it straight back off flash.
    rr_store_log_routines();

    // First successful authenticated push == paired. Persist so the QR does
    // not reappear on reboot, and leave the pairing screen.
    if (!rr_identity_is_paired()) {
        rr_identity_set_paired(true);
    }
    rr_ui_show_paired_status();

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// GATT service definition.
//
// PHASE 3: TIME_SYNC + ROUTINE_PUSH. QUEUE_STATUS / QUEUE_PULL / RUN_ACK are
// still omitted rather than stubbed — advertising characteristics that do
// nothing is worse for debugging than their honest absence.
//
// ── RR_CONTROL: the command channel (contract v2) ────────────────────────────
// Until v2 the pairing nonce and factory_reset rode the ROUTINE_PUSH envelope,
// because the contract was frozen at five characteristics. That made a
// data-push characteristic into a command channel, and every new command made
// it less honest. v2 is a DELIBERATE, coordinated amendment — the freeze
// exists to prevent silent drift, not evolution — adding RR_CONTROL as the
// sixth characteristic and returning ROUTINE_PUSH to pure routine data.
// Both sides regenerated from watchProtocol.ts; the CI drift-guard re-run.
// ─────────────────────────────────────────────────────────────────────────────
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_time_sync_uuid.u,
                .access_cb = time_sync_access_cb,
                // Write WITH response — the phone uses
                // writeCharacteristicWithResponseForDevice, so it waits for our
                // ATT ack and surfaces a real error if we reject the value.
                //
                // PHASE 2: _ENC makes the bond LOAD-BEARING. An unbonded peer
                // now gets ATT error 0x05 (Insufficient Authentication) and the
                // central must pair before the write is accepted. Phase 1 left
                // this off deliberately so a bonding failure could not
                // masquerade as a transport failure; the transport is proven,
                // so the guard belongs here now.
                //
                // Every characteristic added from here on gets _ENC too — the
                // watch carries a child's routine data and must not hand it to
                // an arbitrary central.
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_routine_push_uuid.u,
                .access_cb = routine_push_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                // RR_CONTROL (contract v2) — the command channel.
                // Write-only: the ATT write response already carries success or
                // failure (0x05 on an auth failure), so a notify would add a
                // second, redundant path. Adding notify later is a property
                // change, not a new characteristic.
                .uuid = &s_control_uuid.u,
                .access_cb = control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_queue_status_uuid.u,
                .access_cb = queue_status_access_cb,
                .val_handle = &s_queue_status_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_queue_pull_uuid.u,
                .access_cb = queue_pull_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = &s_run_ack_uuid.u,
                .access_cb = run_ack_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            { 0 }
        },
    },
    { 0 }
};

// ─────────────────────────────────────────────────────────────────────────────
// Advertising
//
// The 128-bit service UUID must be in the ADVERTISING packet, not the scan
// response: the parent app scans filtered by service UUID
// (startDeviceScan([RR_SYNC_SERVICE_UUID], ...)), and iOS only matches that
// filter against adv-data service UUIDs.
//
// A 128-bit UUID costs 18 of the 31 adv bytes, and flags costs 3, so the name
// does not fit alongside it — it goes in the scan response instead.
// ─────────────────────────────────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (uint8_t *) RR_BLE_DEVICE_NAME;
    rsp.name_len = strlen(RR_BLE_DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // The GAP callback MUST be passed here — without it NimBLE delivers no
    // connect / disconnect / encryption-change events at all, so the watch
    // never re-advertises after a peer drops and bonding completion is
    // invisible. (Phase 1 passed NULL; a GATT write still worked, which is
    // why it went unnoticed until -Wunused-function flagged the dead handler.)
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc == BLE_HS_EALREADY) {
        // Benign: a failed connection raises both CONNECT(status!=0) and
        // DISCONNECT, and each re-advertises. Not an error worth logging red.
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\" — service %s", RR_BLE_DEVICE_NAME, RR_SYNC_SERVICE_UUID_STR);
}

// ─────────────────────────────────────────────────────────────────────────────
// GAP events
// ─────────────────────────────────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void) arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            // A new connection starts unauthorised, always.
            s_authed_conn = BLE_HS_CONN_HANDLE_NONE;
            ctl_reset();
            rx_reset();
            ESP_LOGI(TAG, "connected (handle %u)", (unsigned) s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed (status %d) — re-advertising", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d) — re-advertising", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        // Authorisation and any half-reassembled payload must not survive the
        // peer that established them, or the next connection would inherit a
        // stranger's privileges and partial data.
        s_authed_conn = BLE_HS_CONN_HANDLE_NONE;
        rx_reset();
        ctl_reset();
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        // Bonding completed (or failed). Phase 1 does not gate TIME_SYNC on
        // encryption, so this is informational — it tells us the bond path
        // works before Phase 2 starts depending on it.
        ESP_LOGI(TAG, "encryption change: status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr=%u notify=%d",
                 (unsigned) event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %u", (unsigned) event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The peer bonded before but we no longer hold its key (or vice versa).
        // Delete the stale bond and let pairing continue, otherwise the phone
        // is stuck until the user manually forgets the device.
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Host lifecycle
// ─────────────────────────────────────────────────────────────────────────────
// Derive a RANDOM STATIC BLE address from the device_id.
//
// Why not the factory public address: a factory reset regenerates device_id
// but would leave the public address unchanged, so every previously-bonded
// phone still sees the same BLE identity with different keys. iOS/macOS then
// refuse to reconnect ("Peer removed pairing information", CBError 14) and the
// only recovery is the user manually forgetting the device in system settings
// — which for a BLE-only peripheral is often not even offered in the UI.
// Observed for real against macOS during Phase 3 bring-up.
//
// Tying the address to device_id makes a factory reset produce a genuinely new
// BLE identity, which is what it should be. The address is STABLE across
// normal reboots because device_id is, so bonds and reconnection survive.
static void derive_static_addr(ble_addr_t *out)
{
    // FNV-1a over the device_id, then folded to 6 bytes.
    const char *id = rr_identity_device_id();
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = id; *p; p++) {
        h ^= (unsigned char) *p;
        h *= 1099511628211ULL;
    }
    for (int i = 0; i < 6; i++) {
        out->val[i] = (uint8_t) (h >> (i * 8));
    }
    // A random STATIC address requires the two most significant bits set.
    out->val[5] |= 0xC0;
    out->type = BLE_ADDR_RANDOM;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }

    ble_addr_t addr;
    derive_static_addr(&addr);
    rc = ble_hs_id_set_rnd(addr.val);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_rnd failed: %d — falling back to the public address", rc);
        rc = ble_hs_id_infer_auto(0, &s_addr_type);
        if (rc != 0) {
            ESP_LOGE(TAG, "infer_auto failed: %d", rc);
            return;
        }
    } else {
        s_addr_type = BLE_OWN_ADDR_RANDOM;
    }

    uint8_t cur[6] = { 0 };
    ble_hs_id_copy_addr(s_addr_type, cur, NULL);
    ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x (%s, derived from device_id)",
             cur[5], cur[4], cur[3], cur[2], cur[1], cur[0],
             s_addr_type == BLE_OWN_ADDR_RANDOM ? "random-static" : "public");

    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void) param;
    nimble_port_run();               // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// Provided by the NimBLE store/config module; not declared in a public header.
void ble_store_config_init(void);

esp_err_t rr_ble_init(void)
{
    // NVS is initialised by app_main, not here — it is shared with rr_identity
    // and must be up before either module runs. NimBLE's bond store needs it.

    // Build NimBLE-order UUIDs from the generated canonical bytes.
    static const uint8_t svc_canonical[16] = RR_SYNC_SERVICE_UUID_BYTES;
    static const uint8_t ts_canonical[16] = RR_SYNC_CHAR_TIME_SYNC_UUID_BYTES;
    static const uint8_t rp_canonical[16] = RR_SYNC_CHAR_ROUTINE_PUSH_UUID_BYTES;
    static const uint8_t ctl_canonical[16] = RR_SYNC_CHAR_RR_CONTROL_UUID_BYTES;
    static const uint8_t qs_canonical[16] = RR_SYNC_CHAR_QUEUE_STATUS_UUID_BYTES;
    static const uint8_t qp_canonical[16] = RR_SYNC_CHAR_QUEUE_PULL_UUID_BYTES;
    static const uint8_t ra_canonical[16] = RR_SYNC_CHAR_RUN_ACK_UUID_BYTES;
    uuid128_from_canonical(&s_svc_uuid, svc_canonical);
    uuid128_from_canonical(&s_time_sync_uuid, ts_canonical);
    uuid128_from_canonical(&s_routine_push_uuid, rp_canonical);
    uuid128_from_canonical(&s_control_uuid, ctl_canonical);
    uuid128_from_canonical(&s_queue_status_uuid, qs_canonical);
    uuid128_from_canonical(&s_queue_pull_uuid, qp_canonical);
    uuid128_from_canonical(&s_run_ack_uuid, ra_canonical);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Bonding with LE Secure Connections (§6B.2). The watch has no keyboard or
    // display it can use for a passkey during this phase, so Just Works — the
    // QR pairing moment in Phase 2 is what actually authenticates the link.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(RR_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "device_name_set failed: %d", rc);
    }

    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "NimBLE host started (peripheral: TIME_SYNC + ROUTINE_PUSH)");
    return ESP_OK;
}

bool rr_ble_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
