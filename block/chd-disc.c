/*
 * CHD (MAME's Compressed Hunks of Data) disc images, read through libchdr:
 * the layout of the disc in a CHD. See include/block/chd-disc.h.
 *
 * Based on the CD-ROM reading of block/chd.c in xemu-project/xemu pull
 * request 2809, made independent of QEMU's block layer and extended with the
 * DVD layout of chdman's createdvd.
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

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

#include "block/chd-disc.h"

/* The data of a MODE1_RAW sector follows its sync (12 bytes) and header (4) */
#define CD_RAW_DATA_OFFSET 16

/* The tracks of a CD, in the order of its metadata; none with a pregap */
typedef struct ChdTrack {
    uint64_t first_lba;     /* of the disc's 2048-byte sectors */
    uint64_t frames;
    uint64_t chd_frame;     /* the CHD's frame that is its first */
    uint32_t data_offset;   /* of the 2048 bytes in a frame */
} ChdTrack;

struct ChdDisc {
    chd_file *chd;
    uint8_t *hunk_buf;
    uint32_t hunkbytes;
    int64_t cur_hunk;       /* the one in hunk_buf, -1 for none */
    bool dvd;               /* flat 2048-byte units, else a CD's frames */
    uint32_t frames_per_hunk;
    ChdTrack *tracks;
    unsigned num_tracks;
    uint64_t sectors;
};

#ifdef __GNUC__
__attribute__((format(printf, 3, 4)))
#endif
static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!err || !errlen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/*
 * The metadata of track index: CHT2, or CHTR in older CHDs. The strings are
 * read with a width, as libchdr's own formats read them without one and the
 * metadata is the file's. 1 for a track, 0 for no more, -1 for metadata that
 * does not read.
 */
static int read_track_metadata(chd_file *chd, unsigned index, int *tkid,
                               char type[32], char subtype[32], int *frames,
                               int *pregap, int *postgap)
{
    char text[512] = { 0 };
    char pgtype[32] = { 0 }, pgsub[32] = { 0 };
    uint32_t len, tag;
    uint8_t flags;
    int n;

    type[0] = subtype[0] = 0;
    *tkid = -1;
    *frames = 0;
    *pregap = *postgap = 0;

    if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, text,
                         sizeof(text) - 1, &len, &tag, &flags) == CHDERR_NONE) {
        n = sscanf(text,
                   "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d "
                   "PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                   tkid, type, subtype, frames, pregap, pgtype, pgsub,
                   postgap);
        return n >= 4 ? 1 : -1;
    }
    if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, text,
                         sizeof(text) - 1, &len, &tag, &flags) == CHDERR_NONE) {
        n = sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", tkid,
                   type, subtype, frames);
        return n >= 4 ? 1 : -1;
    }
    return 0;
}

/* The bytes per sector and where the data is in one, 0 for a type that is not
 * MODE1 */
static uint32_t track_sector_size(const char *type, uint32_t *data_offset)
{
    if (!strcmp(type, "MODE1") || !strcmp(type, "MODE1/2048")) {
        *data_offset = 0;
        return 2048;
    }
    if (!strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE1/2352")) {
        *data_offset = CD_RAW_DATA_OFFSET;
        return 2352;
    }
    return 0;
}

