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
#include "qemu/error-report.h"
#include "qemu/module.h"

#include <libchdr/chd.h>

/*
 * libchdr's view of the file child: a position in it, as stdio has. It is
 * freed by libchdr's fclose, which chd_open_core_file_callbacks also calls
 * when it fails.
 */
typedef struct ChdFile {
    BdrvChild *child;
    uint64_t size;
    uint64_t pos;
} ChdFile;

typedef struct BDRVChdState {
    CoMutex lock;
    chd_file *chd;
    ChdDisc *disc;
} BDRVChdState;

/*
 * The callbacks run from libchdr, in the open (the main loop) and in
 * chd_co_preadv (a coroutine), with the graph read-locked in both:
 * bdrv_pread works in either.
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
    uint64_t bytes;

    if (!size || !nmemb || f->pos >= f->size) {
        return 0;
    }
    /* as fread: whole items, and no more than the file has */
    bytes = MIN((uint64_t)size * nmemb, f->size - f->pos);
    nmemb = bytes / size;
    bytes = (uint64_t)nmemb * size;
    if (bytes == 0 || bdrv_pread(f->child, f->pos, bytes, ptr, 0) < 0) {
        return 0;
    }
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
    g_free(opaque);
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

    s->disc = chd_disc_new(s->chd, reason, sizeof(reason));
    if (!s->disc) {
        error_setg(errp, "Unsupported CHD: %s", reason);
        chd_close(s->chd);
        s->chd = NULL;
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

static int coroutine_fn GRAPH_RDLOCK
chd_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
              QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    int64_t pos = offset;
    size_t done = 0;
    int ret = 0;

    assert(QEMU_IS_ALIGNED(offset, BDRV_SECTOR_SIZE));
    assert(QEMU_IS_ALIGNED(bytes, BDRV_SECTOR_SIZE));

    qemu_co_mutex_lock(&s->lock);

    while (pos < offset + bytes) {
        uint64_t lba = pos / CHD_DISC_SECTOR_SIZE;
        size_t skip = pos % CHD_DISC_SECTOR_SIZE;
        size_t n = MIN(CHD_DISC_SECTOR_SIZE - skip, offset + bytes - pos);
        char reason[128];
        const uint8_t *sector = chd_disc_sector(s->disc, lba, reason,
                                                sizeof(reason));

        if (!sector) {
            error_report("CHD: %s", reason);
            ret = -EIO;
            break;
        }
        qemu_iovec_from_buf(qiov, done, sector + skip, n);
        pos += n;
        done += n;
    }

    qemu_co_mutex_unlock(&s->lock);
    return ret;
}

static void chd_bdrv_close(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;

    chd_disc_free(s->disc);
    s->disc = NULL;
    if (s->chd) {
        chd_close(s->chd);
        s->chd = NULL;
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
