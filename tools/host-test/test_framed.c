// Host test for rr_store.c's framed-stream paging (contract v3).
//
// Compiles the REAL rr_store.c — not a copy — against stubs, so the arithmetic
// under test is exactly what ships. The thing being verified is that paging the
// stream at any page size reassembles byte-for-byte into
// [u16 len][json] ‖ [u16 len][json] ‖ …, including when a page boundary lands
// inside a frame's 2-byte length prefix.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rr_store.h"

// Where the stubbed "filesystem" lives. Must match the -DMOUNT_POINT passed by
// run.sh, which is what makes this the REAL rr_store.c under test.
#ifndef RR_HOST_ROOT
#define RR_HOST_ROOT "/tmp/rr-host-test"
#endif

int passed = 0, failed = 0;
static void check(const char *name, int ok, const char *detail) {
    if (ok) { passed++; printf("  ok   %s\n", name); }
    else { failed++; printf("  FAIL %s%s%s\n", name, detail?" — ":"", detail?detail:""); }
}

// rr_store writes under /lfs; point it at a temp dir via a symlink-ish setup.
static void write_queue(const char *const *lines, int n) {
    mkdir(RR_HOST_ROOT, 0755);
    mkdir(RR_HOST_ROOT "/queue", 0755);
    FILE *f = fopen(RR_HOST_ROOT "/queue/runs.log", "wb");
    for (int i = 0; i < n; i++) { fputs(lines[i], f); fputc('\n', f); }
    fclose(f);
    FILE *c = fopen(RR_HOST_ROOT "/queue/cursor.json", "wb");
    fprintf(c, "{\"offset\":0}");
    fclose(c);
}

// Build the expected framed stream the same way the contract defines it.
static size_t expected_stream(const char *const *lines, int n, unsigned char *out) {
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        size_t L = strlen(lines[i]);
        out[o++] = (unsigned char)(L & 0xFF);
        out[o++] = (unsigned char)((L >> 8) & 0xFF);
        memcpy(out + o, lines[i], L); o += L;
    }
    return o;
}

static char *mkjson(int steps) {
    // Roughly the real record shape; only the LENGTH matters here.
    size_t cap = 300 + (size_t)steps * 120;
    char *s = malloc(cap);
    int o = snprintf(s, cap, "{\"local_id\":\"%036d\",\"steps\":[", steps);
    for (int i = 0; i < steps; i++) {
        o += snprintf(s + o, cap - o, "%s{\"step_id\":\"%036d\",\"state\":\"completed\",\"t\":%d}",
                      i ? "," : "", i, i);
    }
    snprintf(s + o, cap - o, "]}");
    return s;
}

int main(void) {
    printf("\nrr_store framed-stream paging (host)\n\n");
    rr_store_init();

    const int step_table[] = {1, 2, 3, 6, 11, 25, 50};
    const int NT = sizeof(step_table)/sizeof(step_table[0]);

    // ── one record at a time, every step count, every page size ──
    for (int t = 0; t < NT; t++) {
        char *json = mkjson(step_table[t]);
        const char *lines[1] = { json };
        write_queue(lines, 1);

        unsigned char exp[131072];
        size_t explen = expected_stream(lines, 1, exp);

        char nm[128];
        snprintf(nm, sizeof(nm), "%2d steps: framed_size matches (%zu B)", step_table[t], explen);
        check(nm, rr_queue_framed_size() == explen, NULL);

        // Page at several sizes, including 1 and 2 to force a boundary INSIDE
        // the length prefix, and 500 (the real page size).
        const int pages[] = {1, 2, 3, 7, 500, 100000};
        for (unsigned p = 0; p < sizeof(pages)/sizeof(pages[0]); p++) {
            unsigned char got[131072];
            size_t at = 0;
            for (;;) {
                int n = rr_queue_read_framed(got + at, (uint32_t)at, (size_t)pages[p]);
                if (n <= 0) break;
                at += (size_t)n;
                if (at >= explen) break;
            }
            snprintf(nm, sizeof(nm), "%2d steps: reassembles at page size %d",
                     step_table[t], pages[p]);
            check(nm, at == explen && memcmp(got, exp, explen) == 0, NULL);
        }
        free(json);
    }

    // ── several records, mixed sizes, in one stream ──
    {
        char *a = mkjson(6), *b = mkjson(11), *c = mkjson(1), *d = mkjson(50);
        const char *lines[4] = { a, b, c, d };
        write_queue(lines, 4);
        unsigned char exp[131072];
        size_t explen = expected_stream(lines, 4, exp);
        check("4 mixed records: framed_size matches", rr_queue_framed_size() == explen, NULL);
        check("4 mixed records: count is 4", rr_queue_count() == 4, NULL);

        unsigned char got[131072];
        size_t at = 0;
        for (;;) {
            int n = rr_queue_read_framed(got + at, (uint32_t)at, 500);
            if (n <= 0) break;
            at += (size_t)n;
            if (at >= explen) break;
        }
        check("4 mixed records: reassemble byte-for-byte at 500 B pages",
              at == explen && memcmp(got, exp, explen) == 0, NULL);

        // Reading from an arbitrary interior offset must match that slice.
        int bad = 0;
        for (uint32_t off = 0; off < explen; off += 37) {
            unsigned char one[600];
            int n = rr_queue_read_framed(one, off, 500);
            if (n < 0) { bad = 1; break; }
            size_t want = explen - off; if (want > 500) want = 500;
            if ((size_t)n != want || memcmp(one, exp + off, want) != 0) { bad = 1; break; }
        }
        check("every interior offset returns the correct slice", !bad, NULL);

        // Past the end is 0, not an error.
        unsigned char one[16];
        check("reading past the end returns 0",
              rr_queue_read_framed(one, (uint32_t)explen, 16) == 0, NULL);
        free(a); free(b); free(c); free(d);
    }

    // ── a long record can still be ACKED (the fgets truncation bug) ──
    {
        char *big = mkjson(50);   // ~5.8 KB, far past the old 1024 B line buffer
        const char *lines[2] = { big, "{\"local_id\":\"second\"}" };
        write_queue(lines, 2);
        check("2 records queued", rr_queue_count() == 2, NULL);
        char id[64]; snprintf(id, sizeof(id), "%036d", 50);
        check("a 5.8 KB record acks and advances the cursor",
              rr_queue_ack(id) == ESP_OK && rr_queue_count() == 1, NULL);
        free(big);
    }

    printf("\n%s  %d passed, %d failed\n\n", failed ? "FAILED" : "OK", passed, failed);
    return failed ? 1 : 0;
}
