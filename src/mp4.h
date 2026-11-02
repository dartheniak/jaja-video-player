// mp4.h - ISO base media (MP4 / M4V / MOV) demuxer for the JajaSAMBA player.
// The index (moov) is read once and turned into one sample table per track; the reader then walks the
// tracks in file-offset order, so the SMB reads stay sequential.  Included by player.h after the reader helpers.
#ifndef MP4_H
#define MP4_H

#define MP_MAXTR 10

typedef struct { uint64_t off; int64_t ts; uint32_t size, key; } msamp_t;

typedef struct {
    int id, type;                       // type: 1 video, 2 audio, 17 subtitle, 0 other
    int tidx;                           // index into trk[] (audio / subtitle), -1 otherwise
    uint32_t timescale;
    char lang[8];
    char fcc[5];                        // sample entry format
    uint64_t p_stsd, s_stsd, p_stts, s_stts, p_ctts, s_ctts, p_stss, s_stss, p_stsc, s_stsc, p_stsz, s_stsz, p_stco, s_stco;
    int co64;
    int rot;                            // clockwise quarter turns from the tkhd matrix
    int use;                            // fragmented files: this track gets a sample index
    int64_t mt, empty_us;               // edit list: media time of the first real edit, leading empty edit in us
    uint32_t n;                         // samples
    msamp_t *s;
    uint32_t cur;
    int ok;                             // supported and fully indexed
} mtr_t;

typedef struct { uint32_t id, dur, size, flags; } trex_t;      // fragmented files: per-track defaults (mvex/trex)
static trex_t trex[MP_MAXTR];
static int ntrex;
static mtr_t mtr[MP_MAXTR];
static int nmtr, mp_video = -1;
static uint32_t mp_movie_ts = 1000;
static uint8_t *mp_arena;
static size_t mp_arena_used;

static uint32_t mp_u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t mp_u64(const uint8_t *p) { return ((uint64_t)mp_u32(p) << 32) | mp_u32(p + 4); }
static uint32_t mp_u16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

// ---- box walking ----
// returns 0 and the box layout, -1 at the end / on a broken box
static int mp_box(uint64_t pos, uint64_t end, uint32_t *type, uint64_t *payload, uint64_t *psize) {
    uint8_t h[16];
    if (pos + 8 > end) return -1;
    int n = rd_at(pos, h, 16 < end - pos ? 16 : (size_t)(end - pos));
    if (n < 8) return -1;
    uint64_t sz = mp_u32(h);
    int hl = 8;
    *type = mp_u32(h + 4);
    if (sz == 1) { if (n < 16) return -1; sz = mp_u64(h + 8); hl = 16; }
    else if (sz == 0) sz = end - pos;
    if (sz < (uint64_t)hl || pos + sz > end) { if (pos + sz > end && sz >= (uint64_t)hl) sz = end - pos; else return -1; }
    *payload = pos + (uint64_t)hl;
    *psize = sz - (uint64_t)hl;
    return 0;
}

#define FCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static void mp_lang(mtr_t *t, uint32_t packed) {
    // packed < 0x400 = unset or a QuickTime (Mac) language code
    if (packed < 0x400 || packed == 0x7FFF) { snprintf(t->lang, sizeof(t->lang), "und"); return; }
    t->lang[0] = (char)(((packed >> 10) & 31) + 0x60); t->lang[1] = (char)(((packed >> 5) & 31) + 0x60);
    t->lang[2] = (char)((packed & 31) + 0x60); t->lang[3] = 0;
}

