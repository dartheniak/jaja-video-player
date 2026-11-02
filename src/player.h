// player.h - MKV/HEVC player with E-AC3/AC3/AAC audio for JajaSAMBA.
//
//   SMB file --(reader thread)--> MKV demuxer --> Annex-B packets in a ring buffer
//   ring buffer --(main thread)--> PS4 hardware decoder (libSceVideodec2) --> reorder heap
//   reorder heap --> YUV->BGRA conversion (4 threads) --> framebuffer --> paced by the MKV timestamps
//
// This file is #included by main.c, after the UI code and do_exit(). It uses these names from main.c:
//   LOG, notify, notifyf, cfg, smb_connect, fill_rect, draw_text, text_width, RGB, fb[], back, video,
//   pad_handle, WIDTH, HEIGHT, HOLD_US, BACK_MAX_US, MARGIN_X.

#include <stdbool.h>
#include <orbis/Sysmodule.h>

#define PL_RING_SIZE   (64u * 1024u * 1024u)   // packets waiting for the decoder (direct memory)
#define PL_PQ_N        2048                    // max packets in the ring
#define PL_POOL_N      24                      // decoder output frame buffers
#define PL_HEAP_MAX    16                      // reorder heap capacity
#define PL_AU_MAX      (8u * 1024u * 1024u)    // largest access unit we accept
#define PL_SCRATCH     (8u * 1024u * 1024u)
#define PL_WIN         (1024u * 1024u)         // SMB read window
#define PL_LEAD_US     12000                   // start converting this long before a frame is due
#define PL_DROP_US     70000                   // a frame later than this is dropped
#define PL_UNKNOWN     0xFFFFFFFFFFFFFFFFULL
#define PL_PREBUF      (8u * 1024u * 1024u)

// =====================================================================================
// Direct memory helpers (the decoder needs direct memory; everything is freed in one go)
// =====================================================================================

static struct { void *addr; off_t phys; size_t size; } dm_list[96];
static int dm_n;

static void dmem_free_all(void) {
    for (int i = 0; i < dm_n; i++) {
        sceKernelMunmap(dm_list[i].addr, dm_list[i].size);
        sceKernelReleaseDirectMemory(dm_list[i].phys, dm_list[i].size);
    }
    LOG("[mem] released %d direct memory blocks\n", dm_n);
    dm_n = 0;
}

// type 0 = WB "onion" (CPU friendly), type 3 = WC "garlic" (GPU friendly)
static void *dmem_alloc(int type, size_t size, const char *what) {
    size_t a = 0x200000;
    size_t sz = (size + a - 1) & ~(a - 1);
    off_t phys = 0;
    int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, a, type, &phys);
    void *addr = NULL;
    if (r == 0) r = sceKernelMapDirectMemory(&addr, sz, 0x33, 0, phys, a);
    if (r == 0 && dm_n < 96) { dm_list[dm_n].addr = addr; dm_list[dm_n].phys = phys; dm_list[dm_n].size = sz; dm_n++; }
    if (what) LOGD("[mem] %s: type %d, %zu bytes -> ret 0x%x addr %p\n", what, type, sz, r, addr);
    return r == 0 ? addr : NULL;
}

// =====================================================================================
// libSceVideodec2 (layouts from shadPS4's reverse-engineered header, verified on the PS4 Pro)
// =====================================================================================

typedef struct {
    uint64_t this_size;
    uint32_t resource_type;
    uint32_t codec_type;          // Avc = 1, Hevc = 974921
    uint32_t profile;
    uint32_t max_level;
    int32_t  max_frame_width;
    int32_t  max_frame_height;
    int32_t  max_dpb_frame_count;
    uint32_t decode_pipeline_depth;
    void    *compute_queue;
    uint64_t cpu_affinity_mask;
    int32_t  cpu_thread_priority;
    bool     optimize_progressive_video;
    bool     check_memory_type;
    uint8_t  reserved0, reserved1;
    void    *extra_config_info;
} VdCfg;
_Static_assert(sizeof(VdCfg) == 0x48, "VdCfg");

typedef struct {
    uint64_t this_size;
    uint64_t cpu_memory_size;      void *cpu_memory;
    uint64_t gpu_memory_size;      void *gpu_memory;
    uint64_t cpu_gpu_memory_size;  void *cpu_gpu_memory;
    uint64_t max_frame_buffer_size;
    uint32_t frame_buffer_alignment;
    uint32_t reserved0;
} VdMem;
_Static_assert(sizeof(VdMem) == 0x48, "VdMem");

typedef struct {
    uint64_t this_size;
    void    *au_data;
    uint64_t au_size;
    uint64_t pts_data;
    uint64_t dts_data;
    uint64_t attached_data;
} VdIn;
_Static_assert(sizeof(VdIn) == 0x30, "VdIn");

typedef struct {
    uint64_t this_size;
    bool     is_valid;
    bool     is_error_frame;
    uint8_t  picture_count;
    uint32_t codec_type;
    uint32_t frame_width;
    uint32_t frame_pitch;
    uint32_t frame_height;
    void    *frame_buffer;
    uint64_t frame_buffer_size;
    uint32_t frame_format;
    uint32_t frame_pitch_in_bytes;
} VdOut;
_Static_assert(sizeof(VdOut) == 0x38, "VdOut");

typedef struct {
    uint64_t this_size;
    void    *frame_buffer;
    uint64_t frame_buffer_size;
    bool     is_accepted;
} VdFb;
_Static_assert(sizeof(VdFb) == 0x20, "VdFb");

typedef struct { uint64_t this_size; uint64_t cpu_gpu_memory_size; void *cpu_gpu_memory; } VdCqMem;
_Static_assert(sizeof(VdCqMem) == 0x18, "VdCqMem");
typedef struct { uint64_t this_size; uint16_t pipe_id, queue_id; bool check_memory_type; uint8_t r0; uint16_t r1; } VdCqCfg;
_Static_assert(sizeof(VdCqCfg) == 0x10, "VdCqCfg");

typedef int (*fn_cq_query)(VdCqMem *);
typedef int (*fn_cq_alloc)(const VdCqCfg *, const VdCqMem *, void **);
typedef int (*fn_cq_release)(void *);
typedef int (*fn_query)(const VdCfg *, VdMem *);
typedef int (*fn_create)(const VdCfg *, const VdMem *, void **);
typedef int (*fn_delete)(void *);
typedef int (*fn_decode)(void *, const VdIn *, VdFb *, VdOut *);
typedef int (*fn_flush)(void *, VdFb *, VdOut *);
typedef int (*fn_picinfo)(const VdOut *, void *);

static fn_query   p_query, p_query_hevc;
static fn_create  p_create, p_create_hevc;
static fn_delete  p_delete;
static fn_decode  p_decode;
static fn_flush   p_flush;
static fn_picinfo p_hevcinfo;
static fn_cq_query p_cq_query;
static fn_cq_alloc p_cq_alloc;
static fn_cq_release p_cq_release;
static bool vd_lib_ok;

static OrbisKernelModule vd_find_module(void) {
    OrbisKernelModule list[256];
    size_t avail = 0;
    if (sceKernelGetModuleList(list, 256, &avail) != 0) return 0;
    if (avail > 256) avail = 256;
    for (size_t i = 0; i < avail; i++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(list[i], &info) != 0) continue;
        if (strstr(info.name, "ideodec2")) return list[i];
    }
    return 0;
}

// The linker route kills the app, so: load the system module by ID, find it in the module list, dlsym.
static bool vd_load_lib(void) {
    if (vd_lib_ok) return true;
    OrbisKernelModule h = vd_find_module();
    if (!h) {
        int r = sceSysmoduleLoadModule((enum OrbisSysModule)0xCF);
        LOG("[vd] sysmodule 0xCF -> 0x%x\n", r);
        h = vd_find_module();
    }
    if (!h) { LOG("[vd] Videodec2 module not found\n"); return false; }
#define SYM(var, name) do { void *a = NULL; int r = sceKernelDlsym((int32_t)h, name, &a); \
        var = (void *)a; if (!a) LOG("[vd] %s not found (ret 0x%x)\n", name, r); } while (0)
    SYM(p_query,       "sceVideodec2QueryDecoderMemoryInfo");
    SYM(p_query_hevc,  "sceVideodec2QueryHevcDecoderMemoryInfo");
    SYM(p_create,      "sceVideodec2CreateDecoder");
    SYM(p_create_hevc, "sceVideodec2CreateHevcDecoder");
    SYM(p_delete,      "sceVideodec2DeleteDecoder");
    SYM(p_decode,      "sceVideodec2Decode");
    SYM(p_flush,       "sceVideodec2Flush");
    SYM(p_hevcinfo,    "sceVideodec2GetHevcPictureInfo");
    SYM(p_cq_query,    "sceVideodec2QueryComputeMemoryInfo");
    SYM(p_cq_alloc,    "sceVideodec2AllocateComputeQueue");
    SYM(p_cq_release,  "sceVideodec2ReleaseComputeQueue");
#undef SYM
    vd_lib_ok = (p_query || p_query_hevc) && (p_create || p_create_hevc) && p_decode && p_flush;
    return vd_lib_ok;
}

static struct {
    void *dec, *cq;
    VdMem mem;
    uint8_t *au_buf;
    size_t fb_size;
    uint8_t *pool[PL_POOL_N];
    uint8_t in_use[PL_POOL_N];
} V;

static int vd_codec_avc;                     // 1 = create an H.264 decoder, 0 = HEVC
static int vd_max_w = 1920, vd_max_h = 1080; // size the decoder is configured for
static int vd_make_decoder(int bits10) {
    VdCfg vc;
    bool mem_ready = V.mem.cpu_memory != NULL;
    int avc = vd_codec_avc;
    // HEVC: profile 1 = Main, 2 = Main10; levels are 30 x level.  H.264: the exact numbers are not documented,
    // so a few likely profile_idc / level_idc values are tried in turn (the first that the hardware accepts wins).
    static const uint32_t hlevels[] = {153, 123};
    static const uint32_t aprofs[] = {100, 77, 66, 1, 2, 0};
    static const uint32_t alevels[] = {51, 42, 41, 40, 50, 0};
    int np = avc ? 6 : 2, nl = avc ? 6 : 2;
    for (uint32_t rt = 1; rt <= 2 && !V.dec; rt++)
        for (int pi = 0; pi < np && !V.dec; pi++)
            for (int li = 0; li < nl && !V.dec; li++) {
                uint32_t prof = avc ? aprofs[pi] : (bits10 ? (pi == 0 ? 2 : 1) : (pi == 0 ? 1 : 2));
                uint32_t lev = avc ? alevels[li] : hlevels[li];
                memset(&vc, 0, sizeof(vc));
                vc.this_size = sizeof(vc);
                vc.resource_type = rt;
                vc.codec_type = avc ? 1 : 974921;
                vc.profile = prof;
                vc.max_level = lev;
                vc.max_frame_width = vd_max_w;
                vc.max_frame_height = vd_max_h;
                vc.max_dpb_frame_count = avc ? 16 : 8;
                vc.decode_pipeline_depth = 1;      // 4 corrupts inter frames
                vc.compute_queue = V.cq;
                vc.cpu_affinity_mask = 0x3F;
                vc.cpu_thread_priority = -1;
                vc.optimize_progressive_video = true;
                vc.check_memory_type = true;
                VdMem q;
                memset(&q, 0, sizeof(q));
                q.this_size = sizeof(q);
                int r;
                if (avc) r = p_query ? p_query(&vc, &q) : -1;
                else r = p_query_hevc ? p_query_hevc(&vc, &q) : p_query(&vc, &q);
                if (r != 0) { if (avc) LOG("[vd] H.264 query rt=%u profile=%u level=%u -> 0x%x\n", rt, prof, lev, (unsigned)r); continue; }
                if (!mem_ready) {
                    V.mem = q;
                    V.mem.cpu_memory     = dmem_alloc(0, V.mem.cpu_memory_size, "dec cpu_memory");
                    V.mem.gpu_memory     = dmem_alloc(3, V.mem.gpu_memory_size, "dec gpu_memory");
                    V.mem.cpu_gpu_memory = dmem_alloc(0, V.mem.cpu_gpu_memory_size, "dec cpu_gpu_memory");
                    mem_ready = true;
                }
                void *d = NULL;
                int cr;
                if (avc) cr = p_create ? p_create(&vc, &V.mem, &d) : -1;
                else cr = p_create_hevc ? p_create_hevc(&vc, &V.mem, &d) : p_create(&vc, &V.mem, &d);
                LOG("[vd] CreateDecoder %s rt=%u profile=%u level=%u -> 0x%x\n", avc ? "H.264" : "HEVC", rt, prof, lev, (unsigned)cr);
                if (cr == 0 && d) V.dec = d;
            }
    return V.dec ? 0 : -1;
}

static int vd_open(int bits10, char *err, size_t en) {
    memset(&V, 0, sizeof(V));

    // compute queue (the decoder runs on a GPU compute queue)
    if (p_cq_query && p_cq_alloc) {
        VdCqMem cqm;
        memset(&cqm, 0, sizeof(cqm));
        cqm.this_size = sizeof(cqm);
        if (p_cq_query(&cqm) == 0) {
            cqm.cpu_gpu_memory = dmem_alloc(0, cqm.cpu_gpu_memory_size, "compute queue memory");
            for (int pipe = 4; pipe >= 0 && !V.cq; pipe--)
                for (int q = 7; q >= 0 && !V.cq; q--) {
                    VdCqCfg qc;
                    memset(&qc, 0, sizeof(qc));
                    qc.this_size = sizeof(qc);
                    qc.pipe_id = (uint16_t)pipe;
                    qc.queue_id = (uint16_t)q;
                    qc.check_memory_type = true;
                    void *queue = NULL;
                    if (p_cq_alloc(&qc, &cqm, &queue) == 0) { V.cq = queue; LOG("[vd] compute queue pipe %d queue %d\n", pipe, q); }
                }
        }
    }

    if (vd_make_decoder(bits10) < 0) { snprintf(err, en, "The hardware decoder refused every configuration"); return -1; }

    V.au_buf = dmem_alloc(0, PL_AU_MAX, "AU staging");
    V.fb_size = (V.mem.max_frame_buffer_size + 0xFFFF) & ~(size_t)0xFFFF;
    for (int i = 0; i < PL_POOL_N; i++) V.pool[i] = dmem_alloc(0, V.fb_size, NULL);
    if (!V.au_buf || !V.pool[PL_POOL_N - 1]) { snprintf(err, en, "Out of video memory"); return -1; }
    LOG("[vd] decoder ready, %d output buffers of %zu bytes\n", PL_POOL_N, V.fb_size);
    return 0;
}

