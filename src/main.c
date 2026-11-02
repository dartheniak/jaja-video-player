#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/SysUtil.h>
#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/ImeDialog.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#if __has_include(<orbis/Net.h>)
#include <orbis/Net.h>
#else
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
#endif

// ---------- stb_truetype, configured to avoid libm ----------

static int stbx_floor(double x) { int i = (int)x; return (x < (double)i) ? i - 1 : i; }
static int stbx_ceil(double x)  { int i = (int)x; return (x > (double)i) ? i + 1 : i; }
static double stbx_fabs(double x) { return x < 0 ? -x : x; }
static double stbx_sqrt(double x) {
    if (x <= 0) return 0;
    double g = (x + 1.0) * 0.5;
    for (int i = 0; i < 40; i++) g = 0.5 * (g + x / g);
    return g;
}

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_ifloor(x)   stbx_floor(x)
#define STBTT_iceil(x)    stbx_ceil(x)
#define STBTT_sqrt(x)     stbx_sqrt(x)
#define STBTT_fabs(x)     stbx_fabs(x)
#define STBTT_pow(x, y)   (0.0)
#define STBTT_fmod(x, y)  (0.0)
#define STBTT_cos(x)      (0.0)
#define STBTT_acos(x)     (0.0)
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))
#define STBTT_assert(x)   ((void)0)
#include "stb_truetype.h"

// ---------- constants ----------

#define CONFIG_DIR   "/data/jajasamba"
#define CONFIG_PATH  "/data/jajasamba/config.ini"
#define LOG_PATH     "/data/jajasamba/log.txt"
#define FONT_PATH    "/app0/font.ttf"
#define LOG_PORT     9023
#define HOLD_US      1000000   // hold Circle this long to quit
#define BACK_MAX_US  500000    // a Circle press shorter than this means "go back"
#define CONNECT_TIMEOUT_MS 15000

#define WIDTH        1920
#define HEIGHT       1080
#define NUM_BUFFERS  2
#define FB_SIZE      (8 * 1024 * 1024)
#define DMEM_ALIGN   0x200000

#define VO_USER_MAIN     255
#define VO_BUS_MAIN      0
#define VO_PIXEL_FORMAT  0x80000000
#define VO_TILING_LINEAR 1
#define VO_ASPECT_16_9   0
#define VO_FLIP_VSYNC    1

#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#define ATLAS_W      1024
#define ATLAS_H      512
#define FONT_PX      30.0f
#define GLYPH_FIRST  32
#define GLYPH_COUNT  224          // Latin-1: U+0020 .. U+00FF

#define MAX_ITEMS    1024
#define ROW_H        40
#define VISIBLE_ROWS 20
#define LIST_TOP     150
#define MARGIN_X     80

// Tells the system the user is active, so the PS4 does not go into rest mode / dim during playback or while browsing.
// (Watching a video sends no button presses, which the console counts as inactivity.) Called from every loop.
static void keep_awake(uint64_t now_us) {
    static uint64_t last = 0;
    if (last && now_us - last < 1000000ULL) return;
    last = now_us;
    sceSystemServicePowerTick();
}

// ---------- configuration (loaded from CONFIG_PATH) ----------

typedef struct {
    char server[64];
    char share[64];
    char user[64];
    char pass[64];
    char path[256];
    char log_ip[40];
    char local[256];            // folder shown first under "Internal"
    int sub_size;               // text subtitle size: 0 small, 1 medium, 2 large
    int auto_next;              // 1 = start the next file when one ends
    int debug;                  // 1 = verbose log
    int sort;                   // browser order: 0 name, 1 newest first, 2 largest first
} cfg_t;

static cfg_t cfg;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
    *e = 0;
    return s;
}

static void set_str(char *dst, size_t n, const char *val) {
    strncpy(dst, val, n - 1);
    dst[n - 1] = 0;
}

static void write_template(void) {
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fputs("# Jaja Video Player configuration\n"
          "# Edit this file, then relaunch the app.\n"
          "\n"
          "# IP address of the machine hosting the share\n"
          "server=192.168.1.50\n"
          "# share name (no slashes)\n"
          "share=videos\n"
          "# login (use guest with an empty password for open shares)\n"
          "user=guest\n"
          "password=\n"
          "# folder inside the share to open first (empty = top level)\n"
          "path=\n"
          "# folder shown first under \"Internal\" (L2/R2 switch between SMB, Internal and USB)\n"
          "local_path=/data\n"
          "# optional: IP of a PC running  nc -u -l 9023  to receive debug logs\n"
          "log_ip=\n", f);
    fclose(f);
}

// written by the Settings screen (Start); keeps every key, so config.ini stays valid for hand editing
static void save_config(void) {
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# Jaja Video Player configuration (also editable from the Settings screen: Start)\n\n"
               "server=%s\nshare=%s\nuser=%s\npassword=%s\n"
               "# folder inside the share to open first (empty = top level)\npath=%s\n"
               "# folder shown first under \"Internal\"\nlocal_path=%s\n"
               "# text subtitle size: 0 small, 1 medium, 2 large\nsub_size=%d\n"
               "# 1 = start the next file when one ends, 0 = stop\nauto_next=%d\n"
               "# browser order: 0 name, 1 newest first, 2 largest first (R3 in the browser)\nsort=%d\n"
               "# 1 = verbose log in /data/jajasamba/log.txt\ndebug=%d\n"
               "# optional: IP of a PC running  nc -u -l 9023  to receive debug logs\nlog_ip=%s\n",
            cfg.server, cfg.share, cfg.user, cfg.pass, cfg.path, cfg.local, cfg.sub_size, cfg.auto_next, cfg.sort, cfg.debug, cfg.log_ip);
    fclose(f);
}

// returns 0 on success, -1 if the file could not be opened
static int load_config(void) {
    memset(&cfg, 0, sizeof(cfg));
    set_str(cfg.user, sizeof(cfg.user), "guest");
    set_str(cfg.local, sizeof(cfg.local), "/data");
    cfg.sub_size = 1;
    cfg.auto_next = 1;
    cfg.debug = 0;
    cfg.sort = 0;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == 0 || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);

        if      (!strcmp(key, "server"))   set_str(cfg.server, sizeof(cfg.server), val);
        else if (!strcmp(key, "share"))    set_str(cfg.share,  sizeof(cfg.share),  val);
        else if (!strcmp(key, "user"))     set_str(cfg.user,   sizeof(cfg.user),   val);
        else if (!strcmp(key, "password")) set_str(cfg.pass,   sizeof(cfg.pass),   val);
        else if (!strcmp(key, "path"))     set_str(cfg.path,   sizeof(cfg.path),   val);
        else if (!strcmp(key, "log_ip"))   set_str(cfg.log_ip, sizeof(cfg.log_ip), val);
        else if (!strcmp(key, "local_path")) set_str(cfg.local, sizeof(cfg.local), val);
        else if (!strcmp(key, "sub_size")) { cfg.sub_size = atoi(val); if (cfg.sub_size < 0 || cfg.sub_size > 2) cfg.sub_size = 1; }
        else if (!strcmp(key, "auto_next")) cfg.auto_next = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "debug")) cfg.debug = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "sort")) { cfg.sort = atoi(val); if (cfg.sort < 0 || cfg.sort > 2) cfg.sort = 0; }
    }
    fclose(f);
    return 0;
}

// ---------- logging: file on the PS4 + UDP to a PC (if log_ip is set) + stdout ----------

static int log_sock = -1;
static struct sockaddr_in log_addr;
static FILE *log_file = NULL;

static void log_init(void) {
    log_file = fopen(LOG_PATH, "w");      // flushed after every line

    if (cfg.log_ip[0] == 0) return;
    log_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (log_sock < 0) return;
    memset(&log_addr, 0, sizeof(log_addr));
    log_addr.sin_len = sizeof(log_addr);
    log_addr.sin_family = AF_INET;
    log_addr.sin_port = htons(LOG_PORT);
    inet_pton(AF_INET, cfg.log_ip, &log_addr.sin_addr);
}

static void LOG(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;

    char line[600];
    n = snprintf(line, sizeof(line), "[%7llu ms] %s",
                 (unsigned long long)(sceKernelGetProcessTime() / 1000ULL), buf);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;

    fputs(line, stdout);
    if (log_file) { fwrite(line, 1, n, log_file); fflush(log_file); }
    if (log_sock >= 0)
        sendto(log_sock, line, n, 0, (struct sockaddr *)&log_addr, sizeof(log_addr));
}

// verbose diagnostics (per-packet audio/subtitle details, once-a-few-seconds playback statistics ...):
// build with -DJJ_DEBUG=1 (or change the 0 below) when something needs to be investigated
static int g_debug = 0;                 // Settings -> Debug log (verbose lines in log.txt)
#define LOGD(...) do { if (g_debug) LOG(__VA_ARGS__); } while (0)

static void notify(const char *msg) {
    sceSysUtilSendSystemNotificationWithText(222, msg);
}

static void notifyf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    notify(buf);
}

// ---------- shared state between the UI thread and the network worker ----------

typedef struct {
    char name[256];
    int is_dir;
    uint64_t size;
    char info[32];                  // right-hand text instead of the size (Continue watching list)
    int64_t mtime;                  // modification time (seconds), for sorting
} item_t;

enum { LS_IDLE = 0, LS_LOADING = 1, LS_OK = 2, LS_ERR = 3 };

static item_t work_items[MAX_ITEMS];       // written by the worker
static volatile int work_count = 0;
static item_t items[MAX_ITEMS];            // shown by the UI
static int item_count = 0;

static volatile int list_state = LS_IDLE;  // worker sets OK/ERR, UI sets IDLE
static volatile int req_pending = 0;
static char req_path[512];
static char worker_error[200];             // filled by the worker on failure
static volatile const char *busy_what = "Loading";
static volatile int req_is_bench = 0;       // 1 = the pending request is a speed test
static char bench_text[200];                // result text written by the worker

// ---------- sources: SMB share, internal storage, USB / extended storage ----------
// ids: 0 = SMB, 1 = internal (whole file system, starts in cfg.local), 2..9 = /mnt/usb0..7, 10 = /mnt/ext0
#define SRC_SMB 0
#define SRC_INT 1
#define SRC_USB0 2
#define SRC_EXT0 10
#define SRC_RECENT 11                         // "Continue watching": not a folder tree, a list from resume.txt
#define SRC_N 12
static int g_src = SRC_SMB;                // what the UI is browsing
static volatile int req_src = SRC_SMB;     // what the pending request is for
static int g_play_local = 0;               // play_file(): 1 = the path is a local file