static void mp_walk(uint64_t pos, uint64_t end, int depth, mtr_t *t) {
    uint32_t type; uint64_t pl, ps;
    uint8_t b[64];
    while (mp_box(pos, end, &type, &pl, &ps) == 0) {
        uint64_t next = pl + ps;
        switch (type) {
        case FCC('m','o','o','v'): case FCC('m','d','i','a'): case FCC('m','i','n','f'): case FCC('s','t','b','l'): case FCC('e','d','t','s'): case FCC('m','v','e','x'):
            if (depth < 8) mp_walk(pl, next, depth + 1, t);
            break;
        case FCC('t','r','a','k'):
            if (nmtr < MP_MAXTR && depth < 8) {
                mtr_t *nt = &mtr[nmtr];
                memset(nt, 0, sizeof(*nt));
                nt->tidx = -1; snprintf(nt->lang, sizeof(nt->lang), "und");
                mp_walk(pl, next, depth + 1, nt);
                nmtr++;
            }
            break;
        case FCC('t','r','e','x'):
            if (ps >= 24 && ntrex < MP_MAXTR && rd_at(pl, b, 24) >= 24) {
                trex_t *x = &trex[ntrex++];
                x->id = mp_u32(b + 4); x->dur = mp_u32(b + 12); x->size = mp_u32(b + 16); x->flags = mp_u32(b + 20);
            }
            break;
        case FCC('m','v','h','d'):
            if (ps >= 28 && rd_at(pl, b, 32) >= 28) {
                int v = b[0];
                uint64_t dur;
                if (v == 1) { mp_movie_ts = mp_u32(b + 20); dur = mp_u64(b + 24); }
                else { mp_movie_ts = mp_u32(b + 12); dur = mp_u32(b + 16); }
                if (!mp_movie_ts) mp_movie_ts = 1000;
                P.duration_us = (int64_t)(dur * 1000000ULL / mp_movie_ts);
            }
            break;
        case FCC('t','k','h','d'):
            if (t && ps >= 24 && rd_at(pl, b, 32) >= 24) t->id = (int)(b[0] == 1 ? mp_u32(b + 20) : mp_u32(b + 12));
            if (t) {                                                      // rotation flag: matrix a b u / c d v / x y w (16.16)
                uint8_t tb[96];
                int vv = b[0];
                uint64_t mo = vv == 1 ? 52 : 40;
                if (ps >= mo + 36 && rd_at(pl, tb, (size_t)(mo + 36)) >= (int)(mo + 36)) {
                    int32_t ma = (int32_t)mp_u32(tb + mo), mb = (int32_t)mp_u32(tb + mo + 4), mc = (int32_t)mp_u32(tb + mo + 12), md = (int32_t)mp_u32(tb + mo + 16);
                    t->rot = 0;
                    if (ma == 0 && md == 0 && mb > 0 && mc < 0) t->rot = 1;
                    else if (ma < 0 && md < 0 && mb == 0 && mc == 0) t->rot = 2;
                    else if (ma == 0 && md == 0 && mb < 0 && mc > 0) t->rot = 3;
                }
            }
            break;
        case FCC('e','l','s','t'):
            if (t && ps >= 8 && rd_at(pl, b, 40) >= 8) {
                int v = b[0];
                uint32_t cnt = mp_u32(b + 4);
                uint64_t q = pl + 8;
                t->mt = 0; t->empty_us = 0;
                int got_real = 0;
                for (uint32_t i = 0; i < cnt && i < 8 && !got_real; i++) {
                    uint8_t e[20];
                    size_t el = v == 1 ? 20 : 12;
                    if (rd_at(q, e, el) < (int)el) break;
                    q += el;
                    uint64_t segd; int64_t mt;
                    if (v == 1) { segd = mp_u64(e); mt = (int64_t)mp_u64(e + 8); }
                    else { segd = mp_u32(e); mt = (int32_t)mp_u32(e + 4); }
                    if (mt == -1) t->empty_us += (int64_t)(segd * 1000000ULL / mp_movie_ts);
                    else { t->mt = mt; got_real = 1; }
                }
            }
            break;
        case FCC('m','d','h','d'):
            if (t && ps >= 24 && rd_at(pl, b, 40) >= 24) {
                if (b[0] == 1) { t->timescale = mp_u32(b + 20); mp_lang(t, mp_u16(b + 32)); }
                else { t->timescale = mp_u32(b + 12); mp_lang(t, mp_u16(b + 20)); }
            }
            break;
        case FCC('h','d','l','r'):
            if (t && ps >= 12 && rd_at(pl, b, 16) >= 12) {
                uint32_t h = mp_u32(b + 8);
                if (h == FCC('v','i','d','e')) t->type = 1;
                else if (h == FCC('s','o','u','n')) t->type = 2;
                else if (h == FCC('s','b','t','l') || h == FCC('t','e','x','t') || h == FCC('s','u','b','t')) t->type = 17;
            }
            break;
        case FCC('s','t','s','d'): if (t) { t->p_stsd = pl; t->s_stsd = ps; } break;
        case FCC('s','t','t','s'): if (t) { t->p_stts = pl; t->s_stts = ps; } break;
        case FCC('c','t','t','s'): if (t) { t->p_ctts = pl; t->s_ctts = ps; } break;
        case FCC('s','t','s','s'): if (t) { t->p_stss = pl; t->s_stss = ps; } break;
        case FCC('s','t','s','c'): if (t) { t->p_stsc = pl; t->s_stsc = ps; } break;
        case FCC('s','t','s','z'): if (t) { t->p_stsz = pl; t->s_stsz = ps; } break;
        case FCC('s','t','c','o'): if (t) { t->p_stco = pl; t->s_stco = ps; t->co64 = 0; } break;
        case FCC('c','o','6','4'): if (t) { t->p_stco = pl; t->s_stco = ps; t->co64 = 1; } break;
        default: break;
        }
        if (next <= pos) break;
        pos = next;
    }
}

// ---- streaming reader over one table ----
typedef struct { uint64_t pos, end; uint8_t buf[65536]; uint32_t bi, bn; int err; } mrd_t;
static mrd_t mrd;

static void mrd_open(uint64_t pos, uint64_t size) { mrd.pos = pos; mrd.end = pos + size; mrd.bi = mrd.bn = 0; mrd.err = 0; }
static uint32_t mrd_u32(void) {
    if (mrd.bi + 4 > mrd.bn) {
        uint32_t keep = mrd.bn - mrd.bi, tmp[1] = {0};
        (void)tmp;
        if (keep) memmove(mrd.buf, mrd.buf + mrd.bi, keep);
        uint64_t left = mrd.end - mrd.pos;
        size_t want = sizeof(mrd.buf) - keep;
        if (want > left) want = (size_t)left;
        if (want == 0) { mrd.err = 1; mrd.bi = mrd.bn = 0; return 0; }
        int n = rd_at(mrd.pos, mrd.buf + keep, want);
        if (n <= 0) { mrd.err = 1; mrd.bi = mrd.bn = 0; return 0; }
        mrd.pos += (uint64_t)n;
        mrd.bn = keep + (uint32_t)n; mrd.bi = 0;
        if (mrd.bn < 4) { mrd.err = 1; return 0; }
    }
    uint32_t v = mp_u32(mrd.buf + mrd.bi);
    mrd.bi += 4;
    return v;
}
static uint64_t mrd_u64(void) { uint64_t hi = mrd_u32(); return (hi << 32) | mrd_u32(); }