// throws the decoder away and builds a new one on the same memory (a seek must start from a clean state)
static int vd_reset_decoder(int bits10) {
    if (V.dec && p_delete) LOG("[vd] DeleteDecoder (reset) -> 0x%x\n", p_delete(V.dec));
    V.dec = NULL;
    memset(V.in_use, 0, sizeof(V.in_use));
    return vd_make_decoder(bits10);
}

static void vd_close(void) {
    if (V.dec && p_delete) LOG("[vd] DeleteDecoder -> 0x%x\n", p_delete(V.dec));
    if (V.cq && p_cq_release) LOG("[vd] ReleaseComputeQueue -> 0x%x\n", p_cq_release(V.cq));
    memset(&V, 0, sizeof(V));
}

// =====================================================================================
// Player state
// =====================================================================================

static struct {
    // --- reader thread -> main thread ---
    volatile int rd_state;          // 0 opening, 1 track info ready, -1 error
    volatile int rd_eof;
    volatile int stop_req;          // main -> reader
    char err[200];
    char path[512];
    // --- track info ---
    int vtrack, pw, ph, dispw, disph, bits, nal_len;
    int rot;                        // display rotation in clockwise quarter turns (MP4 tkhd matrix): 0..3
    int vcodec;                     // 0 = HEVC, 1 = H.264/AVC
    int avc_level;                  // level_idc from avcC (H.264)
    int avc_profile;                // profile_idc from avcC (H.264)
    uint64_t tc_scale;              // ns per timecode tick
    int64_t duration_us;
    uint64_t defdur_ns;
    uint32_t ps_len;                // parameter sets (Annex B) from hvcC
    // --- input control ---
    int stop_flag;                  // 1 = back to the browser, 2 = quit the app
    int paused, osd;
    unsigned int pad_prev;
    uint64_t circle_down;
    uint64_t shift_shown;           // time of the last audio shift change (OSD flash)
    int64_t sub_delay_us;           // subtitle delay: > 0 shows subtitles later
    int adj_sub;                    // Left/Right adjust the subtitles instead of the audio (Options toggles)
    // --- seeking (times are MKV timestamps in microseconds) ---
    volatile uint32_t seek_req, seek_parked, seek_go;   // handshake main thread <-> reader
    volatile int64_t seek_target_us;
    int64_t start_us;               // where playback starts (resume), 0 = beginning
    int64_t skip_until_us;          // decode but do not show frames before this
    int64_t file_ts0;               // timestamp of the very first frame shown (for the on-screen clock)
    int file_ts0_set;
    int want_seek;                  // set by the pad code, executed by the main loop
    int64_t seek_to_us;
    uint64_t seg_start;             // file offset of the Segment payload
    int cues_ok;
    uint64_t last_resume_save;
    // --- track switching / messages ---
    int req_a_valid, req_a, req_s_valid, req_s;     // pending choice (positions in aud_idx / sub_idx), applied by do_seek
    char toast[120];
    uint64_t toast_until;
    int start_toast;
    // --- timing ---
    int started;
    int64_t ts0;
    uint64_t base_us, pause_start, stall_start;
    // --- display ---
    int reorder, last_shown;
    int64_t last_ts;
    int vw, vh, ox, oy, dw, dh;     // visible source size, destination rectangle
    int pitch, bpp;
    uint32_t uv_rows_align;
    // --- stats ---
    uint32_t n_dec, n_shown, n_dropped, n_late_order, n_bad, n_skipped;
    uint64_t dec_us_acc, conv_us_acc; uint32_t dec_cnt, conv_cnt, shown_win;
    uint64_t stat_t0, log_t0;
    int fps10, dec_ms10, conv_ms10;
} P;

static uint8_t  pl_ps[4096];                    // parameter sets
static uint8_t  pl_hvcc[4096];
static uint8_t  pl_scratch[PL_SCRATCH];
static uint8_t  pl_win[PL_WIN];
static uint64_t pl_win_off;
static size_t   pl_win_len;
static struct smb2_context *pl_ctx;
static struct smb2fh *pl_fh;
static uint64_t pl_fsize;
static size_t   pl_chunk;

// ---- packet ring (single producer: reader thread, single consumer: main thread) ----
typedef struct { uint32_t off, len; int64_t ts; uint64_t end; } pkt_t;
static pkt_t pq[PL_PQ_N];
static volatile uint32_t pq_head, pq_tail;
static uint8_t *ring;
static volatile uint64_t ring_w, ring_r;

#include "inflate.h"
#include "audio.h"
#include "subs.h"
#include "tsubs.h"

// =====================================================================================
// Reader: SMB window + MKV demuxer
// =====================================================================================

static int pl_lfd = -1;                         // >= 0: the file is local (internal drive / USB), read with pread()

static void set_toast(const char *msg, uint64_t ms);

// The SMB connection dropped (Wi-Fi hiccup, server asleep, share restarted): open a new one and the same file again.
// Tries for about 40 s; returns 0 when reading can continue, -1 when it gave up or the player is stopping.
static int smb_reconnect(void) {
    LOG("[smb] connection lost (%s) - reconnecting\n", pl_ctx ? smb2_get_error(pl_ctx) : "?");
    set_toast("Connection lost - reconnecting...", 60000);
    if (pl_ctx) { smb2_destroy_context(pl_ctx); pl_ctx = NULL; pl_fh = NULL; }   // the old socket is dead: no polite close
    for (int attempt = 1; attempt <= 10; attempt++) {
        for (int i = 0; i < 20 && !P.stop_req; i++) sceKernelUsleep(100000);       // 2 s between tries, stoppable
        if (P.stop_req) return -1;
        struct smb2_context *c = smb2_init_context();
        if (!c) continue;
        smb2_set_user(c, cfg.user);
        smb2_set_password(c, cfg.pass);
        smb2_set_timeout(c, 10);
        if (smb_connect(c) < 0) { LOG("[smb] reconnect %d: connect failed\n", attempt); smb2_destroy_context(c); continue; }
        struct smb2fh *fh = smb2_open(c, P.path, O_RDONLY);
        struct smb2_stat_64 st;
        memset(&st, 0, sizeof(st));
        if (!fh || smb2_fstat(c, fh, &st) < 0 || st.smb2_size != pl_fsize) {
            LOG("[smb] reconnect %d: cannot reopen the file\n", attempt);
            smb2_destroy_context(c);
            continue;
        }
        pl_ctx = c; pl_fh = fh;
        LOG("[smb] reconnected after %d tries\n", attempt);
        set_toast("Reconnected", 2500);
        return 0;
    }
    return -1;
}