static int src_is_local(int id) { return id != SRC_SMB && id != SRC_RECENT; }

// full local path for a path relative to the source root
static void local_full(int id, char *dst, size_t n, const char *rel) {
    if (id == SRC_INT) snprintf(dst, n, "/%s", rel);
    else {
        char root[24];
        if (id == SRC_EXT0) snprintf(root, sizeof(root), "/mnt/ext0");
        else snprintf(root, sizeof(root), "/mnt/usb%d", id - SRC_USB0);
        if (rel[0]) snprintf(dst, n, "%s/%s", root, rel); else snprintf(dst, n, "%s", root);
    }
}

static void src_label(int id, char *dst, size_t n) {
    if (id == SRC_RECENT) snprintf(dst, n, "Continue watching");
    else if (id == SRC_SMB) snprintf(dst, n, "SMB  //%s/%s", cfg.server, cfg.share);
    else if (id == SRC_INT) snprintf(dst, n, "Internal storage");
    else if (id == SRC_EXT0) snprintf(dst, n, "Extended storage  /mnt/ext0");
    else snprintf(dst, n, "USB drive %d  /mnt/usb%d", id - SRC_USB0, id - SRC_USB0);
}

// ---------- SMB (runs only on the worker thread) ----------

static struct smb2_context *ctx = NULL;

typedef struct {
    volatile int done;
    int status;
} conn_state_t;

static void connect_cb(struct smb2_context *smb2, int status, void *command_data, void *cb_data) {
    (void)smb2; (void)command_data;
    conn_state_t *c = (conn_state_t *)cb_data;
    c->status = status;
    c->done = 1;
}

// Connect with libsmb2's async API and our own wait loop. Returns 0 on success.
static int smb_connect(struct smb2_context *smb2) {
    conn_state_t cs;
    cs.done = 0;
    cs.status = 0;

    LOG("[smb] connecting to //%s/%s as %s\n", cfg.server, cfg.share, cfg.user);
    int rc = smb2_connect_share_async(smb2, cfg.server, cfg.share, cfg.user, connect_cb, &cs);
    if (rc < 0) {
        LOG("[smb] connect_share_async failed: %d (%s)\n", rc, smb2_get_error(smb2));
        return rc;
    }

    int waited = 0;
    int prepared_fd = -1;
    while (!cs.done && waited < CONNECT_TIMEOUT_MS) {
        int fd = smb2_get_fd(smb2);
        int ev = smb2_which_events(smb2);

        if (fd < 0) {                      // no socket yet
            sceKernelUsleep(100000);
            waited += 100;
            continue;
        }

        // On the PS4, fcntl(O_NONBLOCK) does not work, and a blocking read after a complete
        // reply stalls libsmb2. Two measures, both logged so we can see which one works:
        //  1. SO_NBIO (0x1200) is the PS4's own non-blocking socket option.
        //  2. A 1 ms receive timeout as a fallback: the extra read returns almost at once
        //     with EAGAIN, which libsmb2 treats as "no more data right now".
        if (fd != prepared_fd) {
            int one = 1;
            int nr = setsockopt(fd, SOL_SOCKET, 0x1200 /* SO_NBIO */, &one, sizeof(one));
            int ne = errno;
            struct timeval tv;
            tv.tv_sec = 0; tv.tv_usec = 1000;
            int tr = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            LOG("[smb] fd %d: SO_NBIO -> %d (errno %d), SO_RCVTIMEO 1ms -> %d\n", fd, nr, ne, tr);
            prepared_fd = fd;
        }

        struct pollfd p;
        p.fd = fd; p.events = ev; p.revents = 0;
        int pr = poll(&p, 1, 100);
        waited += 100;
        if (pr > 0 && p.revents) {
            int sr = smb2_service(smb2, p.revents);
            if (sr < 0) {
                LOG("[smb] service error: %s\n", smb2_get_error(smb2));
                return sr;
            }
        }
    }

    if (!cs.done) {
        LOG("[smb] connect timed out after %d ms\n", waited);
        return -ETIMEDOUT;
    }
    if (cs.status != 0) {
        LOG("[smb] connect callback status %d (%s)\n", cs.status, smb2_get_error(smb2));
        return cs.status < 0 ? cs.status : -cs.status;
    }
    LOG("[smb] connected\n");
    return 0;
}

static void ctx_close(void) {
    if (ctx) {
        smb2_destroy_context(ctx);
        ctx = NULL;
    }
}

static int smb_open_context(void) {
    ctx = smb2_init_context();
    if (ctx == NULL) {
        snprintf(worker_error, sizeof(worker_error), "smb2_init_context failed");
        return -1;
    }
    smb2_set_user(ctx, cfg.user);
    smb2_set_password(ctx, cfg.pass);
    smb2_set_timeout(ctx, 10);

    uint64_t tc0 = sceKernelGetProcessTime();
    int rc = smb_connect(ctx);
    LOG("[time] connect took %llu ms (rc=%d)\n",
        (unsigned long long)((sceKernelGetProcessTime() - tc0) / 1000ULL), rc);
    if (rc < 0) {
        snprintf(worker_error, sizeof(worker_error), "Connect failed (%d): %s", rc, smb2_get_error(ctx));
        ctx_close();
        return rc;
    }
    return 0;
}

static int has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static int is_video_name(const char *n) {
    return has_ext(n, ".mkv") || has_ext(n, ".mp4") || has_ext(n, ".m4v") || has_ext(n, ".mov") || has_ext(n, ".webm");
}

