// rr_ble — RR_SYNC GATT server (NimBLE, peripheral role).
//
// PHASE 1 SCOPE: TIME_SYNC only. The other four characteristics of the frozen
// contract (QUEUE_STATUS / QUEUE_PULL / RUN_ACK / ROUTINE_PUSH) are
// deliberately NOT registered yet — see the note above s_gatt_svcs.
//
// Every UUID and byte layout comes from the generated ble_contract.h. Nothing
// in this file hand-rolls a wire format.

#include "rr_ble.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_contract.h"
#include "rr_rtc.h"

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
// GATT service definition.
//
// PHASE 1: only TIME_SYNC is registered. The other four characteristics exist
// in the frozen contract (§6B.3) and their UUIDs are already in
// ble_contract.h, but declaring them here without backing behaviour would make
// the phone's discovery report characteristics that silently do nothing —
// worse for debugging than their honest absence. They land in Phases 3/5.
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
                .flags = BLE_GATT_CHR_F_WRITE,
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

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
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
            ESP_LOGI(TAG, "connected (handle %u)", (unsigned) s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed (status %d) — re-advertising", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d) — re-advertising", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
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
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }

    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

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
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Build NimBLE-order UUIDs from the generated canonical bytes.
    static const uint8_t svc_canonical[16] = RR_SYNC_SERVICE_UUID_BYTES;
    static const uint8_t ts_canonical[16] = RR_SYNC_CHAR_TIME_SYNC_UUID_BYTES;
    uuid128_from_canonical(&s_svc_uuid, svc_canonical);
    uuid128_from_canonical(&s_time_sync_uuid, ts_canonical);

    err = nimble_port_init();
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

    ESP_LOGI(TAG, "NimBLE host started (peripheral, TIME_SYNC only)");
    return ESP_OK;
}

bool rr_ble_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