static int rd_raw(uint64_t off, uint8_t *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (P.stop_req) return -1;
        size_t want = n - got;
        if (want > pl_chunk) want = pl_chunk;
        int r;
        if (pl_lfd >= 0) {
            r = (int)pread(pl_lfd, dst + got, want, (off_t)(off + got));
            if (r < 0) { snprintf(P.err, sizeof(P.err), "Read error at %llu: %s", (unsigned long long)(off + got), strerror(errno)); return -1; }
        } else {
            r = smb2_pread(pl_ctx, pl_fh, dst + got, (uint32_t)want, off + got);
            if (r < 0) {
                char why[160];
                snprintf(why, sizeof(why), "%s", smb2_get_error(pl_ctx));
                if (smb_reconnect() == 0) continue;                 // same read again on the new connection
                snprintf(P.err, sizeof(P.err), "Read error at %llu: %s (could not reconnect)", (unsigned long long)(off + got), why);
                return -1;
            }
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

// reads n bytes at off through a 1 MiB window; returns bytes available (0 = EOF), -1 = error
static int rd_at(uint64_t off, uint8_t *dst, size_t n) {
    if (off >= pl_fsize) return 0;
    if (off + n > pl_fsize) n = (size_t)(pl_fsize - off);
    if (n > PL_WIN / 2) return rd_raw(off, dst, n);
    if (!(pl_win_len && off >= pl_win_off && off + n <= pl_win_off + pl_win_len)) {
        size_t want = PL_WIN;
        if (off + want > pl_fsize) want = (size_t)(pl_fsize - off);
        int r = rd_raw(off, pl_win, want);
        if (r < 0) return -1;
        pl_win_off = off;
        pl_win_len = (size_t)r;
        if (pl_win_len < n) n = pl_win_len;
    }
    memcpy(dst, pl_win + (off - pl_win_off), n);
    return (int)n;
}

static int vlen(uint8_t b) {
    if (!b) return 0;
    int l = 1;
    while (!(b & (0x80 >> (l - 1)))) l++;
    return l;
}

static uint64_t be_uint(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; i++) v = (v << 8) | p[i];
    return v;
}

// parses an EBML element header; returns 0 on success
static int mk_hdr(const uint8_t *h, int n, uint32_t *id, uint64_t *size, int *hl) {
    if (n < 2) return -1;
    int il = vlen(h[0]);
    if (il == 0 || il > 4 || il >= n) return -1;
    uint32_t i = 0;
    for (int k = 0; k < il; k++) i = (i << 8) | h[k];
    int sl = vlen(h[il]);
    if (sl == 0 || il + sl > n) return -1;
    uint64_t s = h[il] & (0xFFu >> sl);
    for (int k = 1; k < sl; k++) s = (s << 8) | h[il + k];
    if (s == ((1ULL << (7 * sl)) - 1)) s = PL_UNKNOWN;
    *id = i; *size = s; *hl = il + sl;
    return 0;
}

// ---- all tracks of the file ----
typedef struct {
    int num, type;                      // MKV track number, 1 video / 2 audio / 17 subtitle
    char codec[40], lang[8], name[48];
    int def, forced, ch, rate;
    int adec;                           // ADEC_* when this is an audio track we can decode
    int pgs;                            // PGS subtitle track we can show
    int txt;                            // text subtitle track: 1 = SRT/UTF-8, 2 = ASS/SSA
    uint8_t extra[8192]; int extra_len;
    uint32_t frame_us;
    int comp;                           // -1 none, 0 zlib, 3 header stripping
    uint8_t strip[16]; int strip_len;
} trk_t;
#define MAX_TRK 32
static trk_t trk[MAX_TRK];
static int ntrk;
static int aud_idx[16], n_aud, sub_idx[16], n_sub;   // indices into trk[]
static int cur_aud = -1, cur_sub = -1;               // positions in aud_idx / sub_idx (-1 = none / off)
static const trk_t *sel_a, *sel_s;                   // what the reader pushes (changed only while it is parked)
static uint8_t pl_dec[2 * 1024 * 1024];              // decompressed block payload

// ---- preferences (/data/jajasamba/prefs.txt): audio=<lang>  sub=<lang|off> ----
#define PREF_FILE "/data/jajasamba/prefs.txt"
static char pref_audio[8] = "", pref_sub[8] = "off";
static void prefs_load(void) {
    FILE *f = fopen(PREF_FILE, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (strncmp(line, "audio=", 6) == 0) snprintf(pref_audio, sizeof(pref_audio), "%s", line + 6);
        else if (strncmp(line, "sub=", 4) == 0) snprintf(pref_sub, sizeof(pref_sub), "%s", line + 4);
    }
    fclose(f);
}
static void prefs_save(void) {
    FILE *f = fopen(PREF_FILE, "w");
    if (!f) return;
    fprintf(f, "audio=%s\nsub=%s\n", pref_audio, pref_sub);
    fclose(f);
}

// Matroska ContentEncodings: returns 0 when the track data can be used (algo = -1 none, 0 zlib, 3 strip)
static int parse_encodings(const uint8_t *p, size_t n, int *algo, uint8_t *strip, int *striplen) {
    size_t o = 0;
    int bad = 0;
    *algo = -1; *striplen = 0;
    while (o + 2 <= n) {                                            // ContentEncoding entries
        uint32_t id; uint64_t sz; int hl;
        size_t rem = n - o;
        if (mk_hdr(p + o, rem > 16 ? 16 : (int)rem, &id, &sz, &hl) < 0) break;
        o += (size_t)hl;
        if (sz == PL_UNKNOWN || sz > n - o) sz = n - o;
        if (id == 0x6240) {
            const uint8_t *b = p + o; size_t q = 0;
            while (q + 2 <= sz) {
                uint32_t id2; uint64_t sz2; int hl2;
                size_t rem2 = sz - q;
                if (mk_hdr(b + q, rem2 > 16 ? 16 : (int)rem2, &id2, &sz2, &hl2) < 0) break;
                q += (size_t)hl2;
                if (sz2 == PL_UNKNOWN || sz2 > sz - q) sz2 = sz - q;
                if (id2 == 0x5033 && be_uint(b + q, sz2) != 0) bad = 1;          // encryption
                else if (id2 == 0x5034) {                                        // ContentCompression
                    const uint8_t *c = b + q; size_t r = 0;
                    *algo = 0;                                        // ContentCompAlgo defaults to 0 (zlib) when absent
                    while (r + 2 <= sz2) {
                        uint32_t id3; uint64_t sz3; int hl3;
                        size_t rem3 = sz2 - r;
                        if (mk_hdr(c + r, rem3 > 16 ? 16 : (int)rem3, &id3, &sz3, &hl3) < 0) break;
                        r += (size_t)hl3;
                        if (sz3 == PL_UNKNOWN || sz3 > sz2 - r) sz3 = sz2 - r;
                        if (id3 == 0x4254) *algo = (int)be_uint(c + r, sz3);
                        else if (id3 == 0x4255 && sz3 <= 16) { memcpy(strip, c + r, (size_t)sz3); *striplen = (int)sz3; }
                        r += sz3;
                    }
                }
                q += sz2;
            }
        }
        o += sz;
    }
    if (*algo == 0 || *algo == 3) { /* supported */ } else if (*algo != -1) bad = 1;
    return bad;
}

// gives the payload of a block of track t with its content encoding undone; 0 = ok
static int blk_payload(const trk_t *t, const uint8_t *src, uint32_t n, const uint8_t **out, uint32_t *outn) {
    if (t->comp == 0) {
        long r = infl_zlib(src, n, pl_dec, sizeof(pl_dec));
        if (r < 0) return -1;
        *out = pl_dec; *outn = (uint32_t)r;
    } else if (t->comp == 3 && t->strip_len > 0) {
        if (n + (uint32_t)t->strip_len > sizeof(pl_dec)) return -1;
        memcpy(pl_dec, t->strip, (size_t)t->strip_len);
        memcpy(pl_dec + t->strip_len, src, n);
        *out = pl_dec; *outn = n + (uint32_t)t->strip_len;
    } else { *out = src; *outn = n; }
    return 0;
}

static const char *lang_name(const char *c) {
    static const struct { const char *c, *n; } tab[] = {
        {"eng","English"},{"por","Portuguese"},{"spa","Spanish"},{"fre","French"},{"fra","French"},{"ger","German"},{"deu","German"},
        {"ita","Italian"},{"jpn","Japanese"},{"kor","Korean"},{"chi","Chinese"},{"zho","Chinese"},{"rus","Russian"},{"dut","Dutch"},
        {"nld","Dutch"},{"swe","Swedish"},{"nor","Norwegian"},{"dan","Danish"},{"fin","Finnish"},{"pol","Polish"},{"tur","Turkish"},
        {"ara","Arabic"},{"heb","Hebrew"},{"hun","Hungarian"},{"cze","Czech"},{"ces","Czech"},{"gre","Greek"},{"ell","Greek"},
        {"hin","Hindi"},{"tha","Thai"},{"und","Unknown"},
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) if (strcmp(tab[i].c, c) == 0) return tab[i].n;
    return c;
}

static const char *adec_label(int c) {
    switch (c) {
    case ADEC_EAC3: return "E-AC-3";  case ADEC_AC3: return "AC-3";  case ADEC_AAC: return "AAC";  case ADEC_OPUS: return "Opus";
    case ADEC_DTS: return "DTS";      case ADEC_FLAC: return "FLAC"; case ADEC_MP3: return "MP3";  case ADEC_MP2: return "MP2";
    case ADEC_VORBIS: return "Vorbis"; default: return "audio";
    }
}

static uint32_t adec_default_frame_us(int c, int rate) {      // nominal packet length, only used to filter packets after a seek
    if (rate < 8000) rate = 48000;
    switch (c) {
    case ADEC_AAC:    return 1024000000u / (uint32_t)rate;
    case ADEC_OPUS:   return 20000;
    case ADEC_DTS:    return 10667;
    case ADEC_FLAC:   return 4096000000u / (uint32_t)rate;
    case ADEC_MP3:    return 1152000000u / (uint32_t)rate;
    case ADEC_MP2:    return 1152000000u / (uint32_t)rate;
    case ADEC_VORBIS: return 21333;
    default:          return 32000;
    }
}

static void fill_audio_desc(const trk_t *t, char *o, size_t n) {
    const char *codec = t->adec ? adec_label(t->adec) : t->codec;
    char chs[8];
    if (t->ch == 6) snprintf(chs, sizeof(chs), "5.1");
    else if (t->ch == 8) snprintf(chs, sizeof(chs), "7.1");
    else if (t->ch == 2) snprintf(chs, sizeof(chs), "2.0");
    else if (t->ch == 1) snprintf(chs, sizeof(chs), "1.0");
    else snprintf(chs, sizeof(chs), "%dch", t->ch);
    snprintf(o, n, "%s %s %s", lang_name(t->lang), codec, chs);
}

// subtitles: language plus the track name when it is plain ASCII and says something more (SDH, Forced ...)
static void fill_sub_desc(const trk_t *t, char *o, size_t n) {
    int ascii = t->name[0] != 0;
    for (const char *c = t->name; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) ascii = 0;
    if (ascii && strcasecmp(t->name, lang_name(t->lang)) != 0) snprintf(o, n, "%s (%.24s)", lang_name(t->lang), t->name);
    else snprintf(o, n, "%s", lang_name(t->lang));
}

static int is_forced_name(const trk_t *t) {
    if (t->forced) return 1;
    for (const char *c = t->name; *c; c++) if (strncasecmp(c, "forced", 6) == 0) return 1;
    return 0;
}

static void select_audio(int pos) {              // pos: index into aud_idx
    cur_aud = pos;
    if (pos < 0 || pos >= n_aud) { sel_a = NULL; AU.atrack = 0; return; }
    const trk_t *t = &trk[aud_idx[pos]];
    sel_a = t;
    AU.codec = t->adec; AU.rate = t->rate; AU.chans = t->ch; AU.frame_us = t->frame_us;
    AU.extra_len = t->extra_len;
    memcpy(AU.extra, t->extra, sizeof(AU.extra));
    snprintf(AU.name, sizeof(AU.name), "%s", adec_label(t->adec));
    AU.atrack = t->num;
}

static void select_sub(int pos) {                // pos: index into sub_idx, -1 = off
    cur_sub = pos;
    if (pos < 0 || pos >= n_sub) { sel_s = NULL; SUB.strack = 0; return; }
    sel_s = &trk[sub_idx[pos]];
    SUB.strack = sel_s->num;
}

// default choice from the saved preferences (reader thread, right after the track list was read)
static void choose_tracks(void) {
    n_aud = n_sub = 0;
    for (int i = 0; i < ntrk; i++) {
        if (trk[i].type == 2 && trk[i].adec && n_aud < 16) aud_idx[n_aud++] = i;
        else if (trk[i].type == 17 && (trk[i].pgs || trk[i].txt) && n_sub < 16) sub_idx[n_sub++] = i;
    }
    prefs_load();
    int pa = -1, pd = -1, pf = -1;
    for (int i = 0; i < n_aud; i++) {
        const trk_t *t = &trk[aud_idx[i]];
        if (pa < 0 && pref_audio[0] && strcmp(t->lang, pref_audio) == 0) pa = i;
        if (pd < 0 && t->def) pd = i;
    }
    select_audio(pa >= 0 ? pa : (pd >= 0 ? pd : (n_aud ? 0 : -1)));
    int ps = -1;
    if (strcmp(pref_sub, "off") != 0 && pref_sub[0]) {
        for (int i = 0; i < n_sub; i++) {
            const trk_t *t = &trk[sub_idx[i]];
            if (strcmp(t->lang, pref_sub) != 0) continue;
            if (!is_forced_name(t)) { ps = i; break; }
            if (pf < 0) pf = i;
        }
        if (ps < 0) ps = pf;
    }
    select_sub(ps);
    LOG("[mkv] %d audio track(s), %d subtitle track(s); using audio %d, subtitles %d\n", n_aud, n_sub, cur_aud + 1, cur_sub + 1);
}

// common video set-up (Matroska CodecPrivate and MP4 avcC/hvcC share the same record layout)
static void video_setup(int is_avc, int num, int pw, int ph, int dw, int dh, uint64_t defdur, const uint8_t *priv, size_t privn) {
    P.vtrack = num;
    P.pw = pw; P.ph = ph;
    P.dispw = dw ? dw : pw; P.disph = dh ? dh : ph;
    P.defdur_ns = defdur;
    if (privn > sizeof(pl_hvcc)) privn = sizeof(pl_hvcc);
    memcpy(pl_hvcc, priv, privn);

    uint32_t o2 = 0;
    size_t q = 23;
    int arrays = pl_hvcc[22];
    if (is_avc) {
        // avcC -> NAL length size, SPS/PPS in Annex B form
        P.avc_profile = pl_hvcc[1]; P.avc_level = pl_hvcc[3];
        P.nal_len = (pl_hvcc[4] & 3) + 1;
        P.bits = 8;
        q = 5;
        int nsps = pl_hvcc[q++] & 0x1F;
        for (int pass = 0; pass < 2; pass++) {
            int cnt = pass == 0 ? nsps : (q < privn ? pl_hvcc[q++] : 0);
            for (int k = 0; k < cnt && q + 2 <= privn; k++) {
                size_t l = (size_t)((pl_hvcc[q] << 8) | pl_hvcc[q + 1]);
                q += 2;
                if (q + l > privn || o2 + 4 + l > sizeof(pl_ps)) break;
                pl_ps[o2++] = 0; pl_ps[o2++] = 0; pl_ps[o2++] = 0; pl_ps[o2++] = 1;
                memcpy(pl_ps + o2, pl_hvcc + q, l);
                o2 += (uint32_t)l;
                q += l;
            }
        }
        // High 10 / 4:2:2 / 4:4:4 profiles carry the bit depth after the PPS list
        if ((P.avc_profile == 100 || P.avc_profile == 110 || P.avc_profile == 122 || P.avc_profile == 244) && q + 4 <= privn)
            P.bits = (pl_hvcc[q + 1] & 7) + 8;
        arrays = 0;
    } else {
        P.bits = (pl_hvcc[17] & 7) + 8;
        P.nal_len = (pl_hvcc[21] & 3) + 1;
    }
    for (int a = 0; a < arrays && q + 3 <= privn; a++) {
        int cnt = (pl_hvcc[q + 1] << 8) | pl_hvcc[q + 2];
        q += 3;
        for (int k = 0; k < cnt && q + 2 <= privn; k++) {
            size_t l = (size_t)((pl_hvcc[q] << 8) | pl_hvcc[q + 1]);
            q += 2;
            if (q + l > privn || o2 + 4 + l > sizeof(pl_ps)) break;
            pl_ps[o2++] = 0; pl_ps[o2++] = 0; pl_ps[o2++] = 0; pl_ps[o2++] = 1;
            memcpy(pl_ps + o2, pl_hvcc + q, l);
            o2 += (uint32_t)l;
            q += l;
        }
    }
    P.ps_len = o2;
    if (is_avc) LOG("[mkv] video: H.264, %d-bit, NAL length %d, %u bytes of parameter sets, profile %d level %d\n",
        P.bits, P.nal_len, (unsigned)o2, P.avc_profile, P.avc_level);
    else LOG("[mkv] video: %d-bit, NAL length %d, %u bytes of parameter sets, profile_idc %d level %d\n",
        P.bits, P.nal_len, (unsigned)o2, pl_hvcc[1] & 0x1F, pl_hvcc[12]);
}

static void parse_entry(const uint8_t *p, size_t n) {
    int num = 0, type = 0, pw = 0, ph = 0, dw = 0, dh = 0, ch = 0, enc = 0;
    char codec[40] = "", lang[8] = "eng", tname[48] = "";
    int tdef = 1, tforced = 0, calgo = -1, striplen = 0;
    uint8_t strip[16];
    const uint8_t *priv = NULL;
    size_t privn = 0;
    uint64_t defdur = 0;
    double asr = 0;
    size_t o = 0;
    while (o + 2 <= n) {
        uint32_t id; uint64_t sz; int hl;
        size_t rem = n - o;
        if (mk_hdr(p + o, rem > 16 ? 16 : (int)rem, &id, &sz, &hl) < 0) break;
        o += (size_t)hl;
        if (sz == PL_UNKNOWN || sz > n - o) sz = n - o;
        const uint8_t *b = p + o;
        switch (id) {
        case 0xD7:   num = (int)be_uint(b, sz); break;
        case 0x83:   type = (int)be_uint(b, sz); break;
        case 0x86:   { size_t l = sz < sizeof(codec) - 1 ? sz : sizeof(codec) - 1; memcpy(codec, b, l); codec[l] = 0; } break;
        case 0x63A2: priv = b; privn = sz; break;
        case 0x23E383: defdur = be_uint(b, sz); break;
        case 0x6D80: enc = parse_encodings(b, sz, &calgo, strip, &striplen); break;
        case 0x22B59C: { size_t l = sz < sizeof(lang) - 1 ? sz : sizeof(lang) - 1; memcpy(lang, b, l); lang[l] = 0; } break;
        case 0x536E: { size_t l = sz < sizeof(tname) - 1 ? sz : sizeof(tname) - 1; memcpy(tname, b, l); tname[l] = 0; } break;
        case 0x88:   tdef = (int)be_uint(b, sz); break;
        case 0x55AA: tforced = (int)be_uint(b, sz); break;
        case 0xE0: {                                    // Video
            size_t q = 0;
            while (q + 2 <= sz) {
                uint32_t id2; uint64_t sz2; int hl2;
                size_t rem2 = sz - q;
                if (mk_hdr(b + q, rem2 > 16 ? 16 : (int)rem2, &id2, &sz2, &hl2) < 0) break;
                q += (size_t)hl2;
                if (sz2 == PL_UNKNOWN || sz2 > sz - q) sz2 = sz - q;
                if (id2 == 0xB0) pw = (int)be_uint(b + q, sz2);
                else if (id2 == 0xBA) ph = (int)be_uint(b + q, sz2);
                else if (id2 == 0x54B0) dw = (int)be_uint(b + q, sz2);
                else if (id2 == 0x54BA) dh = (int)be_uint(b + q, sz2);
                q += sz2;
            }
        } break;
        case 0xE1: {                                    // Audio
            size_t q = 0;
            while (q + 2 <= sz) {
                uint32_t id2; uint64_t sz2; int hl2;
                size_t rem2 = sz - q;
                if (mk_hdr(b + q, rem2 > 16 ? 16 : (int)rem2, &id2, &sz2, &hl2) < 0) break;
                q += (size_t)hl2;
                if (sz2 == PL_UNKNOWN || sz2 > sz - q) sz2 = sz - q;
                if (id2 == 0x9F) ch = (int)be_uint(b + q, sz2);
                else if (id2 == 0xB5) {
                    if (sz2 == 4) { uint32_t u = (uint32_t)be_uint(b + q, 4); float f; memcpy(&f, &u, 4); asr = f; }
                    else if (sz2 == 8) { uint64_t u = be_uint(b + q, 8); double f; memcpy(&f, &u, 8); asr = f; }
                }
                q += sz2;
            }
        } break;
        default: break;
        }
        o += sz;
    }
    LOG("[mkv] track %d type %d codec %s %dx%d display %dx%d channels %d default-duration %llu ns%s\n", num, type, codec,
        pw, ph, dw, dh, ch, (unsigned long long)defdur, enc ? " (content encoding!)" : "");
    if ((type == 2 || type == 17) && ntrk < MAX_TRK) {
        trk_t *t = &trk[ntrk];
        memset(t, 0, sizeof(*t));
        t->num = num; t->type = type;
        snprintf(t->codec, sizeof(t->codec), "%s", codec);
        snprintf(t->lang, sizeof(t->lang), "%s", lang);
        snprintf(t->name, sizeof(t->name), "%s", tname);
        t->def = tdef; t->forced = tforced; t->ch = ch > 0 ? ch : 2;
        t->rate = asr > 1000 ? (int)(asr + 0.5) : 48000;
        t->comp = calgo;
        if (striplen > 0) { memcpy(t->strip, strip, (size_t)striplen); t->strip_len = striplen; }
        if (type == 2 && !enc) {
            if (strcmp(codec, "A_EAC3") == 0) t->adec = ADEC_EAC3;
            else if (strcmp(codec, "A_AC3") == 0) t->adec = ADEC_AC3;
            else if (strncmp(codec, "A_AAC", 5) == 0) t->adec = ADEC_AAC;
            else if (strcmp(codec, "A_OPUS") == 0) t->adec = ADEC_OPUS;
            else if (strcmp(codec, "A_DTS") == 0 || strncmp(codec, "A_DTS/", 6) == 0) t->adec = ADEC_DTS;
            else if (strcmp(codec, "A_FLAC") == 0) t->adec = ADEC_FLAC;
            else if (strcmp(codec, "A_MPEG/L3") == 0) t->adec = ADEC_MP3;
            else if (strcmp(codec, "A_MPEG/L2") == 0) t->adec = ADEC_MP2;
            else if (strcmp(codec, "A_VORBIS") == 0) t->adec = ADEC_VORBIS;
            if (priv && privn > 0 && privn <= sizeof(t->extra)) { memcpy(t->extra, priv, privn); t->extra_len = (int)privn; }
            t->frame_us = defdur ? (uint32_t)(defdur / 1000) : adec_default_frame_us(t->adec, t->rate);
        }
        if (type == 17 && !enc && strcmp(codec, "S_HDMV/PGS") == 0) t->pgs = 1;
        if (type == 17 && !enc && strcmp(codec, "S_TEXT/UTF8") == 0) t->txt = 1;
        if (type == 17 && !enc && (strcmp(codec, "S_TEXT/ASS") == 0 || strcmp(codec, "S_TEXT/SSA") == 0)) t->txt = 2;
        LOG("[mkv]   %s track %d: %s, language %s, name '%s'%s%s%s\n", type == 2 ? "audio" : "subtitle", num, codec, lang, tname,
            tdef ? ", default" : "", tforced ? ", forced" : "", (type == 2 ? t->adec : (t->pgs || t->txt)) ? "" : "  [not supported]");
        ntrk++;
    }
    if (type == 1 && !P.vtrack) {
        int is_avc = strcmp(codec, "V_MPEG4/ISO/AVC") == 0;
        if (!is_avc && strcmp(codec, "V_MPEGH/ISO/HEVC") != 0) {
            snprintf(P.err, sizeof(P.err), "Unsupported video codec %s (only HEVC and H.264)", codec);
            return;
        }
        if (enc) { snprintf(P.err, sizeof(P.err), "The video track uses MKV header stripping, not supported yet"); return; }
        if (!priv || privn < (is_avc ? 7u : 23u)) { snprintf(P.err, sizeof(P.err), "No %s data in the video track", is_avc ? "avcC" : "hvcC"); return; }
        P.vcodec = is_avc;
        video_setup(is_avc, num, pw, ph, dw, dh, defdur, priv, privn);
    }
}

static void parse_tracks(const uint8_t *p, size_t n) {
    size_t o = 0;
    while (o + 2 <= n) {
        uint32_t id; uint64_t sz; int hl;
        size_t rem = n - o;
        if (mk_hdr(p + o, rem > 16 ? 16 : (int)rem, &id, &sz, &hl) < 0) break;
        o += (size_t)hl;
        if (sz == PL_UNKNOWN || sz > n - o) sz = n - o;
        if (id == 0xAE) parse_entry(p + o, sz);
        o += sz;
    }
}

static void parse_info(const uint8_t *p, size_t n) {
    uint64_t scale = 1000000;
    double dur = 0;
    size_t o = 0;
    while (o + 2 <= n) {
        uint32_t id; uint64_t sz; int hl;
        size_t rem = n - o;
        if (mk_hdr(p + o, rem > 16 ? 16 : (int)rem, &id, &sz, &hl) < 0) break;
        o += (size_t)hl;
        if (sz == PL_UNKNOWN || sz > n - o) sz = n - o;
        if (id == 0x2AD7B1) scale = be_uint(p + o, sz);
        else if (id == 0x4489) {
            if (sz == 4) { uint32_t u = (uint32_t)be_uint(p + o, 4); float f; memcpy(&f, &u, 4); dur = f; }
            else if (sz == 8) { uint64_t u = be_uint(p + o, 8); double f; memcpy(&f, &u, 8); dur = f; }
        }
        o += sz;
    }
    P.tc_scale = scale;
    P.duration_us = (int64_t)(dur * (double)scale / 1000.0);
    LOG("[mkv] timecode scale %llu ns, duration %lld s\n", (unsigned long long)scale, (long long)(P.duration_us / 1000000));
}

// ---- Cues (seek index) ----
#define PL_CUES_MAX 16384
typedef struct { int64_t t_us; uint64_t cpos; } cue_t;
static cue_t cues[PL_CUES_MAX];
static int ncues;
static uint64_t cues_pos;                       // absolute file offset of the Cues element, 0 = unknown
static int64_t rd_aud_min_ts;                   // reader: drop audio before this (after a seek)

static void parse_seekhead(const uint8_t *p, size_t n) {
    size_t o = 0;
    while (o + 2 <= n) {
        uint32_t id; uint64_t sz; int hl;
        size_t rem = n - o;
        if (mk_hdr(p + o, rem > 16 ? 16 : (int)rem, &id, &sz, &hl) < 0) break;
        o += (size_t)hl;
        if (sz == PL_UNKNOWN || sz > n - o) sz = n - o;
        if (id == 0x4DBB) {                                         // Seek
            const uint8_t *b = p + o;
            uint8_t sid[8]; size_t sidn = 0; uint64_t spos = 0; size_t q = 0;
            while (q + 2 <= sz) {
                uint32_t id2; uint64_t sz2; int hl2;
                size_t rem2 = sz - q;
                if (mk_hdr(b + q, rem2 > 16 ? 16 : (int)rem2, &id2, &sz2, &hl2) < 0) break;
                q += (size_t)hl2;
                if (sz2 == PL_UNKNOWN || sz2 > sz - q) sz2 = sz - q;
                if (id2 == 0x53AB && sz2 <= 8) { memcpy(sid, b + q, (size_t)sz2); sidn = (size_t)sz2; }
                else if (id2 == 0x53AC) spos = be_uint(b + q, sz2);
                q += sz2;
            }
            if (sidn == 4 && sid[0] == 0x1C && sid[1] == 0x53 && sid[2] == 0xBB && sid[3] == 0x6B) cues_pos = P.seg_start + spos;
        }
        o += sz;
    }
}

static int cue_cmp(const void *a, const void *b) {
    const cue_t *x = (const cue_t *)a, *y = (const cue_t *)b;
    return x->t_us < y->t_us ? -1 : (x->t_us > y->t_us);
}

static void parse_cues(const uint8_t *p, size_t n) {
    size_t o = 0;
    ncues = 0;
    while (o + 2 <= n) {
        uint32_t id; uint64_t sz; int hl;
        size_t rem = n - o;
        if (mk_hdr(p + o, rem > 16 ? 16 : (int)rem, &id, &sz, &hl) < 0) break;
        o += (size_t)hl;
        if (sz == PL_UNKNOWN || sz > n - o) sz = n - o;
        if (id == 0xBB) {                                           // CuePoint
            const uint8_t *b = p + o;
            uint64_t time = 0, cpos = 0; int have = 0; size_t q = 0;
            while (q + 2 <= sz) {
                uint32_t id2; uint64_t sz2; int hl2;
                size_t rem2 = sz - q;
                if (mk_hdr(b + q, rem2 > 16 ? 16 : (int)rem2, &id2, &sz2, &hl2) < 0) break;
                q += (size_t)hl2;
                if (sz2 == PL_UNKNOWN || sz2 > sz - q) sz2 = sz - q;
                if (id2 == 0xB3) time = be_uint(b + q, sz2);
                else if (id2 == 0xB7 && !have) {                    // CueTrackPositions
                    const uint8_t *c = b + q; uint64_t trk = 0, cp = 0; size_t r = 0; int gotpos = 0;
                    while (r + 2 <= sz2) {
                        uint32_t id3; uint64_t sz3; int hl3;
                        size_t rem3 = sz2 - r;
                        if (mk_hdr(c + r, rem3 > 16 ? 16 : (int)rem3, &id3, &sz3, &hl3) < 0) break;
                        r += (size_t)hl3;
                        if (sz3 == PL_UNKNOWN || sz3 > sz2 - r) sz3 = sz2 - r;
                        if (id3 == 0xF7) trk = be_uint(c + r, sz3);
                        else if (id3 == 0xF1) { cp = be_uint(c + r, sz3); gotpos = 1; }
                        r += sz3;
                    }
                    if (gotpos && (int)trk == P.vtrack) { cpos = cp; have = 1; }
                }
                q += sz2;
            }
            if (have && ncues < PL_CUES_MAX) {
                cues[ncues].t_us = (int64_t)(time * P.tc_scale / 1000ULL);
                cues[ncues].cpos = cpos;
                ncues++;
            }
        }
        o += sz;
    }
    if (ncues > 1) qsort(cues, (size_t)ncues, sizeof(cue_t), cue_cmp);
    LOG("[mkv] %d seek points%s\n", ncues, ncues ? "" : " (seeking not available)");
}

static int load_cues(void) {
    uint8_t h[16];
    if (!cues_pos || cues_pos >= pl_fsize) return -1;
    int n = rd_at(cues_pos, h, 16);
    if (n < 4) return -1;
    uint32_t id; uint64_t sz; int hl;
    if (mk_hdr(h, n, &id, &sz, &hl) < 0 || id != 0x1C53BB6B || sz == PL_UNKNOWN || sz > PL_SCRATCH) return -1;
    if (rd_at(cues_pos + (uint64_t)hl, pl_scratch, (size_t)sz) < (int)sz) return -1;
    parse_cues(pl_scratch, (size_t)sz);
    return ncues ? 0 : -1;
}

// index of the last seek point at or before t (the first one if t is earlier than all of them)
static int cue_find(int64_t t) {
    int best = 0;
    for (int i = 0; i < ncues; i++) { if (cues[i].t_us <= t) best = i; else break; }
    return best;
}

// Waits for room in the ring, then stores one access unit (length-prefixed NALs -> Annex B).
static int pq_push(const uint8_t *src, uint32_t n, int64_t ts, bool with_ps) {
    int ls = P.nal_len;
    uint32_t out = 0, i = 0;
    while (i + (uint32_t)ls <= n) {
        uint32_t l = 0;
        for (int k = 0; k < ls; k++) l = (l << 8) | src[i + k];
        i += (uint32_t)ls;
        if (l == 0 || i + l > n) return -1;
        out += 4 + l;
        i += l;
    }
    if (i != n || out == 0) return -1;
    uint32_t total = out + (with_ps ? P.ps_len : 0);
    if (total > PL_AU_MAX) return -2;

    uint64_t w;
    size_t pos;
    for (;;) {
        if (P.stop_req) return -3;
        if (P.seek_req != P.seek_parked) return -4;
        w = ring_w;
        pos = (size_t)(w % PL_RING_SIZE);
        uint64_t pad = (pos + total > PL_RING_SIZE) ? (PL_RING_SIZE - pos) : 0;
        if (PL_RING_SIZE - (w - ring_r) >= pad + total && (pq_head - pq_tail) < PL_PQ_N) {
            w += pad;
            pos = (size_t)(w % PL_RING_SIZE);
            break;
        }
        sceKernelUsleep(3000);
    }
    uint8_t *d = ring + pos;
    if (with_ps) { memcpy(d, pl_ps, P.ps_len); d += P.ps_len; }
    i = 0;
    while (i + (uint32_t)ls <= n) {
        uint32_t l = 0;
        for (int k = 0; k < ls; k++) l = (l << 8) | src[i + k];
        i += (uint32_t)ls;
        d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 1;
        memcpy(d + 4, src + i, l);
        d += 4 + l;
        i += l;
    }
    pkt_t *e = &pq[pq_head % PL_PQ_N];
    e->off = (uint32_t)pos; e->len = total; e->ts = ts; e->end = w + total;
    ring_w = w + total;
    __sync_synchronize();
    pq_head = pq_head + 1;
    return 0;
}

#include "mp4.h"

static void *reader_thread(void *arg) {
    (void)arg;
    uint64_t pos = 0, cluster_tc = 0;
    bool first_pkt = true, warned_lace = false, need_key = false, init_seek = false;
    uint32_t pkts = 0;

    if (g_play_local) {
        pl_lfd = open(P.path, O_RDONLY);
        if (pl_lfd < 0) { snprintf(P.err, sizeof(P.err), "Cannot open file: %s", strerror(errno)); P.rd_state = -1; return NULL; }
        struct stat lst;
        if (fstat(pl_lfd, &lst) < 0) { snprintf(P.err, sizeof(P.err), "Cannot get file size: %s", strerror(errno)); P.rd_state = -1; return NULL; }
        pl_fsize = (uint64_t)lst.st_size;
        pl_chunk = 1048576;
    } else {
        pl_ctx = smb2_init_context();
        if (!pl_ctx) { snprintf(P.err, sizeof(P.err), "smb2_init_context failed"); P.rd_state = -1; return NULL; }
        smb2_set_user(pl_ctx, cfg.user);
        smb2_set_password(pl_ctx, cfg.pass);
        smb2_set_timeout(pl_ctx, 10);
        int rc = smb_connect(pl_ctx);
        if (rc < 0) {
            snprintf(P.err, sizeof(P.err), "Connect failed (%d): %s", rc, smb2_get_error(pl_ctx));
            P.rd_state = -1;
            return NULL;
        }
        pl_fh = smb2_open(pl_ctx, P.path, O_RDONLY);
        if (!pl_fh) {
            snprintf(P.err, sizeof(P.err), "Cannot open file: %s", smb2_get_error(pl_ctx));
            P.rd_state = -1;
            return NULL;
        }
        struct smb2_stat_64 st;
        memset(&st, 0, sizeof(st));
        if (smb2_fstat(pl_ctx, pl_fh, &st) < 0) {
            snprintf(P.err, sizeof(P.err), "Cannot get file size: %s", smb2_get_error(pl_ctx));
            P.rd_state = -1;
            return NULL;
        }
        pl_fsize = st.smb2_size;
        uint32_t maxread = smb2_get_max_read_size(pl_ctx);
        pl_chunk = maxread ? maxread : 65536;
        if (pl_chunk > 1048576) pl_chunk = 1048576;
    }
    LOG("[play] opened, %llu bytes, read size %u\n", (unsigned long long)pl_fsize, (unsigned)pl_chunk);
    {   // MP4 / MOV start with a box header (ftyp, moov, mdat, ...) instead of an EBML header
        uint8_t h8[12];
        if (rd_at(0, h8, 12) >= 12) {
            uint32_t bt = mp_u32(h8 + 4);
            if (bt == FCC('f','t','y','p') || bt == FCC('m','o','o','v') || bt == FCC('m','d','a','t') || bt == FCC('f','r','e','e') ||
                bt == FCC('w','i','d','e') || bt == FCC('s','k','i','p')) {
                LOG("[play] container: MP4 / MOV\n");
                P.tc_scale = 1000000; ncues = 0; rd_aud_min_ts = 0; ntrk = 0;
                mp4_reader();
                return NULL;
            }
        }
    }

    P.tc_scale = 1000000;
    ncues = 0; cues_pos = 0; rd_aud_min_ts = 0;
    ntrk = 0;
#define RD_IDLE() do { P.rd_eof = 1; __sync_synchronize(); \
        while (!P.stop_req && P.seek_req == P.seek_parked) sceKernelUsleep(5000); } while (0)
#define RD_SEEK_TO(T) do { int _i = cue_find(T); pos = P.seg_start + cues[_i].cpos; cluster_tc = 0; first_pkt = true; \
        need_key = true; rd_aud_min_ts = (T); LOG("[play] seek to %lld ms: cluster at %llu (seek point %lld ms)\n", (long long)((T) / 1000), \
        (unsigned long long)pos, (long long)(cues[_i].t_us / 1000)); } while (0)
    for (;;) {
        if (P.stop_req) break;
        if (P.seek_req != P.seek_parked) {                   // the main thread wants to jump somewhere
            uint32_t g = P.seek_req;
            if (ncues) RD_SEEK_TO(P.seek_target_us);
            P.rd_eof = 0;
            __sync_synchronize();
            P.seek_parked = g;                               // tell it we are idle ...
            while (P.seek_go != g && !P.stop_req) sceKernelUsleep(2000);   // ... until it has flushed its buffers
            continue;
        }
        if (init_seek) {                                     // resume position chosen before the file was opened
            init_seek = false;
            if (ncues) RD_SEEK_TO(P.start_us);
            continue;
        }
        uint8_t h[16];
        int n = rd_at(pos, h, 16);
        if (n < 0) { P.rd_state = P.rd_state ? P.rd_state : -1; break; }
        if (n == 0) { RD_IDLE(); continue; }                 // end of file: wait for a seek or the end of playback
        uint32_t id; uint64_t sz; int hl;
        if (mk_hdr(h, n, &id, &sz, &hl) < 0) {
            if (pos + 16 >= pl_fsize) { RD_IDLE(); continue; }   // trailing junk
            snprintf(P.err, sizeof(P.err), "Broken MKV structure at offset %llu", (unsigned long long)pos);
            if (P.rd_state == 0) P.rd_state = -1;
            break;
        }
        uint64_t body = pos + (uint64_t)hl;

        if (tpend_valid) {                                           // a text cue is waiting for its BlockDuration
            if (id == 0x9B && sz <= 8) {
                uint8_t t8[8];
                if (rd_at(body, t8, (size_t)sz) < (int)sz) break;
                tpend_flush((int64_t)(be_uint(t8, (size_t)sz) * P.tc_scale / 1000ULL));
                pos = body + sz;
                continue;
            }
            if (id == 0x1F43B675 || id == 0xA0 || id == 0xA1 || id == 0xA3) tpend_flush(4000000);
        }

        if (id == 0x18538067 || id == 0x1F43B675 || id == 0xA0) {   // Segment, Cluster, BlockGroup: descend
            if (id == 0x18538067) P.seg_start = body;
            pos = body;
            continue;
        }
        if (sz == PL_UNKNOWN) {
            snprintf(P.err, sizeof(P.err), "MKV element %x has an unknown size", (unsigned)id);
            if (P.rd_state == 0) P.rd_state = -1;
            break;
        }

        if (id == 0x1549A966 || id == 0x1654AE6B) {                 // Info, Tracks
            if (sz > PL_SCRATCH) { pos = body + sz; continue; }
            if (rd_at(body, pl_scratch, (size_t)sz) < (int)sz) break;
            if (id == 0x1549A966) parse_info(pl_scratch, (size_t)sz);
            else {
                parse_tracks(pl_scratch, (size_t)sz);
                if (!P.vtrack) {
                    if (!P.err[0]) snprintf(P.err, sizeof(P.err), "No video track found");
                    P.rd_state = -1;
                    break;
                }
                choose_tracks();
                if (cues_pos && !ncues) load_cues();
                P.cues_ok = ncues > 0;
                if (P.start_us > 0 && ncues) { P.skip_until_us = P.start_us; init_seek = true; }
                else if (P.start_us > 0) LOG("[play] no seek index, cannot resume\n");
                __sync_synchronize();
                P.rd_state = 1;
            }
        } else if (id == 0x114D9B74) {                              // SeekHead
            if (sz <= PL_SCRATCH) {
                if (rd_at(body, pl_scratch, (size_t)sz) < (int)sz) break;
                parse_seekhead(pl_scratch, (size_t)sz);
            }
        } else if (id == 0x1C53BB6B) {                              // Cues met in sequence
            if (!ncues && P.vtrack && sz <= PL_SCRATCH) {
                if (rd_at(body, pl_scratch, (size_t)sz) < (int)sz) break;
                parse_cues(pl_scratch, (size_t)sz);
            }
        } else if (id == 0xE7) {                                    // Cluster timecode
            uint8_t t[8];
            size_t l = sz > 8 ? 8 : (size_t)sz;
            if (rd_at(body, t, l) < (int)l) break;
            cluster_tc = be_uint(t, l);
        } else if ((id == 0xA3 || id == 0xA1) && P.vtrack && sz > 4) {   // SimpleBlock, Block
            uint8_t hb[8];
            if (rd_at(body, hb, 8) < 4) break;
            int tl = vlen(hb[0]);
            if (tl == 0 || tl > 2) { pos = body + sz; continue; }
            int track = hb[0] & (0xFF >> tl);
            if (tl == 2) track = (track << 8) | hb[1];
            if (sel_a && track == sel_a->num && track != P.vtrack) {
                int16_t rel = (int16_t)((hb[tl] << 8) | hb[tl + 1]);
                uint8_t flags = hb[tl + 2];
                uint32_t hs = (uint32_t)tl + 3;
                uint64_t plen = sz - hs;
                if (plen <= PL_SCRATCH && !AU.dead) {
                    if (rd_at(body + hs, pl_scratch, (size_t)plen) < (int)plen) break;
                    int64_t tick = (int64_t)cluster_tc + rel;
                    int64_t ts = (int64_t)((uint64_t)tick * P.tc_scale / 1000ULL);
                    int lace = (flags >> 1) & 3;
                    if (lace == 0) {
                        const uint8_t *pp; uint32_t pn;
                        if (ts + (int64_t)AU.frame_us > rd_aud_min_ts && blk_payload(sel_a, pl_scratch, (uint32_t)plen, &pp, &pn) == 0 &&
                            aq_push(pp, pn, ts) == -3) break;
                    } else if (plen >= 2) {                      // laced block: several frames in one block
                        const uint8_t *d = pl_scratch;
                        uint32_t nf = (uint32_t)d[0] + 1, p = 1, k;
                        static uint32_t fsz[256];
                        uint64_t used = 0;
                        int ok = 1;
                        if (lace == 1) {                         // Xiph
                            for (k = 0; k + 1 < nf && ok; k++) {
                                uint32_t v = 0; int b;
                                do { if (p >= plen) { ok = 0; break; } b = d[p++]; v += (uint32_t)b; } while (b == 255);
                                fsz[k] = v; used += v;
                            }
                        } else if (lace == 2) {                  // fixed size
                            for (k = 0; k + 1 < nf; k++) { fsz[k] = (uint32_t)((plen - 1) / nf); used += fsz[k]; }
                        } else {                                 // EBML
                            for (k = 0; k + 1 < nf && ok; k++) {
                                if (p >= plen) { ok = 0; break; }
                                int L = vlen(d[p]);
                                if (L == 0 || p + (uint32_t)L > plen) { ok = 0; break; }
                                int64_t v = d[p] & (0xFF >> L);
                                for (int q = 1; q < L; q++) v = (v << 8) | d[p + (uint32_t)q];
                                p += (uint32_t)L;
                                if (k > 0) v -= ((int64_t)1 << (7 * L - 1)) - 1, v += (int64_t)fsz[k - 1];
                                if (v < 0) { ok = 0; break; }
                                fsz[k] = (uint32_t)v; used += (uint64_t)v;
                            }
                        }
                        if (ok && p + used <= plen) {
                            fsz[nf - 1] = (uint32_t)(plen - p - used);
                            uint32_t off = p;
                            for (k = 0; k < nf; k++) {
                                int64_t tk = ts + (int64_t)k * (int64_t)AU.frame_us;
                                if (fsz[k] && fsz[k] <= AQ_MAX_PKT && tk + (int64_t)AU.frame_us > rd_aud_min_ts) {
                                    const uint8_t *pp; uint32_t pn;
                                    if (blk_payload(sel_a, d + off, fsz[k], &pp, &pn) != 0) { off += fsz[k]; continue; }
                                    int ar = aq_push(pp, pn, tk);
                                    if (ar == -3 || ar == -4) break;
                                }
                                off += fsz[k];
                            }
                            if (!AU.lace_logged) { AU.lace_logged = 1; LOGD("[audio] laced blocks (type %d), %u frames in the first one\n", lace, nf); }
                        }
                    }
                }
            } else if (track == P.vtrack) {
                int16_t rel = (int16_t)((hb[tl] << 8) | hb[tl + 1]);
                uint8_t flags = hb[tl + 2];
                uint32_t hs = (uint32_t)tl + 3;
                uint64_t plen = sz - hs;
                if (need_key && id == 0xA3 && !(flags & 0x80)) {
                    /* after a seek, wait for a keyframe */
                } else if (flags & 0x06) {
                    if (!warned_lace) { LOG("[play] laced video block ignored\n"); warned_lace = true; }
                } else if (plen > PL_SCRATCH || plen > PL_AU_MAX) {
                    LOG("[play] packet of %llu bytes skipped (too big)\n", (unsigned long long)plen);
                    P.n_skipped++;
                } else {
                    if (rd_at(body + hs, pl_scratch, (size_t)plen) < (int)plen) break;
                    int64_t tick = (int64_t)cluster_tc + rel;
                    int64_t ts = (int64_t)((uint64_t)tick * P.tc_scale / 1000ULL);
                    int pr = pq_push(pl_scratch, (uint32_t)plen, ts, first_pkt);
                    if (pr == -3) break;
                    if (pr == -4) continue;                              // a seek was requested
                    need_key = false;
                    if (pr != 0) { P.n_bad++; if (P.n_bad < 5) LOG("[play] unusable packet (code %d)\n", pr); }
                    else { first_pkt = false; pkts++; }
                }
            } else if (sel_s && sel_s->txt && track == sel_s->num) {
                int16_t rel = (int16_t)((hb[tl] << 8) | hb[tl + 1]);
                uint8_t flags = hb[tl + 2];
                uint32_t hs = (uint32_t)tl + 3;
                uint64_t plen = sz - hs;
                if (!(flags & 0x06) && plen <= PL_SCRATCH) {
                    if (rd_at(body + hs, pl_scratch, (size_t)plen) < (int)plen) break;
                    int64_t tick = (int64_t)cluster_tc + rel;
                    int64_t ts = (int64_t)((uint64_t)tick * P.tc_scale / 1000ULL);
                    const uint8_t *pp; uint32_t pn;
                    if (blk_payload(sel_s, pl_scratch, (uint32_t)plen, &pp, &pn) == 0) {
                        tpend_flush(4000000);
                        tsub_clean(pp, pn, sel_s->txt == 2, tpend.txt, sizeof(tpend.txt));
                        tpend.ts = ts;
                        tpend_valid = tpend.txt[0] != 0;
                    }
                }
            } else if (sel_s && sel_s->pgs && track == sel_s->num && !SUB.dead) {
                int16_t rel = (int16_t)((hb[tl] << 8) | hb[tl + 1]);
                uint8_t flags = hb[tl + 2];
                uint32_t hs = (uint32_t)tl + 3;
                uint64_t plen = sz - hs;
                if (!(flags & 0x06) && plen <= PL_SCRATCH) {
                    if (rd_at(body + hs, pl_scratch, (size_t)plen) < (int)plen) break;
                    int64_t tick = (int64_t)cluster_tc + rel;
                    int64_t ts = (int64_t)((uint64_t)tick * P.tc_scale / 1000ULL);
                    const uint8_t *pp; uint32_t pn;
                    if (blk_payload(sel_s, pl_scratch, (uint32_t)plen, &pp, &pn) == 0) {
                        sub_push(pp, pn, ts);
                        if (SUB.n_pushed++ < 3) LOGD("[sub] reader pushed block ts %lld ms len %u (track %d)\n", (long long)(ts / 1000), pn, track);
                    }
                }
            }
        }
        pos = body + sz;
    }
    LOG("[play] reader finished after %u packets (stop=%d)\n", (unsigned)pkts, P.stop_req);
    if (P.rd_state == 0) P.rd_state = -1;
    __sync_synchronize();
    P.rd_eof = 1;
    return NULL;
}

