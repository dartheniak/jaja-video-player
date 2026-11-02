// inflate.h - small zlib/DEFLATE decoder (RFC 1950 / 1951), structured after the public "puff" reference
// decoder. Used for MKV tracks stored with ContentCompression "zlib" (typical for subtitles).
#ifndef INFLATE_H
#define INFLATE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const uint8_t *in; size_t inlen, incnt;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outlen, outcnt;
    int err;
} infl_t;

typedef struct { short count[16]; short symbol[288]; } infl_huff;

static int infl_bits(infl_t *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = need >= 32 ? 0 : (val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int infl_decode(infl_t *s, const infl_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= infl_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static int infl_construct(infl_huff *h, const short *length, int n) {
    short offs[16];
    for (int len = 0; len <= 15; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int infl_codes(infl_t *s, const infl_huff *lencode, const infl_huff *distcode) {
    static const short lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = infl_decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outcnt >= s->outlen) return -2;
            s->out[s->outcnt++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = lens[sym] + infl_bits(s, lext[sym]);
            int ds = infl_decode(s, distcode);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)dists[ds] + (size_t)infl_bits(s, dext[ds]);
            if (s->err) return -1;
            if (dist > s->outcnt) return -1;
            if (s->outcnt + (size_t)len > s->outlen) return -2;
            for (int i = 0; i < len; i++) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    }
}

static int infl_fixed(infl_t *s) {
    static infl_huff lencode, distcode;
    static int ready;
    if (!ready) {
        short lengths[288];
        int sym;
        for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        infl_construct(&lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        infl_construct(&distcode, lengths, 30);
        ready = 1;
    }
    return infl_codes(s, &lencode, &distcode);
}

static int infl_dynamic(infl_t *s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    infl_huff lencode, distcode;
    int nlen = infl_bits(s, 5) + 257, ndist = infl_bits(s, 5) + 1, ncode = infl_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)infl_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (infl_construct(&lencode, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int sym = infl_decode(s, &lencode);
        if (sym < 0) return -1;
        if (sym < 16) lengths[index++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (index == 0) return -1; len = lengths[index - 1]; rep = 3 + infl_bits(s, 2); }
            else if (sym == 17) rep = 3 + infl_bits(s, 3);
            else rep = 11 + infl_bits(s, 7);
            if (s->err || index + rep > nlen + ndist) return -1;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = infl_construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -1;
    err = infl_construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -1;
    return infl_codes(s, &lencode, &distcode);
}

// raw DEFLATE data -> out; returns the number of bytes written or a negative error
static long infl_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap) {
    infl_t s;
    memset(&s, 0, sizeof(s));
    s.in = in; s.inlen = inlen; s.out = out; s.outlen = outcap;
    int last, ret = 0;
    do {
        last = infl_bits(&s, 1);
        int type = infl_bits(&s, 2);
        if (s.err) return -1;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.incnt + 4 > s.inlen) return -1;
            unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8);
            unsigned nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8);
            s.incnt += 4;
            if (len != (~nlen & 0xFFFF)) return -1;
            if (s.incnt + len > s.inlen) return -1;
            if (s.outcnt + len > s.outlen) return -2;
            memcpy(s.out + s.outcnt, s.in + s.incnt, len);
            s.incnt += len; s.outcnt += len;
        } else if (type == 1) ret = infl_fixed(&s);
        else if (type == 2) ret = infl_dynamic(&s);
        else return -1;
        if (ret) return ret;
    } while (!last);
    return (long)s.outcnt;
}

// zlib stream (2 byte header, deflate data, adler32 which we do not check)
static long infl_zlib(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap) {
    if (inlen < 3) return -1;
    if (((in[0] << 8) | in[1]) % 31 != 0 || (in[0] & 0x0F) != 8) return -1;
    return infl_raw(in + 2, inlen - 2, out, outcap);
}
#endif
