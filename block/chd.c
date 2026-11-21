/*
 * Block driver for MAME CHD disc images (via libchdr).
 *
 * Read-only, for the DVD drive: a CHD of a CD ("createcd") or of a DVD
 * ("createdvd") shows as the disc's 2048-byte sectors. The layout is in
 * chd-disc.c.
 *
 * Based on block/chd.c of xemu-project/xemu pull request 2809, which hands
 * libchdr the file's host path. Here libchdr reads through the file child
 * (libchdr's core file callbacks), so the image can be on any protocol, such
 * as the saf:// of the libretro core, or a node of a json: description.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "block/chd-disc.h"
#include "block/thread-pool.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"

#include <libchdr/chd.h>

/*
 * What the driver does to keep a CHD as cheap to read as a plain image:
 *
 * - libchdr asks for one hunk's compressed bytes at a time, and a read of the
 *   file child for each would be a round trip through the block layer per
 *   hunk. ChdFile keeps windows of the file instead (64 KiB, doubling up to
 *   1 MiB while the reads go on in order), which the hunks are cut from.
 *   There are several, least recently used out first: a hunk that is a copy
 *   of an earlier one (chdman stores every repeated hunk, the zeros of the
 *   padding above all, as a reference) makes libchdr read the earlier one,
 *   far back in the file, and the next hunk after it where it left off. With
 *   one window, each such hunk would throw away the one the disc is being
 *   read through, and take a seek.
 * - Decoding is what costs, and in a coroutine it would hold the main loop,
 *   and with it the big lock the emulated CPU needs for its device accesses.
 *   It runs on the thread pool instead (chd_decode_job), as the file reads of
 *   an ISO do. The thread may not call into the block layer, so it reads what
 *   the window holds, and when it needs more it says so (-EAGAIN): the
 *   coroutine reads that, and sends the job again.
 * - The decoded hunks are kept (chd_disc_sector_cached), 16 MiB of them.
 */
#define CHD_WINDOW_MIN (64 * KiB)
#define CHD_WINDOW_MAX (1 * MiB)

/*
 * libchdr's view of the file child: a position in it, as stdio has. It is
 * freed by libchdr's fclose, which chd_open_core_file_callbacks also calls
 * when it fails.
 */
#define CHD_WINDOWS 4

typedef struct ChdWindow {
    uint8_t *buf;
    uint64_t pos;           /* the file's bytes pos .. pos + len are in buf */
    size_t len;
    size_t cap;
    size_t want;            /* what the last refill took */
    uint64_t used;          /* when it was last read, by ChdFile.clock */
} ChdWindow;

typedef struct ChdFile {
    BdrvChild *child;
    uint64_t size;
    uint64_t pos;

    ChdWindow win[CHD_WINDOWS];
    uint64_t clock;

    bool in_worker;         /* a thread-pool job reads: the windows only */
    bool miss;              /* ... and wanted more: */
    uint64_t miss_pos;
    size_t miss_len;

    uint64_t refills, refill_bytes;
} ChdFile;

typedef struct BDRVChdState {
    CoMutex lock;
    chd_file *chd;
    ChdFile *file;          /* owned by chd */
    ChdDisc *disc;

    /* counters, logged when the image is closed */
    uint64_t reads, bytes, jobs, retries, hits, slow;
    int64_t read_ns, read_max_ns, decode_ns;
} BDRVChdState;

/* The window that holds bytes pos .. pos + bytes of the file, if there is one */
static ChdWindow *chd_file_find(ChdFile *f, uint64_t pos, uint64_t bytes)
{
    for (int i = 0; i < CHD_WINDOWS; i++) {
        ChdWindow *w = &f->win[i];

        if (w->len && pos >= w->pos && pos + bytes <= w->pos + w->len) {
            w->used = ++f->clock;
            return w;
        }
    }
    return NULL;
}

/*
 * Reads the file at pos into a window, at least need bytes of it. From the
 * main loop or a coroutine, with no job running.
 */
static int chd_file_fill(ChdFile *f, uint64_t pos, size_t need)
{
    ChdWindow *w = NULL;
    size_t want;
    int ret;

    /* the reads go on in order, when this one starts in a window (a hunk's
     * bytes run across its end) or just after it: that window takes more, as
     * a readahead does */
    for (int i = 0; i < CHD_WINDOWS; i++) {
        ChdWindow *c = &f->win[i];

        if (c->len && pos >= c->pos && pos <= c->pos + c->len + CHD_WINDOW_MIN) {
            w = c;
            break;
        }
    }
    if (w) {
        want = MIN(w->want * 2, CHD_WINDOW_MAX);
    } else {
        /* anywhere else is a read of another place: it takes the window
         * that was read least recently, and little, so that a read at random
         * stays cheap */
        w = &f->win[0];
        for (int i = 1; i < CHD_WINDOWS; i++) {
            if (f->win[i].used < w->used) {
                w = &f->win[i];
            }
        }
        want = CHD_WINDOW_MIN;
    }
    w->want = want;
    want = MAX(want, need);
    if (want > w->cap) {
        w->buf = g_realloc(w->buf, want);
        w->cap = want;
    }
    want = MIN(want, f->size - pos);

    w->len = 0;
    ret = bdrv_pread(f->child, pos, want, w->buf, 0);
    if (ret < 0) {
        return ret;
    }
    w->pos = pos;
    w->len = want;
    w->used = ++f->clock;
    f->refills++;
    f->refill_bytes += want;
    return 0;
}