// =====================================================================================
// YUV (decoder output) -> BGRA conversion, BT.709 limited range
// =====================================================================================

typedef struct {
    const uint8_t *y, *uv;
    int pitch, bpp;
    int vw, vh;
    uint32_t *dst;
    int ox, oy, dw, dh;
    const int *xmap;
    const int *ymap;                // rotated output only
    int rot;
    int r0, r1;
} cjob_t;

static int xmap_tab[WIDTH];
static int ymap_tab[WIDTH];

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline uint32_t pack_px(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)clamp8(r) << 16) | ((uint32_t)clamp8(g) << 8) | (uint32_t)clamp8(b);
}

// 10-bit value (0..1023) in the low bits of a 16-bit sample, or an 8-bit sample scaled up
static inline int smp(const uint8_t *p, int i, int bpp) {
    return bpp == 2 ? (int)((const uint16_t *)p)[i] : ((int)p[i] << 2);
}

#define CONV_PAIR(Y0, Y1, U, V, OUT0, OUT1) do {                          \
        int _u = (U) - 512, _v = (V) - 512;                               \
        int _rv = _v * 29372, _gg = _u * 3493 + _v * 8731, _bu = _u * 34609; \
        int _y0 = ((Y0) - 64) * 19077, _y1 = ((Y1) - 64) * 19077;         \
        OUT0 = pack_px((_y0 + _rv) >> 16, (_y0 - _gg) >> 16, (_y0 + _bu) >> 16); \
        OUT1 = pack_px((_y1 + _rv) >> 16, (_y1 - _gg) >> 16, (_y1 + _bu) >> 16); \
    } while (0)