static int cmp_items(const void *a, const void *b) {
    const item_t *x = (const item_t *)a;
    const item_t *y = (const item_t *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;   // folders first
    if (!x->is_dir && cfg.sort == 1 && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    if (!x->is_dir && cfg.sort == 2 && x->size != y->size) return x->size < y->size ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

static char g_filter[64];                   // browser search text (empty = show everything)
static int filter_ok(const char *name) {
    if (!g_filter[0]) return 1;
    size_t fl = strlen(g_filter), nl = strlen(name);
    for (size_t i = 0; i + fl <= nl; i++) if (strncasecmp(name + i, g_filter, fl) == 0) return 1;
    return 0;
}

static void do_list(const char *path) {
    LOG("[smb] listing '%s'\n", path);
    uint64_t tl0 = sceKernelGetProcessTime();
    struct smb2dir *dir = smb2_opendir(ctx, path);
    uint64_t tl1 = sceKernelGetProcessTime();
    if (dir == NULL) {
        snprintf(worker_error, sizeof(worker_error), "Cannot open folder: %s", smb2_get_error(ctx));
        LOG("[smb] %s\n", worker_error);
        ctx_close();                 // the next request reconnects
        list_state = LS_ERR;
        return;
    }

    int count = 0;
    struct smb2dirent *ent;
    while ((ent = smb2_readdir(ctx, dir)) != NULL) {
        if (!strcmp(ent->name, ".") || !strcmp(ent->name, "..")) continue;
        if (count >= MAX_ITEMS) break;
        int isd = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
        if (!isd && !is_video_name(ent->name)) continue;                   // only folders and playable videos are shown
        if (!filter_ok(ent->name)) continue;
        item_t *it = &work_items[count++];
        memset(it, 0, sizeof(*it));
        set_str(it->name, sizeof(it->name), ent->name);
        it->is_dir = isd;
        it->size = ent->st.smb2_size;
        it->mtime = (int64_t)ent->st.smb2_mtime;
    }
    smb2_closedir(ctx, dir);
    uint64_t tl2 = sceKernelGetProcessTime();
    LOG("[time] opendir %llu ms, readdir %llu ms, %d entries\n",
        (unsigned long long)((tl1 - tl0) / 1000ULL),
        (unsigned long long)((tl2 - tl1) / 1000ULL), count);

    qsort(work_items, count, sizeof(item_t), cmp_items);
    work_count = count;
    LOG("[smb] %d entries\n", count);
    __sync_synchronize();
    list_state = LS_OK;
}

#define BENCH_BYTES   (64ULL * 1048576ULL)    // stop after this much data...
#define BENCH_MAX_US  10000000ULL             // ...or after this long

static void do_bench(const char *path) {
    LOG("[bench] opening '%s'\n", path);
    uint64_t t0 = sceKernelGetProcessTime();
    struct smb2fh *fh = smb2_open(ctx, path, O_RDONLY);
    uint64_t t1 = sceKernelGetProcessTime();
    if (fh == NULL) {
        snprintf(worker_error, sizeof(worker_error), "Cannot open file: %s", smb2_get_error(ctx));
        LOG("[bench] %s\n", worker_error);
        list_state = LS_ERR;
        return;
    }

    uint32_t maxread = smb2_get_max_read_size(ctx);
    size_t chunk = maxread ? maxread : 65536;
    if (chunk > 1048576) chunk = 1048576;
    LOG("[bench] open took %llu ms, server max read %u, using %u byte reads\n",
        (unsigned long long)((t1 - t0) / 1000ULL), (unsigned)maxread, (unsigned)chunk);

    unsigned char *buf = (unsigned char *)malloc(chunk);
    if (!buf) {
        smb2_close(ctx, fh);
        snprintf(worker_error, sizeof(worker_error), "Out of memory for the read buffer");
        list_state = LS_ERR;
        return;
    }

    uint64_t total = 0, now = t1, last_log = t1;
    int err = 0;
    for (;;) {
        int n = smb2_read(ctx, fh, buf, (uint32_t)chunk);
        now = sceKernelGetProcessTime();
        if (n < 0) { err = n; break; }
        if (n == 0) break;                                  // end of file
        total += (uint64_t)n;
        if (now - last_log >= 1000000ULL) {
            LOG("[bench] %llu KB read so far, %llu ms\n",
                (unsigned long long)(total / 1024ULL),
                (unsigned long long)((now - t1) / 1000ULL));
            last_log = now;
        }
        if (total >= BENCH_BYTES || now - t1 >= BENCH_MAX_US) break;
    }
    smb2_close(ctx, fh);
    free(buf);

    uint64_t el = now - t1;
    if (el == 0) el = 1;
    if (err < 0 && total == 0) {
        snprintf(worker_error, sizeof(worker_error), "Read failed (%d): %s", err, smb2_get_error(ctx));
        LOG("[bench] %s\n", worker_error);
        ctx_close();
        list_state = LS_ERR;
        return;
    }

    // MB/s with one decimal, integer math only
    unsigned long long x10 = (unsigned long long)(total * 10ULL * 1000000ULL / (el * 1048576ULL));
    snprintf(bench_text, sizeof(bench_text),
             "Speed test: %llu.%llu MB/s   (%llu MB in %llu.%llu s, open %llu ms%s)",
             x10 / 10ULL, x10 % 10ULL,
             (unsigned long long)(total / 1048576ULL),
             (unsigned long long)(el / 1000000ULL), (unsigned long long)((el % 1000000ULL) / 100000ULL),
             (unsigned long long)((t1 - t0) / 1000ULL),
             err < 0 ? ", stopped by a read error" : "");
    LOG("[bench] %s\n", bench_text);
    __sync_synchronize();
    list_state = LS_OK;
}

static void do_list_local(int id, const char *rel) {
    char full[600];
    local_full(id, full, sizeof(full), rel);
    LOG("[local] listing '%s'\n", full);
    DIR *d = opendir(full);
    if (!d) {
        snprintf(worker_error, sizeof(worker_error), "Cannot open folder %s: %s", full, strerror(errno));
        LOG("[local] %s\n", worker_error);
        list_state = LS_ERR;
        return;
    }
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (count >= MAX_ITEMS) break;
        char fp[900];
        snprintf(fp, sizeof(fp), "%s/%s", full, de->d_name);
        struct stat st;
        if (stat(fp, &st) != 0) continue;                   // broken link etc.
        if (!S_ISDIR(st.st_mode) && !is_video_name(de->d_name)) continue;   // only folders and playable videos are shown
        if (!filter_ok(de->d_name)) continue;
        item_t *it = &work_items[count++];
        memset(it, 0, sizeof(*it));
        set_str(it->name, sizeof(it->name), de->d_name);
        it->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        it->size = it->is_dir ? 0 : (uint64_t)st.st_size;
        it->mtime = (int64_t)st.st_mtime;
    }
    closedir(d);
    qsort(work_items, count, sizeof(item_t), cmp_items);
    work_count = count;
    LOG("[local] %d entries\n", count);
    __sync_synchronize();
    list_state = LS_OK;
}

static void do_bench_local(int id, const char *rel) {
    char full[600];
    local_full(id, full, sizeof(full), rel);
    uint64_t t0 = sceKernelGetProcessTime();
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        snprintf(worker_error, sizeof(worker_error), "Cannot open file: %s", strerror(errno));
        list_state = LS_ERR;
        return;
    }
    size_t chunk = 1048576;
    unsigned char *buf = (unsigned char *)malloc(chunk);
    if (!buf) { close(fd); snprintf(worker_error, sizeof(worker_error), "Out of memory for the read buffer"); list_state = LS_ERR; return; }
    uint64_t t1 = sceKernelGetProcessTime(), now = t1, total = 0;
    int err = 0;
    for (;;) {
        ssize_t n = read(fd, buf, chunk);
        now = sceKernelGetProcessTime();
        if (n < 0) { err = 1; break; }
        if (n == 0) break;
        total += (uint64_t)n;
        if (total >= BENCH_BYTES || now - t1 >= BENCH_MAX_US) break;
    }
    close(fd);
    free(buf);
    uint64_t el = now - t1;
    if (el == 0) el = 1;
    unsigned long long x10 = (unsigned long long)(total * 10ULL * 1000000ULL / (el * 1048576ULL));
    snprintf(bench_text, sizeof(bench_text), "Speed test: %llu.%llu MB/s   (%llu MB in %llu.%llu s, open %llu ms%s)",
             x10 / 10ULL, x10 % 10ULL, (unsigned long long)(total / 1048576ULL),
             (unsigned long long)(el / 1000000ULL), (unsigned long long)((el % 1000000ULL) / 100000ULL),
             (unsigned long long)((t1 - t0) / 1000ULL), err ? ", stopped by a read error" : "");
    LOG("[bench] %s\n", bench_text);
    __sync_synchronize();
    list_state = LS_OK;
}

// Settings -> "Test connection": fresh connection with the values on screen, then try to open the start folder
static void do_test(const char *path) {
    ctx_close();
    if (!cfg.server[0] || !cfg.share[0]) {
        snprintf(worker_error, sizeof(worker_error), "Fill in the server address and the share name first");
        __sync_synchronize(); list_state = LS_ERR; return;
    }
    if (smb_open_context() < 0) { __sync_synchronize(); list_state = LS_ERR; return; }
    struct smb2dir *d = smb2_opendir(ctx, path);
    if (!d) snprintf(bench_text, sizeof(bench_text), "Connected to //%s/%s, but the start folder \"/%s\" was not found (%s)", cfg.server, cfg.share, path, smb2_get_error(ctx));
    else {
        smb2_closedir(ctx, d);
        snprintf(bench_text, sizeof(bench_text), "Connection OK:  //%s/%s/%s", cfg.server, cfg.share, path);
    }
    __sync_synchronize();
    list_state = LS_OK;
}

static void *net_worker(void *arg) {
    (void)arg;
    for (;;) {
        if (!req_pending) {
            sceKernelUsleep(20000);
            continue;
        }
        __sync_synchronize();
        char path[512];
        set_str(path, sizeof(path), req_path);
        int is_bench = req_is_bench;
        int rsrc = req_src;
        req_pending = 0;

        if (is_bench == 2) { busy_what = "Testing"; do_test(path); continue; }
        if (src_is_local(rsrc)) {                            // no network involved
            if (is_bench) { busy_what = "Speed test"; do_bench_local(rsrc, path); }
            else          { busy_what = "Loading folder"; do_list_local(rsrc, path); }
            continue;
        }
        if (!ctx) {
            busy_what = "Connecting";
            if (smb_open_context() < 0) {
                __sync_synchronize();
                list_state = LS_ERR;
                continue;
            }
        }
        if (is_bench) {
            busy_what = "Speed test";
            do_bench(path);
        } else {
            busy_what = "Loading folder";
            do_list(path);
        }
    }
    return NULL;
}

// ---------- font and text ----------

static unsigned char atlas[ATLAS_W * ATLAS_H];
static stbtt_bakedchar baked[GLYPH_COUNT];
static int font_ok = 0;
#define SUB_PX 46.0f
#define SATLAS_H 1024
static unsigned char satlas[ATLAS_W * SATLAS_H];    // larger glyphs for text subtitles
static unsigned char *font_data = NULL;             // the TTF stays in memory so the subtitle size can change
static int g_sub_pitch = 54;                        // line spacing of text subtitles
static stbtt_bakedchar sbaked[GLYPH_COUNT];
static int sfont_ok = 0;

static const float sub_size_px[3] = { 36.0f, 46.0f, 58.0f };
static void sub_apply_size(void) {
    if (!font_data) return;
    float px = sub_size_px[cfg.sub_size >= 0 && cfg.sub_size <= 2 ? cfg.sub_size : 1];
    sfont_ok = stbtt_BakeFontBitmap(font_data, 0, px, satlas, ATLAS_W, SATLAS_H, GLYPH_FIRST, GLYPH_COUNT, sbaked) > 0;
    g_sub_pitch = (int)(px * 1.17f + 0.5f);
}

static int font_load(void) {
    FILE *f = fopen(FONT_PATH, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -2; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return -3; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return -4; }
    font_data = buf;

    int r = stbtt_BakeFontBitmap(buf, 0, FONT_PX, atlas, ATLAS_W, ATLAS_H,
                                 GLYPH_FIRST, GLYPH_COUNT, baked);
    sub_apply_size();
    return r == 0 ? -5 : 0;
}

// decode one UTF-8 character; anything outside Latin-1 becomes '?'
static int next_cp(const unsigned char **p) {
    const unsigned char *s = *p;
    unsigned c = s[0];
    if (c < 0x80) { (*p)++; return (int)c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        int cp = (int)(((c & 0x1F) << 6) | (s[1] & 0x3F));
        (*p) += 2;
        return cp;
    }
    int len = ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
    int k = 1;
    while (k < len && (s[k] & 0xC0) == 0x80) k++;
    (*p) += k;
    return '?';
}

static const stbtt_bakedchar *glyph_for(int cp) {
    if (cp < GLYPH_FIRST || cp >= GLYPH_FIRST + GLYPH_COUNT) cp = '?';
    return &baked[cp - GLYPH_FIRST];
}

static int text_width(const char *s) {
    if (!font_ok) return 0;
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) w += (int)(glyph_for(next_cp(&p))->xadvance + 0.5f);
    return w;
}

static void blit_glyph(uint32_t *buf, const stbtt_bakedchar *b, int dx, int dy,
                       uint32_t col, int max_x) {
    int gw = b->x1 - b->x0;
    int gh = b->y1 - b->y0;
    int cr = (col >> 16) & 0xFF, cg = (col >> 8) & 0xFF, cb = col & 0xFF;
    for (int j = 0; j < gh; j++) {
        int y = dy + j;
        if (y < 0 || y >= HEIGHT) continue;
        uint32_t *row = buf + y * WIDTH;
        const unsigned char *src = atlas + (b->y0 + j) * ATLAS_W + b->x0;
        for (int i = 0; i < gw; i++) {
            int x = dx + i;
            if (x < 0 || x >= WIDTH || x >= max_x) continue;
            int a = src[i];
            if (a == 0) continue;
            uint32_t d = row[x];
            int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            int r = (cr * a + dr * (255 - a)) / 255;
            int g = (cg * a + dg * (255 - a)) / 255;
            int bl = (cb * a + db * (255 - a)) / 255;
            row[x] = RGB(r, g, bl);
        }
    }
}

static void draw_text(uint32_t *buf, int x, int baseline, const char *s,
                      uint32_t col, int max_x) {
    if (!font_ok) return;
    int penx = x;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        const stbtt_bakedchar *b = glyph_for(next_cp(&p));
        int adv = (int)(b->xadvance + 0.5f);
        if (penx + adv > max_x) break;
        blit_glyph(buf, b, penx + (int)(b->xoff + (b->xoff < 0 ? -0.5f : 0.5f)),
                   baseline + (int)(b->yoff + (b->yoff < 0 ? -0.5f : 0.5f)), col, max_x);
        penx += adv;
    }
}