// ---- sample entry (stsd) ----
static const uint8_t *mp_find_box(const uint8_t *p, size_t n, uint32_t want, size_t *len) {
    size_t o = 0;
    while (o + 8 <= n) {
        uint32_t sz = mp_u32(p + o), ty = mp_u32(p + o + 4);
        if (sz < 8 || o + sz > n) break;
        if (ty == want) { *len = sz - 8; return p + o + 8; }
        o += sz;
    }
    return NULL;
}

// MPEG-4 descriptor walk: returns the AudioSpecificConfig of an esds payload
static const uint8_t *mp_esds_asc(const uint8_t *p, size_t n, size_t *len, int *objtype) {
    size_t o = 4;                                  // version + flags
    *objtype = 0;
    while (o + 2 <= n) {
        int tag = p[o++];
        size_t l = 0; int k = 0, b;
        do { if (o >= n) return NULL; b = p[o++]; l = (l << 7) | (size_t)(b & 0x7F); k++; } while ((b & 0x80) && k < 4);
        if (tag == 0x03) {                                                       // ES_Descr: ES_ID, flags, optional fields
            if (o + 3 > n) return NULL;
            int fl = p[o + 2];
            o += 3;
            if (fl & 0x80) o += 2;
            if (fl & 0x40) { if (o >= n) return NULL; o += 1 + p[o]; }
            if (fl & 0x20) o += 2;
            continue;
        }
        if (tag == 0x04) { if (o + 13 > n) return NULL; *objtype = p[o]; o += 13; continue; }
        if (tag == 0x05) { if (o + l > n) return NULL; *len = l; return p + o; }
        o += l;
    }
    return NULL;
}