static void row_11_16(const uint8_t *yr, const uint8_t *cr, uint32_t *out, int w) {
    const uint16_t *Y = (const uint16_t *)yr, *C = (const uint16_t *)cr;
    int x = 0;
    for (; x + 1 < w; x += 2) CONV_PAIR(Y[x], Y[x + 1], C[x], C[x + 1], out[x], out[x + 1]);
    if (x < w) { uint32_t dummy; CONV_PAIR(Y[x], Y[x], C[x], C[x + 1], out[x], dummy); (void)dummy; }
}

static void row_11_8(const uint8_t *Y, const uint8_t *C, uint32_t *out, int w) {
    int x = 0;
    for (; x + 1 < w; x += 2)
        CONV_PAIR(Y[x] << 2, Y[x + 1] << 2, C[x] << 2, C[x + 1] << 2, out[x], out[x + 1]);
    if (x < w) { uint32_t dummy; CONV_PAIR(Y[x] << 2, Y[x] << 2, C[x] << 2, C[x + 1] << 2, out[x], dummy); (void)dummy; }
}

static void conv_band(const cjob_t *j) {
    if (j->rot) {                                        // rotated display (phone videos): sample the source per output pixel
        for (int r = j->r0; r < j->r1; r++) {
            uint32_t *out = j->dst + (size_t)(j->oy + r) * WIDTH + (size_t)j->ox;
            int rsx = j->rot == 1 ? (int)((int64_t)r * j->vw / j->dh) : (int)((int64_t)(j->dh - 1 - r) * j->vw / j->dh);
            int rsy = (int)((int64_t)(j->dh - 1 - r) * j->vh / j->dh);
            for (int x = 0; x < j->dw; x++) {
                int sx, sy;
                if (j->rot == 2) { sx = j->xmap[j->dw - 1 - x]; sy = rsy; }
                else             { sx = rsx; sy = j->ymap[x]; }
                if (sx >= j->vw) sx = j->vw - 1;
                if (sy >= j->vh) sy = j->vh - 1;
                const uint8_t *yr = j->y + (size_t)sy * (size_t)j->pitch;
                const uint8_t *cr = j->uv + (size_t)(sy >> 1) * (size_t)j->pitch;
                int cx = sx & ~1;
                int yy = smp(yr, sx, j->bpp), u = smp(cr, cx, j->bpp), v = smp(cr, cx + 1, j->bpp);
                uint32_t a, b;
                CONV_PAIR(yy, yy, u, v, a, b);
                (void)b;
                out[x] = a;
            }
        }
        return;
    }
    bool same = (j->dw == j->vw && j->dh == j->vh);
    for (int r = j->r0; r < j->r1; r++) {
        int sy = same ? r : (int)(((int64_t)r * j->vh) / j->dh);
        const uint8_t *yr = j->y + (size_t)sy * (size_t)j->pitch;
        const uint8_t *cr = j->uv + (size_t)(sy >> 1) * (size_t)j->pitch;
        uint32_t *out = j->dst + (size_t)(j->oy + r) * WIDTH + (size_t)j->ox;
        if (same) {
            if (j->bpp == 2) row_11_16(yr, cr, out, j->dw);
            else             row_11_8(yr, cr, out, j->dw);
        } else {
            for (int x = 0; x < j->dw; x++) {
                int sx = j->xmap[x];
                int cx = sx & ~1;
                int yy = smp(yr, sx, j->bpp), u = smp(cr, cx, j->bpp), v = smp(cr, cx + 1, j->bpp);
                uint32_t a, b;
                CONV_PAIR(yy, yy, u, v, a, b);
                (void)b;
                out[x] = a;
            }
        }
    }
}