/*
 * The callbacks run from libchdr: in the open (the main loop), in a thread-pool
 * job (the window only), and, for a read the window does not take, in a
 * coroutine, with the graph read-locked: bdrv_pread works in either.
 */
static uint64_t chd_core_fsize(void *opaque)
{
    ChdFile *f = opaque;

    return f->size;
}

static size_t chd_core_fread(void *ptr, size_t size, size_t nmemb,
                             void *opaque)
{
    ChdFile *f = opaque;
    ChdWindow *w;
    uint64_t bytes;

    if (!size || !nmemb || f->pos >= f->size) {
        return 0;
    }
    /* as fread: whole items, and no more than the file has */
    bytes = MIN((uint64_t)size * nmemb, f->size - f->pos);
    nmemb = bytes / size;
    bytes = (uint64_t)nmemb * size;
    if (bytes == 0) {
        return 0;
    }

    w = chd_file_find(f, f->pos, bytes);
    if (!w) {
        if (f->in_worker) {
            f->miss = true;
            f->miss_pos = f->pos;
            f->miss_len = bytes;
            return 0;
        }
        if (bytes > CHD_WINDOW_MAX) {
            /* the map of the CHD, at the open: no use to keep */
            if (bdrv_pread(f->child, f->pos, bytes, ptr, 0) < 0) {
                return 0;
            }
            f->pos += bytes;
            return nmemb;
        }
        if (chd_file_fill(f, f->pos, bytes) < 0) {
            return 0;
        }
        w = chd_file_find(f, f->pos, bytes);
        if (!w) {
            return 0;
        }
    }
    memcpy(ptr, w->buf + (f->pos - w->pos), bytes);
    f->pos += bytes;
    return nmemb;
}

static int chd_core_fseek(void *opaque, int64_t offset, int whence)
{
    ChdFile *f = opaque;
    int64_t base;

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = f->pos;
        break;
    case SEEK_END:
        base = f->size;
        break;
    default:
        return -1;
    }
    if (offset < -base) {
        return -1;
    }
    f->pos = base + offset;
    return 0;
}

static int chd_core_fclose(void *opaque)
{
    ChdFile *f = opaque;

    for (int i = 0; i < CHD_WINDOWS; i++) {
        g_free(f->win[i].buf);
    }
    g_free(f);
    return 0;
}

static const core_file_callbacks chd_core_callbacks = {
    .fsize = chd_core_fsize,
    .fread = chd_core_fread,
    .fclose = chd_core_fclose,
    .fseek = chd_core_fseek,
};

static int chd_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    if (buf_size < 8 || memcmp(buf, "MComprHD", 8)) {
        return 0;
    }
    return chd_disc_path_is_chd(filename) ? 100 : 10;
}

static int chd_bdrv_open(BlockDriverState *bs, QDict *options, int flags,
                         Error **errp)
{
    BDRVChdState *s = bs->opaque;
    ChdFile *file;
    chd_error cerr;
    char reason[128];
    int64_t size;
    int ret;

    GLOBAL_STATE_CODE();

    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, NULL, errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    size = bdrv_getlength(bs->file->bs);
    if (size <= 0) {
        error_setg(errp, "Could not get the size of the CHD file");
        return size < 0 ? size : -EINVAL;
    }

    file = g_new0(ChdFile, 1);
    file->child = bs->file;
    file->size = size;

    /* libchdr owns file now, and closes it if the open fails */
    cerr = chd_open_core_file_callbacks(&chd_core_callbacks, file,
                                        CHD_OPEN_READ, NULL, &s->chd);
    if (cerr != CHDERR_NONE) {
        s->chd = NULL;
        error_setg(errp, "Could not open CHD: %s", chd_error_string(cerr));
        return -EINVAL;
    }
    s->file = file;

    s->disc = chd_disc_new(s->chd, reason, sizeof(reason));
    if (!s->disc) {
        error_setg(errp, "Unsupported CHD: %s", reason);
        chd_close(s->chd);
        s->chd = NULL;
        s->file = NULL;
        return -ENOTSUP;
    }

    bs->total_sectors = chd_disc_sectors(s->disc) *
                        (CHD_DISC_SECTOR_SIZE / BDRV_SECTOR_SIZE);
    qemu_co_mutex_init(&s->lock);
    return 0;
}

static void chd_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = BDRV_SECTOR_SIZE;
}

typedef struct ChdDecodeJob {
    BDRVChdState *s;
    uint64_t lba;
    const uint8_t *sector;
    char reason[128];
} ChdDecodeJob;