static const stbtt_bakedchar *sglyph_for(int cp) {
    if (cp < GLYPH_FIRST || cp >= GLYPH_FIRST + GLYPH_COUNT) cp = '?';
    return &sbaked[cp - GLYPH_FIRST];
}

static int sub_text_width(const char *s, int n) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    while (p < e && *p) w += (int)(sglyph_for(next_cp(&p))->xadvance + 0.5f);
    return w;
}

static void sblit(uint32_t *buf, const stbtt_bakedchar *b, int dx, int dy, int white) {
    int gw = b->x1 - b->x0, gh = b->y1 - b->y0;
    for (int j = 0; j < gh; j++) {
        int y = dy + j;
        if (y < 0 || y >= HEIGHT) continue;
        const unsigned char *src = satlas + (b->y0 + j) * ATLAS_W + b->x0;
        for (int i = 0; i < gw; i++) {
            int x = dx + i, a = src[i];
            if (a == 0 || x < 0 || x >= WIDTH) continue;
            uint32_t d = buf[y * WIDTH + x];
            int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF, c = white ? 255 : 0;
            buf[y * WIDTH + x] = RGB((c * a + dr * (255 - a)) / 255, (c * a + dg * (255 - a)) / 255, (c * a + db * (255 - a)) / 255);
        }
    }
}

// white text with a black outline (the outline is the glyph drawn at 8 offsets)
static void draw_sub_text(uint32_t *buf, int x, int baseline, const char *s) {
    static const int off[8][2] = {{-2,0},{2,0},{0,-2},{0,2},{-2,-2},{2,-2},{-2,2},{2,2}};
    for (int pass = 0; pass < 9; pass++) {
        int penx = x;
        const unsigned char *p = (const unsigned char *)s;
        int ox = pass < 8 ? off[pass][0] : 0, oy = pass < 8 ? off[pass][1] : 0;
        while (*p) {
            const stbtt_bakedchar *b = sglyph_for(next_cp(&p));
            sblit(buf, b, penx + (int)(b->xoff + (b->xoff < 0 ? -0.5f : 0.5f)) + ox,
                  baseline + (int)(b->yoff + (b->yoff < 0 ? -0.5f : 0.5f)) + oy, pass == 8);
            penx += (int)(b->xadvance + 0.5f);
        }
    }
}

static void fill_rect(uint32_t *buf, int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= HEIGHT) continue;
        uint32_t *row = buf + j * WIDTH;
        for (int i = x; i < x + w; i++) {
            if (i >= 0 && i < WIDTH) row[i] = c;
        }
    }
}

static void fmt_size(char *o, size_t n, uint64_t s) {
    unsigned long long v = (unsigned long long)s;
    if (v < 1024ULL)
        snprintf(o, n, "%llu B", v);
    else if (v < 1048576ULL)
        snprintf(o, n, "%llu.%llu KB", v / 1024ULL, (v % 1024ULL) * 10ULL / 1024ULL);
    else if (v < 1073741824ULL)
        snprintf(o, n, "%llu.%llu MB", v / 1048576ULL, (v % 1048576ULL) * 10ULL / 1048576ULL);
    else
        snprintf(o, n, "%llu.%llu GB", v / 1073741824ULL, (v % 1073741824ULL) * 10ULL / 1073741824ULL);
}

// ---------- UI state ----------

static char cur_path[512];
static char prev_path[512];
static int sel = 0, scroll_top = 0;
static int sel_stack[32], scroll_stack[32];
static int depth = 0;
static char ui_status[200];      // error text shown on screen
static char ui_bench[200];       // last speed-test result
static const char *ui_note = NULL;   // fatal message (e.g. config problems)
static int have_config = 0;

enum { K_REFRESH = 0, K_ENTER = 1, K_BACK = 2, K_BENCH = 3 };
static int pend_kind = K_REFRESH;
static int pend_sel = 0, pend_scroll = 0;

static int redraw_needed = 1;
static void redraw(void) { redraw_needed = 1; }

// ---------- settings screen (Start) ----------

#define SET_N 9
static int settings_open = 0, set_sel = 0, conn_dirty = 0;
static int g_ime_user = 0;
static const char *set_label[SET_N] = { "Server address", "Share name", "Start folder", "User", "Password",
                                        "Test connection", "Subtitle size", "Auto-play next episode", "Debug log" };
static const char *sub_size_name[3] = { "Small", "Medium", "Large" };

static char *set_field(int i, size_t *n) {
    switch (i) {
    case 0: *n = sizeof(cfg.server); return cfg.server;
    case 1: *n = sizeof(cfg.share);  return cfg.share;
    case 2: *n = sizeof(cfg.path);   return cfg.path;
    case 3: *n = sizeof(cfg.user);   return cfg.user;
    case 4: *n = sizeof(cfg.pass);   return cfg.pass;
    }
    return NULL;
}

// --- text entry: the system keyboard when the toolchain provides it, otherwise a built-in one ---

// weak: if libSceImeDialog is not in the toolchain these stay NULL and the built-in keyboard is used
extern int32_t sceImeDialogInit(const OrbisImeDialogSetting *, OrbisImeSettingsExtended *) __attribute__((weak));
extern OrbisDialogStatus sceImeDialogGetStatus(void) __attribute__((weak));
extern int32_t sceImeDialogGetResult(OrbisDialogResult *) __attribute__((weak));
extern int32_t sceImeDialogTerm(void) __attribute__((weak));

static int ime_active = 0, kb_active = 0;
static int te_field = -1;                    // which setting is being edited
static uint16_t ime_buf[300], ime_title[40];

static void text_input_done(int ok, const char *text);

static void utf8_to_u16(uint16_t *dst, size_t n, const char *s) {
    size_t k = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && k + 1 < n) dst[k++] = (uint16_t)next_cp(&p);
    dst[k] = 0;
}

static void u16_to_utf8(char *dst, size_t n, const uint16_t *s) {
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        unsigned c = *s;
        if (c < 0x80) dst[k++] = (char)c;
        else if (c < 0x800) { dst[k++] = (char)(0xC0 | (c >> 6)); dst[k++] = (char)(0x80 | (c & 0x3F)); }
        else { dst[k++] = (char)(0xE0 | (c >> 12)); dst[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); dst[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    dst[k] = 0;
}

static int ime_available(void) {
    return sceImeDialogInit && sceImeDialogGetStatus && sceImeDialogGetResult && sceImeDialogTerm;
}

static int ime_begin(const char *title, const char *initial, int maxlen, int secret) {
    static int loaded = 0;
    if (!ime_available()) return -1;
    if (!loaded) { sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG); loaded = 1; }
    if (maxlen > 250) maxlen = 250;
    memset(ime_buf, 0, sizeof(ime_buf));
    utf8_to_u16(ime_buf, 260, initial);
    utf8_to_u16(ime_title, 40, title);
    OrbisImeDialogSetting s;
    memset(&s, 0, sizeof(s));
    s.userId = (uint32_t)g_ime_user;
    s.type = ORBIS_TYPE_BASIC_LATIN;
    s.supportedLanguages = 0;
    s.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    s.inputMethod = ORBIS__DEFAULT;
    s.filter = NULL;
    s.option = 2 | (secret ? 4 : 0);              // no auto-capitalisation (+ password masking)
    s.maxTextLength = (uint32_t)maxlen;
    s.inputTextBuffer = (wchar_t *)ime_buf;
    s.posx = 960.0f; s.posy = 540.0f;
    s.horizontalAlignment = ORBIS_H_CENTER;
    s.verticalAlignment = ORBIS_V_CENTER;
    s.placeholder = NULL;
    s.title = (const wchar_t *)ime_title;
    int rc = sceImeDialogInit(&s, NULL);
    LOG("[ime] init -> 0x%x\n", rc);
    if (rc != 0) return -2;
    ime_active = 1;
    return 0;
}

static void ime_poll(void) {
    if (sceImeDialogGetStatus() != ORBIS_DIALOG_STATUS_STOPPED) return;
    OrbisDialogResult r;
    memset(&r, 0, sizeof(r));
    sceImeDialogGetResult(&r);
    char t[300];
    u16_to_utf8(t, sizeof(t), ime_buf);
    int ok = (r.endstatus == ORBIS_DIALOG_OK);
    sceImeDialogTerm();
    ime_active = 0;
    LOG("[ime] closed ok=%d\n", ok);
    text_input_done(ok, t);
}

// --- built-in keyboard (D-pad) ---
static char kb_text[260];
static int kb_len, kb_secret, kb_max, kb_page, kb_r, kb_c;
static char kb_title[48];
static const char *kb_rows[3][4] = {
    { "1234567890-_.", "qwertyuiop@:/", "asdfghjkl,;'~", "zxcvbnm!?#+=%" },
    { "1234567890-_.", "QWERTYUIOP@:/", "ASDFGHJKL,;'~", "ZXCVBNM!?#+=%" },
    { "!@#$%^&*()-_=", "[]{}<>\\|/:;\"'", "`~+?,.!@#$%^&", "()[]{}<>=*&^_" } };
static const char *kb_actions[5] = { "Shift", "Space", "Del", "OK", "Cancel" };
static const char *kb_page_name[3] = { "abc", "ABC", "#+=" };

static void kb_begin(const char *title, const char *initial, int maxlen, int secret) {
    set_str(kb_title, sizeof(kb_title), title);
    set_str(kb_text, sizeof(kb_text), initial);
    kb_len = (int)strlen(kb_text);
    kb_max = maxlen > 250 ? 250 : maxlen;
    kb_secret = secret;
    kb_page = 0; kb_r = 1; kb_c = 0;
    kb_active = 1;
}

static void kb_input(unsigned pressed) {
    if (pressed & ORBIS_PAD_BUTTON_LEFT)  { kb_c--; if (kb_c < 0) kb_c = (kb_r == 4 ? 4 : 12); }
    if (pressed & ORBIS_PAD_BUTTON_RIGHT) { kb_c++; if (kb_c > (kb_r == 4 ? 4 : 12)) kb_c = 0; }
    if (pressed & ORBIS_PAD_BUTTON_UP) {
        if (kb_r == 4) kb_c = kb_c * 13 / 5 + 1;
        kb_r = (kb_r + 4) % 5;
        if (kb_r == 4) kb_c = kb_c * 5 / 13;
    }
    if (pressed & ORBIS_PAD_BUTTON_DOWN) {
        if (kb_r == 3) kb_c = kb_c * 5 / 13;
        else if (kb_r == 4) kb_c = kb_c * 13 / 5 + 1;
        kb_r = (kb_r + 1) % 5;
    }
    if (kb_r < 4 && kb_c > 12) kb_c = 12;
    int act = -1;
    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
        if (kb_r < 4) {
            if (kb_len < kb_max && kb_len < (int)sizeof(kb_text) - 1) { kb_text[kb_len++] = kb_rows[kb_page][kb_r][kb_c]; kb_text[kb_len] = 0; }
        } else act = kb_c;
    }
    if (pressed & ORBIS_PAD_BUTTON_SQUARE) act = 2;
    if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) act = 0;
    if (pressed & ORBIS_PAD_BUTTON_L1) act = 1;
    if (pressed & ORBIS_PAD_BUTTON_OPTIONS) act = 3;
    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) act = 4;
    if (act == 0) kb_page = (kb_page + 1) % 3;
    else if (act == 1) { if (kb_len < kb_max) { kb_text[kb_len++] = ' '; kb_text[kb_len] = 0; } }
    else if (act == 2) { if (kb_len > 0) kb_text[--kb_len] = 0; }
    else if (act == 3) { kb_active = 0; text_input_done(1, kb_text); }
    else if (act == 4) { kb_active = 0; text_input_done(0, ""); }
    redraw();
}