static void mp_register_tracks(void) {
    static uint8_t sd[4096];
    int first_aud = 1;
    for (int i = 0; i < nmtr; i++) {
        mtr_t *t = &mtr[i];
        if (t->type == 0 || !t->timescale || !t->s_stsd) continue;
        size_t sn = t->s_stsd > sizeof(sd) ? sizeof(sd) : (size_t)t->s_stsd;
        if (rd_at(t->p_stsd, sd, sn) < (int)sn || sn < 16) continue;
        uint32_t esz = mp_u32(sd + 8);
        uint32_t fmt = mp_u32(sd + 12);
        if (esz > sn - 8) esz = (uint32_t)sn - 8;
        const uint8_t *e = sd + 8;                 // the sample entry
        t->fcc[0] = (char)(fmt >> 24); t->fcc[1] = (char)(fmt >> 16); t->fcc[2] = (char)(fmt >> 8); t->fcc[3] = (char)fmt; t->fcc[4] = 0;
        if (t->type == 1) {
            if (mp_video >= 0) continue;
            int is_avc = (fmt == FCC('a','v','c','1') || fmt == FCC('a','v','c','3'));
            int is_hevc = (fmt == FCC('h','v','c','1') || fmt == FCC('h','e','v','1'));
            LOG("[mp4] video track %d: '%s', timescale %u\n", t->id, t->fcc, t->timescale);
            if (!is_avc && !is_hevc) {
                snprintf(P.err, sizeof(P.err), "Unsupported video codec '%s' (only H.264 and HEVC can be hardware decoded)", t->fcc);
                continue;
            }
            if (esz < 86 + 8) continue;
            int w = (int)mp_u16(e + 32), h = (int)mp_u16(e + 34);
            size_t cl;
            const uint8_t *cfg = mp_find_box(e + 86, esz - 86, is_avc ? FCC('a','v','c','C') : FCC('h','v','c','C'), &cl);
            if (!cfg || cl < (is_avc ? 7u : 23u)) { snprintf(P.err, sizeof(P.err), "No %s data in the video track", is_avc ? "avcC" : "hvcC"); continue; }
            P.vcodec = is_avc;
            video_setup(is_avc, t->id, w, h, w, h, 0, cfg, cl);
            P.rot = t->rot;
            if (t->rot) LOG("[mp4] display rotation %d degrees clockwise\n", t->rot * 90);
            mp_video = i;
        } else if ((t->type == 2 || t->type == 17) && ntrk < MAX_TRK) {
            trk_t *k = &trk[ntrk];
            memset(k, 0, sizeof(*k));
            k->num = t->id; k->type = t->type;
            snprintf(k->codec, sizeof(k->codec), "%s", t->fcc);
            snprintf(k->lang, sizeof(k->lang), "%s", t->lang);
            k->def = 1;
            if (t->type == 2) {
                int ver = esz >= 36 ? (int)mp_u16(e + 16) : 0;
                size_t coff = ver == 1 ? 52 : ver == 2 ? 72 : 36;
                k->ch = esz >= 36 ? (int)mp_u16(e + 24) : 2;
                if (k->ch < 1) k->ch = 2;
                k->rate = esz >= 36 ? (int)mp_u16(e + 32) : 48000;
                if (ver == 2 || k->rate < 8000) k->rate = (int)t->timescale;
                if (fmt == FCC('a','c','-','3')) { k->adec = ADEC_AC3; k->ch = k->ch > 2 ? k->ch : 6; k->frame_us = 32000; }
                else if (fmt == FCC('e','c','-','3')) { k->adec = ADEC_EAC3; k->ch = k->ch > 2 ? k->ch : 6; k->frame_us = 32000; }
                else if (fmt == FCC('f','L','a','C')) {                      // dfLa -> "fLaC" + metadata blocks
                    size_t dl = 0;
                    const uint8_t *kids = esz > coff ? e + coff : NULL;
                    const uint8_t *dfla = kids ? mp_find_box(kids, esz - coff, FCC('d','f','L','a'), &dl) : NULL;
                    if (dfla && dl > 4 && dl - 4 + 4 <= sizeof(k->extra)) {
                        memcpy(k->extra, "fLaC", 4);
                        memcpy(k->extra + 4, dfla + 4, dl - 4);
                        k->extra_len = (int)dl;
                        k->adec = ADEC_FLAC; k->frame_us = adec_default_frame_us(ADEC_FLAC, k->rate);
                    }
                }
                else if (fmt == FCC('.','m','p','3')) { k->adec = ADEC_MP3; k->frame_us = adec_default_frame_us(ADEC_MP3, k->rate); }
                else if (fmt == FCC('O','p','u','s')) {                      // dOps -> OpusHead
                    size_t dl = 0;
                    const uint8_t *kids = esz > coff ? e + coff : NULL;
                    const uint8_t *dops = kids ? mp_find_box(kids, esz - coff, FCC('d','O','p','s'), &dl) : NULL;
                    if (dops && dl >= 11 && dl + 8 <= sizeof(k->extra)) {
                        uint8_t *o = k->extra;
                        memcpy(o, "OpusHead", 8);
                        o[8] = 1; o[9] = dops[1];
                        o[10] = dops[3]; o[11] = dops[2];                          // pre-skip, little endian
                        o[12] = dops[7]; o[13] = dops[6]; o[14] = dops[5]; o[15] = dops[4];
                        o[16] = dops[9]; o[17] = dops[8];                          // output gain
                        memcpy(o + 18, dops + 10, dl - 10);                        // mapping family (+ table)
                        k->extra_len = (int)(18 + dl - 10);
                        k->adec = ADEC_OPUS; k->ch = dops[1] ? dops[1] : 2; k->rate = 48000; k->frame_us = 20000;
                    }
                }
                else if (fmt == FCC('m','p','4','a')) {
                    size_t el = 0;
                    const uint8_t *kids = esz > coff ? e + coff : NULL;
                    size_t kn = esz > coff ? esz - coff : 0;
                    const uint8_t *es = kids ? mp_find_box(kids, kn, FCC('e','s','d','s'), &el) : NULL;
                    if (!es && kids) {                                       // QuickTime: esds inside 'wave'
                        size_t wl; const uint8_t *wv = mp_find_box(kids, kn, FCC('w','a','v','e'), &wl);
                        if (wv) es = mp_find_box(wv, wl, FCC('e','s','d','s'), &el);
                    }
                    int ot = 0; size_t al = 0;
                    const uint8_t *asc = es ? mp_esds_asc(es, el, &al, &ot) : NULL;
                    if (es && (ot == 0x6B || ot == 0x69)) { k->adec = ADEC_MP3; k->frame_us = adec_default_frame_us(ADEC_MP3, k->rate); }
                    else if (asc && (ot == 0x40 || ot == 0x66 || ot == 0x67 || ot == 0x68) && al > 0 && al <= sizeof(k->extra)) {
                        k->adec = ADEC_AAC; memcpy(k->extra, asc, al); k->extra_len = (int)al;
                        k->frame_us = 1024000000u / (uint32_t)k->rate;
                        if (al >= 2) {                                         // real channel count / rate from the ASC
                            static const int rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
                            int sfi = ((asc[0] & 7) << 1) | (asc[1] >> 7), chc = (asc[1] >> 3) & 15;
                            if (sfi < 13) { k->rate = rates[sfi]; k->frame_us = 1024000000u / (uint32_t)k->rate; }
                            if (chc >= 1 && chc <= 7) k->ch = chc == 7 ? 8 : chc;
                        }
                    }
                }
                snprintf(k->name, sizeof(k->name), "%s", "");
                LOG("[mp4] audio track %d: '%s' lang %s, %d ch %d Hz%s\n", t->id, t->fcc, t->lang, k->ch, k->rate, k->adec ? "" : "  [not supported]");
                k->def = first_aud; first_aud = 0;
            } else {
                if (fmt == FCC('t','x','3','g') || fmt == FCC('t','e','x','t')) k->txt = 3;
                LOG("[mp4] subtitle track %d: '%s' lang %s%s\n", t->id, t->fcc, t->lang, k->txt ? "" : "  [not supported]");
                k->def = 0;
            }
            t->tidx = ntrk++;
        }
    }
}

// ---- sample tables ----
static int mp_count(mtr_t *t) {
    uint8_t b[12];
    if (!t->s_stsz || rd_at(t->p_stsz, b, 12) < 12) return -1;
    t->n = mp_u32(b + 8);
    return 0;
}