static void *conv_thread(void *a) { conv_band((const cjob_t *)a); return NULL; }

#define CONV_THREADS 4
static void render_frame(int pool_idx, uint32_t *dst) {
    cjob_t job[CONV_THREADS];
    const uint8_t *buf = V.pool[pool_idx];
    for (int i = 0; i < CONV_THREADS; i++) {
        job[i].y = buf;
        job[i].uv = buf + (size_t)P.pitch * P.uv_rows_align;
        job[i].pitch = P.pitch; job[i].bpp = P.bpp;
        job[i].vw = P.vw; job[i].vh = P.vh;
        job[i].dst = dst;
        job[i].ox = P.ox; job[i].oy = P.oy; job[i].dw = P.dw; job[i].dh = P.dh;
        job[i].xmap = xmap_tab; job[i].ymap = ymap_tab; job[i].rot = P.rot;
        job[i].r0 = P.dh * i / CONV_THREADS;
        job[i].r1 = P.dh * (i + 1) / CONV_THREADS;
    }
    pthread_t th[CONV_THREADS - 1];
    int started[CONV_THREADS - 1];
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    for (int i = 0; i < CONV_THREADS - 1; i++)
        started[i] = (pthread_create(&th[i], &at, conv_thread, &job[i]) == 0);
    conv_band(&job[CONV_THREADS - 1]);
    for (int i = 0; i < CONV_THREADS - 1; i++) {
        if (started[i]) pthread_join(th[i], NULL);
        else conv_band(&job[i]);
    }
    pthread_attr_destroy(&at);
}

// =====================================================================================
// On-screen text, presenting, input
// =====================================================================================

static uint32_t osd_strip[WIDTH * 48];

static void fmt_time(char *o, size_t n, int64_t us) {
    if (us < 0) us = 0;
    long s = (long)(us / 1000000);
    snprintf(o, n, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
}

static void present(void) {
    sceVideoOutSubmitFlip(video, back, VO_FLIP_VSYNC, 0);
    while (sceVideoOutIsFlipPending(video) > 0) sceKernelUsleep(1000);
    back ^= 1;
}

static void draw_osd(uint32_t *dst, int64_t pos_us) {
    char a[16], b[16], line[300];
    fmt_time(a, sizeof(a), pos_us);
    fmt_time(b, sizeof(b), P.duration_us);
    char au[64];
    if (AU.running) snprintf(au, sizeof(au), "audio %s (shift %+d ms)", AU.name, (int)(aud_adv_us / 1000));
    else            snprintf(au, sizeof(au), "no audio");
    snprintf(line, sizeof(line), "%s%s / %s    %d.%d fps    decode %d.%d ms    convert %d.%d ms    buffer %d MB    dropped %u    %s",
             P.paused ? "PAUSED    " : "", a, b, P.fps10 / 10, P.fps10 % 10, P.dec_ms10 / 10, P.dec_ms10 % 10,
             P.conv_ms10 / 10, P.conv_ms10 % 10, (int)((ring_w - ring_r) >> 20), (unsigned)P.n_dropped, au);
    for (int i = 0; i < WIDTH * 48; i++) osd_strip[i] = RGB(0, 0, 0);
    draw_text(osd_strip, 24, 34, line, RGB(255, 255, 255), WIDTH - 24);
    memcpy(dst, osd_strip, sizeof(osd_strip));
}

static void draw_msg(const char *l1, const char *l2) {
    uint32_t *b = fb[back];
    fill_rect(b, 0, 0, WIDTH, HEIGHT, RGB(18, 18, 24));
    draw_text(b, MARGIN_X, 420, l1, RGB(220, 220, 230), WIDTH - MARGIN_X);
    if (l2) draw_text(b, MARGIN_X, 480, l2, RGB(150, 150, 160), WIDTH - MARGIN_X);
    draw_text(b, MARGIN_X, 1020, "Circle: back", RGB(110, 110, 125), WIDTH - MARGIN_X);
    present();
}

static void set_toast(const char *msg, uint64_t ms) {
    snprintf(P.toast, sizeof(P.toast), "%s", msg);
    P.toast_until = sceKernelGetProcessTime() + ms * 1000ULL;
}

static void draw_toast(uint32_t *dst) {
    if (!P.toast[0] || sceKernelGetProcessTime() > P.toast_until) return;
    fill_rect(dst, 0, 60, WIDTH, 48, RGB(0, 0, 0));
    draw_text(dst, 24, 60 + 34, P.toast, RGB(255, 255, 160), WIDTH - 24);
}

static void toast_audio(int pos, int total) {
    char d[80], m[120];
    if (pos < 0 || pos >= n_aud) { set_toast("Audio: no supported track", 3500); return; }
    fill_audio_desc(&trk[aud_idx[pos]], d, sizeof(d));
    snprintf(m, sizeof(m), "Audio %d/%d: %s", pos + 1, total, d);
    set_toast(m, 4500);
}

static void toast_sub(int pos, int total) {
    char d[80], m[120];
    if (pos < 0 || pos >= n_sub) { set_toast("Subtitles: off", 3500); return; }
    fill_sub_desc(&trk[sub_idx[pos]], d, sizeof(d));
    snprintf(m, sizeof(m), "Subtitles %d/%d: %s", pos + 1, total, d);
    set_toast(m, 4500);
}

static void toast_start(void) {
    char a[80] = "none", sb[80] = "off", m[200];
    if (cur_aud >= 0 && cur_aud < n_aud) fill_audio_desc(&trk[aud_idx[cur_aud]], a, sizeof(a));
    if (cur_sub >= 0 && cur_sub < n_sub) fill_sub_desc(&trk[sub_idx[cur_sub]], sb, sizeof(sb));
    snprintf(m, sizeof(m), "Audio: %s    Subtitles: %s    (D-pad Up/Down to change)", a, sb);
    set_toast(m, 5000);
}

// repaint the frame on screen (used when pausing / toggling the overlay while paused)
static void redraw_current(void) {
    if (P.last_shown < 0) return;
    uint32_t *dst = fb[back];
    render_frame(P.last_shown, dst);
    sub_blend(dst);
    tsub_draw(dst);
    if (P.osd || P.paused) draw_osd(dst, P.last_ts - P.file_ts0);
    draw_toast(dst);
    present();
}

static int64_t cur_pos_us(void) { return P.last_ts - P.file_ts0; }

// reads the pad; returns P.stop_flag (0 = keep playing, 1 = back, 2 = quit the app)
static int play_poll(void) {
    OrbisPadData d;
    memset(&d, 0, sizeof(d));
    scePadReadState(pad_handle, &d);
    uint64_t now = sceKernelGetProcessTime();
    keep_awake(now);
    unsigned int b = d.buttons;
    unsigned int pressed = b & ~P.pad_prev;
    unsigned int released = ~b & P.pad_prev;
    P.pad_prev = b;

    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) P.circle_down = now;
    if ((b & ORBIS_PAD_BUTTON_CIRCLE) && P.circle_down && now - P.circle_down >= HOLD_US) P.stop_flag = 2;
    if ((released & ORBIS_PAD_BUTTON_CIRCLE) && P.circle_down) {
        if (now - P.circle_down < BACK_MAX_US && !P.stop_flag) P.stop_flag = 1;
        P.circle_down = 0;
    }
    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
        P.paused = !P.paused;
        if (P.paused) { P.pause_start = now; redraw_current(); }
        else if (P.started) P.base_us += now - P.pause_start;
    }
    {   // L1/R1: +-10 s, L2/R2: +-60 s, Triangle: next file
        int64_t step = 0;
        if (pressed & ORBIS_PAD_BUTTON_L1) step = -10000000;
        if (pressed & ORBIS_PAD_BUTTON_R1) step = 10000000;
        if (pressed & ORBIS_PAD_BUTTON_L2) step = -60000000;
        if (pressed & ORBIS_PAD_BUTTON_R2) step = 60000000;
        if (step) {
            int64_t base = P.want_seek ? P.seek_to_us : (P.started ? P.last_ts : P.skip_until_us);
            P.seek_to_us = base + step;
            P.want_seek = 1;
        }
        if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && !P.stop_flag) P.stop_flag = 3;
        int ch_req = 0;
        if (pressed & ORBIS_PAD_BUTTON_UP) {                                 // next audio track
            if (n_aud < 2) set_toast("Only one audio track in this file", 3000);
            else {
                int nx = ((P.req_a_valid ? P.req_a : cur_aud) + 1) % n_aud;
                P.req_a = nx; P.req_a_valid = 1; ch_req = 1;
                toast_audio(nx, n_aud);
            }
        }
        if (pressed & ORBIS_PAD_BUTTON_DOWN) {                               // next subtitle track, then off
            if (n_sub == 0) set_toast("No subtitles this player can show in this file", 3500);
            else {
                int nx = (P.req_s_valid ? P.req_s : cur_sub) + 1;
                if (nx >= n_sub) nx = -1;
                P.req_s = nx; P.req_s_valid = 1; ch_req = 1;
                toast_sub(nx, n_sub);
            }
        }
        if (ch_req && !P.want_seek) {              // a track change re-reads the file from here
            P.seek_to_us = P.started ? P.last_ts : P.skip_until_us;
            P.want_seek = 1;
        }
    }
    if (pressed & ORBIS_PAD_BUTTON_OPTIONS) {                               // Left/Right: audio sync <-> subtitle delay
        P.adj_sub = !P.adj_sub;
        set_toast(P.adj_sub ? "Left/Right now adjust the SUBTITLE delay (Options: back to audio sync)"
                            : "Left/Right now adjust the AUDIO sync (Options: subtitle delay)", 4000);
    }
    if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
        int up = (pressed & ORBIS_PAD_BUTTON_RIGHT) != 0;
        char tm[96];
        if (P.adj_sub) {                                                    // subtitle delay, 100 ms steps (right = later)
            P.sub_delay_us += up ? 100000 : -100000;
            if (P.sub_delay_us > 60000000) P.sub_delay_us = 60000000;
            if (P.sub_delay_us < -60000000) P.sub_delay_us = -60000000;
            long ms = (long)(P.sub_delay_us / 1000);
            snprintf(tm, sizeof(tm), "Subtitle delay: %s%ld.%01ld s%s", ms < 0 ? "-" : "+", (ms < 0 ? -ms : ms) / 1000, ((ms < 0 ? -ms : ms) % 1000) / 100,
                     ms > 0 ? "  (later)" : ms < 0 ? "  (earlier)" : "");
        } else {                                                            // audio sync fine tuning, 10 ms steps
            aud_adv_us += up ? 10000 : -10000;
            if (aud_adv_us > 1000000) aud_adv_us = 1000000;
            if (aud_adv_us < -1000000) aud_adv_us = -1000000;
            LOGD("[audio] shift now %lld ms\n", (long long)(aud_adv_us / 1000));
            snprintf(tm, sizeof(tm), "Audio sync: %+d ms", (int)(aud_adv_us / 1000));
        }
        set_toast(tm, 2500);
        P.shift_shown = now;
    }
    if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
        P.osd = !P.osd;
        if (P.paused) redraw_current();
    }
    return P.stop_flag;
}

