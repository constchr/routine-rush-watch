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
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_random.h"
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
#include "rr_routine.h"
#include "rr_sched.h"
#include "rr_audio.h"
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
#define RR_FW_VERSION 6      /**< packed firmware version reported in QUEUE_STATUS */

// No connection-parameter constants: the watch deliberately accepts whatever the
// central chooses. Requesting a relaxed link made ROUTINE_PUSH 25x slower for a
// saving worth ~0.03 mAh/day — see the note in BLE_GAP_EVENT_ENC_CHANGE.

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
    ESP_LOGI(TAG, "  RTC after:  %s UTC (osc_ok=%d)  <<< TIME SYNCED", safter, (int) after.osc_ok);

    // The clock the scheduler measures everything against just moved, so its
    // computed wait is now wrong by however far it moved. Re-arm rather than
    // let it wake at the old offset and fire late (or early).
    rr_sched_rearm("TIME_SYNC");

    // Print the LOCAL time this implies, right next to the UTC one. The
    // timezone bug hid here: this handler reported success while the face
    // showed a time three hours off, and nothing in the log connected the two.
    // If no offset has arrived yet, say so — a silent absence is what made the
    // original failure invisible.
    if (rr_rtc_has_utc_offset()) {
        rr_rtc_time_t local;
        char slocal[24] = "<unreadable>";
        if (rr_rtc_get_local(&local) == ESP_OK) rr_rtc_format(&local, slocal, sizeof(slocal));
        ESP_LOGI(TAG, "  local:      %s (offset_s=%" PRId32 ")",
                 slocal, rr_rtc_get_utc_offset());
    } else {
        ESP_LOGW(TAG, "  local:      UNKNOWN — no set_tz received yet, the face "
                      "will show UTC. The phone should send set_tz with every TIME_SYNC.");
    }

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
//
// The strictest gate the watch has, and shared by every command that is an act
// of CONTROL over the device rather than a data sync: factory_reset and
// start_routine. Deliberately NOT conn_is_authorised() — a nonce proves the
// peer saw the QR, which is a pairing-time question and no answer at all to
// "may you wipe this watch / start a routine on this child's wrist".
static bool peer_is_trusted(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;

    if (!desc.sec_state.encrypted) {
        ESP_LOGW(TAG, "privileged command refused: link is not encrypted");
        return false;
    }
    if (!rr_identity_is_paired_peer(desc.peer_id_addr.val, desc.peer_id_addr.type)) {
        ESP_LOGW(TAG, "privileged command refused: peer %02x:%02x:%02x:%02x:%02x:%02x "
                      "is bonded but is NOT the peer that paired this watch",
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

// ═════════════════════════════════════════════════════════════════════════════
// QUEUE_PULL — read, PAGED (contract v3)
//
// The framing constant is contract and is generated; rr_store restates it
// because including ble_contract.h there would be a dependency cycle (rr_ble
// already depends on rr_store). This file sees both, so it is where the two can
// actually be checked — a contract change that is not mirrored fails the build
// instead of silently corrupting every frame on the wire.
_Static_assert(RR_QUEUE_FRAME_PREFIX_BYTES == RR_QUEUE_PULL_LEN_PREFIX_BYTES,
               "rr_store's frame prefix disagrees with the generated contract");

// The read cursor, in WIRE bytes from the head of the unacked region.
//
// SEPARATE FROM THE ACK CURSOR, AND THAT SEPARATION IS THE DESIGN. The ack
// cursor (in rr_store, on flash) is what makes a record retired; this one only
// tracks how far the phone has READ. A record that is pulled but never acked
// stays queued and gets re-sent, which is the idempotency guarantee v3 had to
// preserve while adding paging.
//
// It is RAM-only and deliberately so: it resets to 0 on connect, on disconnect
// and on every accepted RUN_ACK, so a reconnect always restarts from a record
// boundary rather than from wherever a dropped link happened to stop.
//
// ONLY queue_seek MOVES IT. Serving a page does not — see the long note at the
// end of queue_pull_access_cb for why a read must be idempotent.
static uint32_t s_pull_offset;

static void pull_offset_reset(const char *why)
{
    if (s_pull_offset != 0) {
        ESP_LOGI(TAG, "QUEUE_PULL: read offset reset to 0 (%s)", why);
    }
    s_pull_offset = 0;
}

static int queue_pull_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) attr_handle; (void) arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    if (!conn_is_authorised(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;

    const uint32_t total = rr_queue_framed_size();

    // Past the end (including an empty queue) is a normal answer, not an error:
    // an empty page tells the phone "nothing more here" in the same shape as
    // every other page, so its loop has exactly one termination condition.
    uint32_t offset = s_pull_offset;
    if (offset > total) offset = total;

    // ═══════════════════════════════════════════════════════════════════════
    // ⚠️ SIZE THE PAGE TO THE NEGOTIATED MTU, NOT TO THE ATTRIBUTE CEILING.
    //
    // TWO DIFFERENT LIMITS, AND CONFLATING THEM IS WHAT BROKE THE DRAIN:
    //   • RR_QUEUE_PULL_MAX_VALUE (512) is BLE_ATT_ATTR_MAX_LEN — the largest an
    //     ATT attribute VALUE may be. It bounds what we may ever store.
    //   • ATT_MTU - 1 is the largest a single ATT_READ_RSP can CARRY (one opcode
    //     byte). At the MTU iOS actually granted (256) that is 255 bytes, so the
    //     page lands at 255 - 12 = 243 B of payload — which is exactly the stride
    //     the hardware log shows: pages @0, @243, @486, @729.
    //
    // A 512-byte value under a 256-byte MTU is only fetchable with a LONG READ
    // (ATT_READ_BLOB_REQ), and we cannot depend on every central issuing one —
    // ble-plx's readCharacteristicForDevice did not, so the read failed outright
    // even after MTU negotiation was fixed. Serving a value that needs a long
    // read is a bet on client behaviour; serving one that fits is not.
    //
    // So the page is capped to what THIS connection can actually deliver. The
    // paging design already carries offset/total/MORE and a per-page `len`, so a
    // smaller page costs only more pages — there is no correctness question, and
    // it removes a cross-platform dependency entirely.
    //
    // ble_att_mtu() returns 0 for an unknown handle; fall back to the ATT
    // default (23) rather than to the ceiling, because guessing high here is
    // exactly the failure being fixed.
    uint16_t conn_mtu = ble_att_mtu(conn_handle);
    if (conn_mtu == 0) conn_mtu = BLE_ATT_MTU_DFLT;

    // MTU - 1 is the response capacity (one opcode byte); the page header eats
    // the rest. Signed, because a tiny MTU can make this negative.
    int mtu_budget = (int) conn_mtu - 1 - (int) RR_QUEUE_PULL_HEADER_SIZE;
    if (mtu_budget < 0) mtu_budget = 0;

    int page_cap = mtu_budget < (int) RR_QUEUE_PULL_MAX_PAYLOAD
                 ? mtu_budget : (int) RR_QUEUE_PULL_MAX_PAYLOAD;

    uint8_t *payload = NULL;
    int got = 0;
    if (offset < total && page_cap > 0) {
        payload = malloc((size_t) page_cap);
        if (payload == NULL) return BLE_ATT_ERR_INSUFFICIENT_RES;
        got = rr_queue_read_framed(payload, offset, (size_t) page_cap);
        if (got < 0) {
            free(payload);
            ESP_LOGE(TAG, "QUEUE_PULL: framed read failed at offset %" PRIu32, offset);
            return BLE_ATT_ERR_UNLIKELY;
        }
    }

    // ── CLAMP BEFORE BUILDING, NOT AFTER ────────────────────────────────────
    //
    // ⚠️ THE FAILURE THIS REPLACES WAS FATAL, NOT NOISY. A page that exceeded the
    // ATT ceiling was detected after the mbuf had been built, and the handler
    // then returned an error — so NimBLE dropped the packet
    // ("ATT handler not found; packet dropped"), the phone got nothing, the
    // offset never advanced, and the drain stalled FOREVER at queued=1. An
    // off-by-one in the page arithmetic therefore cost the whole feature rather
    // than one slow page.
    //
    // Clamping here makes that impossible by construction: the payload is cut to
    // whatever fits under the ceiling BEFORE anything is appended, so the worst a
    // future arithmetic slip can do is serve a shorter page.
    //
    // A SHORT PAGE IS ALWAYS LEGAL, which is what makes this safe rather than a
    // corruption risk. The v3 contract states it explicitly: "A trailing PARTIAL
    // frame is legal; retain the tail and resume with the next page." The phone's
    // decoder already reassembles frames across page boundaries, so it cannot
    // tell a clamped page from a naturally-ending one — no frame alignment is
    // needed here, and attempting to align to the last whole frame would need
    // this code to re-parse the stream it just asked rr_store to serialise.
    // ⚠️ BUDGET AGAINST THE BUFFER WE WERE HANDED, NOT AGAINST ZERO.
    //
    // The first version of this clamp assumed ctxt->om arrives empty and
    // budgeted MAX_VALUE - HEADER. On hardware it does NOT always arrive empty:
    //
    //     page @0    +500 -> value 513 B    <- one byte too many
    //     page @500  +500 -> value 512 B    <- identical inputs, correct
    //     page @1000 +489 -> value 501 B    <- correct
    //
    // Same `got`, different totals, so the payload arithmetic was never wrong —
    // the FIRST read after a queue_seek was entered with a byte already in the
    // mbuf. The clamp above could not see that, because 500 <= 500 passed.
    //
    // So measure. Whatever NimBLE hands us counts against the ATT ceiling just
    // as much as our own bytes do, and this is the only formulation that cannot
    // be wrong about it.
    const uint16_t pre_existing = OS_MBUF_PKTLEN(ctxt->om);
    if (pre_existing != 0) {
        ESP_LOGW(TAG, "QUEUE_PULL: mbuf arrived holding %u B — budgeting around it",
                 (unsigned) pre_existing);
    }

    int max_payload = (int) RR_QUEUE_PULL_MAX_VALUE
                    - (int) pre_existing
                    - (int) RR_QUEUE_PULL_HEADER_SIZE;
    if (max_payload < 0) max_payload = 0;

    if (got > max_payload) {
        ESP_LOGW(TAG, "QUEUE_PULL: clamping payload %d -> %d B (ceiling %d, header %d, "
                      "already in buffer %u) so the drain slows instead of stalling",
                 got, max_payload, RR_QUEUE_PULL_MAX_VALUE, RR_QUEUE_PULL_HEADER_SIZE,
                 (unsigned) pre_existing);
        got = max_payload;
    }

    // ⚠️ The page header is built from the GENERATED struct, so its byte layout
    // cannot drift from the phone's decoder.
    rr_queue_pull_header_t hdr = {
        .version = RR_QUEUE_PULL_VERSION,
        .flags   = (offset + (uint32_t) got < total) ? RR_QUEUE_PULL_FLAG_MORE : 0,
        .offset  = offset,
        .total   = total,
        .len     = (uint16_t) got,
    };

    int rc = 0;
    if (os_mbuf_append(ctxt->om, &hdr, sizeof(hdr)) != 0 ||
        (got > 0 && os_mbuf_append(ctxt->om, payload, (uint16_t) got) != 0)) {
        rc = BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    free(payload);
    if (rc != 0) return rc;

    // A page is header + at most RR_QUEUE_PULL_MAX_PAYLOAD by construction, so
    // it cannot exceed the ATT ceiling. Assert it anyway: an over-long value is
    // the exact defect v3 exists to remove, and it is invisible from this side —
    // the phone just silently fails to read the tail.
    const uint16_t value_len = OS_MBUF_PKTLEN(ctxt->om);
    if (value_len > RR_QUEUE_PULL_MAX_VALUE) {
        // Should now be unreachable — `got` was clamped above and the header is
        // a fixed 12 bytes by static assert. If it fires anyway, the mbuf held
        // something before this callback appended to it, which is the only term
        // left. Print EVERY component rather than just the total: the previous
        // version reported "built a 513-byte value" and that single number was
        // not enough to say whether the header, the payload or the buffer was
        // wrong, which is why the arithmetic could not be located from a log.
        ESP_LOGE(TAG, "QUEUE_PULL: BUG — value %u B = header %d + payload %d, but the "
                      "mbuf reports %u. Something appended to this buffer before us.",
                 (unsigned) value_len, RR_QUEUE_PULL_HEADER_SIZE, got,
                 (unsigned) value_len);
        // Do NOT return an error: that drops the packet and stalls the drain
        // forever, which is worse than a page the phone may reject once.
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ⚠️ THE CURSOR IS NOT TOUCHED HERE. A READ IS IDEMPOTENT.
    //
    // This callback used to end with `s_pull_offset = offset + got`, so a drain
    // was one read per page with no round trip. It cost every other page:
    //
    //     watch served:  @0  @243  @486  @729       (four pages)
    //     phone received: @0        @486            (two)
    //
    // TWO ATT read callbacks can fire for ONE logical central read. iOS is free
    // to issue extra ATT_READ_REQ/ATT_READ_BLOB_REQ, and ble-plx's
    // readCharacteristicForDevice surfaces only the last value — the central
    // cannot even detect that it happened. Auto-advance encodes position as "how
    // many times was I read", and a GATT server has no way to know that number.
    // It is not an off-by-one to correct; it is an unobservable quantity.
    //
    // So position lives on the phone, which is the one side that knows how many
    // pages it actually received: it sets the cursor with queue_seek before every
    // page (contract v3, CURSOR SEMANTICS). A duplicated underlying read now
    // returns the same bytes twice and is harmless instead of destructive.
    //
    // The phone's desync guard stays on the other side of this: it compares each
    // page's echoed offset against what it seeked to, which is what caught this
    // in the first place.
    //
    // Unrelated to the ACK cursor, which is on flash in rr_store and still moves
    // only on a matching RUN_ACK. Pulling a record never retires it.

    ESP_LOGI(TAG, "QUEUE_PULL: page @%" PRIu32 " +%d of %" PRIu32 " B%s (value %u B, "
                  "mtu %u, cap %d)",
             offset, got, total,
             (hdr.flags & RR_QUEUE_PULL_FLAG_MORE) ? ", MORE" : ", END",
             (unsigned) value_len, (unsigned) conn_mtu, page_cap);
    return 0;
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

    // ⚠️ AN ACK MOVES THE BASE OF THE UNACKED STREAM, so every outstanding
    // QUEUE_PULL offset now points somewhere else. Reset rather than try to
    // rebase: 0 is the head of whatever is left, which is a record boundary and
    // therefore always safe. The phone re-seeks per drain pass anyway; this
    // makes the watch correct even if it does not.
    pull_offset_reset("RUN_ACK advanced the queue");

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

    // ── start_routine: remote start, paired-peer only ────────────────────────
    //
    // Gated on peer_is_trusted(), the SAME gate as factory_reset and
    // deliberately NOT the looser conn_is_authorised(): starting a routine on
    // a child's wrist is an act of control over the device, so it belongs to
    // the phone this watch was paired with and to nothing else. The nonce path
    // would not help anyway — a paired watch shows no QR, so there is no nonce
    // to present, and a watch that IS showing its QR has no routines cached to
    // start.
    //
    // The outcome rides the ATT status of this write. RR_CONTROL is write-only
    // with no notify by design (§6B.3: "the ATT write response already carries
    // success or failure"), so the reply channel is the return value here:
    //
    //   0     started — it is on screen
    //   0x80  busy    — a routine is running and was NOT interrupted
    //   0x81  unknown — that routine_id is not cached; the phone must push
    //   0x05  the peer is not the one this watch paired with
    //
    // 0x80/0x81 are in the ATT APPLICATION error range (0x80-0x9F), which the
    // core spec reserves for exactly this. No new characteristic, no byte
    // moved — the frozen contract is untouched.
    if (strcmp(cmd, "start_routine") == 0) {
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(env, "routine_id");
        char routine_id[40] = { 0 };
        if (cJSON_IsString(jid)) {
            strlcpy(routine_id, jid->valuestring, sizeof(routine_id));
        }
        cJSON_Delete(env);

        if (!peer_is_trusted(conn_handle)) {
            ESP_LOGW(TAG, "start_routine REJECTED — untrusted peer");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        if (routine_id[0] == '\0') {
            ESP_LOGE(TAG, "start_routine: no \"routine_id\" in the envelope");
            return BLE_ATT_ERR_UNLIKELY;
        }

        switch (rr_routine_request_start(routine_id)) {
        case RR_START_OK:
            ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
            ESP_LOGI(TAG, "║ REMOTE START accepted — routine %s", routine_id);
            ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");
            return 0;
        case RR_START_BUSY:
            // Not an error on the wire so much as an answer: the phone turns
            // this into "the watch is busy" and the child keeps their routine.
            return RR_CONTROL_ATT_BUSY;
        case RR_START_UNKNOWN_ROUTINE:
            return RR_CONTROL_ATT_UNKNOWN_ROUTINE;
        case RR_START_ERROR:
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }
    }

    // ── set_tz: the UTC offset for local wall-clock display ──────────────────
    //
    // Gated on conn_is_authorised() — the ROUTINE_PUSH gate (nonce OR paired
    // peer), NOT the stricter peer_is_trusted() used by factory_reset and
    // start_routine. This is configuration data of the same kind as the
    // routine set, not an act of control over the device, and it must work
    // during PAIRING, when the phone has only the nonce.
    //
    // The offset is applied to the SCREEN only; the RTC keeps UTC. See
    // rr_rtc.h for why that split is load-bearing (run records are UTC "Z"
    // instants and would silently be wrong by the offset otherwise).
    // ── queue_seek: set the QUEUE_PULL read offset (contract v3) ─────────────
    //
    // The paging recovery primitive. Reads auto-advance, so a normal drain never
    // needs this; it exists so the phone can make its own view authoritative
    // instead of the two sides inferring position from each other:
    //   • seek(0) to start a drain, or to resume after a dropped link
    //   • seek(n) to resync if a page arrives with an unexpected offset
    //
    // An offset past the end is deliberately NOT an error — the next read simply
    // returns an empty page. Clamping-by-answering keeps the phone's loop to a
    // single termination condition.
    if (strcmp(cmd, "queue_seek") == 0) {
        const cJSON *joff = cJSON_GetObjectItemCaseSensitive(env, "offset");
        const bool have = cJSON_IsNumber(joff) && joff->valuedouble >= 0
                          && joff->valuedouble <= (double) UINT32_MAX;
        const uint32_t want = have ? (uint32_t) joff->valuedouble : 0;
        cJSON_Delete(env);

        if (!conn_is_authorised(conn_handle)) {
            ESP_LOGW(TAG, "queue_seek REJECTED — connection not authorised");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        if (!have) {
            ESP_LOGE(TAG, "queue_seek: no valid u32 \"offset\" in the envelope");
            return BLE_ATT_ERR_UNLIKELY;
        }

        s_pull_offset = want;
        ESP_LOGI(TAG, "queue_seek: read offset -> %" PRIu32 " (of %" PRIu32 " B queued)",
                 want, rr_queue_framed_size());
        return 0;
    }

    if (strcmp(cmd, "set_tz") == 0) {
        const cJSON *joff = cJSON_GetObjectItemCaseSensitive(env, "offset_s");
        const bool have = cJSON_IsNumber(joff);
        const int32_t offset_s = have ? (int32_t) joff->valuedouble : 0;
        cJSON_Delete(env);

        if (!conn_is_authorised(conn_handle)) {
            ESP_LOGW(TAG, "set_tz REJECTED — connection not authorised");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        if (!have) {
            ESP_LOGE(TAG, "set_tz: no numeric \"offset_s\" in the envelope");
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (rr_rtc_set_utc_offset(offset_s) != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        // Log the RESULT, not just the receipt. The original bug was invisible
        // precisely because nothing ever printed the local time the watch
        // believed in — "TIME_SYNC received" looked like success while the
        // face was three hours out.
        rr_rtc_time_t local;
        char lbuf[24] = "<unreadable>";
        if (rr_rtc_get_local(&local) == ESP_OK) rr_rtc_format(&local, lbuf, sizeof(lbuf));
        ESP_LOGI(TAG, "set_tz received: offset_s=%" PRId32 " -> local %s",
                 offset_s, lbuf);

        // Schedules are authored as LOCAL "HH:MM", so changing the offset
        // moves every fire time. A DST boundary is exactly this event.
        rr_sched_rearm("set_tz");
        return 0;
    }

    // ── set_audio: speaker volume + quiet hours ──────────────────────────────
    //
    // Gated on conn_is_authorised(), like set_tz and for the same reason: this
    // is CONFIGURATION, not an action on the child's screen. A phone holding
    // the pairing nonce must be able to send it during setup, before the
    // paired-peer anchor exists.
    //
    // Every field is optional and omitted fields keep their current value, so
    // a volume-slider change does not have to restate the quiet window.
    if (strcmp(cmd, "set_audio") == 0) {
        if (!conn_is_authorised(conn_handle)) {
            ESP_LOGW(TAG, "set_audio REJECTED — connection not authorised");
            cJSON_Delete(env);
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        rr_audio_policy_t p;
        rr_audio_get_policy(&p);

        const cJSON *v = cJSON_GetObjectItemCaseSensitive(env, "volume_pct");
        if (cJSON_IsNumber(v)) p.volume_pct = (uint8_t) v->valueint;

        const cJSON *qv = cJSON_GetObjectItemCaseSensitive(env, "quiet_volume_pct");
        if (cJSON_IsNumber(qv)) p.quiet_volume_pct = (uint8_t) qv->valueint;

        // quiet_from: null is how the app turns the window OFF — distinct from
        // omitting the field, which leaves it alone.
        const cJSON *qf = cJSON_GetObjectItemCaseSensitive(env, "quiet_from");
        if (cJSON_IsNull(qf)) {
            p.quiet_from_min = RR_AUDIO_QUIET_DISABLED;
        } else if (cJSON_IsString(qf)) {
            int hh = 0, mm = 0;
            if (sscanf(qf->valuestring, "%d:%d", &hh, &mm) != 2 ||
                hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                ESP_LOGE(TAG, "set_audio: quiet_from \"%s\" is not LOCAL HH:MM",
                         qf->valuestring);
                cJSON_Delete(env);
                return BLE_ATT_ERR_UNLIKELY;
            }
            p.quiet_from_min = (int16_t) (hh * 60 + mm);
        }

        const cJSON *qt = cJSON_GetObjectItemCaseSensitive(env, "quiet_to");
        if (cJSON_IsString(qt)) {
            int hh = 0, mm = 0;
            if (sscanf(qt->valuestring, "%d:%d", &hh, &mm) != 2 ||
                hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                ESP_LOGE(TAG, "set_audio: quiet_to \"%s\" is not LOCAL HH:MM",
                         qt->valuestring);
                cJSON_Delete(env);
                return BLE_ATT_ERR_UNLIKELY;
            }
            p.quiet_to_min = (int16_t) (hh * 60 + mm);
        }

        cJSON_Delete(env);

        if (rr_audio_set_policy(&p) != ESP_OK) {
            ESP_LOGE(TAG, "set_audio REJECTED — values out of range");
            return BLE_ATT_ERR_UNLIKELY;
        }
        // Log the RESULT, not the receipt — the same lesson set_tz taught: a
        // handler that reports success while the device behaves differently is
        // the hardest kind of bug to see from outside.
        ESP_LOGI(TAG, "set_audio applied: volume %u%%, effective right now %u%%%s",
                 p.volume_pct, rr_audio_effective_volume(),
                 rr_audio_in_quiet_hours() ? " (inside quiet hours)" : "");
        return 0;
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

    // New schedules — including a routine whose time a parent just edited in
    // the app. The scheduler is asleep on a wait computed from the OLD cache,
    // so it has to be told, or the change only takes effect after the next
    // resync wake.
    rr_sched_rearm("ROUTINE_PUSH");

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

// ═════════════════════════════════════════════════════════════════════════════
// Bond store: protect the paired phone, evict anything else
//
// ⚠️ THIS REPLACES NimBLE's ble_store_util_status_rr, WHOSE OWN SOURCE SAYS IT
// IS WRONG FOR A PRODUCT:
//
//     "This is not the best behavior for an actual product because
//      uninteresting peers could cause important bonds to be deleted."
//
// It calls ble_gap_unpair_oldest_peer() — three slots (CONFIG_BT_NIMBLE_MAX_BONDS
// = 3), evict whatever is oldest. On this watch exactly ONE bond matters: the
// phone that paired it. Everything else is a development laptop.
//
// It bit us for real. A dev build that randomised the watch's BLE address every
// boot made each reboot look like a new peripheral to macOS, and each of those
// bonded — a bond entry ON THE WATCH per boot. With three slots, the phone's bond
// was evicted by junk, encryption then failed (enc_change status 7), iOS
// terminated the link (reason 531), and a Sync worked only when the phone
// happened to hold a slot. It presented as "sync is stuck", which is a long way
// from its cause.
//
// So: on overflow, delete a bond that is NOT the paired peer's. Fall back to the
// stock behaviour only if the paired peer is somehow the only bond, because
// refusing to evict anything at all would fail the pairing outright.
// Set by log_bond_store() when this boot found PAIRED WITH ZERO BONDS — the
// state from which no connection can ever succeed. Lowers the rotation
// threshold in handle_encryption_failure() to a single observed failure.
static bool s_zero_bonds_at_boot;

// ── Re-advertise backoff ────────────────────────────────────────────────────
//
// A broken link does not fail once, it fails as fast as the radio allows:
// measured at ~40 connect → encrypt-fail → disconnect → re-advertise cycles in
// TWO MINUTES. Each cycle is a full connection setup at 30-60 ms advertising,
// so the failure mode was not just useless, it was the most expensive thing the
// watch can do with its radio — on a device whose entire power budget this phase
// exists to defend.
//
// So consecutive failures back off: 0.5 s, 1 s, 2 s, 4 s ... capped. Reset the
// moment anything succeeds. This is orthogonal to the rotation fix — rotation
// ends the loop for good, this makes the loop cheap in the window before it
// triggers, and cheap for any OTHER repeated-failure cause we have not thought
// of yet.
#define ADV_BACKOFF_AFTER   2        /* failures before backing off at all */
#define ADV_BACKOFF_BASE_MS 500
#define ADV_BACKOFF_MAX_MS  16000

static int s_consec_link_fails;
static esp_timer_handle_t s_adv_retry_timer;

static void advertise(void);

static void adv_retry_cb(void *arg)
{
    (void) arg;
    ESP_LOGI(TAG, "backoff elapsed — advertising again");
    advertise();
}

/** Re-advertise, but not faster than the backoff allows. */
static void advertise_after_failure(void)
{
    s_consec_link_fails++;

    if (s_consec_link_fails <= ADV_BACKOFF_AFTER) {
        advertise();
        return;
    }

    int delay_ms = ADV_BACKOFF_BASE_MS << (s_consec_link_fails - ADV_BACKOFF_AFTER - 1);
    if (delay_ms > ADV_BACKOFF_MAX_MS || delay_ms <= 0) delay_ms = ADV_BACKOFF_MAX_MS;

    ESP_LOGW(TAG, "%d consecutive link failures — holding off %d ms before "
                  "advertising again", s_consec_link_fails, delay_ms);

    if (s_adv_retry_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = adv_retry_cb,
            .name = "rr_adv_retry",
        };
        if (esp_timer_create(&args, &s_adv_retry_timer) != ESP_OK) {
            ESP_LOGE(TAG, "could not create the backoff timer — advertising immediately");
            advertise();
            return;
        }
    }
    esp_timer_stop(s_adv_retry_timer);   // harmless if not running
    if (esp_timer_start_once(s_adv_retry_timer, (uint64_t) delay_ms * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "could not arm the backoff timer — advertising immediately");
        advertise();
    }
}

/** Anything worked: connection encrypted, or a clean session ended. */
static void note_link_success(void)
{
    if (s_consec_link_fails != 0) {
        ESP_LOGI(TAG, "link healthy again — backoff reset (was %d)", s_consec_link_fails);
        s_consec_link_fails = 0;
    }
}

static int bond_store_status(struct ble_store_status_event *event, void *arg)
{
    if (event->event_code != BLE_STORE_EVENT_OVERFLOW) {
        return ble_store_util_status_rr(event, arg);
    }

    switch (event->overflow.obj_type) {
    case BLE_STORE_OBJ_TYPE_OUR_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_ADDR: {
        // Walk the stored peers and unpair the first that is not ours to keep.
        ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
        int count = 0;
        if (ble_store_util_bonded_peers(peers, &count,
                                        sizeof(peers) / sizeof(peers[0])) == 0) {
            for (int i = 0; i < count; i++) {
                if (rr_identity_is_paired_peer(peers[i].val, peers[i].type)) continue;
                ESP_LOGW(TAG, "bond store full — evicting a NON-paired peer "
                              "%02x:%02x:%02x:%02x:%02x:%02x to make room",
                         peers[i].val[5], peers[i].val[4], peers[i].val[3],
                         peers[i].val[2], peers[i].val[1], peers[i].val[0]);
                return ble_gap_unpair(&peers[i]);
            }
            ESP_LOGW(TAG, "bond store full and every stored bond is the paired peer "
                          "— falling back to evicting the oldest");
        }
        return ble_gap_unpair_oldest_peer();
    }
    default:
        return ble_store_util_status_rr(event, arg);
    }
}

// Report how full the bond store is, every boot.
//
// This failure mode is completely invisible otherwise: a watch whose phone bond
// has been evicted looks healthy, advertises normally, answers scans — and simply
// cannot be connected to. One line here would have pointed straight at it instead
// of a session spent chasing crashes and connection parameters.
static void log_bond_store(void)
{
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, sizeof(peers) / sizeof(peers[0])) != 0) {
        ESP_LOGW(TAG, "bond store unreadable");
        return;
    }

    ESP_LOGI(TAG, "bond store: %d/%d slot(s) used", count, MYNEWT_VAL(BLE_STORE_MAX_BONDS));
    bool have_paired = false;
    for (int i = 0; i < count; i++) {
        const bool mine = rr_identity_is_paired_peer(peers[i].val, peers[i].type);
        if (mine) have_paired = true;
        ESP_LOGI(TAG, "  [%d] %02x:%02x:%02x:%02x:%02x:%02x%s", i,
                 peers[i].val[5], peers[i].val[4], peers[i].val[3],
                 peers[i].val[2], peers[i].val[1], peers[i].val[0],
                 mine ? "  <- the paired phone" : "");
    }
    if (rr_identity_is_paired() && !have_paired) {
        // The exact state that presents as "sync is stuck".
        ESP_LOGE(TAG, "PAIRED BUT NO BOND FOR THE PAIRED PEER — the phone will fail "
                      "encryption and iOS will drop the link.");

        // ⚠️ ZERO BONDS IS THE UNRECOVERABLE ONE, AND IT IS NOW ARMED RATHER
        // THAN MERELY NARRATED.
        //
        // With no bond at all there is no key to match and nothing to evict, and
        // the peer arrives as an iOS RPA we cannot resolve without the keys we
        // lost — so every runtime test for "is this our phone?" answers no. That
        // is how this state used to slip past handle_encryption_failure()
        // entirely and loop forever while printing this very line every boot.
        //
        // Arming here does NOT rotate. It lowers the rotation threshold to a
        // single observed encryption failure, so recovery is bounded by one
        // failed connection instead of three — while still requiring proof that
        // encryption actually fails. That distinction is the whole reason the
        // previous boot-time heuristic was removed: zero bonds is also what a
        // watch looks like moments before a legitimate FIRST pair, and rotating
        // on the snapshot alone turned a crash loop into a pairing-prompt loop.
        if (count == 0) {
            s_zero_bonds_at_boot = true;
            ESP_LOGE(TAG, "ZERO BONDS — this watch cannot ever encrypt with a phone whose "
                          "key it does not hold, and iOS cannot forget a BLE bond. Armed: "
                          "the next encryption failure rotates the BLE identity and reboots. "
                          "device_id, the child, the routine cache and queued runs are kept.");
        } else {
            ESP_LOGE(TAG, "Re-pair, or clear the bond store "
                          "(idf.py -DRR_CLEAR_BONDS=1) and pair again.");
        }
    }
}

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

// ── Advertising intervals, in 0.625 ms BLE units ─────────────────────────────
//
// ⚠️ THESE ARE APPLE'S PUBLISHED VALUES, NOT ROUND NUMBERS. Getting that wrong
// broke connectivity on hardware, so it is worth stating why.
//
// The first cut of this used 1000-1500 ms for the idle state. iOS still FOUND
// the watch — scanning only needs one ADV_IND — but connecting failed every
// time: establishing a link needs the central to catch a SUBSEQUENT advertising
// event inside its connect timeout, and at 1-1.5 s it kept missing. Observed as
// four consecutive "connection failed" retries against a watch that was
// advertising perfectly and answering scans in 150 ms.
//
// Apple's Accessory Design Guidelines list the intervals iOS aligns its scan
// cadence to: 152.5, 211.25, 318.75, 417.5, 546.25, 760, 852.5, 1022.5 ms. A
// value from that list connects reliably; an arbitrary one in between does not.
// min == max on purpose — a RANGE lets the controller pick something off-list.
//
//   852.5 ms idle: Phase 10 took the step the previous comment here left open.
//     See the discovery-budget note below for why this is the ceiling and not
//     1022.5 ms.
//   152.5 ms eager: Apple's fastest listed value, for the 30 s after a run is
//     queued when a phone may be in the next room.
//   30-60 ms pairing: fast is explicitly allowed for the first 30 s of a
//     connectable accessory, and a parent is standing there holding the QR.
//
// ── Why 852.5 and not the top of Apple's list (Phase 10) ────────────────────
//
// DISCOVERY BUDGET. A scanner sees an advertiser once their windows coincide;
// with the app scanning a full 8 s window, worst-case discovery measured 1.2 s
// at 417.5 ms — ~2.9x the interval, which is the usual ratio once the three
// channels and the 0-10 ms random delay are accounted for. Doubling the
// interval doubles that: ~2.4 s worst case, leaving 5.6 s (3.3x) of margin
// inside the 8 s window. That is still comfortable.
//
// The ceiling is NOT set by discovery, it is set by CONNECTION SETUP, and that
// is the part this file has been burned by before (see the four-retry incident
// above). Establishing a link needs the central to catch a SUBSEQUENT
// advertising event inside its connect timeout, and 1000-1500 ms failed that
// every time. 1022.5 ms is Apple-listed and would be another ~17% saving, but
// it sits right on top of the value that broke, and IDLE is the state a Sync
// press arrives in — adv_state() only escalates to EAGER on locally-known work
// (a run queued), never on a connection the phone is about to attempt, so idle
// advertising must be reliably CONNECTABLE, not merely discoverable.
//
// 852.5 ms is the largest Apple-listed value with real daylight between it and
// the failure, so it is the maximum that is safe without re-running the
// connect-reliability test on iOS.
#define ADV_ITVL_PAIRING_MIN  0x0030   /*    30 ms */
#define ADV_ITVL_PAIRING_MAX  0x0060   /*    60 ms */
#define ADV_ITVL_EAGER_MIN    0x00F4   /*   152.5 ms — Apple-listed */
#define ADV_ITVL_EAGER_MAX    0x00F4
#define ADV_ITVL_IDLE_MIN     0x0554   /*   852.5 ms — Apple-listed */
#define ADV_ITVL_IDLE_MAX     0x0554

/** How long a freshly-queued run keeps advertising brisk. */
#define ADV_EAGER_MS (30 * 1000)

typedef enum { ADV_IDLE = 0, ADV_EAGER, ADV_PAIRING } rr_adv_state_t;

static rr_adv_state_t s_adv_state = ADV_PAIRING;   /* first boot advertises fast */
static int64_t s_queue_activity_ms = INT64_MIN;

static const char *adv_state_name(rr_adv_state_t s)
{
    switch (s) {
    case ADV_PAIRING: return "pairing/fast";
    case ADV_EAGER:   return "eager";
    default:          return "idle/slow";
    }
}

static rr_adv_state_t adv_state(void)
{
    // Unpaired means the QR is on screen and a parent is waiting: latency is the
    // product here, so this state never gets slowed down.
    if (!rr_identity_is_paired()) return ADV_PAIRING;

    const int64_t now = (int64_t) xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_queue_activity_ms != INT64_MIN && now - s_queue_activity_ms < ADV_EAGER_MS) {
        return ADV_EAGER;
    }
    // Deliberately NOT "eager while anything is queued": a watch worn all day
    // with an undrained run would then advertise briskly for hours, which is the
    // draw this change exists to remove. The records are durable and the phone's
    // foreground drain will collect them.
    return ADV_IDLE;
}

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

    // ── Advertising interval: adapted to state, not fixed ────────────────────
    //
    // This used to leave itvl_min/itvl_max at 0, which makes NimBLE substitute
    // its default for CONN_MODE_UND: BLE_GAP_ADV_FAST_INTERVAL1, 30-60 ms,
    // started BLE_HS_FOREVER and never varied. A watch asleep on a nightstand
    // transmitted 20-30 times a second all night — on a 400 mAh cell that was
    // the single largest idle draw (est. 4-8 mA of a 12-20 mA budget).
    //
    // Three states, because the right interval genuinely differs:
    //
    //   PAIRING (unpaired) — fast. A parent is standing there with the QR on
    //     screen waiting for the app to find it; latency is the whole
    //     experience and the window is seconds long.
    //   WORK PENDING — brisk, for a while. A run was just queued, so a phone
    //     coming into range should be found quickly. Time-boxed: after
    //     ADV_EAGER_MS the records are no less safe, they just wait for the
    //     next foreground drain rather than costing battery all night.
    //   IDLE — slow. Nothing to say. The phone's foreground drain scans
    //     actively and retries, so it tolerates a longer window.
    //
    // ⚠️ THE TRADE, STATED: idle discovery gets slower. Measured before this
    // change, a cold scan first saw a sleeping watch in 0.35 s typical / 1.2 s
    // worst (spec §6B.7). At a 1000-1500 ms interval expect roughly 1-2 s
    // typical and ~3 s worst. That is absorbed by design — BLEService.
    // connectToWatch() scans a full window and then retries once transparently
    // (packages/ble/src/index.ts) — but it is a real regression in the "press
    // Sync and watch it work" feel, and the reason this is tunable in one place.
    const rr_adv_state_t st = adv_state();
    switch (st) {
    case ADV_PAIRING:
        params.itvl_min = ADV_ITVL_PAIRING_MIN;
        params.itvl_max = ADV_ITVL_PAIRING_MAX;
        break;
    case ADV_EAGER:
        params.itvl_min = ADV_ITVL_EAGER_MIN;
        params.itvl_max = ADV_ITVL_EAGER_MAX;
        break;
    case ADV_IDLE:
    default:
        params.itvl_min = ADV_ITVL_IDLE_MIN;
        params.itvl_max = ADV_ITVL_IDLE_MAX;
        break;
    }

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
    s_adv_state = st;
    ESP_LOGI(TAG, "advertising as \"%s\" — %s (%u-%u ms), service %s",
             RR_BLE_DEVICE_NAME, adv_state_name(st),
             (unsigned) (params.itvl_min * 625 / 1000),
             (unsigned) (params.itvl_max * 625 / 1000),
             RR_SYNC_SERVICE_UUID_STR);
}

// Re-advertise at a new interval if the state that picks it has changed.
//
// ble_gap_adv_start cannot change the interval of a running advertisement, so
// this stops and restarts. That opens a sub-millisecond gap in advertising,
// which is why it only runs on an actual state CHANGE rather than on every
// queue event or timer tick.
static void adv_refresh(const char *reason)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;   // advertising stops while connected
    const rr_adv_state_t want = adv_state();
    if (want == s_adv_state) return;

    ESP_LOGI(TAG, "advertising %s -> %s (%s)",
             adv_state_name(s_adv_state), adv_state_name(want), reason);
    ble_gap_adv_stop();
    advertise();
}

void rr_ble_note_queue_activity(void)
{
    // Timestamp first, so the state function sees the new value.
    s_queue_activity_ms = (int64_t) xTaskGetTickCount() * portTICK_PERIOD_MS;
    adv_refresh("run queued");
}

// ═════════════════════════════════════════════════════════════════════════════
// Making an encryption failure legible
//
// "encryption change: status=7" was the entire diagnostic surface, and it sent
// this project down the wrong path twice. Two things about that number:
//
//   1. IT IS NOT AN SMP CODE. It is a NimBLE ble_hs error, and the SMP pairing
//      reasons live at a +0x400/+0x500 offset (BLE_HS_ERR_SM_US_BASE /
//      _SM_PEER_BASE). Reading 7 as SMP "Command Not Supported" is a natural
//      mistake and completely wrong.
//   2. 7 IS BLE_HS_ENOTCONN, AND IT HAS EXACTLY ONE MEANING HERE: a security
//      procedure was still in flight when the link went away. NimBLE raises it
//      from ble_sm_connection_broken(), which runs from ble_gap_conn_broken()
//      BEFORE the disconnect event is delivered — which is why the log always
//      shows it immediately above "disconnected (reason 531)".
//
// Crucially, NimBLE only raises it if an SM proc EXISTED (ble_sm_process_result
// returns early on proc == NULL). So seeing this at all PROVES the central
// started a security procedure. It is the opposite of "iOS never initiates
// pairing", which is what Phase 7 inferred from the macOS harnesses.
// ═════════════════════════════════════════════════════════════════════════════
static const char *sec_status_str(int status, char *buf, size_t buflen)
{
    // The SMP "pairing failed" reasons, shared by both direction bases.
    static const char *const sm_reason[] = {
        [0x01] = "passkey entry failed",        [0x02] = "OOB not available",
        [0x03] = "authentication requirements", [0x04] = "confirm value failed",
        [0x05] = "pairing not supported",       [0x06] = "encryption key size",
        [0x07] = "command not supported",       [0x08] = "unspecified",
        [0x09] = "repeated attempts",           [0x0A] = "invalid parameters",
        [0x0B] = "DHKey check failed",          [0x0C] = "numeric comparison failed",
        [0x0D] = "BR/EDR pairing in progress",  [0x0E] = "cross-transport key derivation",
    };
    const size_t sm_reason_n = sizeof(sm_reason) / sizeof(sm_reason[0]);

    if (status == 0) {
        snprintf(buf, buflen, "success");
    } else if (status == BLE_HS_ENOTCONN) {
        snprintf(buf, buflen,
                 "BLE_HS_ENOTCONN — the link dropped while security was still in "
                 "progress (the peer gave up and terminated)");
    } else if (status == BLE_HS_ETIMEOUT) {
        snprintf(buf, buflen, "BLE_HS_ETIMEOUT — the peer stopped responding mid-pairing");
    } else if (status >= BLE_HS_ERR_HCI_BASE && status < BLE_HS_ERR_L2C_BASE) {
        const int hci = status - BLE_HS_ERR_HCI_BASE;
        snprintf(buf, buflen, "HCI 0x%02x%s", hci,
                 hci == 0x06 ? " — PIN OR KEY MISSING (we do not hold the peer's key)"
               : hci == 0x13 ? " — remote user terminated"
               : hci == 0x3D ? " — MIC failure"
               : "");
    } else if (status >= BLE_HS_ERR_SM_US_BASE && status < BLE_HS_ERR_SM_PEER_BASE) {
        const int r = status - BLE_HS_ERR_SM_US_BASE;
        snprintf(buf, buflen, "SMP fail (we sent it) 0x%02x — %s", r,
                 (r > 0 && (size_t) r < sm_reason_n && sm_reason[r]) ? sm_reason[r] : "unknown");
    } else if (status >= BLE_HS_ERR_SM_PEER_BASE && status < BLE_HS_ERR_HW_BASE) {
        const int r = status - BLE_HS_ERR_SM_PEER_BASE;
        snprintf(buf, buflen, "SMP fail (PEER sent it) 0x%02x — %s", r,
                 (r > 0 && (size_t) r < sm_reason_n && sm_reason[r]) ? sm_reason[r] : "unknown");
    } else if (status >= BLE_HS_ERR_ATT_BASE && status < BLE_HS_ERR_HCI_BASE) {
        snprintf(buf, buflen, "ATT 0x%02x", status - BLE_HS_ERR_ATT_BASE);
    } else {
        snprintf(buf, buflen, "ble_hs error %d", status);
    }
    return buf;
}

/** Do we hold a bond for this peer? Answers "stale bond" vs "no bond at all". */
static bool have_bond_for(const ble_addr_t *peer)
{
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, sizeof(peers) / sizeof(peers[0])) != 0) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (peers[i].type == peer->type && memcmp(peers[i].val, peer->val, 6) == 0) return true;
    }
    return false;
}

/**
 * How many consecutive encryption failures before the watch changes identity.
 *
 * Each Sync press is one attempt, so this is "press Sync three times and it
 * heals itself" — slow enough that a single RF glitch, or a parent walking out
 * of range mid-pairing, never triggers it; fast enough that a parent finds it
 * before they find support. See rr_identity.h for why it must be > 1.
 */
#define RR_ENC_FAILS_BEFORE_ROTATE 3

// Most fresh identities we will ever present. Each one costs the parent a
// permanent, manually-removable row in their phone's Bluetooth list, so this is
// a user-visible budget, not an internal retry count. Past it, recovery needs a
// human — see the refusal in handle_encryption_failure().
#define RR_MAX_BLE_GENERATIONS 3

// ─────────────────────────────────────────────────────────────────────────────
// The stale-bond dead end, and the only way out of it.
//
// THE STATE. The phone holds an LTK for this watch's BLE address; the watch
// does not hold the matching key. iOS opens the link, goes straight to
// encryption with the key it has (it never sends a Pairing Request, so
// BLE_GAP_EVENT_REPEAT_PAIRING — our existing handler for "bonded before, keys
// disagree" — CANNOT fire), NimBLE answers the controller's LTK request with a
// negative reply, and iOS terminates. Forever, identically, every attempt.
//
// WHY DROPPING OUR BOND IS NOT ENOUGH. It is correct hygiene and we do it, but
// it changes nothing on its own: the phone still holds its key and still tries
// it, and an iOS app CANNOT delete a BLE bond — a BLE-only watch is not even
// listed in Settings > Bluetooth, so there is nothing for a parent to forget.
//
// WHAT ACTUALLY WORKS is presenting a BLE address the phone has never seen, so
// it bonds fresh. That is what rr_identity's generation counter is for, and it
// keeps device_id — so the server-side pairing, the child, and the routine
// cache all survive. The cost is one dead entry in the phone's bond list.
//
// Until now the only recovery was erasing NVS, which regenerates device_id and
// costs a QR re-registration for what is purely a link-layer problem.
// ─────────────────────────────────────────────────────────────────────────────
static void handle_encryption_failure(uint16_t conn_handle, int status)
{
    char why[160];
    ESP_LOGE(TAG, "╔══ ENCRYPTION FAILED — status=%d ══", status);
    ESP_LOGE(TAG, "║ %s", sec_status_str(status, why, sizeof(why)));

    // The connection still exists here even when the failure IS the teardown:
    // ble_gap_conn_broken() calls into SMP before it deletes the conn, so
    // conn_find works and the peer identity is readable.
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGE(TAG, "║ peer already gone — cannot tell whether the bond was stale");
        ESP_LOGE(TAG, "╚══════════════════════════════════════");
        return;
    }

    const bool bonded = have_bond_for(&desc.peer_id_addr);
    const bool is_paired_peer =
        rr_identity_is_paired_peer(desc.peer_id_addr.val, desc.peer_id_addr.type);

    ESP_LOGE(TAG, "║ peer %02x:%02x:%02x:%02x:%02x:%02x (type %u)",
             desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
             desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
             (unsigned) desc.peer_id_addr.type);
    ESP_LOGE(TAG, "║ we hold a bond for it: %s | it is our paired phone: %s",
             bonded ? "YES" : "no", is_paired_peer ? "YES" : "no");

    // ⚠️ THE CONDITION THAT MUST NOT COME BACK.
    //
    // This used to be:
    //
    //     if (!rr_identity_is_paired() || !(bonded || is_paired_peer)) return;
    //
    // and it returned BEFORE rr_identity_note_enc_fail(), so in the worst state
    // this watch can reach the counter never moved and the rotation that exists
    // to escape it was never approached. Observed on hardware: ~40 connect →
    // encrypt-fail → disconnect → re-advertise cycles in two minutes, every one
    // of them logging "not a stale-bond case — leaving the bond store alone",
    // forever, with no way out that did not involve a USB cable.
    //
    // The state it missed is PAIRED WITH ZERO BONDS, which is the *definitive*
    // dead end rather than an edge case:
    //   • we hold no key, so `bonded` is false;
    //   • the peer is an iOS RPA and resolving it needs the very keys we lost,
    //     so `is_paired_peer` is false too;
    //   • the phone still holds ITS key and no iOS app can delete a BLE bond.
    // So the one situation with no escape but rotation was the one situation
    // the guard treated as "nothing to do here".
    //
    // What actually matters is ONLY whether this watch believes it is in
    // service. An unpaired watch failing encryption is a first pairing that went
    // wrong — retry is right there, and rotating would be actively harmful. A
    // PAIRED watch failing encryption repeatedly is broken no matter whose bond
    // it is or whether we can identify the peer at all.
    if (!rr_identity_is_paired()) {
        ESP_LOGE(TAG, "║ not paired — a first pairing that failed, retry is correct");
        ESP_LOGE(TAG, "╚══════════════════════════════════════");
        return;
    }

    // Drop OUR side of a bond that demonstrably does not work. Keeping it only
    // occupies one of three slots and makes log_bond_store() lie about health.
    // Conditional because there may be nothing to drop — which is precisely the
    // zero-bond case, and is no longer a reason to stop.
    // ⚠️ ENOTCONN IS NOT EVIDENCE OF A BAD KEY, AND DROPPING A BOND ON IT IS A
    // SELF-INFLICTED RE-PAIR.
    //
    // BLE_HS_ENOTCONN (7) means a security procedure was still in flight when
    // the LINK WENT AWAY — NimBLE raises it from ble_sm_connection_broken()
    // before the disconnect event is delivered. That happens for every ordinary
    // mid-session disconnect, including a central that hangs up after finishing
    // its reads. It says nothing whatsoever about whether the key was correct.
    //
    // Observed doing real damage: a drain completed all three pages, the phone
    // dropped the link before RUN_ACK, and this handler deleted a bond whose
    // encryption had SUCCEEDED moments earlier (the log said bond: YES, our
    // paired phone: YES, right after "ENCRYPTED bonded=1"). The next connect
    // then had no key and had to re-pair — manufacturing the very stale-bond
    // state this function exists to clean up.
    //
    // So only drop on a status that actually implicates the key. A disconnect
    // mid-procedure is a disconnect; let the reconnect prove whether the key
    // works.
    const bool key_implicated = (status != BLE_HS_ENOTCONN);
    if (bonded && key_implicated) {
        int rc = ble_store_util_delete_peer(&desc.peer_id_addr);
        ESP_LOGW(TAG, "║ dropped our stale bond for this peer (rc=%d)", rc);
    } else if (bonded) {
        ESP_LOGI(TAG, "║ KEEPING the bond — status %d is a mid-procedure disconnect, "
                      "not a key mismatch", status);
    }

    // A watch that booted PAIRED WITH ZERO BONDS is already known to be in the
    // unrecoverable state, so it does not need three failures to prove it — the
    // first one is confirmation. Still failure-triggered rather than acted on at
    // boot, and that distinction is load-bearing: an earlier attempt DID rotate
    // straight from the boot-time snapshot and turned a crash loop into a
    // pairing-prompt loop, because zero bonds is also what a watch looks like in
    // the moments before a legitimate first pair. Requiring one OBSERVED
    // encryption failure keeps that window safe while still bounding recovery to
    // a single failed connection.
    const uint8_t threshold = s_zero_bonds_at_boot ? 1 : RR_ENC_FAILS_BEFORE_ROTATE;

    // Nor should that same mid-procedure disconnect count TOWARDS rotation. A
    // watch with a working bond that the phone hangs up on is healthy; counting
    // it would walk an ordinary drain toward a BLE identity change, and each of
    // those costs the parent a permanent row in their Bluetooth settings.
    //
    // The zero-bond dead end still counts, because there `bonded` is false —
    // which is exactly the case that needs the escape.
    if (bonded && !key_implicated) {
        ESP_LOGI(TAG, "║ not counted toward rotation — the bond is intact");
        ESP_LOGE(TAG, "╚══════════════════════════════════════");
        return;
    }

    const uint8_t fails = rr_identity_note_enc_fail();
    ESP_LOGE(TAG, "║ consecutive encryption failures: %u of %u before this watch "
                  "changes BLE identity%s", (unsigned) fails, threshold,
             s_zero_bonds_at_boot ? " (armed: booted paired with zero bonds)" : "");

    if (fails < threshold) {
        ESP_LOGE(TAG, "║ press Sync again — this heals itself at %u", threshold);
        ESP_LOGE(TAG, "╚══════════════════════════════════════");
        return;
    }

    // ⚠️ HARD CAP ON ROTATIONS. EVERY ROTATION COSTS THE PARENT A PERMANENT
    // ENTRY IN THEIR PHONE'S BLUETOOTH LIST.
    //
    // A new BLE address is a new device as far as iOS is concerned: a pairing
    // prompt, and a row in Settings > Bluetooth that only the parent can remove.
    // That is an acceptable ONE-TIME price for escaping a dead pairing. It is
    // not acceptable repeatedly — and repeatedly is exactly what happened when
    // this was armed at boot: the watch rotates, reboots, comes up STILL paired
    // with zero bonds (the phone has not bonded to the new identity yet),
    // re-arms at a threshold of one, and rotates again on the next failure.
    // Observed as "the app keeps asking me to pair, and each pair adds a
    // Bluetooth device".
    //
    // So rotation is bounded. If several fresh identities have not fixed it, the
    // problem is not the identity and another one will not help — stop, and say
    // what will. Better a watch that needs a deliberate factory reset than one
    // that quietly fills a parent's Bluetooth settings forever.
    if (rr_identity_ble_generation() >= RR_MAX_BLE_GENERATIONS) {
        ESP_LOGE(TAG, "║ ALREADY AT BLE GENERATION %u — REFUSING TO ROTATE AGAIN.",
                 (unsigned) rr_identity_ble_generation());
        ESP_LOGE(TAG, "║ Rotating has not fixed this, and each attempt leaves another");
        ESP_LOGE(TAG, "║ stale device in the phone's Bluetooth list. Recover by hand:");
        ESP_LOGE(TAG, "║   1. forget every 'RoutineRush Watch' in iOS Settings > Bluetooth");
        ESP_LOGE(TAG, "║   2. hold BOOT for 10 s to factory reset, then re-pair from the QR");
        ESP_LOGE(TAG, "╚══════════════════════════════════════");
        // Keep the count pinned at the threshold rather than clearing it: this
        // watch is in a state that needs a human, and pretending otherwise would
        // just re-approach the cap on the next failure.
        s_zero_bonds_at_boot = false;
        return;
    }

    ESP_LOGE(TAG, "║ THRESHOLD REACHED — rotating the BLE address so the phone");
    ESP_LOGE(TAG, "║ bonds fresh. device_id, the child and the queued runs are kept.");
    ESP_LOGE(TAG, "╚══════════════════════════════════════");

    if (rr_identity_bump_ble_generation("repeated encryption failure (stale bond)") != ESP_OK) {
        ESP_LOGE(TAG, "could not persist a new BLE generation — NOT rebooting, because "
                      "a reboot that did not change the address would just loop");
        return;
    }
    // Clear the count for the new identity: it is a fresh peripheral and gets a
    // fresh budget. Leaving it at the threshold would rotate again on the next
    // single failure, which is how the old boot-time heuristic reached gen 7.
    rr_identity_clear_enc_fails();

    // Reboot rather than re-address in place. The address is applied once in
    // on_sync(), and unwinding live NimBLE/advertising state from inside a GAP
    // callback during a teardown is exactly the kind of thing rr_ble_factory_reset
    // already refuses to do. The parent presses Sync again ~2 s later.
    ESP_LOGW(TAG, "rebooting to adopt the new BLE identity...");
    vTaskDelay(pdMS_TO_TICKS(400));   // let the log drain over USB-JTAG
    esp_restart();
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
            // A new peer inherits no paging position from the previous one.
            pull_offset_reset("new connection");
            ESP_LOGI(TAG, "connected (handle %u)", (unsigned) s_conn_handle);

            // Connection parameters are NOT requested here — see
            // BLE_GAP_EVENT_ENC_CHANGE. Issuing an L2CAP parameter update in the
            // same instant as the central's MTU exchange and bonding is a known
            // way to upset a link, and a mid-drain disconnect was observed on the
            // first hardware run with it here. Waiting for encryption costs
            // nothing: the drain cannot start before then anyway.
            // ── ASK FOR SECURITY OURSELVES. NOW IN PRODUCTION. ───────────────
            //
            // WHAT THIS SENDS: an SMP Security Request. It is a request, not a
            // command — the central decides what to do with it. A central that
            // holds a key starts encryption with it; a central that holds none
            // starts pairing. Either way the link is encrypted before the first
            // ATT write instead of after a failed one.
            //
            // WHAT IT REPLACES. Until now the watch said nothing and relied on
            // the central reacting to an ATT error: every characteristic is
            // WRITE_ENC, so NimBLE answers a write on an unencrypted link with
            // ATT 0x0F (Insufficient Encryption), and the central is supposed to
            // pair and retry. iOS does. macOS does NOT — CoreBluetooth pairs on
            // 0x05 but not on 0x0F — which is what blocked the laptop harnesses
            // and is why this line existed behind a dev flag at all.
            //
            // ⚠️ WHAT IT DOES *NOT* FIX: the stale-bond failure below. A central
            // that holds a key uses that key whether it was asked to encrypt or
            // volunteered — NimBLE routes both into the same LTK-restore path.
            // Shipping this is a robustness change, not the cure for status=7.
            //
            // WHY IT IS SAFE TO SHIP. Pairing stays Just Works with no IO
            // capability, so no prompt appears and nothing about the QR/nonce
            // handshake moves; the nonce is still what authenticates, and
            // conn_is_authorised() is unchanged. What changes is ORDER: a first
            // pairing now encrypts on connect rather than on the first rejected
            // write, and a reconnect encrypts a few ms earlier than it did. The
            // Phase 2 flow is a strict subset of that — it still works if the
            // central ignores the request entirely, because the WRITE_ENC gate
            // is untouched and the old ATT-0x0F path is still there underneath.
            // ⚠️ DO NOT RE-REQUEST SECURITY ON AN ALREADY-ENCRYPTED LINK.
            //
            // A bonded iOS central can encrypt so fast that it completes BEFORE
            // this callback runs, which produced the giveaway ordering in the
            // hardware log:
            //
            //     ENCRYPTED (bonded=1 ...)          <- already done
            //     security requested (rc=0)          <- us, asking anyway
            //
            // A Security Request on a link that is already encrypted is at best
            // redundant and at worst a reason for the central to tear the link
            // down — which is what was happening immediately after the final
            // QUEUE_PULL page, killing the drain before RUN_ACK.
            //
            // BLE_HS_EALREADY does not cover this: it means a security procedure
            // is IN PROGRESS, not that one already finished. So ask the link.
            struct ble_gap_conn_desc cd;
            if (ble_gap_conn_find(s_conn_handle, &cd) == 0 && cd.sec_state.encrypted) {
                ESP_LOGI(TAG, "already encrypted on connect (bonded=%d) — not re-requesting",
                         (int) cd.sec_state.bonded);
                return 0;
            }

            int sec = ble_gap_security_initiate(s_conn_handle);
            if (sec == 0 || sec == BLE_HS_EALREADY) {
                // EALREADY == the central beat us to it. Normal on reconnect.
                ESP_LOGI(TAG, "security requested (rc=%d)", sec);
            } else {
                // Not fatal: the ATT 0x0F path still prompts a compliant central.
                ESP_LOGW(TAG, "security_initiate failed (rc=%d) — falling back to the "
                              "ATT 0x0F path; iOS handles it, macOS does not", sec);
            }
        } else {
            ESP_LOGW(TAG, "connect failed (status %d) — re-advertising", event->connect.status);
            advertise_after_failure();
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
        // Drop the paging position: a resumed drain must restart from a record
        // boundary, never from wherever the link happened to die.
        pull_offset_reset("peer disconnected");
        // Force a re-pick of the interval: the state may have changed while the
        // phone was connected (a routine finished, or pairing completed).
        s_adv_state = (rr_adv_state_t) -1;
        // Reason 531 (BLE_ERR_REM_USER_CONN_TERM via the encryption path) is what
        // a link torn down over a failed key looks like, and it is the one that
        // arrived ~40 times in two minutes. Route the disconnect through the
        // backoff whenever a failure is already being counted; a clean session
        // will have called note_link_success() and reset it to zero, so this
        // costs a normal disconnect nothing.
        if (s_consec_link_fails > 0) advertise_after_failure();
        else advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        // ── NO CONNECTION-PARAMETER REQUEST HERE. REVERTED, MEASURED. ────────
        //
        // Phase 10 item 5 asked for a relaxed link (90-150 ms, slave latency 4)
        // to stop the auto-sync drain from costing radio time. It was a bad
        // trade and hardware said so within one test:
        //
        //   ROUTINE_PUSH went from ~4 s to ~112 s. The watch log showed 20-byte
        //   chunks landing every ~855 ms — which is exactly (1 + latency) *
        //   itvl_max = 5 * 150 ms. Slave latency lets a peripheral SKIP
        //   connection events when it has nothing to send; with
        //   write-with-response traffic it delays every single response instead.
        //
        // And the upside was never real: the estimate was 1-3 mA WHILE
        // CONNECTED, and connections here last seconds a day — call it
        // 0.03 mAh/day against a ~400 mAh cell. Paying a 25x slower sync for
        // that is indefensible.
        //
        // If this is ever revisited: latency must stay 0 during transfers, and
        // any relaxation belongs AFTER the drain completes, not before it. The
        // CONN_UPDATE logging below is kept so the link's actual parameters are
        // visible in any capture.
        if (event->enc_change.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "ENCRYPTED (bonded=%d authenticated=%d key_size=%d)",
                         (int) desc.sec_state.bonded, (int) desc.sec_state.authenticated,
                         (int) desc.sec_state.key_size);
            } else {
                ESP_LOGI(TAG, "encryption change: success");
            }
            // Whatever was wrong is no longer wrong. Reset the escape-hatch
            // budget here and nowhere else, so the counter only ever describes
            // an UNBROKEN run of failures.
            rr_identity_clear_enc_fails();
            // A successful encryption also means the watch is no longer in the
            // zero-bond dead end — the phone has just bonded fresh. Disarm, so a
            // later unrelated failure does not rotate on a single strike.
            s_zero_bonds_at_boot = false;
            note_link_success();
        } else {
            handle_encryption_failure(event->enc_change.conn_handle,
                                      event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr=%u notify=%d",
                 (unsigned) event->subscribe.attr_handle, event->subscribe.cur_notify);
        // Push the queue depth the moment the phone subscribes.
        //
        // This is the "on connect" notification, and it belongs HERE rather than
        // in BLE_GAP_EVENT_CONNECT: a notify sent before the central has written
        // the CCCD is discarded by the stack, so notifying on connect would look
        // correct and reach nobody. Subscription is the first instant the phone
        // can actually hear us.
        //
        // It matters because the phone otherwise has to poll to discover work:
        // a watch carrying three offline runs said nothing until asked.
        if (event->subscribe.attr_handle == s_queue_status_handle &&
            event->subscribe.cur_notify) {
            ESP_LOGI(TAG, "phone subscribed to QUEUE_STATUS — pushing current depth");
            rr_ble_notify_queue_status();
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        // Report what the link ACTUALLY settled on. A requested interval is a
        // request; iOS in particular applies its own policy, and assuming the
        // relaxed values were granted would make any power measurement a guess.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "conn params now: itvl=%u (%u ms) latency=%u timeout=%u (status=%d)",
                     (unsigned) desc.conn_itvl, (unsigned) (desc.conn_itvl * 125 / 100),
                     (unsigned) desc.conn_latency, (unsigned) desc.supervision_timeout,
                     event->conn_update.status);
        }
        return 0;
    }

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
    // FNV-1a over the device_id AND the BLE address generation, folded to 6 bytes.
    //
    // The generation lets the watch present a fresh BLE identity — so a phone
    // holding a stale bond will pair again — WITHOUT changing device_id, which is
    // what the server-side pairing is keyed on. See rr_identity.h.
    const char *id = rr_identity_device_id();
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = id; *p; p++) {
        h ^= (unsigned char) *p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t) rr_identity_ble_generation();
    h *= 1099511628211ULL;
    for (int i = 0; i < 6; i++) {
        out->val[i] = (uint8_t) (h >> (i * 8));
    }