static int mp_build(mtr_t *t, uint32_t *stsc_buf, uint32_t stsc_cap) {
    uint8_t b[16];
    if (!t->n) return -1;
    // sizes
    uint32_t fixed;
    if (rd_at(t->p_stsz, b, 12) < 12) return -1;
    fixed = mp_u32(b + 4);
    if (!fixed) { mrd_open(t->p_stsz + 12, t->s_stsz > 12 ? t->s_stsz - 12 : 0); }
    for (uint32_t i = 0; i < t->n; i++) { t->s[i].size = fixed ? fixed : mrd_u32(); if (mrd.err) return -2; }
    // chunk layout
    if (!t->s_stsc || !t->s_stco) return -3;
    if (rd_at(t->p_stsc, b, 8) < 8) return -3;
    uint32_t nruns = mp_u32(b + 4);
    if (nruns == 0 || nruns > stsc_cap) return -3;
    mrd_open(t->p_stsc + 8, t->s_stsc - 8);
    for (uint32_t i = 0; i < nruns * 3; i++) { stsc_buf[i] = mrd_u32(); if (mrd.err) return -3; }
    if (rd_at(t->p_stco, b, 8) < 8) return -3;
    uint32_t nchunks = mp_u32(b + 4);
    mrd_open(t->p_stco + 8, t->s_stco - 8);
    uint32_t si = 0, run = 0;
    for (uint32_t c = 1; c <= nchunks && si < t->n; c++) {
        uint64_t co = t->co64 ? mrd_u64() : mrd_u32();
        if (mrd.err) return -3;
        while (run + 1 < nruns && stsc_buf[(run + 1) * 3] <= c) run++;
        uint32_t spc = stsc_buf[run * 3 + 1];
        for (uint32_t j = 0; j < spc && si < t->n; j++) { t->s[si].off = co; co += t->s[si].size; si++; }
    }
    if (si < t->n) t->n = si;
    // decode times
    if (!t->s_stts || rd_at(t->p_stts, b, 8) < 8) return -4;
    uint32_t ne = mp_u32(b + 4);
    mrd_open(t->p_stts + 8, t->s_stts - 8);
    int64_t dts = 0; uint32_t k = 0;
    for (uint32_t i = 0; i < ne && k < t->n; i++) {
        uint32_t cnt = mrd_u32(), d = mrd_u32();
        if (mrd.err) return -4;
        for (uint32_t j = 0; j < cnt && k < t->n; j++) { t->s[k++].ts = dts; dts += d; }
    }
    for (; k < t->n; k++) t->s[k].ts = dts;
    if (t->s_ctts && rd_at(t->p_ctts, b, 8) >= 8) {
        ne = mp_u32(b + 4);
        mrd_open(t->p_ctts + 8, t->s_ctts - 8);
        k = 0;
        for (uint32_t i = 0; i < ne && k < t->n; i++) {
            uint32_t cnt = mrd_u32(); int32_t off = (int32_t)mrd_u32();
            if (mrd.err) break;
            for (uint32_t j = 0; j < cnt && k < t->n; j++) t->s[k++].ts += off;
        }
    }
    // sync samples
    if (t->type == 1 && t->s_stss && rd_at(t->p_stss, b, 8) >= 8) {
        ne = mp_u32(b + 4);
        for (uint32_t i = 0; i < t->n; i++) t->s[i].key = 0;
        mrd_open(t->p_stss + 8, t->s_stss - 8);
        for (uint32_t i = 0; i < ne; i++) {
            uint32_t sn = mrd_u32();
            if (mrd.err) break;
            if (sn >= 1 && sn <= t->n) t->s[sn - 1].key = 1;
        }
    } else for (uint32_t i = 0; i < t->n; i++) t->s[i].key = 1;
    // ticks -> microseconds
    for (uint32_t i = 0; i < t->n; i++)
        t->s[i].ts = (t->s[i].ts - t->mt) * 1000000LL / (int64_t)t->timescale + t->empty_us;
    return 0;
}


// ---- fragmented MP4 (moov with an empty sample table + a moof/mdat pair per fragment) ----
static uint8_t mp_fbuf[1 << 20];
static uint64_t mp_fbuf_pos;
static size_t mp_fbuf_len;
static int64_t fr_dts[MP_MAXTR];
static uint32_t fr_fill[MP_MAXTR];

static int mp_ti(uint32_t id) { for (int i = 0; i < nmtr; i++) if ((uint32_t)mtr[i].id == id) return i; return -1; }

// makes [pos, pos+need) available in mp_fbuf (reads at least 64 KB so the next box header is usually included)
static const uint8_t *mp_fetch(uint64_t pos, size_t need) {
    if (need > sizeof(mp_fbuf)) return NULL;
    if (mp_fbuf_len && pos >= mp_fbuf_pos && pos + need <= mp_fbuf_pos + mp_fbuf_len) return mp_fbuf + (pos - mp_fbuf_pos);
    size_t want = need > 65536 ? need : 65536;
    if (pos + want > pl_fsize) want = (size_t)(pl_fsize - pos);
    if (want < need) return NULL;
    int r = rd_raw(pos, mp_fbuf, want);
    if (r < (int)need) { mp_fbuf_len = 0; return NULL; }
    mp_fbuf_pos = pos; mp_fbuf_len = (size_t)r;
    return mp_fbuf;
}