// =====================================================================================
// Decode + reorder + display
// =====================================================================================

typedef struct { int buf; int64_t ts; } hent_t;
static hent_t heap[PL_HEAP_MAX];
static int hn;
static int64_t ts_ring[256];
static uint64_t dec_idx;

static void release_buf(int k) { if (k >= 0 && k < PL_POOL_N) V.in_use[k] = 0; }

static void stats_tick(uint64_t now) {
    if (!P.stat_t0) { P.stat_t0 = P.log_t0 = now; return; }
    if (now - P.stat_t0 >= 1000000) {
        uint64_t dt = now - P.stat_t0;
        P.fps10 = (int)((uint64_t)P.shown_win * 10000000ULL / dt);
        P.dec_ms10 = P.dec_cnt ? (int)(P.dec_us_acc / P.dec_cnt / 100) : 0;
        P.conv_ms10 = P.conv_cnt ? (int)(P.conv_us_acc / P.conv_cnt / 100) : 0;
        P.shown_win = 0; P.dec_us_acc = P.conv_us_acc = 0; P.dec_cnt = P.conv_cnt = 0;
        P.stat_t0 = now;
    }
    if (now - P.log_t0 >= 5000000) {
        P.log_t0 = now;
        LOGD("[play] t=%llds shown %u dropped %u (decoded %u) fps %d.%d dec %d.%d ms conv %d.%d ms buffer %d MB reorder %d late-order %u\n",
            (long long)(cur_pos_us() / 1000000), P.n_shown, P.n_dropped, P.n_dec, P.fps10 / 10, P.fps10 % 10,
            P.dec_ms10 / 10, P.dec_ms10 % 10, P.conv_ms10 / 10, P.conv_ms10 % 10, (int)((ring_w - ring_r) >> 20),
            P.reorder, P.n_late_order);
    }
}

// shows the frame with the smallest timestamp in the heap
static void show_min(void) {
    if (hn == 0) return;
    int m = 0;
    for (int i = 1; i < hn; i++) if (heap[i].ts < heap[m].ts) m = i;
    hent_t e = heap[m];
    heap[m] = heap[--hn];

    uint64_t now = sceKernelGetProcessTime();
    if (!P.started) {
        if (e.ts < P.skip_until_us) { release_buf(e.buf); return; }      // decoded only to get to the seek target
        if (!P.file_ts0_set) { P.file_ts0 = e.ts; P.file_ts0_set = 1; P.start_toast = 1; }
        P.ts0 = e.ts; P.base_us = now; P.last_ts = e.ts;
        __sync_synchronize();
        P.started = 1;
    }
    if (e.ts < P.last_ts) {                      // reorder window was too small
        P.n_late_order++;
        if (P.reorder < PL_HEAP_MAX - 2) P.reorder++;
    }

    // wait until it is time (a few ms early, conversion and vsync take time)
    for (;;) {
        if (play_poll() || P.want_seek) { release_buf(e.buf); return; }
        now = sceKernelGetProcessTime();
        if (P.paused) { sceKernelUsleep(5000); continue; }
        int64_t due = (int64_t)P.base_us + (e.ts - P.ts0);
        int64_t wait = due - PL_LEAD_US - (int64_t)now;
        if (wait <= 0) break;
        sceKernelUsleep(wait > 4000 ? 2000 : (unsigned)wait);
    }
    now = sceKernelGetProcessTime();
    int64_t due = (int64_t)P.base_us + (e.ts - P.ts0);
    if ((int64_t)now - due > PL_DROP_US && hn > 0) {      // far too late: skip it, catch up
        P.n_dropped++;
        release_buf(e.buf);
        return;
    }

    uint32_t *dst = fb[back];
    uint64_t c0 = sceKernelGetProcessTime();
    render_frame(e.buf, dst);
    uint64_t c1 = sceKernelGetProcessTime();
    P.conv_us_acc += c1 - c0; P.conv_cnt++;
    P.last_ts = e.ts > P.last_ts ? e.ts : P.last_ts;
    sub_advance(e.ts - P.sub_delay_us);
    tsub_advance(e.ts - P.sub_delay_us);
    sub_blend(dst);
    tsub_draw(dst);
    if (P.osd) draw_osd(dst, e.ts - P.file_ts0);
    if (P.start_toast) { P.start_toast = 0; toast_start(); }
    draw_toast(dst);
    present();
    P.n_shown++; P.shown_win++;
    release_buf(P.last_shown);
    P.last_shown = e.buf;
    stats_tick(sceKernelGetProcessTime());
}

// puts a decoder output frame into the reorder heap; returns false if it was not usable
static bool take_output(const VdOut *out, int64_t ts_hint, bool use_hint) {
    if (!out->is_valid || !out->frame_buffer) return false;
    int k = -1;
    for (int i = 0; i < PL_POOL_N; i++) if (V.pool[i] == out->frame_buffer) { k = i; break; }
    if (k < 0) { LOG("[play] decoder returned a buffer that is not ours (%p)\n", out->frame_buffer); return false; }
    V.in_use[k] = 1;

    int64_t ts = ts_hint;
    if (!use_hint) {
        uint8_t pi[0xB8];
        memset(pi, 0, sizeof(pi));
        *(uint64_t *)pi = sizeof(pi);
        if (p_hevcinfo && p_hevcinfo(out, pi) == 0) ts = ts_ring[*(uint64_t *)(pi + 24) & 255];   // dts = decode index
        else ts = ts_hint;
    }
    if (P.pitch == 0) {
        P.pitch = (int)out->frame_pitch_in_bytes;
        P.bpp = P.bits > 8 ? 2 : 1;
        uint32_t fh = out->frame_height ? out->frame_height : (uint32_t)P.ph;
        P.uv_rows_align = fh;   // the chroma plane starts right after the visible luma rows (not rounded up to 16)
        LOG("[play] first frame: decoder says %ux%u pitch %u (%u bytes), format %u, picture count %u; stream says %dx%d\n",
            out->frame_width, out->frame_height, out->frame_pitch, out->frame_pitch_in_bytes, out->frame_format,
            out->picture_count, P.pw, P.ph);
    }
    if (hn >= PL_HEAP_MAX) { release_buf(k); return false; }
    heap[hn].buf = k; heap[hn].ts = ts; hn++;
    return true;
}

static int feed_au(const uint8_t *src, uint32_t len, int64_t ts) {
    int idx = -1;
    for (int k = 0; k < PL_POOL_N; k++) if (!V.in_use[k]) { idx = k; break; }
    if (idx < 0) { LOG("[play] no free output buffer\n"); return -1; }
    memcpy(V.au_buf, src, len);
    ts_ring[dec_idx & 255] = ts;

    VdIn in;
    VdFb fb_in;
    VdOut out;
    memset(&in, 0, sizeof(in));
    memset(&fb_in, 0, sizeof(fb_in));
    memset(&out, 0, sizeof(out));
    in.this_size = sizeof(in);
    in.au_data = V.au_buf;
    in.au_size = len;
    in.pts_data = dec_idx;
    in.dts_data = dec_idx;
    fb_in.this_size = sizeof(fb_in);
    fb_in.frame_buffer = V.pool[idx];
    fb_in.frame_buffer_size = V.fb_size;
    out.this_size = sizeof(out);
    dec_idx++;

    uint64_t t0 = sceKernelGetProcessTime();
    int rc = p_decode(V.dec, &in, &fb_in, &out);
    P.dec_us_acc += sceKernelGetProcessTime() - t0; P.dec_cnt++;
    P.n_dec++;
    if (rc != 0) {
        if (P.n_bad++ < 10) LOG("[play] Decode -> 0x%x (AU %llu, %u bytes)\n", rc, (unsigned long long)(dec_idx - 1), len);
        return -1;
    }
    if (!take_output(&out, ts, false)) return 0;
    return 1;
}

// ---- resume positions: /data/jajasamba/resume.txt, one line per file:  position_s|duration_s|path ----
#define RS_FILE "/data/jajasamba/resume.txt"
#define RS_MAX  100
typedef struct { char path[480]; long pos_s, dur_s; } rs_t;
static rs_t rs_tab[RS_MAX];     // most recently played first
static int rs_n;

static void rs_load(void) {
    rs_n = 0;
    FILE *f = fopen(RS_FILE, "r");
    if (!f) return;
    char line[600];
    while (rs_n < RS_MAX && fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        long p = 0, d = 0; int off = 0;
        if (sscanf(line, "%ld|%ld|%n", &p, &d, &off) >= 2 && off > 0 && line[off]) {
            rs_tab[rs_n].pos_s = p; rs_tab[rs_n].dur_s = d;
            snprintf(rs_tab[rs_n].path, sizeof(rs_tab[rs_n].path), "%s", line + off);
            rs_n++;
        }
    }
    fclose(f);
}

static void rs_save(void) {
    FILE *f = fopen(RS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < rs_n; i++) fprintf(f, "%ld|%ld|%s\n", rs_tab[i].pos_s, rs_tab[i].dur_s, rs_tab[i].path);
    fclose(f);
}

static int rs_index(const char *path) {
    for (int i = 0; i < rs_n; i++) if (strcmp(rs_tab[i].path, path) == 0) return i;
    return -1;
}

static void rs_remove(const char *path) {
    int i = rs_index(path);
    if (i < 0) return;
    memmove(&rs_tab[i], &rs_tab[i + 1], (size_t)(rs_n - i - 1) * sizeof(rs_t));
    rs_n--;
    rs_save();
}

// remembers where we are; positions near the start or the end are not worth keeping
static void rs_set(const char *path, long pos_s, long dur_s) {
    if (pos_s < 20 || (dur_s > 0 && pos_s > dur_s - 90)) { rs_remove(path); return; }
    int i = rs_index(path);
    if (i < 0) {
        if (rs_n == RS_MAX) rs_n--;
        i = rs_n++;
        snprintf(rs_tab[i].path, sizeof(rs_tab[i].path), "%s", path);
    }
    rs_tab[i].pos_s = pos_s; rs_tab[i].dur_s = dur_s;
    if (i > 0) { rs_t t = rs_tab[i]; memmove(&rs_tab[1], &rs_tab[0], (size_t)i * sizeof(rs_t)); rs_tab[0] = t; }
    rs_save();
}

static void rs_remember_now(void) {
    if (!P.started) return;
    rs_set(P.path, (long)(P.last_ts / 1000000), (long)(P.duration_us / 1000000));
}

// accessors for the "Continue watching" list in the browser
static void recent_reload(void) { rs_load(); }
static int recent_count(void) { return rs_n; }
static const char *recent_path(int i) { return rs_tab[i].path; }
static long recent_pos(int i) { return rs_tab[i].pos_s; }
static long recent_dur(int i) { return rs_tab[i].dur_s; }

// 1 = resume, 0 = start over, -1 = go back
static int resume_prompt(long pos_s) {
    char t[16], l1[80];
    fmt_time(t, sizeof(t), (int64_t)pos_s * 1000000);
    snprintf(l1, sizeof(l1), "Resume from %s?", t);
    draw_msg(l1, "Cross: resume      Square: start from the beginning");
    for (;;) {
        OrbisPadData d;
        memset(&d, 0, sizeof(d));
        scePadReadState(pad_handle, &d);
        unsigned int pressed = d.buttons & ~P.pad_prev;
        P.pad_prev = d.buttons;
        if (pressed & ORBIS_PAD_BUTTON_CROSS) return 1;
        if (pressed & ORBIS_PAD_BUTTON_SQUARE) return 0;
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE) return -1;
        sceKernelUsleep(20000);
    }
}

static int pl_ath_started;                       // the audio thread was started for this file
static int pl_bits10;

// Jumps to target_us (an MKV timestamp). Returns 0 when done (or when seeking is not possible), -1 on a hard failure.
static int do_seek(int64_t target_us) {
    int64_t lo = P.file_ts0_set ? P.file_ts0 : 0;
    int64_t hi = P.duration_us > 10000000 ? lo + P.duration_us - 5000000 : lo;
    if (target_us > hi) target_us = hi;
    if (target_us < lo) target_us = lo;
    if (!P.cues_ok) { notify("JajaSAMBA: this file has no seek index"); return 0; }

    char tl[16], l1[64];
    fmt_time(tl, sizeof(tl), target_us - lo);
    if (P.req_a_valid || P.req_s_valid) snprintf(l1, sizeof(l1), "Changing track...");
    else snprintf(l1, sizeof(l1), "Seeking to %s", tl);
    draw_msg(l1, NULL);
    LOG("[play] seek requested: %lld ms\n", (long long)(target_us / 1000));

    P.seek_target_us = target_us;
    __sync_synchronize();
    uint32_t g = P.seek_req + 1;
    P.seek_req = g;
    uint64_t t0 = sceKernelGetProcessTime();
    while (P.seek_parked != g && !P.stop_req) {
        if (sceKernelGetProcessTime() - t0 > 30000000ULL) { LOG("[play] reader did not answer the seek request\n"); P.seek_go = g; return -1; }
        sceKernelUsleep(1000);
    }

    // the reader is parked: nothing is being added to the buffers now
    if (P.req_a_valid) {
        P.req_a_valid = 0;
        if (P.req_a != cur_aud && P.req_a >= 0 && P.req_a < n_aud) {
            select_audio(P.req_a);
            AU.reopen = 1;
            snprintf(pref_audio, sizeof(pref_audio), "%s", trk[aud_idx[cur_aud]].lang);
            prefs_save();
        }
    }
    if (P.req_s_valid) {
        P.req_s_valid = 0;
        if (P.req_s != cur_sub && P.req_s < n_sub) {
            select_sub(P.req_s);
            SUB.n_pushed = 0;
            LOGD("[sub] selected subtitle position %d (track %d), dead=%d running=%d\n", P.req_s, sel_s ? sel_s->num : 0, SUB.dead, SUB.running);
            snprintf(pref_sub, sizeof(pref_sub), "%s", cur_sub < 0 ? "off" : trk[sub_idx[cur_sub]].lang);
            prefs_save();
        }
    }
    P.started = 0;                                  // the audio thread holds still while this is 0
    P.paused = 0;
    P.stall_start = 0;
    P.skip_until_us = target_us;
    P.last_ts = target_us;
    __sync_synchronize();

    pq_tail = pq_head;
    ring_r = ring_w;
    hn = 0;
    P.last_shown = -1;
    if (V.dec) {
        if (vd_reset_decoder(pl_bits10) < 0) { LOG("[play] could not recreate the decoder\n"); P.seek_go = g; return -1; }
    }
    if (pl_ath_started && !AU.dead && AU.running) {
        AU.flush_req = 1;
        uint64_t t1 = sceKernelGetProcessTime();
        while (AU.flush_req && !AU.dead && !P.stop_req && sceKernelGetProcessTime() - t1 < 1000000ULL) sceKernelUsleep(1000);
    }
    if (AU.flush_req || !pl_ath_started || AU.dead || !AU.running) {
        AU.flush_req = 0;
        aq_tail = aq_head;
        aring_r = aring_w;
    }
    tsub_reset();
    sub_free_cur();
    if (SUB.running) {
        SUB.flush_req = 1;
        uint64_t t2 = sceKernelGetProcessTime();
        while (SUB.flush_req && SUB.running && !P.stop_req && sceKernelGetProcessTime() - t2 < 1000000ULL) sceKernelUsleep(1000);
    }
    if (SUB.flush_req || !SUB.running) {
        SUB.flush_req = 0;
        sq_tail = sq_head; sring_r = sring_w;
        while (sev_tail != sev_head) { sev_free(&sev[sev_tail % SEV_N]); sev_tail = sev_tail + 1; }
    }
    __sync_synchronize();
    P.seek_go = g;                                  // the reader may continue from the new position
    return 0;
}