static void draw_keyboard(uint32_t *buf) {
    const int cw = 104, ch = 64, gw = 13 * cw, gx = (WIDTH - gw) / 2, gy = 400;
    fill_rect(buf, gx - 30, 190, gw + 60, 730, RGB(28, 28, 40));
    fill_rect(buf, gx - 30, 190, gw + 60, 4, RGB(120, 200, 255));
    draw_text(buf, gx, 250, kb_title, RGB(120, 200, 255), WIDTH);
    char shown[260];
    if (kb_secret) { int n = kb_len < 250 ? kb_len : 250; memset(shown, '*', (size_t)n); shown[n] = 0; }
    else set_str(shown, sizeof(shown), kb_text);
    fill_rect(buf, gx, 280, gw, 70, RGB(12, 12, 18));
    int tw = text_width(shown);
    const char *sp = shown;
    while (tw > gw - 40 && *sp) { sp++; tw = text_width(sp); }          // keep the end visible
    draw_text(buf, gx + 14, 328, sp, RGB(255, 255, 255), gx + gw);
    fill_rect(buf, gx + 14 + tw + 2, 296, 3, 40, RGB(255, 255, 255));
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 13; c++) {
            int x = gx + c * cw, y = gy + r * (ch + 8);
            int cur = (kb_r == r && kb_c == c);
            fill_rect(buf, x + 3, y, cw - 6, ch, cur ? RGB(40, 80, 160) : RGB(52, 52, 70));
            char s[2] = { kb_rows[kb_page][r][c], 0 };
            draw_text(buf, x + cw / 2 - text_width(s) / 2, y + 44, s, RGB(235, 235, 245), WIDTH);
        }
    for (int c = 0; c < 5; c++) {
        int aw = gw / 5, x = gx + c * aw, y = gy + 4 * (ch + 8);
        int cur = (kb_r == 4 && kb_c == c);
        fill_rect(buf, x + 4, y, aw - 8, ch, cur ? RGB(40, 80, 160) : RGB(70, 60, 90));
        char lab[32];
        if (c == 0) snprintf(lab, sizeof(lab), "Shift (%s)", kb_page_name[(kb_page + 1) % 3]);
        else set_str(lab, sizeof(lab), kb_actions[c]);
        draw_text(buf, x + aw / 2 - text_width(lab) / 2, y + 44, lab, RGB(235, 235, 245), WIDTH);
    }
    draw_text(buf, gx, gy + 5 * (ch + 8) + 36, "D-pad move   Cross type   Square delete   Triangle shift   L1 space   Start OK   Circle cancel",
              RGB(150, 150, 165), WIDTH - 40);
}

static void draw_settings(uint32_t *buf, int dots) {
    draw_text(buf, MARGIN_X, 80, "Settings", RGB(120, 200, 255), 900);
    fill_rect(buf, MARGIN_X, 136, WIDTH - 2 * MARGIN_X, 2, RGB(70, 70, 90));
    const int pitch = 62;
    for (int i = 0; i < SET_N; i++) {
        int top = LIST_TOP + i * pitch;
        if (i == set_sel) fill_rect(buf, MARGIN_X - 20, top, WIDTH - 2 * MARGIN_X + 40, pitch - 6, RGB(40, 80, 160));
        draw_text(buf, MARGIN_X, top + 38, set_label[i], RGB(235, 235, 245), 700);
        char v[300] = "";
        uint32_t col = RGB(190, 220, 255);
        switch (i) {
        case 0: case 1: case 3: { size_t n; char *p = set_field(i, &n); if (p[0]) set_str(v, sizeof(v), p); else { set_str(v, sizeof(v), "(not set)"); col = RGB(150, 150, 160); } } break;
        case 2: if (cfg.path[0]) snprintf(v, sizeof(v), "/%s", cfg.path); else { set_str(v, sizeof(v), "(top level of the share)"); col = RGB(150, 150, 160); } break;
        case 4: { int n = (int)strlen(cfg.pass); if (n > 40) n = 40; if (n) { memset(v, '*', (size_t)n); v[n] = 0; } else { set_str(v, sizeof(v), "(empty)"); col = RGB(150, 150, 160); } } break;
        case 5: set_str(v, sizeof(v), list_state == LS_LOADING ? "Testing..." : "Cross to try the connection"); col = RGB(150, 210, 150); break;
        case 6: snprintf(v, sizeof(v), "<  %s  >", sub_size_name[cfg.sub_size >= 0 && cfg.sub_size <= 2 ? cfg.sub_size : 1]); break;
        case 7: snprintf(v, sizeof(v), "<  %s  >", cfg.auto_next ? "On" : "Off"); break;
        case 8: snprintf(v, sizeof(v), "<  %s  >   (for troubleshooting; log.txt)", cfg.debug ? "On" : "Off"); break;
        }
        draw_text(buf, MARGIN_X + 620, top + 38, v, col, WIDTH - MARGIN_X);
    }
    int ry = LIST_TOP + SET_N * pitch + 30;
    if (list_state == LS_LOADING) {
        char t[64] = "Testing the connection";
        for (int i = 0; i < dots && i < 3; i++) strcat(t, ".");
        draw_text(buf, MARGIN_X, ry, t, RGB(255, 210, 90), WIDTH - MARGIN_X);
    } else if (ui_bench[0]) draw_text(buf, MARGIN_X, ry, ui_bench, RGB(120, 230, 140), WIDTH - MARGIN_X);
    else if (ui_status[0])  draw_text(buf, MARGIN_X, ry, ui_status, RGB(255, 120, 120), WIDTH - MARGIN_X);
    draw_text(buf, MARGIN_X, HEIGHT - 50, "Up/Down move   Cross edit / run   Left/Right change   Start or Circle: save and close",
              RGB(150, 150, 165), WIDTH - MARGIN_X);
    if (kb_active) draw_keyboard(buf);
}