static void mp_traf(const uint8_t *p, const uint8_t *pe, uint64_t moofpos, int pass) {
    int ti = -1;
    uint32_t ddur = 0, dsize = 0, dflags = 0;
    uint64_t base = moofpos, cur = 0;
    int have_cur = 0;
    while (p + 8 <= pe) {
        uint32_t s = mp_u32(p), ty = mp_u32(p + 4);
        if (s < 8 || p + s > pe) break;
        const uint8_t *b = p + 8;
        size_t bl = s - 8;
        if (ty == FCC('t','f','h','d') && bl >= 8) {
            uint32_t fl = mp_u32(b) & 0xFFFFFF, id = mp_u32(b + 4);
            ti = mp_ti(id);
            if (ti < 0) return;
            for (int k = 0; k < ntrex; k++) if (trex[k].id == id) { ddur = trex[k].dur; dsize = trex[k].size; dflags = trex[k].flags; }
            size_t o = 8;
            if ((fl & 1) && bl >= o + 8) { base = mp_u64(b + o); o += 8; }
            if (fl & 2) o += 4;
            if ((fl & 8) && bl >= o + 4) { ddur = mp_u32(b + o); o += 4; }
            if ((fl & 0x10) && bl >= o + 4) { dsize = mp_u32(b + o); o += 4; }
            if ((fl & 0x20) && bl >= o + 4) { dflags = mp_u32(b + o); o += 4; }
        } else if (ty == FCC('t','f','d','t') && ti >= 0 && bl >= 8) {
            fr_dts[ti] = b[0] == 1 && bl >= 12 ? (int64_t)mp_u64(b + 4) : (int64_t)mp_u32(b + 4);
        } else if (ty == FCC('t','r','u','n') && ti >= 0 && bl >= 8) {
            mtr_t *t = &mtr[ti];
            uint32_t fl = mp_u32(b) & 0xFFFFFF, cnt = mp_u32(b + 4);
            size_t o = 8;
            if ((fl & 1) && bl >= o + 4) { cur = base + (uint64_t)(int64_t)(int32_t)mp_u32(b + o); have_cur = 1; o += 4; }
            else if (!have_cur) { cur = base; have_cur = 1; }
            uint32_t first_flags = dflags;
            int has_first = 0;
            if ((fl & 4) && bl >= o + 4) { first_flags = mp_u32(b + o); has_first = 1; o += 4; }
            size_t per = ((fl & 0x100) ? 4 : 0) + ((fl & 0x200) ? 4 : 0) + ((fl & 0x400) ? 4 : 0) + ((fl & 0x800) ? 4 : 0);
            for (uint32_t i = 0; i < cnt; i++, o += per) {
                if (bl < o + per) break;
                size_t q = o;
                uint32_t d = ddur, sz = dsize, f = (i == 0 && has_first) ? first_flags : dflags;
                int32_t cto = 0;
                if (fl & 0x100) { d = mp_u32(b + q); q += 4; }
                if (fl & 0x200) { sz = mp_u32(b + q); q += 4; }
                if (fl & 0x400) { f = mp_u32(b + q); q += 4; }
                if (fl & 0x800) { cto = (int32_t)mp_u32(b + q); q += 4; }
                if (t->use) {
                    if (pass == 0) t->n++;
                    else if (fr_fill[ti] < t->n) {
                        msamp_t *sp = &t->s[fr_fill[ti]++];
                        sp->off = cur; sp->size = sz;
                        sp->ts = (fr_dts[ti] + cto - t->mt) * 1000000LL / (int64_t)t->timescale + t->empty_us;
                        sp->key = t->type == 1 ? !(f & 0x10000) : 1;
                    }
                }
                cur += sz;
                fr_dts[ti] += d;
            }
        }
        p += s;
    }
}

// pass 0 counts the samples of the tracks we use, pass 1 fills the tables
static void mp_frag_scan(int pass) {
    memset(fr_dts, 0, sizeof(fr_dts)); memset(fr_fill, 0, sizeof(fr_fill));
    mp_fbuf_len = 0;
    uint64_t pos = 0;
    int nm = 0;
    while (pos + 8 <= pl_fsize && !P.stop_req) {
        const uint8_t *h = mp_fetch(pos, 16 < pl_fsize - pos ? 16 : (size_t)(pl_fsize - pos));
        if (!h) break;
        uint64_t sz = mp_u32(h);
        uint32_t ty = mp_u32(h + 4);
        size_t hl = 8;
        if (sz == 1) { sz = mp_u64(h + 8); hl = 16; }
        else if (sz == 0) sz = pl_fsize - pos;
        if (sz < hl) break;
        if (ty == FCC('m','o','o','f') && sz <= sizeof(mp_fbuf)) {
            const uint8_t *m = mp_fetch(pos, (size_t)sz);
            if (!m) break;
            const uint8_t *q = m + hl, *qe = m + sz;
            while (q + 8 <= qe) {
                uint32_t s2 = mp_u32(q);
                if (s2 < 8 || q + s2 > qe) break;
                if (mp_u32(q + 4) == FCC('t','r','a','f')) mp_traf(q + 8, q + s2, pos, pass);
                q += s2;
            }
            nm++;
        }
        pos += sz;
    }
    LOG("[mp4] fragmented: pass %d scanned %d fragments\n", pass, nm);
}

