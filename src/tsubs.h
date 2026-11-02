// tsubs.h - text subtitles (SRT / ASS / SSA in Matroska): queue, tag clean-up and drawing.
#ifndef TSUBS_H
#define TSUBS_H

#define TQ_N      256
#define TQ_TXT    300
#define TACT_N    4

typedef struct { int64_t ts, end; char txt[TQ_TXT]; } tcue_t;
static tcue_t tq[TQ_N];
static volatile uint32_t tq_head, tq_tail;       // reader (producer) / main thread (consumer)
static tcue_t tact[TACT_N];
static int tact_n;
static tcue_t tpend;                             // reader only: a cue waiting for its BlockDuration
static int tpend_valid;
static int tdbg = 8;

// SRT: strip <tags>.  ASS/SSA: skip the 8 leading fields, strip {overrides}, \N -> newline, \h -> space.
static void tsub_clean(const uint8_t *src, uint32_t n, int ass, char *out, size_t cap) {
    uint32_t i = 0;
    size_t o = 0;
    if (ass) {
        int commas = 0;
        while (i < n && commas < 8) { if (src[i] == ',') commas++; i++; }
        if (commas < 8) { out[0] = 0; return; }
    }
    int brace = 0, angle = 0;
    for (; i < n && o + 2 < cap; i++) {
        uint8_t c = src[i];
        if (c == '\r') continue;
        if (ass) {
            if (c == '{') { brace = 1; continue; }
            if (c == '}') { brace = 0; continue; }
            if (brace) continue;
            if (c == '\\' && i + 1 < n) {
                if (src[i + 1] == 'N' || src[i + 1] == 'n') { out[o++] = '\n'; i++; continue; }
                if (src[i + 1] == 'h') { out[o++] = ' '; i++; continue; }
            }
        } else {
            if (c == '<') { angle = 1; continue; }
            if (c == '>') { angle = 0; continue; }
            if (angle) continue;
        }
        out[o++] = (char)c;
    }
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ')) o--;
    out[o] = 0;
}

static void tq_push(int64_t ts, int64_t dur, const char *txt) {
    if (!txt[0] || (tq_head - tq_tail) >= TQ_N) return;
    tcue_t *c = &tq[tq_head % TQ_N];
    c->ts = ts; c->end = ts + dur;
    snprintf(c->txt, sizeof(c->txt), "%s", txt);
    __sync_synchronize();
    tq_head = tq_head + 1;
}

static void tpend_flush(int64_t dur_us) {        // reader thread
    if (tpend_valid) {
        tpend_valid = 0;
        tq_push(tpend.ts, dur_us, tpend.txt);
        if (tdbg > 0) { tdbg--; LOGD("[tsub] cue %lld ms for %lld ms: %.60s\n", (long long)(tpend.ts / 1000), (long long)(dur_us / 1000), tpend.txt); }
    }
}

static void tsub_reset(void) {                   // reader parked
    tq_tail = tq_head;
    tact_n = 0;
    tpend_valid = 0;
}

static void tsub_advance(int64_t ts) {           // main thread, once per shown frame
    while (tq_tail != tq_head) {
        tcue_t *c = &tq[tq_tail % TQ_N];
        if (c->ts > ts) break;
        if (c->end > ts) {
            if (tact_n == TACT_N) { memmove(&tact[0], &tact[1], sizeof(tact[0]) * (TACT_N - 1)); tact_n--; }
            tact[tact_n++] = *c;
        }
        __sync_synchronize();
        tq_tail = tq_tail + 1;
    }
    for (int i = 0; i < tact_n; ) {
        if (tact[i].end <= ts) { memmove(&tact[i], &tact[i + 1], sizeof(tact[0]) * (size_t)(tact_n - i - 1)); tact_n--; }
        else i++;
    }
}

#define TL_MAX 12
static void tsub_draw(uint32_t *dst) {
    if (tact_n == 0 || !sfont_ok) return;
    char lines[TL_MAX][160];
    int nl = 0, maxw = WIDTH - 240;
    for (int i = 0; i < tact_n; i++) {
        const char *p = tact[i].txt;
        while (*p && nl < TL_MAX) {
            char ln[300]; int k = 0;
            while (*p && *p != '\n' && k < (int)sizeof(ln) - 1) ln[k++] = *p++;
            if (*p == '\n') p++;
            ln[k] = 0;
            while (ln[0] && nl < TL_MAX) {                       // greedy wrap on spaces
                int cut = (int)strlen(ln);
                while (cut > 0 && sub_text_width(ln, cut) > maxw) {
                    int s = cut - 1;
                    while (s > 0 && ln[s] != ' ') s--;
                    cut = s > 0 ? s : cut - 1;
                }
                if (cut <= 0) break;
                int m = cut < 159 ? cut : 159;
                memcpy(lines[nl], ln, (size_t)m); lines[nl][m] = 0; nl++;
                int r = cut; while (ln[r] == ' ') r++;
                memmove(ln, ln + r, strlen(ln + r) + 1);
            }
        }
    }
    int base = HEIGHT - 80;
    for (int l = nl - 1; l >= 0; l--) {
        int w = sub_text_width(lines[l], (int)strlen(lines[l]));
        draw_sub_text(dst, (WIDTH - w) / 2, base, lines[l]);
        base -= g_sub_pitch;
    }
}

#endif
