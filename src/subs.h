// subs.h - PGS (Blu-ray bitmap) subtitles for player.h
//
//   reader thread --(sub_push)--> subtitle packet ring --> sub thread: libavcodec "pgssub" decoder
//   --> events (ARGB bitmaps already scaled to the screen) --> main thread: sub_advance(ts) + sub_blend()
//
// Included by player.h after audio.h. Needs: P, LOG, WIDTH, HEIGHT, dmem is not used (plain malloc).

#include <libavcodec/avcodec.h>
#include <stdlib.h>

#define SQ_RING_SIZE   (4u * 1024u * 1024u)
#define SQ_N           512
#define SQ_MAX_PKT     (1024u * 1024u)
#define SEV_N          12
#define SEV_RECTS      4

typedef struct { uint32_t off, len; int64_t ts; uint64_t end; } spkt_t;
static spkt_t sq[SQ_N];
static volatile uint32_t sq_head, sq_tail;
static uint8_t *sring;
static volatile uint64_t sring_w, sring_r;

typedef struct { int x, y, w, h; uint32_t *px; } srect_t;
typedef struct { int64_t ts; int n; srect_t r[SEV_RECTS]; } sev_t;
static sev_t sev[SEV_N];
static volatile uint32_t sev_head, sev_tail;
static sev_t sub_cur;                       // what is on screen now (main thread only)

static struct {
    int strack;                             // MKV track number being shown, 0 = subtitles off
    volatile int dead, running, flush_req;
    uint32_t n_pkts, n_events, n_pushed;
} SUB;

// ---- producer side (reader thread): never blocks, a full ring just loses the packet ----
static int sub_push(const uint8_t *src, uint32_t n, int64_t ts) {
    if (n == 0 || n > SQ_MAX_PKT || SUB.dead || !sring) return 0;
    uint64_t w = sring_w;
    size_t pos = (size_t)(w % SQ_RING_SIZE);
    uint64_t pad = (pos + n > SQ_RING_SIZE) ? (SQ_RING_SIZE - pos) : 0;
    if (SQ_RING_SIZE - (w - sring_r) < pad + n || (sq_head - sq_tail) >= SQ_N) return 0;
    w += pad;
    pos = (size_t)(w % SQ_RING_SIZE);
    memcpy(sring + pos, src, n);
    spkt_t *e = &sq[sq_head % SQ_N];
    e->off = (uint32_t)pos; e->len = n; e->ts = ts; e->end = w + n;
    sring_w = w + n;
    __sync_synchronize();
    sq_head = sq_head + 1;
    return 0;
}

static void sev_free(sev_t *e) {
    for (int i = 0; i < e->n; i++) { free(e->r[i].px); e->r[i].px = NULL; }
    e->n = 0;
}

static void sub_free_cur(void) { sev_free(&sub_cur); }

// ---- consumer: decoder thread ----
static AVCodecContext *sub_ctx;
static AVPacket *sub_pkt;
static uint8_t *sub_pad;

static void sub_build_event(const AVSubtitle *s, int64_t ts) {
    sev_t *e = &sev[sev_head % SEV_N];
    memset(e, 0, sizeof(*e));
    e->ts = ts;
    int pw = sub_ctx->width > 0 ? sub_ctx->width : 1920, ph = sub_ctx->height > 0 ? sub_ctx->height : 1080;
    for (unsigned i = 0; i < s->num_rects && e->n < SEV_RECTS; i++) {
        const AVSubtitleRect *R = s->rects[i];
        if (R->type != SUBTITLE_BITMAP || R->w <= 0 || R->h <= 0 || !R->data[0] || !R->data[1]) continue;
        uint32_t pal[256];
        memset(pal, 0, sizeof(pal));
        int nc = R->nb_colors > 256 ? 256 : R->nb_colors;
        memcpy(pal, R->data[1], (size_t)nc * 4);
        // destination rectangle on the screen (the subtitle plane covers the whole screen)
        int dx = (int)((int64_t)R->x * WIDTH / pw), dy = (int)((int64_t)R->y * HEIGHT / ph);
        int dw = (int)((int64_t)R->w * WIDTH / pw), dh = (int)((int64_t)R->h * HEIGHT / ph);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        uint32_t *px = (uint32_t *)malloc((size_t)dw * (size_t)dh * 4);
        if (!px) continue;
        for (int y = 0; y < dh; y++) {
            int sy = dh == R->h ? y : (int)((int64_t)y * R->h / dh);
            const uint8_t *src = R->data[0] + (size_t)sy * (size_t)R->linesize[0];
            uint32_t *d = px + (size_t)y * (size_t)dw;
            if (dw == R->w) for (int x = 0; x < dw; x++) d[x] = pal[src[x]];
            else            for (int x = 0; x < dw; x++) d[x] = pal[src[(int)((int64_t)x * R->w / dw)]];
        }
        e->r[e->n].x = dx; e->r[e->n].y = dy; e->r[e->n].w = dw; e->r[e->n].h = dh; e->r[e->n].px = px;
        e->n++;
    }
    __sync_synchronize();
    sev_head = sev_head + 1;
    SUB.n_events++;
}

static int sub_dbg;                         // log budget (reset at every flush)
static int sub_adv_dbg;
static void sub_do_flush(void) {
    sub_dbg = 12; sub_adv_dbg = 12;
    sq_tail = sq_head;
    sring_r = sring_w;
    while (sev_tail != sev_head) { sev_free(&sev[sev_tail % SEV_N]); sev_tail = sev_tail + 1; }
    if (sub_ctx) avcodec_flush_buffers(sub_ctx);
    __sync_synchronize();
    SUB.flush_req = 0;
}