static void draw_ui(uint32_t *buf, int dots) {
    fill_rect(buf, 0, 0, WIDTH, HEIGHT, RGB(18, 18, 24));
    if (settings_open) { draw_settings(buf, dots); return; }

    // header
    draw_text(buf, MARGIN_X, 80, "Jaja Video Player", RGB(120, 200, 255), 900);
    char hdr[200];
    src_label(g_src, hdr, sizeof(hdr));
    int hw = text_width(hdr);
    draw_text(buf, WIDTH - MARGIN_X - hw, 80, hdr, RGB(150, 150, 160), WIDTH - MARGIN_X);

    char pathline[560];
    if (g_src == SRC_RECENT) snprintf(pathline, sizeof(pathline), "Press Cross to resume");
    else if (g_src == SRC_SMB) snprintf(pathline, sizeof(pathline), "/%s", cur_path);
    else local_full(g_src, pathline, sizeof(pathline), cur_path);
    {
        static const char *sort_name[3] = { "name", "newest", "largest" };
        char tag[120] = "";
        if (g_src != SRC_RECENT) snprintf(tag, sizeof(tag), "%ssort: %s", g_filter[0] ? "" : "", sort_name[cfg.sort]);
        if (g_src != SRC_RECENT && g_filter[0]) snprintf(tag, sizeof(tag), "search: \"%.40s\"   sort: %s", g_filter, sort_name[cfg.sort]);
        int tw = text_width(tag);
        draw_text(buf, WIDTH - MARGIN_X - tw, 125, tag, g_filter[0] ? RGB(255, 210, 90) : RGB(130, 130, 145), WIDTH - MARGIN_X);
        draw_text(buf, MARGIN_X, 125, pathline, RGB(200, 200, 210), WIDTH - MARGIN_X - tw - 40);
    }
    fill_rect(buf, MARGIN_X, 136, WIDTH - 2 * MARGIN_X, 2, RGB(70, 70, 90));

    int loading = (list_state == LS_LOADING);
    char dotstr[8] = "";
    for (int i = 0; i < dots && i < 3; i++) strcat(dotstr, ".");

    if (ui_note) {
        draw_text(buf, MARGIN_X, LIST_TOP + 60, ui_note, RGB(255, 120, 120), WIDTH - MARGIN_X);
    } else if (item_count == 0) {
        char msg[300];
        if (loading) {
            snprintf(msg, sizeof(msg), "%s%s", (const char *)busy_what, dotstr);
            draw_text(buf, MARGIN_X, LIST_TOP + 60, msg, RGB(220, 220, 230), WIDTH - MARGIN_X);
        } else if (ui_status[0]) {
            draw_text(buf, MARGIN_X, LIST_TOP + 60, ui_status, RGB(255, 120, 120), WIDTH - MARGIN_X);
            draw_text(buf, MARGIN_X, LIST_TOP + 110, g_src == SRC_SMB ? "Press Triangle to retry. Check config.ini if it keeps failing."
                      : "Press Triangle to retry, Circle to go up, L2/R2 to change source.", RGB(150, 150, 160), WIDTH - MARGIN_X);
        } else {
            draw_text(buf, MARGIN_X, LIST_TOP + 60, g_src == SRC_RECENT ? "Nothing to continue yet - films and episodes you stop halfway appear here"
                      : "No folders or playable videos here", RGB(150, 150, 160), WIDTH - MARGIN_X);
        }
    } else {
        for (int r = 0; r < VISIBLE_ROWS; r++) {
            int idx = scroll_top + r;
            if (idx >= item_count) break;
            const item_t *it = &items[idx];
            int top = LIST_TOP + r * ROW_H;
            if (idx == sel)
                fill_rect(buf, MARGIN_X - 20, top, WIDTH - 2 * MARGIN_X + 40, ROW_H - 2, RGB(40, 80, 160));

            uint32_t col = it->is_dir ? RGB(255, 220, 100) : RGB(235, 235, 240);
            fill_rect(buf, MARGIN_X, top + 12, 14, 14, it->is_dir ? RGB(255, 200, 60) : RGB(110, 110, 125));
            draw_text(buf, MARGIN_X + 34, top + 30, it->name, col, 1520);
            if (it->info[0]) {
                int w = text_width(it->info);
                draw_text(buf, WIDTH - MARGIN_X - w, top + 30, it->info, RGB(150, 210, 150), WIDTH - MARGIN_X);
            } else if (!it->is_dir) {
                char sz[32];
                fmt_size(sz, sizeof(sz), it->size);
                int w = text_width(sz);
                draw_text(buf, WIDTH - MARGIN_X - w, top + 30, sz, RGB(170, 170, 185), WIDTH - MARGIN_X);
            }
        }
        // scrollbar
        if (item_count > VISIBLE_ROWS) {
            int track_h = VISIBLE_ROWS * ROW_H;
            int bar_h = track_h * VISIBLE_ROWS / item_count;
            if (bar_h < 20) bar_h = 20;
            int bar_y = LIST_TOP + (track_h - bar_h) * scroll_top / (item_count - VISIBLE_ROWS);
            fill_rect(buf, WIDTH - MARGIN_X + 40, LIST_TOP, 6, track_h, RGB(40, 40, 55));
            fill_rect(buf, WIDTH - MARGIN_X + 40, bar_y, 6, bar_h, RGB(120, 120, 150));
        }
    }

    // status and hint lines
    char st[260];
    if (loading) {
        snprintf(st, sizeof(st), "%s%s", (const char *)busy_what, dotstr);
        draw_text(buf, MARGIN_X, 985, st, RGB(220, 220, 230), 1400);
    } else if (ui_bench[0]) {
        draw_text(buf, MARGIN_X, 985, ui_bench, RGB(120, 220, 140), WIDTH - MARGIN_X);
    } else if (ui_status[0] && item_count > 0) {
        draw_text(buf, MARGIN_X, 985, ui_status, RGB(255, 120, 120), WIDTH - MARGIN_X);
    } else if (item_count > 0) {
        snprintf(st, sizeof(st), "%d of %d", sel + 1, item_count);
        draw_text(buf, MARGIN_X, 985, st, RGB(150, 150, 160), 1400);
    }
    draw_text(buf, MARGIN_X, 1020,
              "Up/Down move  L1/R1 page  L2/R2 source  Cross open  Circle back  Square speed test  Triangle refresh  hold Circle: quit",
              RGB(110, 110, 125), WIDTH - MARGIN_X);
    draw_text(buf, MARGIN_X, 1052, "Start settings  Touchpad search  R3 sort order", RGB(110, 110, 125), WIDTH - MARGIN_X);
    if (kb_active) draw_keyboard(buf);
}

// ---------- navigation ----------


static void request_list(int kind, const char *new_path) {
    set_str(prev_path, sizeof(prev_path), cur_path);
    set_str(cur_path, sizeof(cur_path), new_path);
    pend_kind = kind;
    if (kind != K_REFRESH) g_filter[0] = 0;               // a search only lasts for the folder it was made in
    ui_status[0] = 0;
    ui_bench[0] = 0;
    req_is_bench = 0;

    if (kind == K_ENTER) {
        if (depth < 32) { sel_stack[depth] = sel; scroll_stack[depth] = scroll_top; }
        depth++;
        pend_sel = 0; pend_scroll = 0;
    } else if (kind == K_BACK) {
        int d = depth - 1;
        pend_sel = (d >= 0 && d < 32) ? sel_stack[d] : 0;
        pend_scroll = (d >= 0 && d < 32) ? scroll_stack[d] : 0;
    } else {
        pend_sel = sel; pend_scroll = scroll_top;
    }

    set_str(req_path, sizeof(req_path), cur_path);
    req_src = g_src;
    list_state = LS_LOADING;
    __sync_synchronize();
    req_pending = 1;
    redraw();
}

static char autoplay_name[256];            // play this file as soon as the folder listing arrives
static void last_save(void);
static void enter_selected(void);

static void apply_result(void) {
    int st = list_state;
    if (pend_kind == K_BENCH) {                 // a speed test never changes the folder view
        if (st == LS_OK)       set_str(ui_bench, sizeof(ui_bench), bench_text);
        else if (st == LS_ERR) set_str(ui_status, sizeof(ui_status), worker_error);
        list_state = LS_IDLE;
        redraw();
        return;
    }
    if (st == LS_OK) {
        item_count = work_count;
        memcpy(items, work_items, sizeof(item_t) * (size_t)item_count);
        if (pend_kind == K_BACK && depth > 0) depth--;
        sel = pend_sel;
        scroll_top = pend_scroll;
        if (sel >= item_count) sel = item_count - 1;
        if (sel < 0) sel = 0;
        if (scroll_top > sel) scroll_top = sel;
        if (scroll_top < sel - VISIBLE_ROWS + 1) scroll_top = sel - VISIBLE_ROWS + 1;
        if (scroll_top < 0) scroll_top = 0;
        ui_status[0] = 0;
        list_state = LS_IDLE;
        redraw();
        last_save();
        if (autoplay_name[0]) {                            // opened from Continue watching: start that file
            for (int j = 0; j < item_count; j++)
                if (!items[j].is_dir && !strcmp(items[j].name, autoplay_name)) {
                    sel = j;
                    if (scroll_top > sel) scroll_top = sel;
                    if (scroll_top < sel - VISIBLE_ROWS + 1) scroll_top = sel - VISIBLE_ROWS + 1;
                    autoplay_name[0] = 0;
                    enter_selected();
                    break;
                }
            autoplay_name[0] = 0;
        }
    } else if (st == LS_ERR) {
        autoplay_name[0] = 0;
        set_str(ui_status, sizeof(ui_status), worker_error);
        set_str(cur_path, sizeof(cur_path), prev_path);       // stay where we were
        if (pend_kind == K_ENTER && depth > 0) depth--;
        list_state = LS_IDLE;
        redraw();
    }
}

static void move_sel(int delta) {
    if (item_count == 0) return;
    int n = sel + delta;
    if (n < 0) n = 0;
    if (n >= item_count) n = item_count - 1;
    if (n == sel) return;
    sel = n;
    if (sel < scroll_top) scroll_top = sel;
    if (sel >= scroll_top + VISIBLE_ROWS) scroll_top = sel - VISIBLE_ROWS + 1;
    redraw();
}

static int g_quit = 0;
static int g_play_skipped = 0;             // set by play_file(): the file was skipped with Triangle, not finished
static int play_file(const char *path);
static void recent_reload(void);
static int recent_count(void);
static const char *recent_path(int i);
static long recent_pos(int i);
static long recent_dur(int i);

// the path play_file() gets: the share-relative path for SMB, a full local path otherwise
static void play_path_for(char *dst, size_t n, const char *rel) {
    g_play_local = src_is_local(g_src);
    if (g_play_local) local_full(g_src, dst, n, rel); else snprintf(dst, n, "%s", rel);
}

static void open_recent(int i);

static void enter_selected(void) {
    if (item_count == 0 || sel >= item_count) return;
    if (g_src == SRC_RECENT) { open_recent(sel); return; }
    const item_t *it = &items[sel];
    char np[512];
    if (cur_path[0]) snprintf(np, sizeof(np), "%s/%s", cur_path, it->name);
    else             snprintf(np, sizeof(np), "%s", it->name);
    if (strlen(np) >= sizeof(np) - 1) return;
    if (!it->is_dir) {
        if (!is_video_name(it->name)) { notify("Jaja Video Player: only .mkv, .mp4, .m4v, .mov and .webm files can be played"); return; }
        for (;;) {
            char pp[900];
            play_path_for(pp, sizeof(pp), np);
            int r = play_file(pp);
            if (r == 1) { g_quit = 1; break; }
            if (r != 2) break;
            if (!cfg.auto_next && !g_play_skipped) break;   // Settings: do not start the next file by itself
            int nx = -1;                                    // next playable file in this folder
            for (int j = sel + 1; j < item_count; j++)
                if (!items[j].is_dir && is_video_name(items[j].name)) { nx = j; break; }
            if (nx < 0) { notify("Jaja Video Player: end of the folder"); break; }
            sel = nx;
            if (sel < scroll_top) scroll_top = sel;
            if (sel >= scroll_top + VISIBLE_ROWS) scroll_top = sel - VISIBLE_ROWS + 1;
            if (cur_path[0]) snprintf(np, sizeof(np), "%s/%s", cur_path, items[sel].name);
            else             snprintf(np, sizeof(np), "%s", items[sel].name);
        }
        redraw();
        return;
    }
    request_list(K_ENTER, np);
}

static void request_bench(void) {
    if (item_count == 0 || sel >= item_count) return;
    const item_t *it = &items[sel];
    if (it->is_dir || g_src == SRC_RECENT) return;   // the speed test reads a file
    char np[512];
    if (cur_path[0]) snprintf(np, sizeof(np), "%s/%s", cur_path, it->name);
    else             snprintf(np, sizeof(np), "%s", it->name);
    if (strlen(np) >= sizeof(np) - 1) return;

    pend_kind = K_BENCH;
    ui_status[0] = 0;
    ui_bench[0] = 0;
    set_str(req_path, sizeof(req_path), np);
    req_src = g_src;
    req_is_bench = 1;
    list_state = LS_LOADING;
    __sync_synchronize();
    req_pending = 1;
    redraw();
}