// returns 0 when the file was indexed (video track found), -1 otherwise (P.err set)
static int mp4_open(void) {
    nmtr = 0; mp_video = -1; mp_arena = NULL; mp_arena_used = 0; ntrex = 0;
    // find moov at the top level
    uint64_t pos = 0, moov = 0, moov_sz = 0;
    uint32_t type; uint64_t pl, ps;
    while (mp_box(pos, pl_fsize, &type, &pl, &ps) == 0) {
        if (type == FCC('m','o','o','v')) { moov = pl; moov_sz = ps; break; }
        pos = pl + ps;
    }
    if (!moov) { snprintf(P.err, sizeof(P.err), "No moov box: the file looks incomplete or fragmented"); return -1; }
    mp_walk(moov, moov + moov_sz, 0, NULL);
    mp_register_tracks();
    if (mp_video < 0) { if (!P.err[0]) snprintf(P.err, sizeof(P.err), "No supported video track found"); return -1; }
    if (ntrex > 0) {                                           // fragmented: the samples are described by the moof boxes
        size_t ftotal = 0;
        for (int i = 0; i < nmtr; i++) {
            mtr_t *t = &mtr[i];
            t->use = (i == mp_video) || (t->tidx >= 0 && ((t->type == 2 && trk[t->tidx].adec) || (t->type == 17 && trk[t->tidx].txt)));
            t->n = 0; t->s = NULL; t->ok = 0;
        }
        mp_frag_scan(0);
        for (int i = 0; i < nmtr; i++) if (mtr[i].use && mtr[i].n) ftotal += ((size_t)mtr[i].n * sizeof(msamp_t) + 63) & ~(size_t)63;
        if (!ftotal) { snprintf(P.err, sizeof(P.err), "Fragmented file without any fragments"); return -1; }
        mp_arena = (uint8_t *)dmem_alloc(0, ftotal + 4096, "sample tables");
        if (!mp_arena) { snprintf(P.err, sizeof(P.err), "Out of memory for the sample index"); return -1; }
        size_t fused = 0;
        for (int i = 0; i < nmtr; i++) {
            mtr_t *t = &mtr[i];
            if (!t->use || !t->n) { t->n = 0; continue; }
            t->s = (msamp_t *)(mp_arena + fused);
            fused += ((size_t)t->n * sizeof(msamp_t) + 63) & ~(size_t)63;
        }
        mp_frag_scan(1);
        for (int i = 0; i < nmtr; i++) {
            mtr_t *t = &mtr[i];
            if (!t->n) continue;
            if (fr_fill[i] < t->n) t->n = fr_fill[i];
            if (!t->n) continue;
            t->ok = 1;
            LOG("[mp4] track %d: %u samples (fragmented), first ts %lld ms, last %lld ms\n", t->id, t->n, (long long)(t->s[0].ts / 1000), (long long)(t->s[t->n - 1].ts / 1000));
        }
        goto finish;
    }
    size_t total = 0, max_stsc = 0;
    for (int i = 0; i < nmtr; i++) {
        mtr_t *t = &mtr[i];
        int use = (i == mp_video) || (t->tidx >= 0 && ((t->type == 2 && trk[t->tidx].adec) || (t->type == 17 && trk[t->tidx].txt)));
        if (!use || mp_count(t) < 0 || !t->n) { t->n = 0; continue; }
        total += ((size_t)t->n * sizeof(msamp_t) + 63) & ~(size_t)63;
        if (t->s_stsc > max_stsc) max_stsc = (size_t)t->s_stsc;
    }
    size_t stsc_bytes = (max_stsc / 12 + 16) * 12;
    mp_arena = (uint8_t *)dmem_alloc(0, total + stsc_bytes + 4096, "sample tables");
    if (!mp_arena) { snprintf(P.err, sizeof(P.err), "Out of memory for the sample index"); return -1; }
    uint32_t *stsc_buf = (uint32_t *)(mp_arena + total);
    uint32_t stsc_cap = (uint32_t)(stsc_bytes / 12);
    size_t used = 0;
    for (int i = 0; i < nmtr; i++) {
        mtr_t *t = &mtr[i];
        if (!t->n) continue;
        t->s = (msamp_t *)(mp_arena + used);
        used += ((size_t)t->n * sizeof(msamp_t) + 63) & ~(size_t)63;
        int r = mp_build(t, stsc_buf, stsc_cap);
        if (r < 0) { LOG("[mp4] track %d: sample table error %d\n", t->id, r); t->n = 0; continue; }
        t->ok = 1;
        LOG("[mp4] track %d: %u samples, first ts %lld ms, last %lld ms\n", t->id, t->n, (long long)(t->s[0].ts / 1000), (long long)(t->s[t->n - 1].ts / 1000));
    }
finish:
    if (!mtr[mp_video].ok) { snprintf(P.err, sizeof(P.err), "The video sample table could not be read"); return -1; }
    mtr_t *v = &mtr[mp_video];
    if (v->n > 1) {
        int64_t span = v->s[v->n - 1].ts - v->s[0].ts;
        P.defdur_ns = (uint64_t)(span * 1000LL / (int64_t)(v->n - 1));
        if (!P.duration_us || P.duration_us < span) P.duration_us = span;
    }
    return 0;
}

// ---- positioning ----
static int mp_find_ts(mtr_t *t, int64_t T) {          // first sample with ts >= T (roughly monotonic tables)
    uint32_t lo = 0, hi = t->n;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (t->s[m].ts < T) lo = m + 1; else hi = m; }
    return (int)lo;
}