static void *sub_thread(void *arg) {
    (void)arg;
    const AVCodec *c = avcodec_find_decoder_by_name("pgssub");
    if (!c) { LOG("[sub] no PGS decoder in this build\n"); SUB.dead = 1; return NULL; }
    sub_ctx = avcodec_alloc_context3(c);
    if (!sub_ctx || avcodec_open2(sub_ctx, c, NULL) < 0) { LOG("[sub] cannot open the PGS decoder\n"); SUB.dead = 1; return NULL; }
    sub_pkt = av_packet_alloc();
    sub_pad = (uint8_t *)malloc(SQ_MAX_PKT + 64);
    if (!sub_pkt || !sub_pad) { SUB.dead = 1; return NULL; }
    SUB.running = 1;
    sub_dbg = 12; sub_adv_dbg = 12;
    LOG("[sub] PGS decoder ready\n");
    while (!P.stop_req) {
        if (SUB.flush_req) { sub_do_flush(); continue; }
        if (sq_tail == sq_head || (sev_head - sev_tail) >= SEV_N) { sceKernelUsleep(4000); continue; }
        __sync_synchronize();
        spkt_t pk = sq[sq_tail % SQ_N];
        memcpy(sub_pad, sring + pk.off, pk.len);
        memset(sub_pad + pk.len, 0, 64);
        sub_pkt->data = sub_pad; sub_pkt->size = (int)pk.len;
        AVSubtitle s;
        memset(&s, 0, sizeof(s));
        int got = 0;
        int r = avcodec_decode_subtitle2(sub_ctx, &s, &got, sub_pkt);
        sub_pkt->data = NULL; sub_pkt->size = 0;
        sring_r = pk.end;
        __sync_synchronize();
        sq_tail = sq_tail + 1;
        SUB.n_pkts++;
        if (sub_dbg > 0) {
            sub_dbg--;
            { char hx[100]; int hn = pk.len < 32 ? (int)pk.len : 32; for (int q = 0; q < hn; q++) snprintf(hx + q * 3, 4, "%02x ", sub_pad[q]); hx[hn * 3] = 0; LOGD("[sub]   bytes: %s\n", hx); }
            LOGD("[sub] pkt ts %lld ms len %u -> r=%d got=%d rects=%u fmt %dx%d\n", (long long)(pk.ts / 1000), pk.len, r, got,
                got ? s.num_rects : 0, sub_ctx->width, sub_ctx->height);
            if (got && s.num_rects > 0) LOGD("[sub]   rect0 type %d %dx%d at %d,%d colors %d\n", (int)s.rects[0]->type, s.rects[0]->w, s.rects[0]->h, s.rects[0]->x, s.rects[0]->y, s.rects[0]->nb_colors);
        }
        if (r < 0) { if (SUB.n_pkts < 4) LOG("[sub] decode error %d\n", r); continue; }
        if (got) { sub_build_event(&s, pk.ts); avsubtitle_free(&s); }
    }
    sub_do_flush();
    avcodec_free_context(&sub_ctx);
    av_packet_free(&sub_pkt);
    free(sub_pad); sub_pad = NULL;
    SUB.running = 0;
    return NULL;
}

// ---- main thread ----

// takes over every event that is due at or before frame time ts (an event without rectangles clears the screen)
static void sub_advance(int64_t ts) {
    while (sev_tail != sev_head) {
        sev_t *e = &sev[sev_tail % SEV_N];
        if (e->ts > ts) break;
        if (sub_adv_dbg > 0) { sub_adv_dbg--; LOGD("[sub] show event ts %lld ms (frame %lld ms) rects %d\n", (long long)(e->ts / 1000), (long long)(ts / 1000), e->n); }
        sub_free_cur();
        sub_cur = *e;
        memset(e, 0, sizeof(*e));
        __sync_synchronize();
        sev_tail = sev_tail + 1;
    }
}

static inline uint32_t mul255(uint32_t v) { v += 128; return (v + (v >> 8)) >> 8; }

static void sub_blend(uint32_t *dst) {
    for (int i = 0; i < sub_cur.n; i++) {
        const srect_t *r = &sub_cur.r[i];
        for (int y = 0; y < r->h; y++) {
            int yy = r->y + y;
            if (yy < 0 || yy >= HEIGHT) continue;
            const uint32_t *s = r->px + (size_t)y * (size_t)r->w;
            uint32_t *d = dst + (size_t)yy * WIDTH;
            int x0 = r->x < 0 ? -r->x : 0, x1 = r->x + r->w > WIDTH ? WIDTH - r->x : r->w;
            for (int x = x0; x < x1; x++) {
                uint32_t p = s[x], a = p >> 24;
                if (a == 0) continue;
                uint32_t *o = &d[r->x + x];
                if (a == 255) { *o = p | 0xFF000000u; continue; }
                uint32_t q = *o, ia = 255 - a;
                uint32_t rr = mul255(((p >> 16) & 255) * a + ((q >> 16) & 255) * ia);
                uint32_t gg = mul255(((p >> 8) & 255) * a + ((q >> 8) & 255) * ia);
                uint32_t bb = mul255((p & 255) * a + (q & 255) * ia);
                *o = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
}