#ifdef RR_DEV_NONCE_WHEN_PAIRED
    // TEST BUILDS ONLY (see the root CMakeLists): a fresh identity EVERY BOOT.
    //
    // A fixed dev address is not enough. macOS caches the bond, the watch's
    // bond store does not always still have it after a reflash, and from then
    // on CoreBluetooth refuses to connect at all ("Peer removed pairing
    // information") with no way to clear it from the command line. Randomising
    // per boot sidesteps the whole cache: every test session is a device macOS
    // has never seen. It costs a junk entry in the host's bond list per boot,
    // which is the right trade for a throwaway image.
    out->val[0] ^= (uint8_t) (esp_random() & 0xFF);
    out->val[1] ^= (uint8_t) (esp_random() & 0xFF);
#endif
    // A random STATIC address requires the two most significant bits set.
    out->val[5] |= 0xC0;
    out->type = BLE_ADDR_RANDOM;
}

/**
 * Detect "paired but no bond" and escape it by rotating the BLE address.
 *
 * Must run BEFORE derive_static_addr(), because the whole point is to change what
 * that derives. Returns true if the generation was bumped.
 *
 * The state is unrecoverable otherwise: the phone encrypts with a key the watch
 * lost, iOS drops the link, and no app can make iOS forget a BLE bond. Rotating
 * our address makes us a new peripheral to the phone, which then bonds cleanly —
 * at the cost of one stale entry in the phone's bond list, which is invisible to
 * the parent and far cheaper than a factory reset and a QR re-registration.
 *
 * Conditions are deliberately narrow: paired, an anchor recorded, and ZERO bonds.
 * A watch with any bond at all is left alone, because rotating then would break a
 * link that might be working.
 */