static bool read_cd_tracks(ChdDisc *d, char *err, size_t errlen)
{
    uint64_t total = 0, chd_frame = 0;

    for (;;) {
        char type[32], subtype[32];
        int tkid, frames, pregap, postgap;
        uint32_t data_offset = 0;
        ChdTrack *tr, *grown;
        int r = read_track_metadata(d->chd, d->num_tracks, &tkid, type,
                                    subtype, &frames, &pregap, &postgap);

        if (r == 0) {
            break;
        }
        if (r < 0) {
            set_err(err, errlen, "invalid CD track metadata");
            return false;
        }
        if (d->num_tracks >= CD_MAX_TRACKS) {
            set_err(err, errlen, "too many tracks");
            return false;
        }
        if (strcmp(subtype, "NONE") != 0 || pregap != 0 || postgap != 0) {
            set_err(err, errlen,
                    "track pregap/postgap/subtypes are not supported");
            return false;
        }
        if (tkid != (int)d->num_tracks + 1) {
            set_err(err, errlen, "unexpected track numbering");
            return false;
        }
        if (!strcmp(type, "AUDIO")) {
            set_err(err, errlen, "audio tracks are not supported for DVD-ROM");
            return false;
        }
        if (!track_sector_size(type, &data_offset)) {
            set_err(err, errlen, "unsupported track type '%s'", type);
            return false;
        }
        if (frames <= 0) {
            set_err(err, errlen, "empty track");
            return false;
        }

        grown = realloc(d->tracks, (d->num_tracks + 1) * sizeof(*d->tracks));
        if (!grown) {
            set_err(err, errlen, "out of memory");
            return false;
        }
        d->tracks = grown;
        tr = &d->tracks[d->num_tracks++];
        tr->first_lba = total;
        tr->frames = frames;
        tr->chd_frame = chd_frame;
        tr->data_offset = data_offset;

        total += frames;
        /* chdman pads each track to a multiple of CD_TRACK_PADDING frames */
        chd_frame += (uint64_t)((frames + CD_TRACK_PADDING - 1) /
                                CD_TRACK_PADDING) * CD_TRACK_PADDING;
    }

    d->sectors = total;
    return true;
}