/* On a thread-pool thread: decodes the hunk of job->lba, from the window */
static int chd_decode_job(void *opaque)
{
    ChdDecodeJob *job = opaque;
    BDRVChdState *s = job->s;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    s->file->in_worker = true;
    s->file->miss = false;
    job->sector = chd_disc_sector(s->disc, job->lba, job->reason,
                                  sizeof(job->reason));
    s->file->in_worker = false;
    s->decode_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;

    if (job->sector) {
        return 0;
    }
    return s->file->miss ? -EAGAIN : -EIO;
}

/* The 2048 bytes of sector lba, decoded on the thread pool if they are not
 * in the cache yet. With s->lock held. */
static int coroutine_fn GRAPH_RDLOCK
chd_fetch_sector(BDRVChdState *s, uint64_t lba, const uint8_t **sector,
                 char *reason, size_t reason_len)
{
    ChdDecodeJob job = { .s = s, .lba = lba };
    int ret = -EIO;

    *sector = chd_disc_sector_cached(s->disc, lba);
    if (*sector) {
        s->hits++;
        return 0;
    }

    for (int attempt = 0; attempt < 4; attempt++) {
        s->jobs++;
        ret = thread_pool_submit_co(chd_decode_job, &job);
        if (ret == 0) {
            *sector = job.sector;
            return 0;
        }
        if (ret != -EAGAIN) {
            break;
        }
        /* the job needed compressed bytes the window does not hold: read
         * them here, where the block layer can be called, and try again */
        s->retries++;
        if (chd_file_fill(s->file, s->file->miss_pos, s->file->miss_len) < 0) {
            snprintf(job.reason, sizeof(job.reason),
                     "could not read the CHD file");
            ret = -EIO;
            break;
        }
    }
    snprintf(reason, reason_len, "%s",
             job.reason[0] ? job.reason : "CHD read failed");
    return ret < 0 ? ret : -EIO;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
              QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    int64_t pos = offset;
    size_t done = 0;
    int ret = 0;
    int64_t t0, dt;

    assert(QEMU_IS_ALIGNED(offset, BDRV_SECTOR_SIZE));
    assert(QEMU_IS_ALIGNED(bytes, BDRV_SECTOR_SIZE));

    qemu_co_mutex_lock(&s->lock);
    t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    while (pos < offset + bytes) {
        uint64_t lba = pos / CHD_DISC_SECTOR_SIZE;
        size_t skip = pos % CHD_DISC_SECTOR_SIZE;
        size_t n = MIN(CHD_DISC_SECTOR_SIZE - skip, offset + bytes - pos);
        char reason[128];
        const uint8_t *sector;

        ret = chd_fetch_sector(s, lba, &sector, reason, sizeof(reason));
        if (ret < 0) {
            error_report("CHD: %s", reason);
            break;
        }
        qemu_iovec_from_buf(qiov, done, sector + skip, n);
        pos += n;
        done += n;
    }

    dt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    s->reads++;
    s->bytes += done;
    s->read_ns += dt;
    s->read_max_ns = MAX(s->read_max_ns, dt);
    if (dt > 10 * SCALE_MS) {
        s->slow++;
    }
    qemu_co_mutex_unlock(&s->lock);
    return ret;
}

static void chd_bdrv_close(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;

    if (s->reads) {
        /* in the session log of the core, with xemu's other messages */
        info_report("CHD: %" PRIu64 " reads, %" PRIu64 " MiB; %" PRIu64
                    " sectors from the cache, %" PRIu64 " hunks decoded on "
                    "worker threads (%.0f ms, %" PRIu64 " sent again), %" PRIu64
                    " window reads (%" PRIu64 " MiB of the file); a read "
                    "took %.0f us on average and %.1f ms at most, %" PRIu64
                    " over 10 ms",
                    s->reads, s->bytes / MiB, s->hits, s->jobs,
                    s->decode_ns / 1e6, s->retries,
                    s->file ? s->file->refills : 0,
                    s->file ? s->file->refill_bytes / MiB : 0,
                    s->read_ns / 1e3 / s->reads, s->read_max_ns / 1e6,
                    s->slow);
    }
    chd_disc_free(s->disc);
    s->disc = NULL;
    if (s->chd) {
        chd_close(s->chd);
        s->chd = NULL;
        s->file = NULL;
    }
}

static BlockDriver bdrv_chd = {
    .format_name            = "chd",
    .instance_size          = sizeof(BDRVChdState),
    .bdrv_probe             = chd_probe,
    .bdrv_open              = chd_bdrv_open,
    .bdrv_child_perm        = bdrv_default_perms,
    .bdrv_refresh_limits    = chd_refresh_limits,
    .bdrv_co_preadv         = chd_co_preadv,
    .bdrv_close             = chd_bdrv_close,
    .is_format              = true,
};

static void bdrv_chd_init(void)
{
    bdrv_register(&bdrv_chd);
}

block_init(bdrv_chd_init);
