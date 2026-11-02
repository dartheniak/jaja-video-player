// audio.h - audio playback for player.h
//
//   reader thread --(aq_push)--> audio packet ring --> audio thread: FFmpeg decode (adec.h) -> stereo s16
//   -> sceAudioOutOutput, 1024-sample grains. The audio thread follows the video clock (P.base_us / P.ts0):
//   if the audio runs ahead it inserts silence, if it runs behind it skips samples.
//
// Included by player.h after the global state P is declared. Needs: P (started, stop_req, paused,
// stall_start, base_us, ts0), LOG, dmem_alloc(), sceKernelGetProcessTime(), sceKernelUsleep().

#include "adec.h"
#ifndef AUD_HOST_TEST
#include <orbis/AudioOut.h>
#endif

#define AQ_RING_SIZE   (8u * 1024u * 1024u)
#define AQ_N           4096
#define AQ_MAX_PKT     (64u * 1024u)
#define AUD_GRAIN      1024                // samples per sceAudioOutOutput call (21.3 ms at 48 kHz)
#define AUD_RATE       48000
#define AUD_QUEUE_US   32000               // how far behind the submitted data the speakers are
#define AUD_TH_US      30000               // resync when more than this far off
#define AUD_BUF_MAX    20000               // samples per decoded packet after resampling

typedef struct { uint32_t off, len; int64_t ts; uint64_t end; } apkt_t;
static apkt_t aq[AQ_N];
static volatile uint32_t aq_head, aq_tail;
static uint8_t *aring;
static volatile uint64_t aring_w, aring_r;

static int g_user_id;                      // set by main()
static int64_t aud_adv_us;                 // user adjustment: > 0 plays the audio earlier (kept between files)

static struct {
    int atrack;                            // MKV track number, 0 = no audio
    int codec;                             // ADEC_*
    int rate, chans;
    uint8_t extra[8192]; int extra_len;
    char name[24];
    volatile int dead;                     // audio thread could not start / died: reader discards audio
    volatile int running;
    volatile int flush_req;                // set by the main thread after a seek
    volatile int reopen;                   // the track changed: build a new decoder at the next flush
    uint32_t n_zero; uint32_t frame_us; int lace_logged; uint32_t n_underrun, n_inserted_ms, n_skipped_ms, n_decode_err, n_pkts;
    int64_t last_err_us;
} AU;

// ---- producer side (reader thread) ----
static int aq_push(const uint8_t *src, uint32_t n, int64_t ts) {
    if (n == 0 || n > AQ_MAX_PKT) return -1;
    uint64_t w; size_t pos;
    for (;;) {
        if (P.stop_req) return -3;
        if (P.seek_req != P.seek_parked) return -4;
        if (AU.dead) return 0;
        w = aring_w;
        pos = (size_t)(w % AQ_RING_SIZE);
        uint64_t pad = (pos + n > AQ_RING_SIZE) ? (AQ_RING_SIZE - pos) : 0;
        if (AQ_RING_SIZE - (w - aring_r) >= pad + n && (aq_head - aq_tail) < AQ_N) {
            w += pad;
            pos = (size_t)(w % AQ_RING_SIZE);
            break;
        }
        sceKernelUsleep(3000);
    }
    memcpy(aring + pos, src, n);
    apkt_t *e = &aq[aq_head % AQ_N];
    e->off = (uint32_t)pos; e->len = n; e->ts = ts; e->end = w + n;
    aring_w = w + n;
    __sync_synchronize();
    aq_head = aq_head + 1;
    return 0;
}

// ---- consumer side (audio thread) ----
static adec_t  au_dec;
static float   au_tmp[2 * 16384];
static int16_t au_buf[2 * AUD_BUF_MAX];
static int     au_n, au_pos;               // decoded samples in au_buf, read cursor
static int64_t au_buf_ts;                  // timestamp (us) of au_buf[0]
static int64_t au_anc_ts, au_anc_cnt;
static int     au_have_anchor;
static int     au_reanchors;
static double  rs_x, rs_step;              // linear resampler state
static float   rs_last[2];