ChdDisc *chd_disc_new(chd_file *chd, char *err, size_t errlen)
{
    const chd_header *hdr = chd_get_header(chd);
    ChdDisc *d;

    if (!hdr || hdr->hunkbytes == 0) {
        set_err(err, errlen, "invalid CHD header");
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    d->chd = chd;
    d->hunkbytes = hdr->hunkbytes;
    d->cur_hunk = -1;

    if (!read_cd_tracks(d, err, errlen)) {
        goto fail;
    }

    if (d->num_tracks) {
        /* a CD: its hunks hold whole frames */
        uint64_t frames;

        if (d->hunkbytes % CD_FRAME_SIZE) {
            set_err(err, errlen, "invalid hunk size for a CD-ROM");
            goto fail;
        }
        d->frames_per_hunk = d->hunkbytes / CD_FRAME_SIZE;
        d->sectors = d->tracks[d->num_tracks - 1].first_lba +
                     d->tracks[d->num_tracks - 1].frames;
        frames = d->tracks[d->num_tracks - 1].chd_frame +
                 d->tracks[d->num_tracks - 1].frames;
        if (frames > (uint64_t)hdr->totalhunks * d->frames_per_hunk) {
            set_err(err, errlen, "the CHD has fewer frames than its tracks");
            goto fail;
        }
    } else if (hdr->unitbytes == CHD_DISC_SECTOR_SIZE &&
               d->hunkbytes % CHD_DISC_SECTOR_SIZE == 0) {
        /* chdman createdvd: the sectors, one after the other */
        d->dvd = true;
        d->sectors = hdr->logicalbytes / CHD_DISC_SECTOR_SIZE;
        if (d->sectors > (uint64_t)hdr->totalhunks * d->hunkbytes /
                         CHD_DISC_SECTOR_SIZE) {
            set_err(err, errlen, "the CHD is shorter than its size says");
            goto fail;
        }
    } else {
        set_err(err, errlen,
                "not a CD-ROM or DVD CHD (no tracks, and not 2048-byte units)");
        goto fail;
    }

    if (d->sectors == 0) {
        set_err(err, errlen, "empty CHD image");
        goto fail;
    }
    d->hunk_buf = malloc(d->hunkbytes);
    if (!d->hunk_buf) {
        set_err(err, errlen, "out of memory");
        goto fail;
    }
    return d;

fail:
    free(d->tracks);
    free(d);
    return NULL;
}

void chd_disc_free(ChdDisc *d)
{
    if (!d) {
        return;
    }
    free(d->hunk_buf);
    free(d->tracks);
    free(d);
}

uint64_t chd_disc_sectors(const ChdDisc *d)
{
    return d->sectors;
}

static bool load_hunk(ChdDisc *d, uint64_t hunk, char *err, size_t errlen)
{
    chd_error cerr;

    if ((int64_t)hunk == d->cur_hunk) {
        return true;
    }
    d->cur_hunk = -1;
    cerr = chd_read(d->chd, (uint32_t)hunk, d->hunk_buf);
    if (cerr != CHDERR_NONE) {
        set_err(err, errlen, "CHD read failed: %s", chd_error_string(cerr));
        return false;
    }
    d->cur_hunk = (int64_t)hunk;
    return true;
}

const uint8_t *chd_disc_sector(ChdDisc *d, uint64_t lba, char *err,
                               size_t errlen)
{
    if (lba >= d->sectors) {
        set_err(err, errlen, "LBA %" PRIu64 " out of range", lba);
        return NULL;
    }

    if (d->dvd) {
        uint64_t byte = lba * CHD_DISC_SECTOR_SIZE;

        if (!load_hunk(d, byte / d->hunkbytes, err, errlen)) {
            return NULL;
        }
        return d->hunk_buf + byte % d->hunkbytes;
    } else {
        const ChdTrack *tr = NULL;
        uint64_t frame;
        unsigned i;

        for (i = 0; i < d->num_tracks; i++) {
            if (lba >= d->tracks[i].first_lba &&
                lba < d->tracks[i].first_lba + d->tracks[i].frames) {
                tr = &d->tracks[i];
                break;
            }
        }
        if (!tr) {
            set_err(err, errlen, "LBA %" PRIu64 " is in no track", lba);
            return NULL;
        }
        frame = tr->chd_frame + (lba - tr->first_lba);
        if (!load_hunk(d, frame / d->frames_per_hunk, err, errlen)) {
            return NULL;
        }
        return d->hunk_buf +
               (frame % d->frames_per_hunk) * CD_FRAME_SIZE + tr->data_offset;
    }
}

bool chd_disc_path_is_chd(const char *path)
{
    static const char ext[] = ".chd";
    size_t len, i;

    if (!path) {
        return false;
    }
    len = strlen(path);
    if (len < sizeof(ext) - 1) {
        return false;
    }
    path += len - (sizeof(ext) - 1);
    for (i = 0; i < sizeof(ext) - 1; i++) {
        char c = path[i];

        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

struct ChdDiscFile {
    chd_file *chd;
    ChdDisc *disc;
};

ChdDiscFile *chd_disc_file_open(const char *path)
{
    ChdDiscFile *f;
    char err[128];

    /* libchdr reads a plain path; a URI is the frontend's VFS's */
    if (!path || !path[0] || strstr(path, "://")) {
        return NULL;
    }
    f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    if (chd_open(path, CHD_OPEN_READ, NULL, &f->chd) != CHDERR_NONE) {
        free(f);
        return NULL;
    }
    f->disc = chd_disc_new(f->chd, err, sizeof(err));
    if (!f->disc) {
        chd_close(f->chd);
        free(f);
        return NULL;
    }
    return f;
}

bool chd_disc_file_read(ChdDiscFile *f, uint64_t offset, void *buf, size_t len)
{
    uint8_t *out = buf;
    uint64_t size = chd_disc_sectors(f->disc) * CHD_DISC_SECTOR_SIZE;

    if (offset > size || len > size - offset) {
        return false;
    }
    while (len) {
        uint64_t lba = offset / CHD_DISC_SECTOR_SIZE;
        size_t skip = offset % CHD_DISC_SECTOR_SIZE;
        size_t n = CHD_DISC_SECTOR_SIZE - skip;
        const uint8_t *sector = chd_disc_sector(f->disc, lba, NULL, 0);

        if (!sector) {
            return false;
        }
        if (n > len) {
            n = len;
        }
        memcpy(out, sector + skip, n);
        out += n;
        offset += n;
        len -= n;
    }
    return true;
}

void chd_disc_file_close(ChdDiscFile *f)
{
    if (!f) {
        return;
    }
    chd_disc_free(f->disc);
    chd_close(f->chd);
    free(f);
}
