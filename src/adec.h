/*
 * adec.h - thin audio decoder wrapper around FFmpeg's libavcodec (LGPL).
 * Decodes E-AC-3 / AC-3 / AAC / Opus / DTS (core) / FLAC / MP3 / MP2 / Vorbis packets into interleaved float
 * stereo (multichannel is downmixed here) at the stream's sample rate.
 */
#ifndef ADEC_H
#define ADEC_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

#define ADEC_EAC3   1
#define ADEC_AC3    2
#define ADEC_AAC    3
#define ADEC_OPUS   4
#define ADEC_DTS    5
#define ADEC_FLAC   6
#define ADEC_MP3    7
#define ADEC_MP2    8
#define ADEC_VORBIS 9

#define ADEC_MAXCH  8

typedef struct {
    AVCodecContext *ctx;
    AVPacket       *pkt;
    AVFrame        *frm;
    uint8_t        *padbuf;
    int             padcap;
    float          *pl[ADEC_MAXCH];   /* planar float copies for formats that are not already FLTP */
    int             plcap;
    int             sample_rate;
    int             channels;    /* channels as decoded */
} adec_t;

static const char *adec_name(int codec)
{
    switch (codec) {
    case ADEC_EAC3:   return "eac3";
    case ADEC_AC3:    return "ac3";
    case ADEC_OPUS:   return "opus";
    case ADEC_DTS:    return "dca";
    case ADEC_FLAC:   return "flac";
    case ADEC_MP3:    return "mp3float";
    case ADEC_MP2:    return "mp2float";
    case ADEC_VORBIS: return "vorbis";
    default:          return "aac";
    }
}

static int adec_open(adec_t *a, int codec, int sample_rate, int channels,
                     const uint8_t *extra, int extra_len)
{
    const AVCodec *c = avcodec_find_decoder_by_name(adec_name(codec));
    memset(a, 0, sizeof(*a));
    if (!c) return -1;
    a->ctx = avcodec_alloc_context3(c);
    if (!a->ctx) return -2;
    a->ctx->sample_rate = sample_rate;
    av_channel_layout_default(&a->ctx->ch_layout, channels > 0 ? channels : 2);
    if (extra && extra_len > 0) {
        a->ctx->extradata = av_mallocz(extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!a->ctx->extradata) return -3;
        memcpy(a->ctx->extradata, extra, extra_len);
        a->ctx->extradata_size = extra_len;
    }
    if (codec == ADEC_DTS && a->ctx->priv_data)       /* DTS-HD: use the core only (the lossless extension is far too heavy) */
        av_opt_set(a->ctx->priv_data, "core_only", "1", 0);
    if (avcodec_open2(a->ctx, c, NULL) < 0) return -4;
    a->pkt = av_packet_alloc();
    a->frm = av_frame_alloc();
    if (!a->pkt || !a->frm) return -5;
    return 0;
}

static void adec_close(adec_t *a)
{
    int i;
    if (a->frm) av_frame_free(&a->frm);
    if (a->pkt) av_packet_free(&a->pkt);
    if (a->ctx) avcodec_free_context(&a->ctx);
    free(a->padbuf);
    for (i = 0; i < ADEC_MAXCH; i++) free(a->pl[i]);
    memset(a, 0, sizeof(*a));
}

static void adec_flush(adec_t *a) { if (a->ctx) avcodec_flush_buffers(a->ctx); }