static void mp_seek(int64_t T, const trk_t *ka, const trk_t *ks) {
    for (int i = 0; i < nmtr; i++) {
        mtr_t *t = &mtr[i];
        if (!t->ok) continue;
        if (i == mp_video) {
            int k = mp_find_ts(t, T + 1);
            if (k > 0) k--;
            if ((uint32_t)k >= t->n) k = (int)t->n - 1;
            while (k > 0 && !t->s[k].key) k--;
            t->cur = (uint32_t)k;
        } else if (t->tidx >= 0 && ((ka && trk + t->tidx == ka) || (ks && trk + t->tidx == ks))) {
            int k = mp_find_ts(t, T - (ka && trk + t->tidx == ka ? (int64_t)ka->frame_us : 0));
            if (ks && trk + t->tidx == ks) { k -= 24; if (k < 0) k = 0; }
            t->cur = (uint32_t)k;
        }
    }
    LOG("[mp4] seek to %lld ms: video sample %u (key at %lld ms)\n", (long long)(T / 1000), mtr[mp_video].cur,
        (long long)(mtr[mp_video].s[mtr[mp_video].cur].ts / 1000));
}

// ---- the reader loop (replaces the Matroska parser when the file is MP4 / MOV) ----
static void mp4_reader(void) {
    bool first_pkt = true;
    uint32_t pkts = 0;
    if (mp4_open() < 0) { if (!P.err[0]) snprintf(P.err, sizeof(P.err), "Cannot read this MP4 file"); P.rd_state = -1; return; }
    choose_tracks();
    P.cues_ok = 1;
    int init_seek = 0;
    if (P.start_us > 0) { P.skip_until_us = P.start_us; init_seek = 1; }
    else mp_seek(0, sel_a, sel_s);
    __sync_synchronize();
    P.rd_state = 1;
    for (;;) {
        if (P.stop_req) break;
        if (P.seek_req != P.seek_parked) {
            uint32_t g = P.seek_req;
            mp_seek(P.seek_target_us, sel_a, sel_s);
            first_pkt = true; rd_aud_min_ts = P.seek_target_us; tpend_valid = 0;
            P.rd_eof = 0;
            __sync_synchronize();
            P.seek_parked = g;
            while (P.seek_go != g && !P.stop_req) sceKernelUsleep(2000);
            continue;
        }
        if (init_seek) { init_seek = 0; mp_seek(P.start_us, sel_a, sel_s); first_pkt = true; rd_aud_min_ts = P.start_us; continue; }
        // the track whose next sample is first in the file
        int best = -1; uint64_t bo = ~0ULL;
        for (int i = 0; i < nmtr; i++) {
            mtr_t *t = &mtr[i];
            if (!t->ok || t->cur >= t->n) continue;
            int use = (i == mp_video) || (t->tidx >= 0 && ((sel_a && trk + t->tidx == sel_a) || (sel_s && trk + t->tidx == sel_s)));
            if (!use) continue;
            if (t->s[t->cur].off < bo) { bo = t->s[t->cur].off; best = i; }
        }
        if (best < 0) { P.rd_eof = 1; __sync_synchronize(); while (!P.stop_req && P.seek_req == P.seek_parked) sceKernelUsleep(5000); continue; }
        mtr_t *t = &mtr[best];
        msamp_t *sm = &t->s[t->cur];
        if (sm->size == 0 || sm->size > PL_SCRATCH) {
            if (best == mp_video) { P.n_skipped++; }
            t->cur++;
            continue;
        }
        if (rd_at(sm->off, pl_scratch, sm->size) < (int)sm->size) { if (P.stop_req) break; P.rd_state = P.rd_state ? P.rd_state : -1; break; }
        if (best == mp_video) {
            int pr = pq_push(pl_scratch, sm->size, sm->ts, first_pkt);
            if (pr == -3) break;
            if (pr == -4) continue;
            if (pr != 0) { P.n_bad++; if (P.n_bad < 5) LOG("[play] unusable packet (code %d)\n", pr); }
            else { first_pkt = false; pkts++; }
        } else if (t->type == 2) {
            if (!AU.dead && sm->ts + (int64_t)AU.frame_us > rd_aud_min_ts) {
                int ar = aq_push(pl_scratch, sm->size, sm->ts);
                if (ar == -3) break;
                if (ar == -4) continue;
            }
        } else {                                                  // text subtitle (tx3g): 16-bit length + text
            if (sm->size > 2) {
                uint32_t tl = mp_u16(pl_scratch);
                if (tl > sm->size - 2) tl = sm->size - 2;
                if (tl > 0 && sm->ts + 20000000LL > rd_aud_min_ts) {
                    static char txt[TQ_TXT];
                    tsub_clean(pl_scratch + 2, tl, 0, txt, sizeof(txt));
                    int64_t dur = t->cur + 1 < t->n ? t->s[t->cur + 1].ts - sm->ts : 3000000;
                    if (dur <= 0 || dur > 30000000) dur = 3000000;
                    if (txt[0]) { tq_push(sm->ts, dur, txt); if (tdbg > 0) { tdbg--; LOGD("[tsub] cue %lld ms for %lld ms: %.60s\n", (long long)(sm->ts / 1000), (long long)(dur / 1000), txt); } }
                }
            }
        }
        t->cur++;
    }
    LOG("[play] reader finished after %u packets (stop=%d)\n", (unsigned)pkts, P.stop_req);
    if (P.rd_state == 0) P.rd_state = -1;
    __sync_synchronize();
    P.rd_eof = 1;
}

#endif