static void go_back(void) {
    if (cur_path[0] == 0) return;               // already at the top of the share
    char np[512];
    set_str(np, sizeof(np), cur_path);
    char *slash = strrchr(np, '/');
    if (slash) *slash = 0; else np[0] = 0;
    request_list(K_BACK, np);
}

// ---------- switching between sources (L2 / R2) ----------

typedef struct {
    int valid;
    char path[512];
    int sel, scroll, depth;
    int sel_stack[32], scroll_stack[32];
} srcsave_t;
static srcsave_t src_save[SRC_N];

static void normalize_start_path(char *dst, size_t n, const char *src);


static void fill_recent_items(void) {
    recent_reload();
    int n = recent_count();
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    for (int i = 0; i < n; i++) {
        item_t *it = &items[i];
        memset(it, 0, sizeof(*it));
        const char *p = recent_path(i);
        const char *base = strrchr(p, '/');
        const char *prev = base;
        if (base) { prev = base - 1; while (prev > p && *prev != '/') prev--; if (*prev == '/') prev++; }
        snprintf(it->name, sizeof(it->name), "%s", prev ? prev : p);
        long ps = recent_pos(i), ds = recent_dur(i);
        char t[16];
        if (ps >= 3600) snprintf(t, sizeof(t), "%ld:%02ld:%02ld", ps / 3600, (ps / 60) % 60, ps % 60);
        else snprintf(t, sizeof(t), "%ld:%02ld", ps / 60, ps % 60);
        if (ds > 0) snprintf(it->info, sizeof(it->info), "%ld%%   %s", ps * 100 / ds, t);
        else snprintf(it->info, sizeof(it->info), "%s", t);
    }
    item_count = n;
}

static int src_available(int id) {
    if (id == SRC_SMB) return have_config;
    if (id == SRC_INT) return 1;
    if (id == SRC_RECENT) { recent_reload(); return recent_count() > 0; }
    char p[24];
    local_full(id, p, sizeof(p), "");
    DIR *d = opendir(p);
    if (!d) return 0;
    closedir(d);
    return 1;
}

// the folder to open first for a source that has no remembered position
static void src_start_path(int id, char *dst, size_t n) {
    if (id == SRC_SMB) normalize_start_path(dst, n, cfg.path);
    else if (id == SRC_INT) normalize_start_path(dst, n, cfg.local);
    else dst[0] = 0;
}

// last place visited, restored at start-up
#define LAST_FILE "/data/jajasamba/last.txt"
static void last_save(void) {
    FILE *f = fopen(LAST_FILE, "w");
    if (!f) return;
    fprintf(f, "%d\n%s\n", g_src, g_src == SRC_RECENT ? "" : cur_path);
    fclose(f);
}

static void last_load(int *src, char *path, size_t n) {
    *src = -1; path[0] = 0;
    FILE *f = fopen(LAST_FILE, "r");
    if (!f) return;
    char l1[16] = "", l2[512] = "";
    if (fgets(l1, sizeof(l1), f)) {
        if (fgets(l2, sizeof(l2), f)) { size_t k = strlen(l2); while (k && (l2[k - 1] == '\n' || l2[k - 1] == '\r')) l2[--k] = 0; }
        *src = atoi(l1);
        set_str(path, n, l2);
    }
    fclose(f);
}

// make `next` the browsed source; forced = open that folder instead of the remembered / default one
static void enter_source(int next, const char *forced) {
    srcsave_t *s = &src_save[g_src];                       // remember where we were
    s->valid = 1; s->sel = sel; s->scroll = scroll_top; s->depth = depth;
    set_str(s->path, sizeof(s->path), cur_path);
    memcpy(s->sel_stack, sel_stack, sizeof(sel_stack)); memcpy(s->scroll_stack, scroll_stack, sizeof(scroll_stack));

    g_src = next;
    g_filter[0] = 0;
    item_count = 0;
    ui_status[0] = 0; ui_bench[0] = 0;
    s = &src_save[g_src];
    if (forced) {
        set_str(cur_path, sizeof(cur_path), forced);
        sel = 0; scroll_top = 0; depth = 0;
    } else if (s->valid) {
        set_str(cur_path, sizeof(cur_path), s->path);
        sel = s->sel; scroll_top = s->scroll; depth = s->depth;
        memcpy(sel_stack, s->sel_stack, sizeof(sel_stack)); memcpy(scroll_stack, s->scroll_stack, sizeof(scroll_stack));
    } else {
        src_start_path(g_src, cur_path, sizeof(cur_path));
        sel = 0; scroll_top = 0; depth = 0;
    }
    char lab[200];
    src_label(g_src, lab, sizeof(lab));
    LOG("[main] source -> %s\n", lab);
    if (g_src == SRC_RECENT) {
        cur_path[0] = 0;
        fill_recent_items();
        sel = 0; scroll_top = 0; depth = 0;
        list_state = LS_IDLE;
        last_save();
        redraw();
        return;
    }
    request_list(K_REFRESH, cur_path);
}

static void switch_source(int dir) {
    int next = g_src;
    for (int k = 0; k < SRC_N; k++) {
        next = (next + dir + SRC_N) % SRC_N;
        if (next != g_src && src_available(next)) break;
        if (k == SRC_N - 1) next = g_src;
    }
    if (next == g_src) { notify("Jaja Video Player: no other source available"); return; }
    enter_source(next, NULL);
}

// Continue watching -> open the folder of that file and start it, so "next episode" works as usual
static void open_recent(int i) {
    const char *p = recent_path(i);
    int src;
    const char *rel;
    if (p[0] == '/') {
        if (!strncmp(p, "/mnt/usb", 8) && p[8] >= '0' && p[8] <= '7' && p[9] == '/') { src = SRC_USB0 + (p[8] - '0'); rel = p + 10; }
        else if (!strncmp(p, "/mnt/ext0/", 10)) { src = SRC_EXT0; rel = p + 10; }
        else { src = SRC_INT; rel = p + 1; }
    } else {
        if (!have_config) { notify("Jaja Video Player: this entry is on the SMB share, which is not configured"); return; }
        src = SRC_SMB; rel = p;
    }
    char dir[512];
    set_str(dir, sizeof(dir), rel);
    char *slash = strrchr(dir, '/');
    const char *base = slash ? slash + 1 : dir;
    set_str(autoplay_name, sizeof(autoplay_name), base);
    if (slash) *slash = 0; else dir[0] = 0;
    if (src != SRC_SMB && src != SRC_INT && !src_available(src)) { notify("Jaja Video Player: that drive is not connected"); autoplay_name[0] = 0; return; }
    enter_source(src, dir);
}

// ---------- settings: logic ----------

static void search_apply(const char *text) {
    char t[300];
    set_str(t, sizeof(t), text);
    set_str(g_filter, sizeof(g_filter), trim(t));
    sel = 0; scroll_top = 0;
    request_list(K_REFRESH, cur_path);
}

static void search_begin(void) {
    if (g_src == SRC_RECENT || list_state != LS_IDLE) return;
    te_field = 99;
    if (ime_begin("Search in this folder (empty = show all)", g_filter, 60, 0) != 0) kb_begin("Search in this folder (empty = show all)", g_filter, 60, 0);
    redraw();
}

static void sort_cycle(void) {
    if (g_src == SRC_RECENT || list_state != LS_IDLE) return;
    cfg.sort = (cfg.sort + 1) % 3;
    save_config();
    sel = 0; scroll_top = 0;
    request_list(K_REFRESH, cur_path);
}

static void settings_open_now(void) {
    settings_open = 1; set_sel = 0; conn_dirty = 0;
    ui_status[0] = 0; ui_bench[0] = 0;
    redraw();
}

static void settings_close(void) {
    settings_open = 0;
    ui_status[0] = 0; ui_bench[0] = 0;
    save_config();
    if (conn_dirty) {
        conn_dirty = 0;
        ctx_close();                                   // the worker is idle here
        have_config = (cfg.server[0] && cfg.share[0]);
        src_save[SRC_SMB].valid = 0;
        if (g_src == SRC_SMB) {
            normalize_start_path(cur_path, sizeof(cur_path), cfg.path);
            sel = 0; scroll_top = 0; depth = 0;
            item_count = 0;
            if (have_config) request_list(K_REFRESH, cur_path);
        }
    }
    redraw();
}

static void search_apply(const char *text);
static void text_input_done(int ok, const char *text) {
    if (te_field == 99) {                              // browser search
        te_field = -1;
        if (ok) search_apply(text);
        redraw();
        return;
    }
    if (ok && te_field >= 0) {
        size_t n; char *p = set_field(te_field, &n);
        if (p) {
            char t[300];
            set_str(t, sizeof(t), text);
            char *s = trim(t);
            if (te_field == 2) { while (*s == '/' || *s == '\\') s++; }       // folder is stored without a leading slash
            for (char *q = s; *q; q++) if (te_field == 2 && *q == '\\') *q = '/';
            if (te_field != 4 && te_field != 2) { /* server/share/user: no spaces around */ }
            set_str(p, n, s);
            size_t l = strlen(p);
            while (te_field == 2 && l && p[l - 1] == '/') p[--l] = 0;
            conn_dirty = 1;
            ui_bench[0] = 0; ui_status[0] = 0;
        }
    }
    te_field = -1;
    redraw();
}

static void settings_edit(int i) {
    size_t n;
    char *p = set_field(i, &n);
    if (!p) return;
    te_field = i;
    int secret = (i == 4);
    if (ime_begin(set_label[i], p, (int)n - 1, secret) != 0) kb_begin(set_label[i], p, (int)n - 1, secret);
    redraw();
}

static void settings_test(void) {
    if (list_state != LS_IDLE) return;
    ctx_close();                                      // the worker is idle
    char np[256];
    normalize_start_path(np, sizeof(np), cfg.path);
    pend_kind = K_BENCH;
    ui_status[0] = 0; ui_bench[0] = 0;
    set_str(req_path, sizeof(req_path), np);
    req_src = SRC_SMB;
    req_is_bench = 2;
    list_state = LS_LOADING;
    __sync_synchronize();
    req_pending = 1;
    redraw();
}