static inline float adec_clip(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

/* Gives planar float access to the channels of the current frame (converting when the decoder's format differs).
 * Returns 0 on success. */
static int adec_planes(adec_t *a, const float *p[ADEC_MAXCH], int ch)
{
    AVFrame *f = a->frm;
    int n = f->nb_samples, c, i;
    enum AVSampleFormat fmt = (enum AVSampleFormat)f->format;
    if (fmt == AV_SAMPLE_FMT_FLTP) {
        for (c = 0; c < ch; c++) p[c] = (const float *)f->extended_data[c];
        return 0;
    }
    if (a->plcap < n) {
        for (c = 0; c < ADEC_MAXCH; c++) { free(a->pl[c]); a->pl[c] = (float *)malloc((size_t)n * sizeof(float)); if (!a->pl[c]) { a->plcap = 0; return -1; } }
        a->plcap = n;
    }
    for (c = 0; c < ch; c++) {
        float *o = a->pl[c];
        switch (fmt) {
        case AV_SAMPLE_FMT_S16P: { const int16_t *s = (const int16_t *)f->extended_data[c]; for (i = 0; i < n; i++) o[i] = s[i] * (1.0f / 32768.0f); } break;
        case AV_SAMPLE_FMT_S32P: { const int32_t *s = (const int32_t *)f->extended_data[c]; for (i = 0; i < n; i++) o[i] = s[i] * (1.0f / 2147483648.0f); } break;
        case AV_SAMPLE_FMT_FLT:  { const float *s = (const float *)f->extended_data[0] + c; for (i = 0; i < n; i++) o[i] = s[(size_t)i * ch]; } break;
        case AV_SAMPLE_FMT_S16:  { const int16_t *s = (const int16_t *)f->extended_data[0] + c; for (i = 0; i < n; i++) o[i] = s[(size_t)i * ch] * (1.0f / 32768.0f); } break;
        case AV_SAMPLE_FMT_S32:  { const int32_t *s = (const int32_t *)f->extended_data[0] + c; for (i = 0; i < n; i++) o[i] = s[(size_t)i * ch] * (1.0f / 2147483648.0f); } break;
        default: return -2;
        }
        p[c] = o;
    }
    return 0;
}

/* Convert the frame in a->frm to interleaved stereo float, appended at out.
 * Returns the number of sample frames written (0 if out has no room). */
static int adec_frame_to_stereo(adec_t *a, float *out, int room)
{
    AVFrame *f = a->frm;
    int n = f->nb_samples, ch = f->ch_layout.nb_channels, i;
    const float *p[ADEC_MAXCH];
    if (n > room) return 0;
    a->sample_rate = f->sample_rate;
    a->channels = ch;
    if (ch < 1) return 0;
    if (ch > ADEC_MAXCH) ch = ADEC_MAXCH;
    if (adec_planes(a, p, ch) != 0) return 0;
    if (ch == 1) {
        for (i = 0; i < n; i++) out[2*i] = out[2*i+1] = adec_clip(p[0][i]);
    } else if (ch == 2) {
        for (i = 0; i < n; i++) { out[2*i] = adec_clip(p[0][i]); out[2*i+1] = adec_clip(p[1][i]); }
    } else if (ch >= 6) {
        /* FFmpeg native order: FL FR FC LFE BL BR (SL SR for 7.1) */
        const float k = 0.70710678f, g = 0.70710678f;
        for (i = 0; i < n; i++) {
            float c = p[2][i] * k, lfe = p[3][i] * 0.5f, bl, br;
            if (ch >= 8) { bl = 0.5f * (p[4][i] + p[6][i]); br = 0.5f * (p[5][i] + p[7][i]); }
            else         { bl = p[4][i] * k;                br = p[5][i] * k; }
            out[2*i]   = adec_clip(g * (p[0][i] + c + bl + lfe));
            out[2*i+1] = adec_clip(g * (p[1][i] + c + br + lfe));
        }
    } else {
        /* 3/4/5 channels: use front pair + centre only */
        for (i = 0; i < n; i++) {
            float c = ch >= 3 ? p[2][i] * 0.70710678f : 0.0f;
            out[2*i]   = adec_clip(0.8f * (p[0][i] + c));
            out[2*i+1] = adec_clip(0.8f * (p[1][i] + c));
        }
    }
    return n;
}

/* Decode one packet. Writes interleaved stereo float to out (capacity
 * out_room sample frames). Returns number of sample frames produced, <0 on error. */
static int adec_decode(adec_t *a, const uint8_t *data, int size, float *out, int out_room)
{
    int total = 0, r;
    if (size <= 0) return 0;
    if (a->padcap < size + AV_INPUT_BUFFER_PADDING_SIZE) {
        free(a->padbuf);
        a->padcap = size + AV_INPUT_BUFFER_PADDING_SIZE + 4096;
        a->padbuf = (uint8_t *)malloc(a->padcap);
        if (!a->padbuf) { a->padcap = 0; return -1; }
    }
    memcpy(a->padbuf, data, size);
    memset(a->padbuf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    a->pkt->data = a->padbuf;
    a->pkt->size = size;
    r = avcodec_send_packet(a->ctx, a->pkt);
    a->pkt->data = NULL; a->pkt->size = 0;
    if (r < 0) return -2;
    for (;;) {
        r = avcodec_receive_frame(a->ctx, a->frm);
        if (r < 0) break;
        total += adec_frame_to_stereo(a, out + 2 * total, out_room - total);
        av_frame_unref(a->frm);
    }
    return total;
}

#endif
