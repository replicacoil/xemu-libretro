/*
 * CHD (MAME's Compressed Hunks of Data) disc images, read through libchdr.
 *
 * The layout code of block/chd.c, with nothing of QEMU's block layer in it, so
 * that what runs before qemu_init (is this content an Xbox disc? where does
 * its game partition start? which game is it?) can read a CHD as well. The
 * block driver reads through its file child, this file's chd_disc_file_*
 * through a plain path.
 *
 * An image's 2048-byte sectors are what a DVD drive hands the guest. Two kinds
 * of CHD hold them:
 *  - "createdvd": flat 2048-byte units, with the DVD metadata tag;
 *  - "createcd": a CD, with one MODE1 (2048 bytes) or MODE1_RAW (2352 bytes,
 *    the data 16 bytes in) track in 2448-byte frames, whatever its size.
 * Pregaps, other track types, several tracks with audio and parent CHDs are
 * not supported.
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

#ifndef BLOCK_CHD_DISC_H
#define BLOCK_CHD_DISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHD_DISC_SECTOR_SIZE 2048

/* libchdr's handle, without its header */
struct _chd_file;

typedef struct ChdDisc ChdDisc;

/*
 * The layout of the disc in an open CHD, which stays the caller's to close,
 * after the disc. NULL when the CHD is not a disc this code reads, with the
 * reason in err.
 */
ChdDisc *chd_disc_new(struct _chd_file *chd, char *err, size_t errlen);
void chd_disc_free(ChdDisc *disc);

/* Its size in 2048-byte sectors */
uint64_t chd_disc_sectors(const ChdDisc *disc);

/*
 * The 2048 bytes of sector lba, valid until the next call on this disc (the
 * hunk they are in is kept); NULL on an error, with the reason in err. Not
 * thread-safe: one reader at a time.
 */
const uint8_t *chd_disc_sector(ChdDisc *disc, uint64_t lba,
                               char *err, size_t errlen);

/* Whether path names a CHD: ends in ".chd", in any case */
bool chd_disc_path_is_chd(const char *path);

/*
 * A CHD opened by its path (a plain host path: not a saf:// or other URI that
 * only the frontend's VFS reads), for reading the disc byte by byte.
 */
typedef struct ChdDiscFile ChdDiscFile;

ChdDiscFile *chd_disc_file_open(const char *path);
/* len bytes at offset of the disc; false when they are not all there */
bool chd_disc_file_read(ChdDiscFile *file, uint64_t offset, void *buf,
                        size_t len);
void chd_disc_file_close(ChdDiscFile *file);

#endif /* BLOCK_CHD_DISC_H */