static void settings_adjust(int dir) {
    if (set_sel == 6) {
        cfg.sub_size = (cfg.sub_size + dir + 3) % 3;
        sub_apply_size();
    } else if (set_sel == 7) cfg.auto_next = !cfg.auto_next;
    else if (set_sel == 8) { cfg.debug = !cfg.debug; g_debug = cfg.debug; }
    redraw();
}

static void settings_input(unsigned pressed) {
    if (kb_active) { kb_input(pressed); return; }
    if (list_state != LS_IDLE) return;
    if (pressed & ORBIS_PAD_BUTTON_UP)    { set_sel = (set_sel + SET_N - 1) % SET_N; redraw(); }
    if (pressed & ORBIS_PAD_BUTTON_DOWN)  { set_sel = (set_sel + 1) % SET_N; redraw(); }
    if (pressed & ORBIS_PAD_BUTTON_LEFT)  settings_adjust(-1);
    if (pressed & ORBIS_PAD_BUTTON_RIGHT) settings_adjust(1);
    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
        if (set_sel <= 4) settings_edit(set_sel);
        else if (set_sel == 5) settings_test();
        else settings_adjust(1);
    }
    if (pressed & (ORBIS_PAD_BUTTON_CIRCLE | ORBIS_PAD_BUTTON_OPTIONS)) settings_close();
}

// ---------- main ----------

static int pad_handle = -1;
static int video = -1;
static uint32_t *fb[NUM_BUFFERS];
static int back = 0;

static void do_exit(void) {
    LOG("[main] shutting down\n");
    if (pad_handle >= 0) scePadClose(pad_handle);
    if (video >= 0) sceVideoOutClose(video);
    sceSystemServiceLoadExec("exit", NULL);
    sceKernelUsleep(500000);
    _exit(0);
}

static void normalize_start_path(char *dst, size_t n, const char *src) {
    while (*src == '/' || *src == '\\') src++;
    set_str(dst, n, src);
    size_t len = strlen(dst);
    while (len > 0 && (dst[len - 1] == '/' || dst[len - 1] == '\\')) dst[--len] = 0;
    for (char *c = dst; *c; c++) if (*c == '\\') *c = '/';
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
    const uint64_t *w = (const uint64_t *)uc;   // FreeBSD amd64 ucontext_t: mcontext starts at +16
    LOG("[CRASH] signal %d fault_addr=%p rip=0x%llx\n", sig, info ? info->si_addr : NULL, (unsigned long long)w[22]);
    _exit(0);
}

static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.__sa_handler.__sa_sigaction = (void (*)(int, struct __siginfo *, void *))crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

#include "player.h"

int main(void) {
    sceUserServiceInitialize(NULL);
    scePadInit();

    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    g_user_id = userId;
    g_ime_user = userId;
    pad_handle = scePadOpen(userId, 0, 0, NULL);

    // ---- config ----
    if (load_config() != 0) {
        write_template();
        notify("Jaja Video Player: no SMB config yet, template created at /data/jajasamba/config.ini (local storage still works)");
    } else if (cfg.server[0] == 0 || cfg.share[0] == 0) {
        notify("Jaja Video Player: config.ini needs server and share for SMB (local storage still works)");
    } else {
        have_config = 1;
    }

    // ---- network + logging ----
    int nrc = sceNetInit();
    int pool = sceNetPoolCreate("jajasamba", 256 * 1024, 0);
    log_init();
    install_crash_handler();
    LOG("[main] started. sceNetInit=%d pool=%d logfile=%s config=%d\n",
        nrc, pool, log_file ? "yes" : "NO", have_config);

    // ---- font ----
    g_debug = cfg.debug;
    int fr = font_load();
    font_ok = (fr == 0);
    LOG("[main] font_load -> %d\n", fr);
    if (!font_ok) notifyf("Jaja Video Player: font.ttf missing from the package (%d)", fr);

    // ---- video: fixed 1920x1080, set up once ----
    video = sceVideoOutOpen(VO_USER_MAIN, VO_BUS_MAIN, 0, NULL);
    size_t total = (size_t)FB_SIZE * NUM_BUFFERS;
    off_t phys = 0;
    int ret = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, DMEM_ALIGN, 3, &phys);
    void *mem = NULL;
    ret |= sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, DMEM_ALIGN);
    LOG("[main] video=%d framebuffer alloc/map ret=0x%x addr=%p\n", video, ret, mem);
    if (ret != 0 || mem == NULL) {
        notify("Jaja Video Player: framebuffer allocation failed");
        sceKernelUsleep(3000000);
        do_exit();
    }
    for (int i = 0; i < NUM_BUFFERS; i++)
        fb[i] = (uint32_t *)((char *)mem + (size_t)i * FB_SIZE);

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, VO_PIXEL_FORMAT, VO_TILING_LINEAR,
                                  VO_ASPECT_16_9, WIDTH, HEIGHT, WIDTH);
    void *addrs[NUM_BUFFERS] = { fb[0], fb[1] };
    ret = sceVideoOutRegisterBuffers(video, 0, addrs, NUM_BUFFERS, &attr);
    LOG("[main] RegisterBuffers: 0x%x\n", ret);
    sceVideoOutSetFlipRate(video, 0);

    // ---- start the network worker and ask for the first listing ----
    {
        pthread_t th;
        pthread_attr_t pattr;
        pthread_attr_init(&pattr);
        pthread_attr_setstacksize(&pattr, 1024 * 1024);
        int trc = pthread_create(&th, &pattr, net_worker, NULL);
        LOG("[main] pthread_create -> %d\n", trc);
        if (trc != 0) {
            ui_note = "Could not start the worker thread.";
        } else {
            int ls; char lp[512];
            last_load(&ls, lp, sizeof(lp));
            if (ls >= 0 && ls < SRC_N && ls != SRC_RECENT && src_available(ls)) {
                g_src = ls;                                 // back where we left off
                set_str(cur_path, sizeof(cur_path), lp);
                depth = 0;
                for (const char *q = lp; *q; q++) if (*q == '/') depth++;
                if (lp[0]) depth++;
                if (depth > 31) depth = 31;
            } else if (ls == SRC_RECENT && src_available(SRC_RECENT)) {
                g_src = SRC_RECENT;
                cur_path[0] = 0;
                fill_recent_items();
                redraw();
            } else {
                g_src = have_config ? SRC_SMB : SRC_INT;
                normalize_start_path(cur_path, sizeof(cur_path), g_src == SRC_SMB ? cfg.path : cfg.local);
            }
            if (g_src != SRC_RECENT) request_list(K_REFRESH, cur_path);
        }
    }

    // ---- main loop ----
    int dirty[NUM_BUFFERS] = { 1, 1 };
    unsigned int prev = 0;
    uint64_t circle_down = 0;
    uint64_t rep_up = 0, rep_down = 0;
    uint64_t last_anim = 0;
    int dots = 0;

    while (!g_quit) {
        OrbisPadData d;
        memset(&d, 0, sizeof(d));
        scePadReadState(pad_handle, &d);
        uint64_t now = sceKernelGetProcessTime();
        keep_awake(now);
        unsigned int b = d.buttons;
        unsigned int pressed = b & ~prev;
        unsigned int released = ~b & prev;

        if (ime_active) { ime_poll(); circle_down = 0; prev = b; } else {
        // Circle: short press = back, hold = quit
        if (settings_open || kb_active) circle_down = 0;
        else {
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE) circle_down = now;
        if ((b & ORBIS_PAD_BUTTON_CIRCLE) && circle_down && now - circle_down >= HOLD_US) {
            g_quit = 1;
            break;
        }
        if ((released & ORBIS_PAD_BUTTON_CIRCLE) && circle_down) {
            if (now - circle_down < BACK_MAX_US && list_state == LS_IDLE) go_back();
            circle_down = 0;
        }
        }

        // a finished listing is picked up here
        if (list_state == LS_OK || list_state == LS_ERR) apply_result();

        if (settings_open) {
            settings_input(pressed);
        } else if (kb_active) {
            kb_input(pressed);
        } else if (list_state == LS_IDLE && !ui_note) {
            if (pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) search_begin();
            if (pressed & ORBIS_PAD_BUTTON_R3)        sort_cycle();
            if (pressed & ORBIS_PAD_BUTTON_OPTIONS)  settings_open_now();
            if (pressed & ORBIS_PAD_BUTTON_UP)   { move_sel(-1); rep_up = now + 400000; }
            else if ((b & ORBIS_PAD_BUTTON_UP) && now >= rep_up) { move_sel(-1); rep_up = now + 70000; }

            if (pressed & ORBIS_PAD_BUTTON_DOWN) { move_sel(1); rep_down = now + 400000; }
            else if ((b & ORBIS_PAD_BUTTON_DOWN) && now >= rep_down) { move_sel(1); rep_down = now + 70000; }

            if (pressed & ORBIS_PAD_BUTTON_L2)       switch_source(-1);
            if (pressed & ORBIS_PAD_BUTTON_R2)       switch_source(1);
            if (pressed & ORBIS_PAD_BUTTON_L1)       move_sel(-VISIBLE_ROWS);
            if (pressed & ORBIS_PAD_BUTTON_R1)       move_sel(VISIBLE_ROWS);
            if (pressed & ORBIS_PAD_BUTTON_CROSS)    enter_selected();
            if (pressed & ORBIS_PAD_BUTTON_SQUARE)   request_bench();
            if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                if (g_src == SRC_RECENT) { fill_recent_items(); if (sel >= item_count) sel = item_count ? item_count - 1 : 0; redraw(); }
                else request_list(K_REFRESH, cur_path);
            }
        }
        prev = b;
        }

        // animated dots while something is loading
        if (list_state == LS_LOADING && now - last_anim > 400000) {
            last_anim = now;
            dots = (dots + 1) % 4;
            redraw();
        }

        if (redraw_needed) {
            dirty[0] = dirty[1] = 1;
            redraw_needed = 0;
        }
        if (dirty[back]) {
            draw_ui(fb[back], dots);
            dirty[back] = 0;
        }

        sceVideoOutSubmitFlip(video, back, VO_FLIP_VSYNC, 0);
        while (sceVideoOutIsFlipPending(video) > 0) sceKernelUsleep(1000);
        back ^= 1;

        sceKernelUsleep(8000);
    }

    do_exit();
    return 0;
}