static void show_error(const char *msg) {
    LOG("[play] ERROR: %s\n", msg);
    notify(msg);
    draw_msg("Cannot play this file", msg);
    for (int i = 0; i < 200 && !P.stop_flag; i++) {
        play_poll();
        sceKernelUsleep(20000);
    }
}

static void black_fill(void) {
    for (int i = 0; i < NUM_BUFFERS; i++) fill_rect(fb[i], 0, 0, WIDTH, HEIGHT, RGB(0, 0, 0));
}

// returns 0 = back to the browser, 1 = quit the app, 2 = the file ended (or was skipped): play the next one
static int play_file(const char *path) {
    pthread_t rth;
    int rth_ok = 0;
    char msg[240];
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

    memset(&P, 0, sizeof(P));
    memset(&AU, 0, sizeof(AU));
    memset(&SUB, 0, sizeof(SUB));
    ntrk = n_aud = n_sub = 0; cur_aud = cur_sub = -1; sel_a = sel_s = NULL;
    sq_head = sq_tail = 0; sring_w = sring_r = 0; sring = NULL;
    sev_head = sev_tail = 0;
    tq_head = tq_tail = 0; tact_n = 0; tpend_valid = 0; tdbg = 8;
    memset(&sub_cur, 0, sizeof(sub_cur));
    int sth_ok = 0;
    pthread_t sth;
    aq_head = aq_tail = 0;
    aring_w = aring_r = 0;
    aring = NULL;
    int ath_ok = 0, ended = 0;
    pl_ath_started = 0;
    pthread_t ath;
    P.reorder = 6;
    P.last_shown = -1;
    P.base_us = 0;
    pq_head = pq_tail = 0;
    ring_w = ring_r = 0;
    hn = 0;
    dec_idx = 0;
    pl_win_len = 0;
    pl_ctx = NULL; pl_fh = NULL; pl_lfd = -1;
    set_str(P.path, sizeof(P.path), path);
    LOG("[play] ===== %s =====\n", path);

    {   // swallow whatever the pad was doing when we were started
        OrbisPadData d;
        memset(&d, 0, sizeof(d));
        scePadReadState(pad_handle, &d);
        P.pad_prev = d.buttons;
        if (d.buttons & ORBIS_PAD_BUTTON_CIRCLE) P.circle_down = sceKernelGetProcessTime();
    }
    rs_load();
    {
        int ri = rs_index(path);
        if (ri >= 0 && rs_tab[ri].pos_s >= 20) {
            int a = resume_prompt(rs_tab[ri].pos_s);
            if (a < 0) goto done;
            if (a > 0) P.start_us = (int64_t)rs_tab[ri].pos_s * 1000000;
        }
    }
    draw_msg("Opening...", name);

    ring = dmem_alloc(0, PL_RING_SIZE, "packet ring");
    if (!ring) { show_error("Out of memory for the packet buffer"); goto done; }
    aring = dmem_alloc(0, AQ_RING_SIZE, "audio ring");
    if (!aring) { show_error("Out of memory for the audio buffer"); goto done; }
    sring = dmem_alloc(0, SQ_RING_SIZE, "subtitle ring");
    if (!sring) { show_error("Out of memory for the subtitle buffer"); goto done; }

    {
        pthread_attr_t pa;
        pthread_attr_init(&pa);
        pthread_attr_setstacksize(&pa, 512 * 1024);
        rth_ok = (pthread_create(&rth, &pa, reader_thread, NULL) == 0);
        pthread_attr_destroy(&pa);
        if (!rth_ok) { show_error("Could not start the reader thread"); goto done; }
    }

    {   // wait for the track info
        uint64_t t0 = sceKernelGetProcessTime();
        while (P.rd_state == 0 && !P.stop_flag) {
            play_poll();
            if (sceKernelGetProcessTime() - t0 > 40000000ULL) { snprintf(P.err, sizeof(P.err), "Timed out opening the file"); P.rd_state = -1; }
            sceKernelUsleep(20000);
        }
    }
    if (P.stop_flag) goto done;
    if (P.rd_state < 0) { show_error(P.err[0] ? P.err : "Could not read the file"); goto done; }

    LOG("[play] video %dx%d (display %dx%d), %d-bit\n", P.pw, P.ph, P.dispw, P.disph, P.bits);
    if ((int64_t)P.pw * P.ph > 1920 * 1088 || P.pw > 2160 || P.ph > 2160 || P.pw < 16 || P.ph < 16) {
        snprintf(msg, sizeof(msg), "Video size %dx%d is not supported (the hardware decoder handles up to 1920x1080 worth of pixels)", P.pw, P.ph);
        show_error(msg);
        goto done;
    }
    if (P.bits != 8 && P.bits != 10) {
        snprintf(msg, sizeof(msg), "%d-bit video is not supported (only 8 and 10 bit)", P.bits);
        show_error(msg);
        goto done;
    }

    draw_msg("Starting the hardware decoder...", name);
    if (!vd_load_lib()) { show_error("Could not load the hardware video decoder (libSceVideodec2)"); goto done; }
    vd_codec_avc = P.vcodec;
    vd_max_w = P.pw > 1920 ? P.pw : 1920;
    vd_max_h = P.ph > 1080 ? ((P.ph + 15) & ~15) : 1080;
    if (vd_open(P.bits > 8, msg, sizeof(msg)) < 0) { show_error(msg); goto done; }
    pl_bits10 = P.bits > 8;

    // geometry: fit the display aspect inside 1920x1080, 1:1 when it already fits exactly
    P.vw = P.pw; P.vh = P.ph;
    {
        int64_t aw = P.dispw, ah = P.disph;
        if (P.rot & 1) { int64_t t = aw; aw = ah; ah = t; }      // a quarter turn swaps width and height
        if (aw * 1080 > ah * 1920) { P.dw = 1920; P.dh = (int)(ah * 1920 / aw); }
        else                       { P.dh = 1080; P.dw = (int)(aw * 1080 / ah); }
        if (!P.rot && P.pw == 1920 && P.ph == 1080 && P.dispw == 1920 && P.disph == 1080) {       // exactly full HD: 1:1 (smaller 16:9 sizes are scaled up)
            P.dw = 1920; P.dh = 1080;
        }
        if (!P.rot && P.pw == 1920 && P.ph <= 1080 && P.dispw == 1920) { P.dw = 1920; P.dh = P.ph; }   // scope movies etc.
        P.dw &= ~1; P.dh &= ~1;
        if (P.dw > WIDTH) P.dw = WIDTH;
        if (P.dh > HEIGHT) P.dh = HEIGHT;
        P.ox = (WIDTH - P.dw) / 2;
        P.oy = (HEIGHT - P.dh) / 2;
        for (int x = 0; x < P.dw; x++) xmap_tab[x] = (int)((int64_t)x * P.vw / P.dw);
        for (int x = 0; x < P.dw; x++) ymap_tab[x] = P.rot == 1 ? (int)((int64_t)(P.dw - 1 - x) * P.vh / P.dw) : (int)((int64_t)x * P.vh / P.dw);
        LOG("[play] screen rectangle %dx%d at %d,%d (%s)\n", P.dw, P.dh, P.ox, P.oy,
            (P.dw == P.vw && P.dh == P.vh) ? "1:1" : "scaled");
    }

    int any_pgs = 0;
    for (int ti = 0; ti < ntrk; ti++) if (trk[ti].type == 17 && trk[ti].pgs) any_pgs = 1;
    if (any_pgs) {
        pthread_attr_t pa;
        pthread_attr_init(&pa);
        pthread_attr_setstacksize(&pa, 1024 * 1024);
        sth_ok = (pthread_create(&sth, &pa, sub_thread, NULL) == 0);
        pthread_attr_destroy(&pa);
        if (!sth_ok) { LOG("[sub] could not start the subtitle thread\n"); SUB.dead = 1; }
    }
    if (AU.atrack) {
        pthread_attr_t pa;
        pthread_attr_init(&pa);
        pthread_attr_setstacksize(&pa, 1024 * 1024);
        ath_ok = (pthread_create(&ath, &pa, audio_thread, NULL) == 0);
        pthread_attr_destroy(&pa);
        if (!ath_ok) { LOG("[audio] could not start the audio thread\n"); AU.dead = 1; }
        pl_ath_started = ath_ok;
    }

    {   // buffer some data first
        uint64_t t0 = sceKernelGetProcessTime();
        uint64_t last_draw = 0;
        while (!P.stop_flag) {
            uint64_t have = ring_w - ring_r;
            if (have >= PL_PREBUF || P.rd_eof || (pq_head - pq_tail) >= PL_PQ_N - 16 || (aq_head - aq_tail) >= AQ_N - 16) break;   // low bitrate: the packet queue fills first
            if (P.rd_state < 0) break;
            uint64_t now = sceKernelGetProcessTime();
            if (now - last_draw > 300000) {
                char l[80];
                snprintf(l, sizeof(l), "Buffering... %d%%", (int)(have * 100 / PL_PREBUF));
                draw_msg(l, name);
                last_draw = now;
            }
            if (now - t0 > 60000000ULL) break;
            play_poll();
            sceKernelUsleep(20000);
        }
    }
    if (P.stop_flag) goto done;
    if (pq_head == pq_tail && P.rd_eof) { show_error(P.err[0] ? P.err : "The file contains no video data"); goto done; }

    black_fill();
    LOG("[play] starting playback, %d MB buffered\n", (int)((ring_w - ring_r) >> 20));

    // ---------------- main decode / display loop ----------------
    for (;;) {
        if (play_poll()) break;
        if (P.want_seek) {
            P.want_seek = 0;
            if (do_seek(P.seek_to_us) < 0) { show_error("Could not jump to that position"); break; }
            continue;
        }
        if (P.paused) { sceKernelUsleep(10000); continue; }
        {
            uint64_t tn = sceKernelGetProcessTime();
            if (P.started && tn - P.last_resume_save > 30000000ULL) { P.last_resume_save = tn; rs_remember_now(); }
        }

        if (pq_tail == pq_head) {                              // nothing buffered
            if (P.rd_eof) { ended = (P.rd_state > 0); break; }
            uint64_t now = sceKernelGetProcessTime();
            if (!P.stall_start) { P.stall_start = now; LOG("[play] buffer ran empty\n"); }
            sceKernelUsleep(3000);
            continue;
        }
        if (P.stall_start) {                                   // time spent waiting does not count as playback time
            if (P.started) P.base_us += sceKernelGetProcessTime() - P.stall_start;
            P.stall_start = 0;
        }
        __sync_synchronize();
        pkt_t pk = pq[pq_tail % PL_PQ_N];
        int r = feed_au(ring + pk.off, pk.len, pk.ts);
        ring_r = pk.end;
        __sync_synchronize();
        pq_tail = pq_tail + 1;
        if (r < 0 && P.n_bad > 200) { show_error("Too many decoder errors"); break; }
        while (hn > P.reorder && !P.stop_flag && !P.want_seek) show_min();
        if (P.stop_flag) break;
    }

    // ---------------- drain ----------------
    if (!P.stop_flag && V.dec) {
        for (int i = 0; i < 16; i++) {
            int idx = -1;
            for (int k = 0; k < PL_POOL_N; k++) if (!V.in_use[k]) { idx = k; break; }
            if (idx < 0) break;
            VdFb f;
            VdOut o;
            memset(&f, 0, sizeof(f));
            memset(&o, 0, sizeof(o));
            f.this_size = sizeof(f);
            f.frame_buffer = V.pool[idx];
            f.frame_buffer_size = V.fb_size;
            o.this_size = sizeof(o);
            if (p_flush(V.dec, &f, &o) != 0 || !o.is_valid) break;
            take_output(&o, P.last_ts, false);
        }
        while (hn > 0 && !P.stop_flag) show_min();
        if (!P.stop_flag) sceKernelUsleep(300000);
    }

done:
    LOG("[play] stopping: shown %u dropped %u decoded %u bad %u, late-order events %u\n", P.n_shown, P.n_dropped, P.n_dec,
        P.n_bad, P.n_late_order);
    P.stop_req = 1;
    if (rth_ok) pthread_join(rth, NULL);
    if (ath_ok) pthread_join(ath, NULL);
    if (sth_ok) pthread_join(sth, NULL);
    sub_free_cur();
    if (pl_lfd >= 0) { close(pl_lfd); pl_lfd = -1; }
    if (pl_fh && pl_ctx) smb2_close(pl_ctx, pl_fh);
    if (pl_ctx) smb2_destroy_context(pl_ctx);
    pl_fh = NULL; pl_ctx = NULL;
    vd_close();
    dmem_free_all();
    ring = NULL;
    aring = NULL;
    sring = NULL;
    if (P.stop_flag) ended = 0;
    g_play_skipped = (P.stop_flag == 3);
    if (ended || P.stop_flag == 3) rs_remove(path);        // finished or skipped: forget the position
    else rs_remember_now();
    int ret = 0;
    if (P.stop_flag == 2) ret = 1;                          // quit the app
    else if (ended || P.stop_flag == 3) ret = 2;            // play the next file
    black_fill();
    return ret;
}