static inline int16_t f2s(float v) {
    v *= 32767.0f;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

// converts n interleaved stereo float frames into au_buf (resampling to 48 kHz when needed)
static int au_convert(const float *in, int n) {
    int o = 0;
    if (AU.rate == AUD_RATE || rs_step == 0) {
        if (n > AUD_BUF_MAX) n = AUD_BUF_MAX;
        for (int i = 0; i < 2 * n; i++) au_buf[i] = f2s(in[i]);
        return n;
    }
    while (rs_x < n - 1 && o < AUD_BUF_MAX) {
        int i0 = (int)rs_x;                     // may be -1: the last sample of the previous packet
        float fr = (float)(rs_x - (double)i0);
        for (int c = 0; c < 2; c++) {
            float a = i0 < 0 ? rs_last[c] : in[2 * i0 + c];
            float b = in[2 * (i0 + 1) + c];
            au_buf[2 * o + c] = f2s(a + (b - a) * fr);
        }
        o++;
        rs_x += rs_step;
    }
    rs_x -= n;
    rs_last[0] = in[2 * (n - 1)]; rs_last[1] = in[2 * (n - 1) + 1];
    return o;
}

// takes the next packet from the ring and decodes it into au_buf; 0 = nothing available
static int au_fetch(void) {
    for (;;) {
        if (aq_tail == aq_head) return 0;
        __sync_synchronize();
        apkt_t pk = aq[aq_tail % AQ_N];
        int n = adec_decode(&au_dec, aring + pk.off, (int)pk.len, au_tmp, 16384);
        aring_r = pk.end;
        __sync_synchronize();
        aq_tail = aq_tail + 1;
        AU.n_pkts++;
        if (AU.n_pkts <= 4)
            LOGD("[audio] packet %u: ts %lld ms, %u bytes, head %02x %02x %02x %02x %02x %02x -> decoded %d samples\n", AU.n_pkts,
                (long long)(pk.ts / 1000), pk.len, aring[pk.off], aring[pk.off + 1], aring[pk.off + 2], aring[pk.off + 3],
                aring[pk.off + 4], aring[pk.off + 5], n);
        if (n == 0) AU.n_zero++;
        if (n < 0) { if (AU.n_decode_err++ < 5) LOG("[audio] decode error %d\n", n); continue; }
        if (n == 0) continue;
        int m = au_convert(au_tmp, n);
        if (m <= 0) continue;
        int64_t exp = au_anc_ts + au_anc_cnt * 1000000 / AUD_RATE;
        int64_t d = pk.ts - exp;
        if (!au_have_anchor || d > 100000 || d < -100000) {
            if (au_reanchors++ < 12)
                LOGD("[audio] anchor: packet ts %lld ms (expected %lld), video ts0 %lld ms, video clock now %lld ms\n", (long long)(pk.ts / 1000),
                    (long long)(exp / 1000), (long long)(P.ts0 / 1000),
                    (long long)((P.ts0 + (int64_t)sceKernelGetProcessTime() - (int64_t)P.base_us) / 1000));
            au_anc_ts = pk.ts; au_anc_cnt = 0; au_have_anchor = 1;
        }
        au_buf_ts = au_anc_ts + au_anc_cnt * 1000000 / AUD_RATE;
        au_anc_cnt += m;
        au_n = m; au_pos = 0;
        return 1;
    }
}

// copies up to want sample frames into dst (dst == NULL: just discard them); returns how many were available
static int au_pull(int16_t *dst, int want) {
    int done = 0;
    while (done < want) {
        if (au_pos >= au_n && !au_fetch()) break;
        int k = au_n - au_pos;
        if (k > want - done) k = want - done;
        if (dst) memcpy(dst + 2 * done, au_buf + 2 * au_pos, (size_t)k * 4);
        au_pos += k; done += k;
    }
    return done;
}

// drops everything queued (the reader is parked while this runs) and forgets the stream position
static void au_do_flush(void) {
    aq_tail = aq_head;
    aring_r = aring_w;
    au_n = au_pos = 0; au_have_anchor = 0; au_anc_cnt = 0; au_anc_ts = 0;
    rs_x = 0; rs_last[0] = rs_last[1] = 0;
    if (AU.reopen) {                                    // another audio track was chosen
        adec_close(&au_dec);
        if (adec_open(&au_dec, AU.codec, AU.rate, AU.chans, AU.extra, AU.extra_len) < 0) {
            LOG("[audio] cannot open the %s decoder for the new track\n", AU.name);
            AU.dead = 1;
        }
        rs_step = (AU.rate > 0 && AU.rate != AUD_RATE) ? (double)AU.rate / (double)AUD_RATE : 0;
        LOG("[audio] switched to %s (%d Hz, %d ch)\n", AU.name, AU.rate, AU.chans);
        AU.reopen = 0;
    } else adec_flush(&au_dec);
    __sync_synchronize();
    AU.flush_req = 0;
}

static void *audio_thread(void *arg) {
    (void)arg;
    int16_t grain[2 * AUD_GRAIN];
    int h = -1, starving = 0;
    uint64_t last_log = 0;

    memset(&au_dec, 0, sizeof(au_dec));
    int rc = adec_open(&au_dec, AU.codec, AU.rate, AU.chans, AU.extra, AU.extra_len);
    if (rc < 0) { LOG("[audio] cannot open the %s decoder (%d)\n", AU.name, rc); AU.dead = 1; return NULL; }
    au_n = au_pos = 0; au_have_anchor = 0; au_reanchors = 0; au_anc_cnt = 0; au_anc_ts = 0;
    rs_x = 0; rs_last[0] = rs_last[1] = 0;
    rs_step = (AU.rate > 0 && AU.rate != AUD_RATE) ? (double)AU.rate / (double)AUD_RATE : 0;
    LOG("[audio] decoder %s ready (%d Hz, %d ch)\n", AU.name, AU.rate, AU.chans);

    while (!P.started && !P.stop_req) { if (AU.flush_req) au_do_flush(); sceKernelUsleep(4000); }
    if (P.stop_req) goto out;

#ifndef AUD_HOST_TEST
    LOG("[audio] sceAudioOutInit -> 0x%x\n", (unsigned)sceAudioOutInit());
#endif
    {   // the user id that works varies; 0xFF is "system" and is what homebrew samples use
        static const int ids[3] = { 0xFF, 0, 0 };
        int tries[3] = { ids[0], g_user_id, ids[1] };
        unsigned grains[1] = { AUD_GRAIN };
        for (int gi = 0; gi < 1 && h < 0; gi++)
            for (int i = 0; i < 3 && h < 0; i++) {
                h = sceAudioOutOpen(tries[i], ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, grains[gi], AUD_RATE,
                                    ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                LOG("[audio] sceAudioOutOpen(user 0x%x, grain %u) -> 0x%x\n", tries[i], grains[gi], (unsigned)h);
            }
    }
    if (h < 0) { AU.dead = 1; goto out; }
    AU.running = 1;

    while (!P.stop_req) {
        if (AU.flush_req) au_do_flush();
        int64_t now = (int64_t)sceKernelGetProcessTime();
        int fill = 0;
        int hold = P.paused || P.stall_start || !P.started;

        if (!hold && !au_have_anchor && !au_fetch()) hold = 1;       // wait for the first packet, then align
        if (!hold) {
            if (au_have_anchor) {
                int64_t S = au_buf_ts + (int64_t)au_pos * 1000000 / AUD_RATE;          // next sample to submit
                int64_t W = P.ts0 + (now - (int64_t)P.base_us);                       // what the video shows now
                int64_t e = (S - AUD_QUEUE_US) - (W + aud_adv_us);                    // > 0: audio is ahead
                AU.last_err_us = e;
                if (e > AUD_TH_US) {
                    int pad = (int)(e * AUD_RATE / 1000000);
                    if (pad > AUD_GRAIN) pad = AUD_GRAIN;
                    memset(grain, 0, (size_t)pad * 4);
                    fill = pad;
                    AU.n_inserted_ms += (uint32_t)(pad * 1000 / AUD_RATE);
                } else if (e < -AUD_TH_US) {
                    int skip = (int)((-e) * AUD_RATE / 1000000);
                    if (skip > 4 * AUD_GRAIN) skip = 4 * AUD_GRAIN;
                    int got = au_pull(NULL, skip);
                    AU.n_skipped_ms += (uint32_t)(got * 1000 / AUD_RATE);
                }
            }
            int got = au_pull(grain + 2 * fill, AUD_GRAIN - fill);
            fill += got;
            if (fill < AUD_GRAIN) {
                if (!starving && got == 0 && fill == 0) { AU.n_underrun++; starving = 1; }
            } else starving = 0;
        }
        if (fill < AUD_GRAIN) memset(grain + 2 * fill, 0, (size_t)(AUD_GRAIN - fill) * 4);

        int r = sceAudioOutOutput(h, grain);

        if (r < 0) sceKernelUsleep(20000);

        uint64_t t = sceKernelGetProcessTime();
        if (t - last_log >= 10000000ULL) {
            last_log = t;
            LOGD("[audio] err %lld ms, shift %lld ms, queued %u packets, underruns %u, inserted %u ms, skipped %u ms, decode errors %u, pushed %u, consumed %u, empty-output %u\n",
                (long long)(AU.last_err_us / 1000), (long long)(aud_adv_us / 1000), (unsigned)(aq_head - aq_tail), AU.n_underrun,
                AU.n_inserted_ms, AU.n_skipped_ms, AU.n_decode_err, (unsigned)aq_head, AU.n_pkts, AU.n_zero);
        }
    }
out:
    if (h >= 0) sceAudioOutClose(h);
    adec_close(&au_dec);
    AU.running = 0;
    return NULL;
}