#ifdef RR_ROTATE_BLE_ADDR
static bool heal_lost_bond(void)
{
    if (!rr_identity_is_paired()) return false;

    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, sizeof(peers) / sizeof(peers[0])) != 0) {
        return false;   // store unreadable: do not act on a guess
    }
    if (count > 0) return false;

    ESP_LOGE(TAG, "PAIRED WITH ZERO BONDS — the phone cannot encrypt to us and iOS "
                  "cannot be told to forget us. Rotating the BLE address so it "
                  "bonds again.");
    return rr_identity_bump_ble_generation("paired but no bond") == ESP_OK;
}
#endif

static void on_sync(void)
{
#ifdef RR_ROTATE_BLE_ADDR
    // Opt-in only: `idf.py -DRR_ROTATE_BLE_ADDR=1`.
    //
    // ⚠️ THIS WAS AUTOMATIC AND THAT WAS A MISTAKE. Run on every boot it turned a
    // crash loop into a pairing loop: each panic rebooted the watch, each boot saw
    // zero bonds, each boot rotated the address, and the phone was asked to pair
    // again — generation reached 7 and the parent got repeated pairing prompts for
    // what was actually a stack overflow on the ack path.
    //
    // "Zero bonds" is not by itself proof of the unrecoverable state; it is also
    // what a watch looks like moments before a legitimate first pairing, or after
    // any reboot that raced the store. Rotating identity is disruptive enough that
    // it should be a decision, not an inference.
    heal_lost_bond();
#endif

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
#ifdef RR_CLEAR_BONDS
    // ⚠️ ONE-SHOT RECOVERY BUILD: `idf.py -DRR_CLEAR_BONDS=1`.
    //
    // Wipes ONLY the BLE bond store. Deliberately NOT a factory reset: that also
    // regenerates device_id, which orphans the server-side pairing and costs a
    // QR re-registration. This leaves device_id, the paired flag, the peer anchor
    // and the whole littlefs cache alone — the phone just has to bond again,
    // which it does by connecting once.
    //
    // Flash this, boot once, then flash a normal build.
    {
        int crc = ble_store_clear();
        ESP_LOGW(TAG, "╔══ RR_CLEAR_BONDS: BLE bond store cleared (rc=%d) ══", crc);
        ESP_LOGW(TAG, "║ device_id, pairing state and the cache are UNTOUCHED.");
        ESP_LOGW(TAG, "║ Connect the phone once to re-bond, then flash a normal build.");
        ESP_LOGW(TAG, "╚═══════════════════════════════════════════════════════");
    }
#endif

    log_bond_store();

    ble_hs_id_copy_addr(s_addr_type, cur, NULL);
    ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x (%s, device_id + gen %u)",
             cur[5], cur[4], cur[3], cur[2], cur[1], cur[0],
             s_addr_type == BLE_OWN_ADDR_RANDOM ? "random-static" : "public",
             (unsigned) rr_identity_ble_generation());

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
    ble_hs_cfg.store_status_cb = bond_store_status;

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
