/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marco Pistella
 * ADFDMSView is released under the MIT License: see the file LICENSE for the full text. */

/*
 * ADFDMSView - Far Manager 3.x plug-in for Amiga ADF/DMS disk images (32 and 64 bit)
 * ===========================================================================================================
 * Opens Amiga ADF disk images (OFS and FFS) and DMS archives as a read-only virtual panel: browse, view and copy
 * (F5, also whole directories) the files of the disk. Names are Amiga names (Latin-1), made valid for Windows.
 *
 * Structure of the file:
 *   1. Portable core (no Win32, no Far headers; build it alone with -DADF_CORE_ONLY):
 *        - the DMS unpacker (modes NOCOMP, SIMPLE, QUICK, MEDIUM, DEEP, HEAVY1, HEAVY2), which produces a disk image
 *          in memory;
 *        - the ADF reader (OFS/FFS), which works on an image in memory.
 *      Both are written to keep going on damaged input and to say what they found in a text report (CoreLog).
 *   2. The Far plug-in: panel items, names, copy dialog, information panel, exported functions.
 *
 * DMS unpacker:
 *   - every track is verified (CRC-16 of the packed data and 16-bit checksum of the unpacked track) and the state of
 *     each track is kept: right, repaired, kept but not verified, kept as unpacked (best effort), lost, absent (DMT_*);
 *   - a track header damaged in one or two bytes (or only its CRC field) is repaired; the archive is recognized also
 *     with a damaged signature or first track header; a header that cannot be repaired is skipped by searching the
 *     next valid one;
 *   - the data of a track end at the next valid track header, so that a lost or an added byte is seen from the length
 *     (the last track has no next header: the end of the file gives its length); a truncated last track is unpacked as
 *     far as it goes;
 *   - a track whose packed-data CRC is wrong is unpacked as it is first and kept if its checksum is right; else the
 *     packed data are repaired when one byte has a wrong value, was lost or was added (the length of the data says
 *     which, the linearity of the CRC-16 points at the candidates) or, if no single byte explains the CRC, when a
 *     burst of up to 16 bits or two bits are wrong (solved from the CRC, the fewest bits first, accepted only if the
 *     unpacked data also end where the packed data end and no other candidate of the class fits). A candidate is
 *     accepted only if the checksum of the unpacked track is then right. The search is bounded by DMS_REPAIR_SECONDS
 *     (the time spent in the repairs, not the search for a key), DMS_REPAIR_BUDGET and, for the wrong bits,
 *     DMS_BITS_TRIALS; a correction of bits that is found after a class of candidates with fewer wrong bits was
 *     skipped because it was too large is shown as not verified;
 *   - in the modes that keep a dictionary from one track to the next (QUICK, MEDIUM, DEEP, HEAVY) a track that fails
 *     after a lost or damaged track is decoded a second time from an empty dictionary; a track that cannot be verified
 *     is kept as unpacked (best effort), so that the tracks that copy from its dictionary are not lost too;
 *   - the decoders are bounds-checked and tolerate what real archives hold: a last RLE run or a last match that goes
 *     past the end of the track is clipped, an incomplete table of distances in HEAVY is read as "repeat the last
 *     distance", incomplete input gives the bytes decoded so far;
 *   - encrypted archives are decrypted: the key is found from the checksum of the first disk track (a search of at most
 *     DMS_KEY_SECONDS; if it is stopped the archive is shown as damaged), and the running key follows the packed data
 *     of the next tracks; a first track that several keys fit, or that only a weak key fits (the checksum is right but
 *     the packed data do not end where the unpacked data end), is shown as not verified; an archive of which no track
 *     can be decrypted is refused;
 *   - a track that appears twice in the archive is stored once (the verified copy, or else the first one);
 *   - the unpacked image is also shown as <archive>.adf, so that it can be copied out;
 *   - archives that do not hold a disk image (disk type 7) are refused.
 *
 * ADF reader:
 *   - the image size need not be a multiple of 512 (the whole blocks are used); the bootblock need not be "DOS" when
 *     a valid root block exists; a root block with a wrong checksum is accepted if its hash table still points at
 *     header blocks; with no usable root block the files are found by scanning for file headers; with neither a root
 *     block nor a file header, the files are rebuilt from the OFS data blocks whose checksum is right;
 *   - OFS or FFS is detected from the data blocks, not only from the bootblock;
 *   - in an OFS disk every data block of a file is verified: it names this file and this position, its checksum is
 *     right and it holds as many bytes as the file needs there. A block that fails validation is replaced by the block
 *     that names this file and this position (looked up by (file header, sequence number), which also serves when the
 *     block table is damaged or lost); a block that belongs to another file is never copied into a file; a size field
 *     that makes no sense is taken from the data blocks;
 *   - what is kept although it cannot be verified (a data block with a wrong checksum, a damaged header or too few
 *     bytes; a file header or extension block with a wrong checksum) makes the file "incomplete or unverified": the
 *     Description column says so, the copy (F5) counts it and the report gives the block;
 *   - files and data blocks that the directory tree does not reach appear in the folder _RECOVERED (paths rebuilt
 *     from the parent pointers; DATA_<block>.bin for data blocks whose file header is lost);
 *   - items that could not be read completely say so in the Description column; _RECOVERY_REPORT.txt (shown only for a
 *     damaged disk) lists what the readers found and repaired.
 *
 * Effort limits at the top of the DMS engine: DMS_REPAIR_SECONDS and DMS_REPAIR_BUDGET (search for repairs),
 * DMS_BITS_TRIALS and DMS_BITS_SECONDS (search for wrong bits in a track), DMS_KEY_SECONDS (search for the key of an encrypted archive),
 * and MAX_FILE_SIZE (biggest image or archive that is opened).
 *
 * Far side:
 *   - the panel is read-only: PutFilesW, DeleteFilesW, MakeDirectoryW and a move (F6) out of the panel show a box and
 *     fail. They must be exported: a missing function is taken by Far as "cancelled" or "failed" without a message.
 *   - F5 out of the panel shows a copy dialog (destination folder, what to do with existing files). There is no
 *     dialog when Far copies to a temporary folder (F3, F4, copy between plug-in panels).
 *   - only .adf and .dms files are offered to the plug-in. A file that is recognized but cannot be opened is
 *     explained in a box and Far is told that it was dealt with (PANEL_STOP); while Far searches in archives or works
 *     silently the file is declined (NULL) so that other plug-ins can try.
 *   - every text shown comes from a Far language file (.lng) in the folder of the DLL; the English texts are also
 *     built in (see "Messages"). The report _RECOVERY_REPORT.txt is written by the core and is always in English.
 *
 * Build: see build.bat (MinGW-w64, plugin.hpp of Far 3 in .\PluginSDK). A 32 bit DLL must be linked with
 * -Wl,--kill-at so that the stdcall exports have the undecorated names that Far looks for.
 */

#ifndef ADF_CORE_ONLY
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include <wchar.h>
#ifndef ADF_CORE_ONLY
#include "plugin.hpp"

#ifndef PANEL_STOP
#define PANEL_STOP ((HANDLE)(-1))          /* "the file was dealt with": no other plug-in is asked */
#endif
#endif

/* ====================================================================================================== */
/* PORTABLE CORE (no Win32, no Far): the resilient DMS unpacker and the resilient ADF file system reader.     */
/* ====================================================================================================== */

#if defined(__GNUC__)
#  define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#else
#  define PRINTF_LIKE(a, b)
#endif

/* Memory: an allocation that fails jumps back to the guard of the entry point that is running (the plug-in must
 * never kill Far); the memory of that call is leaked, which is the lesser evil. */
static jmp_buf *g_oom_jmp;

static void *core_nomem(void)
{
    if (g_oom_jmp) longjmp(*g_oom_jmp, 1);
    abort();
    return NULL;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    return p ? p : core_nomem();
}

static void *xcalloc(size_t a, size_t b)
{
    void *p = calloc(a ? a : 1, b ? b : 1);
    return p ? p : core_nomem();
}

static void *xrealloc(void *old, size_t n)
{
    void *p = realloc(old, n ? n : 1);
    return p ? p : core_nomem();
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Report of what was found and repaired: text lines, shown by the panel as _RECOVERY_REPORT.txt */
typedef struct {
    char    *text;
    size_t   len, cap;
    unsigned lines;
    bool     full;
} CoreLog;

#define CORE_LOG_MAX (256u * 1024u)

static CoreLog *g_clog;                                 /* the report that the running entry point fills */

static void core_log_line(int level, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void core_log_line(int level, const char *fmt, ...)
{
    char buf[640];
    const char *prefix = level == 1 ? "ERROR: " : (level == 2 ? "note: " : "");
    size_t n;
    va_list ap;
    if (!g_clog) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    n = strlen(prefix) + strlen(buf) + 2;
    if (g_clog->len + n >= CORE_LOG_MAX) {
        if (!g_clog->full) {
            static const char tail[] = "... (report truncated)\n";
            g_clog->full = true;
            if (g_clog->len + sizeof(tail) + 1 > g_clog->cap) {
                g_clog->cap = g_clog->len + sizeof(tail) + 1;
                g_clog->text = (char *)xrealloc(g_clog->text, g_clog->cap);
            }
            memcpy(g_clog->text + g_clog->len, tail, sizeof(tail));
            g_clog->len += sizeof(tail) - 1;
        }
        return;
    }
    if (g_clog->len + n + 1 > g_clog->cap) {
        g_clog->cap = (g_clog->len + n + 1) * 2;
        g_clog->text = (char *)xrealloc(g_clog->text, g_clog->cap);
    }
    g_clog->len += (size_t)sprintf(g_clog->text + g_clog->len, "%s%s\n", prefix, buf);
    g_clog->lines++;
}

#define log_msg(...)   core_log_line(0, __VA_ARGS__)
#define log_error(...) core_log_line(1, __VA_ARGS__)
#define log_note(...)  core_log_line(2, __VA_ARGS__)

#define DMS_HEADLEN        56
#define DMS_THLEN          20
#define DMS_TRACK_MAX      32000          /* maximum packed / unpacked track size */
#define DMS_SLACK          1024           /* a match may run past the end of the output */
#define DMS_STD_TRACKS     80             /* tracks 0..79 are disk data */
#define DMS_MAX_TRACKS     160            /* the plug-in also takes 160 half-cylinder tracks of 5632 bytes */
#define DMS_TRACK_HALF     5632
#define DMS_REPAIR_BUDGET  400000         /* Candidate decodes allowed for archive repair (bounds the opening time). */
#define DMS_BITS_TRIALS    40000          /* Candidate decodes allowed for bit repairs in one track. */
#ifndef DMS_BITS_SECONDS
#  define DMS_BITS_SECONDS 1              /* time for the repair of wrong bits in one track */
#endif
#define DMS_QUIET_WEIGHT   4              /* most wrong bits of a burst when the damage does not put the stream out of step */
#define DMS_TAIL_BITS      128            /* a right decode ends at most this many bits before the end of the packed data */
#ifndef DMS_REPAIR_SECONDS
#  define DMS_REPAIR_SECONDS 6            /* time for the repairs of one archive */
#endif
#ifndef DMS_KEY_SECONDS
#  define DMS_KEY_SECONDS  30             /* time for the search of the key of an encrypted archive */
#endif
#define DMS_TEXT_SIZE      0x4000         /* largest dictionary (mask 0x3FFF) */
#define DMS_TRACK_DD       11264          /* one cylinder, DD: 2 x 11 x 512 */
#define DMS_TRACK_HD       22528
#define DMS_MAX_LOGGED     20             /* detailed error lines per archive */

/* True if the packed data are used up to a few bits before their end: what a right decode of a right stream does */
static bool dms_tail_ok(long unread)
{
    return unread >= 0 && unread <= DMS_TAIL_BITS;
}

/* heavy */
#define DMS_NC             510
#define DMS_NPT            20
#define DMS_N1             510
#define DMS_OFFSET         253
/* deep */
#define DMS_F              60
#define DMS_THRESHOLD      2
#define DMS_N_CHAR         (256 - DMS_THRESHOLD + DMS_F)
#define DMS_T              (DMS_N_CHAR * 2 - 1)
#define DMS_R              (DMS_T - 1)
#define DMS_MAX_FREQ       0x8000

typedef struct {
    uint8_t  text[DMS_TEXT_SIZE];                 /* dictionary shared by all the modes */
    uint16_t quick_loc, medium_loc, heavy_loc, deep_loc;
    int      deep_init;
    /* deep */
    uint16_t freq[DMS_T + 1], prnt[DMS_T + DMS_N_CHAR], son[DMS_T];
    /* heavy */
    uint16_t left[2 * DMS_NC - 1], right[2 * DMS_NC - 1 + 9];
    uint8_t  c_len[DMS_NC], pt_len[DMS_NPT];
    uint16_t c_table[4096], pt_table[256];
    uint16_t lastlen, np;
    bool     noclip, ptab_alt;                    /* the reading of the data of the attempt that is running (DMS_V_*) */
} dms_ctx_t;

/* bit reader: reads past the end give zero bits and are counted; a request for more bits than
 * are available is flagged: the decoders stop as soon as `bad` or too much `over` is seen */
typedef struct {
    const uint8_t *p, *end;
    uint32_t buf;
    unsigned cnt, over;
    bool     bad;
} dms_bits_t;

static void dms_fill(dms_bits_t *b)
{
    while (b->cnt < 16) {
        uint32_t v = 0;
        if (b->p < b->end) v = *b->p++;
        else b->over++;
        b->buf = (b->buf << 8) | v;
        b->cnt += 8;
    }
}

static void dms_bits_init(dms_bits_t *b, const uint8_t *in, size_t len)
{
    b->p = in;
    b->end = in + len;
    b->buf = 0;
    b->cnt = 0;
    b->over = 0;
    b->bad = false;
    dms_fill(b);
}

#define DMS_GETBITS(b, n) ((uint16_t)((b)->buf >> ((b)->cnt - (unsigned)(n))))

static void dms_drop(dms_bits_t *b, unsigned n)
{
    if (n > b->cnt) { b->bad = true; n = b->cnt; }
    b->cnt -= n;
    b->buf &= (1u << b->cnt) - 1u;
    dms_fill(b);
}

/* a legitimate stream asks for at most 3 bytes past its end (the refill) */
static bool dms_bits_fail(const dms_bits_t *b) { return b->bad || b->over > 8; }

/* ---------- checksum and CRC16 ---------- */
static uint16_t dms_checksum(const uint8_t *m, size_t n)
{
    uint16_t u = 0;
    while (n--) u = (uint16_t)(u + *m++);
    return u;
}


/* CRC-16/ARC (polynomial 0xA001 reflected, initial value 0). Table 0 is the classic one; tables 1 to 7 let eight bytes
 * go through at once. The tables are shared by the checks and by the repairs. */
static uint16_t g_dms_crc[8][256];

static void dms_crc_init(void)
{
    static bool init = false;
    unsigned i, k;
    if (init) return;
    for (i = 0; i < 256; i++) {
        unsigned c = i;
        for (k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0xA001u : (c >> 1);
        g_dms_crc[0][i] = (uint16_t)c;
    }
    for (k = 1; k < 8; k++)
        for (i = 0; i < 256; i++)
            g_dms_crc[k][i] = (uint16_t)((g_dms_crc[k - 1][i] >> 8) ^ g_dms_crc[0][g_dms_crc[k - 1][i] & 0xFF]);
    init = true;
}

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define DMS_CRC_SLICE8 1
#endif

static uint16_t dms_crc16(const uint8_t *m, size_t n)
{
    uint16_t c = 0;
    dms_crc_init();
#ifdef DMS_CRC_SLICE8
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, m, 8);
        w ^= c;                                 /* the 16-bit state meets the first two bytes only */
        c = (uint16_t)(g_dms_crc[7][w & 0xFF] ^ g_dms_crc[6][(w >> 8) & 0xFF] ^ g_dms_crc[5][(w >> 16) & 0xFF] ^
                       g_dms_crc[4][(w >> 24) & 0xFF] ^ g_dms_crc[3][(w >> 32) & 0xFF] ^
                       g_dms_crc[2][(w >> 40) & 0xFF] ^ g_dms_crc[1][(w >> 48) & 0xFF] ^ g_dms_crc[0][w >> 56]);
        m += 8;
        n -= 8;
    }
#endif
    while (n--) c = (uint16_t)((c >> 8) ^ g_dms_crc[0][(c ^ *m++) & 0xFF]);
    return c;
}


/* ---------- tables of MEDIUM and DEEP ---------- */
static const uint8_t dms_d_code[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
    0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F,
    0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13,
    0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x17,
    0x18, 0x18, 0x19, 0x19, 0x1A, 0x1A, 0x1B, 0x1B, 0x1C, 0x1C, 0x1D, 0x1D, 0x1E, 0x1E, 0x1F, 0x1F,
    0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,
    0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

static const uint8_t dms_d_len[256] = {
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08
};


/* ---------- decompressor state ---------- */
/* What a decoder reports besides its status: the bytes it wrote (a damaged track is still used as far as it could be
 * decoded), the anomalies it tolerated and how much of the packed data it left unread. */
#define DMS_Q_RLE_CLIP    1u    /* the last run of the RLE data was longer than the track: clipped */
#define DMS_Q_MATCH_CLIP  2u    /* the last match of HEAVY went past the end of the data: clipped */
#define DMS_Q_PTAB        4u    /* HEAVY: incomplete table of distances, every distance is "repeat the last one" */
#define DMS_Q_PTAB_ALT    8u    /* HEAVY: incomplete table of distances, read as a Huffman code (what is not assigned is symbol 0) */
#define DMS_Q_MATCH_PAST  16u   /* HEAVY: the last match went past the end of the data and was not clipped */

/* The readings of a HEAVY track. The default one clips the last match and reads an incomplete table of distances as
 * "repeat the last distance"; when its checksum is wrong the others are tried, in this order. */
#define DMS_V_NOCLIP      1u    /* the last match is completed: the dictionary advances by all of it */
#define DMS_V_PTAB_ALT    2u    /* an incomplete table of distances is read as a Huffman code */
typedef struct {
    size_t   got;               /* bytes written to the output (never more than the size asked for) */
    unsigned quirks;            /* DMS_Q_* */
    long     unread;            /* bits of the packed data not consumed (negative: read past its end); a right
                                 * decode of a right stream ends within a few bits of the end of the data */
} dms_out_t;

/* The state of every mode: the dictionaries and the tables are cleared (a track whose flags do not ask to keep them
 * starts from this state) */

static void dms_init_decrunchers(dms_ctx_t *x)
{
    x->quick_loc = 251;
    x->medium_loc = 0x3fbe;
    x->heavy_loc = 0;
    x->deep_loc = 0x3fc4;
    x->deep_init = 1;
    x->lastlen = 0;
    x->np = 0;
    x->noclip = x->ptab_alt = false;
    memset(x->text, 0, 0x3fc8);
}


/* ---------- RLE (used by every mode but NOCOMP) ---------- */
/* A run longer than the room left in the output is clipped: some encoders write the last run of a track past its end,
 * and the checksum of the track decides. Returns 1 if the input ended before the output was full. */

static int dms_unpack_rle(const uint8_t *in, size_t inlen, uint8_t *out, size_t origsize, dms_out_t *o)
{
    const uint8_t *ie = in + inlen;
    uint8_t *start = out, *outend = out + origsize;
    int rc = 0;

    while (out < outend) {
        uint8_t a, b;
        unsigned n;
        if (in >= ie) { rc = 1; break; }
        a = *in++;
        if (a != 0x90) { *out++ = a; continue; }
        if (in >= ie) { rc = 1; break; }
        b = *in++;
        if (b == 0) { *out++ = a; continue; }
        if (in >= ie) { rc = 1; break; }
        a = *in++;
        if (b == 0xff) {
            if (ie - in < 2) { rc = 1; break; }
            n = ((unsigned)in[0] << 8) | in[1];
            in += 2;
        } else {
            n = b;
        }
        if ((size_t)(outend - out) < n) {
            n = (unsigned)(outend - out);
            o->quirks |= DMS_Q_RLE_CLIP;
        }
        memset(out, a, n);
        out += n;
    }
    o->got = (size_t)(out - start);
    o->unread = 8 * (long)(ie - in);
    return rc;
}


/* bits of the input that a bit-reader decoder did not consume (negative: it read past the end) */

static long dms_unread_bits(const dms_bits_t *b, const uint8_t *in, size_t inlen)
{
    return 8 * (long)inlen - (8 * ((long)(b->p - in) + (long)b->over) - (long)b->cnt);
}


/* bytes written by a dictionary decoder: a final match may run a few bytes past the end (DMS_SLACK) */

static size_t dms_written(const uint8_t *start, const uint8_t *out, size_t origsize)
{
    size_t n = (size_t)(out - start);
    return n < origsize ? n : origsize;
}


/* ---------- QUICK ---------- */

static int dms_unpack_quick(dms_ctx_t *x, const uint8_t *in, size_t inlen, uint8_t *out,
                            uint16_t origsize, dms_out_t *o)
{
    dms_bits_t b;
    uint8_t *start = out, *outend = out + origsize;
    uint16_t i, j;

    dms_bits_init(&b, in, inlen);
    while (out < outend && !dms_bits_fail(&b)) {
        if (DMS_GETBITS(&b, 1) != 0) {
            dms_drop(&b, 1);
            *out++ = x->text[x->quick_loc++ & 0xff] = (uint8_t)DMS_GETBITS(&b, 8);
            dms_drop(&b, 8);
        } else {
            dms_drop(&b, 1);
            j = (uint16_t)(DMS_GETBITS(&b, 2) + 2);
            dms_drop(&b, 2);
            i = (uint16_t)(x->quick_loc - DMS_GETBITS(&b, 8) - 1);
            dms_drop(&b, 8);
            while (j--) *out++ = x->text[x->quick_loc++ & 0xff] = x->text[i++ & 0xff];
        }
    }
    o->got = dms_written(start, out, origsize);
    o->unread = dms_unread_bits(&b, in, inlen);
    if (out < outend) return 1;                    /* a failed read: the position in the dictionary stays */
    x->quick_loc = (uint16_t)((x->quick_loc + 5) & 0xff);
    return dms_bits_fail(&b) ? 1 : 0;
}


/* ---------- MEDIUM ---------- */

static int dms_unpack_medium(dms_ctx_t *x, const uint8_t *in, size_t inlen, uint8_t *out,
                             uint16_t origsize, dms_out_t *o)
{
    dms_bits_t b;
    uint8_t *start = out, *outend = out + origsize;
    uint16_t i, j, c;
    uint8_t u;

    dms_bits_init(&b, in, inlen);
    while (out < outend && !dms_bits_fail(&b)) {
        if (DMS_GETBITS(&b, 1) != 0) {
            dms_drop(&b, 1);
            *out++ = x->text[x->medium_loc++ & 0x3fff] = (uint8_t)DMS_GETBITS(&b, 8);
            dms_drop(&b, 8);
        } else {
            dms_drop(&b, 1);
            c = DMS_GETBITS(&b, 8);
            dms_drop(&b, 8);
            j = (uint16_t)(dms_d_code[c] + 3);
            u = dms_d_len[c];
            c = (uint16_t)(((c << u) | DMS_GETBITS(&b, u)) & 0xff);
            dms_drop(&b, u);
            u = dms_d_len[c];
            c = (uint16_t)((dms_d_code[c] << 8) | (((c << u) | DMS_GETBITS(&b, u)) & 0xff));
            dms_drop(&b, u);
            i = (uint16_t)(x->medium_loc - c - 1);
            while (j--) *out++ = x->text[x->medium_loc++ & 0x3fff] = x->text[i++ & 0x3fff];
        }
    }
    o->got = dms_written(start, out, origsize);
    o->unread = dms_unread_bits(&b, in, inlen);
    if (out < outend) return 1;
    x->medium_loc = (uint16_t)((x->medium_loc + 66) & 0x3fff);
    return dms_bits_fail(&b) ? 1 : 0;
}


/* ---------- DEEP (LZ + dynamic Huffman) ---------- */
static void dms_deep_init_tabs(dms_ctx_t *x)
{
    uint16_t i, j;
    for (i = 0; i < DMS_N_CHAR; i++) {
        x->freq[i] = 1;
        x->son[i] = (uint16_t)(i + DMS_T);
        x->prnt[i + DMS_T] = i;
    }
    i = 0;
    j = DMS_N_CHAR;
    while (j <= DMS_R) {
        x->freq[j] = (uint16_t)(x->freq[i] + x->freq[i + 1]);
        x->son[j] = i;
        x->prnt[i] = x->prnt[i + 1] = j;
        i = (uint16_t)(i + 2);
        j++;
    }
    x->freq[DMS_T] = 0xffff;
    x->prnt[DMS_R] = 0;
    x->deep_init = 0;
}

static void dms_deep_reconst(dms_ctx_t *x)
{
    uint16_t i, j, k, f, l;

    j = 0;
    for (i = 0; i < DMS_T; i++) {
        if (x->son[i] >= DMS_T) {
            x->freq[j] = (uint16_t)((x->freq[i] + 1) / 2);
            x->son[j] = x->son[i];
            j++;
        }
    }
    for (i = 0, j = DMS_N_CHAR; j < DMS_T; i = (uint16_t)(i + 2), j++) {
        k = (uint16_t)(i + 1);
        f = x->freq[j] = (uint16_t)(x->freq[i] + x->freq[k]);
        for (k = (uint16_t)(j - 1); f < x->freq[k]; k--) { }
        k++;
        l = (uint16_t)((j - k) * 2);
        memmove(&x->freq[k + 1], &x->freq[k], (size_t)l);
        x->freq[k] = f;
        memmove(&x->son[k + 1], &x->son[k], (size_t)l);
        x->son[k] = i;
    }
    for (i = 0; i < DMS_T; i++) {
        if ((k = x->son[i]) >= DMS_T) {
            x->prnt[k] = i;
        } else {
            x->prnt[k] = x->prnt[k + 1] = i;
        }
    }
}

static void dms_deep_update(dms_ctx_t *x, uint16_t c)
{
    uint16_t i, j, k, l;

    if (x->freq[DMS_R] == DMS_MAX_FREQ) dms_deep_reconst(x);
    c = x->prnt[c + DMS_T];
    do {
        k = ++x->freq[c];
        if (k > x->freq[l = (uint16_t)(c + 1)]) {
            while (k > x->freq[++l]) { }
            l--;
            x->freq[c] = x->freq[l];
            x->freq[l] = k;
            i = x->son[c];
            x->prnt[i] = l;
            if (i < DMS_T) x->prnt[i + 1] = l;
            j = x->son[l];
            x->son[l] = i;
            x->prnt[j] = c;
            if (j < DMS_T) x->prnt[j + 1] = c;
            x->son[c] = j;
            c = l;
        }
    } while ((c = x->prnt[c]) != 0);
}

static uint16_t dms_deep_decode_char(dms_ctx_t *x, dms_bits_t *b)
{
    uint16_t c = x->son[DMS_R];
    while (c < DMS_T) {
        c = x->son[c + DMS_GETBITS(b, 1)];
        dms_drop(b, 1);
    }
    c = (uint16_t)(c - DMS_T);
    dms_deep_update(x, c);
    return c;
}

static uint16_t dms_deep_decode_pos(dms_bits_t *b)
{
    uint16_t i, j, c;
    i = DMS_GETBITS(b, 8);
    dms_drop(b, 8);
    c = (uint16_t)(dms_d_code[i] << 8);
    j = dms_d_len[i];
    i = (uint16_t)(((i << j) | DMS_GETBITS(b, j)) & 0xff);
    dms_drop(b, j);
    return (uint16_t)(c | i);
}



static int dms_unpack_deep(dms_ctx_t *x, const uint8_t *in, size_t inlen, uint8_t *out,
                           uint16_t origsize, dms_out_t *o)
{
    dms_bits_t b;
    uint8_t *start = out, *outend = out + origsize;
    uint16_t i, j, c;

    dms_bits_init(&b, in, inlen);
    if (x->deep_init) dms_deep_init_tabs(x);
    while (out < outend && !dms_bits_fail(&b)) {
        c = dms_deep_decode_char(x, &b);
        if (c < 256) {
            *out++ = x->text[x->deep_loc++ & 0x3fff] = (uint8_t)c;
        } else {
            j = (uint16_t)(c - 255 + DMS_THRESHOLD);
            i = (uint16_t)(x->deep_loc - dms_deep_decode_pos(&b) - 1);
            while (j--) *out++ = x->text[x->deep_loc++ & 0x3fff] = x->text[i++ & 0x3fff];
        }
    }
    o->got = dms_written(start, out, origsize);
    o->unread = dms_unread_bits(&b, in, inlen);
    if (out < outend) return 1;
    x->deep_loc = (uint16_t)((x->deep_loc + 60) & 0x3fff);
    return dms_bits_fail(&b) ? 1 : 0;
}


/* ---------- HEAVY 1 / 2 (LZ + static Huffman, from UNIX LHA) ---------- */
typedef struct {
    int16_t         c;
    uint16_t        n, tblsiz, len, depth, maxdepth, avail, codeword, bit, taberr;
    uint16_t       *tbl, *left, *right;
    const uint8_t  *blen;
} dms_mk_t;

/* Builds the decoding table of a canonical Huffman code, walking the tree of the code lengths: the first `tablebits`
 * bits of the stream index the table; a longer code goes on in the nodes left[] / right[] (indices from the number of
 * symbols up). Sets taberr for an over-subscribed code, a code that is too deep or too many nodes. */

static uint16_t dms_mktbl(dms_mk_t *m)
{
    uint16_t i = 0;

    if (m->taberr) return 0;
    if (m->len == m->depth) {
        while (++m->c < (int)m->n)
            if (m->blen[m->c] == m->len) {
                i = m->codeword;
                m->codeword = (uint16_t)(m->codeword + m->bit);
                if (m->codeword > m->tblsiz) {
                    m->taberr = 1;
                    return 0;
                }
                while (i < m->codeword) m->tbl[i++] = (uint16_t)m->c;
                return (uint16_t)m->c;
            }
        m->c = -1;
        m->len++;
        m->bit = (uint16_t)(m->bit >> 1);
    }
    m->depth++;
    if (m->depth < m->maxdepth) {
        dms_mktbl(m);
        dms_mktbl(m);
    } else if (m->depth > 32) {
        m->taberr = 2;
        return 0;
    } else {
        if ((i = m->avail++) >= 2 * m->n - 1) {
            m->taberr = 3;
            return 0;
        }
        m->left[i] = dms_mktbl(m);
        m->right[i] = dms_mktbl(m);
        if (m->codeword >= m->tblsiz) {
            m->taberr = 4;
            return 0;
        }
        if (m->depth == m->maxdepth) m->tbl[m->codeword++] = i;
    }
    m->depth--;
    return i;
}


/* The table of nchar symbols with lengths bitlen[]: returns 0, or an error number if the code is not complete or cannot
 * be built */

static uint16_t dms_make_table(dms_ctx_t *x, uint16_t nchar, const uint8_t *bitlen,
                               uint16_t tablebits, uint16_t *table)
{
    dms_mk_t m;
    memset(&m, 0, sizeof(m));
    m.n = m.avail = nchar;
    m.blen = bitlen;
    m.tbl = table;
    m.left = x->left;
    m.right = x->right;
    m.tblsiz = (uint16_t)(1U << tablebits);
    m.bit = (uint16_t)(m.tblsiz / 2);
    m.maxdepth = (uint16_t)(tablebits + 1);
    m.depth = m.len = 1;
    m.c = -1;
    dms_mktbl(&m);                                 /* left subtree */
    if (m.taberr) return m.taberr;
    dms_mktbl(&m);                                 /* right subtree */
    if (m.taberr) return m.taberr;
    if (m.codeword != m.tblsiz) return 5;
    return 0;
}



static int dms_heavy_read_tree_c(dms_ctx_t *x, dms_bits_t *b)
{
    uint16_t i, n;
    n = DMS_GETBITS(b, 9);
    dms_drop(b, 9);
    if (n > 0) {
        if (n > DMS_NC) return 1;                  /* more lengths than c_len holds */
        for (i = 0; i < n; i++) {
            x->c_len[i] = (uint8_t)DMS_GETBITS(b, 5);
            dms_drop(b, 5);
        }
        for (i = n; i < DMS_NC; i++) x->c_len[i] = 0;
        if (dms_make_table(x, DMS_NC, x->c_len, 12, x->c_table)) return 1;
    } else {
        n = DMS_GETBITS(b, 9);
        dms_drop(b, 9);
        for (i = 0; i < DMS_NC; i++) x->c_len[i] = 0;
        for (i = 0; i < 4096; i++) x->c_table[i] = n;
    }
    return 0;
}


/* Decoding table of a canonical Huffman code (lengths in bitlen[0..nchar-1]) that may leave part of the code space
 * unassigned: what is not assigned decodes as symbol 0. The first `tablebits` bits of the stream index `table`; a longer
 * code goes on in the nodes left[] / right[] (indices from nchar up). Returns 0, or an error number for an
 * over-subscribed code, a length the decoders cannot read or too many nodes. */
static int dms_make_table_inc(dms_ctx_t *x, uint16_t nchar, const uint8_t *bitlen,
                          uint16_t tablebits, uint16_t *table)
{
    uint32_t count[32], next[32], code;
    uint64_t kraft = 0;
    uint16_t tblsiz = (uint16_t)(1U << tablebits), avail = nchar, s, k, i;
    const uint16_t NONE = 0xFFFF;
    const unsigned maxlen = tablebits + 16u;                   /* the decoders read 16 bits after the table bits */

    memset(count, 0, sizeof(count));
    for (s = 0; s < nchar; s++) {
        if (bitlen[s] > maxlen) return 1;
        count[bitlen[s]]++;
    }
    count[0] = 0;
    for (i = 1; i <= maxlen; i++) kraft += (uint64_t)count[i] << (maxlen - i);
    if (kraft > ((uint64_t)1 << maxlen)) return 2;             /* more codes than the code space holds */
    code = 0;
    for (i = 1; i <= maxlen; i++) {
        next[i] = code;
        code = (code + count[i]) << 1;
    }
    for (k = 0; k < tblsiz; k++) table[k] = NONE;
    for (s = 0; s < nchar; s++) {
        unsigned len = bitlen[s];
        uint32_t c;
        if (len == 0) continue;
        c = next[len]++;
        if (len <= tablebits) {
            uint32_t from = c << (tablebits - len), to = (c + 1) << (tablebits - len);
            for (; from < to; from++) table[from] = s;
        } else {
            uint16_t node = table[c >> (len - tablebits)];
            int pos;
            if (node == NONE) {
                if (avail >= 2 * nchar - 1) return 3;
                node = avail++;
                x->left[node] = x->right[node] = NONE;
                table[c >> (len - tablebits)] = node;
            } else if (node < nchar) {
                return 4;                                      /* a shorter code is the prefix of this one */
            }
            for (pos = (int)(len - tablebits) - 1; pos >= 0; pos--) {
                uint16_t *slot = ((c >> pos) & 1u) ? &x->right[node] : &x->left[node];
                if (pos == 0) {
                    *slot = s;
                } else {
                    if (*slot == NONE) {
                        if (avail >= 2 * nchar - 1) return 3;
                        *slot = avail;
                        x->left[avail] = x->right[avail] = NONE;
                        avail++;
                    } else if (*slot < nchar) {
                        return 4;
                    }
                    node = *slot;
                }
            }
        }
    }
    for (k = 0; k < tblsiz; k++) if (table[k] == NONE) table[k] = 0;
    for (i = nchar; i < avail; i++) {
        if (x->left[i] == NONE) x->left[i] = 0;
        if (x->right[i] == NONE) x->right[i] = 0;
    }
    return 0;
}

/* The table of distances. An incomplete table (its lengths do not fill the code space) cannot be built: it is read as
 * "repeat the last distance, no bits" (DMS_Q_PTAB), which gives the right data for a fill made of one long match always at
 * the same distance; the checksum of the track tells whether that was the right reading; if it was not, the table is read as a Huffman code in
 * which what is not assigned is symbol 0 (ptab_alt). */

static int dms_heavy_read_tree_p(dms_ctx_t *x, dms_bits_t *b, dms_out_t *o)
{
    uint16_t i, n;
    unsigned kraft = 0;
    n = DMS_GETBITS(b, 5);
    dms_drop(b, 5);
    if (n > 0) {
        if (n > DMS_NPT) return 1;                 /* more lengths than pt_len holds */
        for (i = 0; i < n; i++) {
            x->pt_len[i] = (uint8_t)DMS_GETBITS(b, 4);
            dms_drop(b, 4);
        }
        for (i = n; i < x->np; i++) x->pt_len[i] = 0;
        for (i = 0; i < x->np; i++)
            if (x->pt_len[i]) kraft += 1u << (16 - x->pt_len[i]);
        if (kraft < 0x10000u && x->ptab_alt) {
            /* an incomplete table: the other reading (see above) */
            o->quirks |= DMS_Q_PTAB_ALT;
            if (dms_make_table_inc(x, x->np, x->pt_len, 8, x->pt_table)) return 1;
        } else if (kraft < 0x10000u) {
            /* an incomplete table: see above */
            for (i = 0; i < x->np; i++) x->pt_len[i] = 0;
            for (i = 0; i < 256; i++) x->pt_table[i] = (uint16_t)(x->np - 1);
            o->quirks |= DMS_Q_PTAB;
        } else if (dms_make_table(x, x->np, x->pt_len, 8, x->pt_table)) {
            return 1;
        }
    } else {
        n = DMS_GETBITS(b, 5);
        dms_drop(b, 5);
        for (i = 0; i < x->np; i++) x->pt_len[i] = 0;
        for (i = 0; i < 256; i++) x->pt_table[i] = n;
    }
    return 0;
}



/* returns the symbol, or 0xFFFF on a corrupt tree */
static uint16_t dms_heavy_decode_c(dms_ctx_t *x, dms_bits_t *b)
{
    uint16_t i, j, m;

    j = x->c_table[DMS_GETBITS(b, 12)];
    if (j < DMS_N1) {
        dms_drop(b, x->c_len[j]);
    } else {
        dms_drop(b, 12);
        i = DMS_GETBITS(b, 16);
        m = 0x8000;
        do {
            if (m == 0) return 0xFFFF;
            j = (i & m) ? x->right[j] : x->left[j];
            m = (uint16_t)(m >> 1);
        } while (j >= DMS_N1);
        if (x->c_len[j] < 12) return 0xFFFF;
        dms_drop(b, (unsigned)x->c_len[j] - 12u);
    }
    return j;
}

static uint16_t dms_heavy_decode_p(dms_ctx_t *x, dms_bits_t *b)
{
    uint16_t i, j, m;

    j = x->pt_table[DMS_GETBITS(b, 8)];
    if (j < x->np) {
        dms_drop(b, x->pt_len[j]);
    } else {
        dms_drop(b, 8);
        i = DMS_GETBITS(b, 16);
        m = 0x8000;
        do {
            if (m == 0) return 0xFFFF;
            j = (i & m) ? x->right[j] : x->left[j];
            m = (uint16_t)(m >> 1);
        } while (j >= x->np);
        if (x->pt_len[j] < 8) return 0xFFFF;
        dms_drop(b, (unsigned)x->pt_len[j] - 8u);
    }
    if (j != x->np - 1) {
        if (j > 0) {
            i = (uint16_t)(j - 1);
            j = (uint16_t)(DMS_GETBITS(b, i) | (1U << (j - 1)));
            dms_drop(b, i);
        }
        x->lastlen = j;
    }
    return x->lastlen;
}


/* A match that would go past the end of the data is clipped, and the dictionary advances only by the bytes really
 * written (with x->noclip the match is completed instead, into the slack of the buffer, and the dictionary advances by all
 * of it). Returns 1 on damaged data (the tables, a code, the end of the input). */

static int dms_unpack_heavy(dms_ctx_t *x, const uint8_t *in, size_t inlen, uint8_t *out,
                            uint8_t flags, uint16_t origsize, dms_out_t *o)
{
    dms_bits_t b;
    uint8_t *start = out, *outend = out + origsize;
    uint16_t j, i, c, bitmask;
    bool bad = false;

    if (flags & 8) { x->np = 15; bitmask = 0x1fff; }   /* Heavy 2: 8 KiB dictionary */
    else           { x->np = 14; bitmask = 0x0fff; }   /* Heavy 1: 4 KiB dictionary */
    dms_bits_init(&b, in, inlen);
    if ((flags & 2) && (dms_heavy_read_tree_c(x, &b) || dms_heavy_read_tree_p(x, &b, o))) {
        o->got = 0;
        o->unread = dms_unread_bits(&b, in, inlen);
        return 1;
    }
    while (out < outend) {
        if (dms_bits_fail(&b)) { bad = true; break; }
        c = dms_heavy_decode_c(x, &b);
        if (c == 0xFFFF) { bad = true; break; }
        if (c < 256) {
            *out++ = x->text[x->heavy_loc++ & bitmask] = (uint8_t)c;
        } else {
            uint16_t p = dms_heavy_decode_p(x, &b);
            if (p == 0xFFFF) { bad = true; break; }
            j = (uint16_t)(c - DMS_OFFSET);
            i = (uint16_t)(x->heavy_loc - p - 1);
            if (j > (size_t)(outend - out)) {
                if (x->noclip) {
                    o->quirks |= DMS_Q_MATCH_PAST;                 /* completed, into the slack of the buffer */
                } else {
                    j = (uint16_t)(outend - out);
                    o->quirks |= DMS_Q_MATCH_CLIP;
                }
            }
            while (j--) *out++ = x->text[x->heavy_loc++ & bitmask] = x->text[i++ & bitmask];
        }
    }
    o->got = (size_t)(out - start);
    o->unread = dms_unread_bits(&b, in, inlen);
    return bad || dms_bits_fail(&b) ? 1 : 0;
}


/* ---------- one track ---------- */
/* The first stage of the modes with a dictionary (QUICK, MEDIUM, DEEP, HEAVY1/2). */

static int dms_stage1(dms_ctx_t *x, unsigned cmode, const uint8_t *b1, size_t pklen1, uint8_t *b2,
                      uint16_t pklen2, uint8_t flags, dms_out_t *m)
{
    switch (cmode) {
    case 2:  return dms_unpack_quick(x, b1, pklen1, b2, pklen2, m);
    case 3:  return dms_unpack_medium(x, b1, pklen1, b2, pklen2, m);
    case 4:  return dms_unpack_deep(x, b1, pklen1, b2, pklen2, m);
    default: return dms_unpack_heavy(x, b1, pklen1, b2, cmode == 5 ? (uint8_t)(flags & 7) : (uint8_t)(flags | 8),
                                     pklen2, m);
    }
}


/* b1: packed track (pklen1 bytes); the unpacked track is returned in b2 (unpklen bytes). Both buffers have
 * DMS_TRACK_MAX + DMS_SLACK bytes. Returns 0 (decoded), 1 (damaged or too short data: b2 holds what could be decoded,
 * the rest is zero) or 2 (unknown mode: b2 not valid). With 1 and 2 the state of the decompressor is not reset. *o:
 * bytes of b2 really decoded, anomalies tolerated, packed bits left unread. The RLE stage reads all that the first
 * stage produced, also what a last match put past pklen2. */

static int dms_unpack_track(dms_ctx_t *x, uint8_t *b1, size_t pklen1, uint8_t *b2,
                            uint16_t pklen2, uint16_t unpklen, uint8_t cmode, uint8_t flags, dms_out_t *o)
{
    dms_out_t m = { 0, 0, 0 };                     /* first stage */
    int rc = 0;

    o->got = 0;
    o->quirks = 0;
    o->unread = 0;
    switch (cmode) {
    case 0:                                        /* NOCOMP */
        o->got = pklen1 < unpklen ? pklen1 : unpklen;
        memcpy(b2, b1, o->got);
        rc = pklen1 < unpklen;
        o->unread = 8 * ((long)pklen1 - (long)o->got);
        break;
    case 1:                                        /* SIMPLE */
        rc = dms_unpack_rle(b1, pklen1, b2, unpklen, o);
        break;
    case 2: case 3: case 4: case 5: case 6:        /* QUICK, MEDIUM, DEEP, HEAVY 1, HEAVY 2 */
        rc = dms_stage1(x, cmode, b1, pklen1, b2, pklen2, flags, &m);
        o->quirks = m.quirks;
        o->unread = m.unread;
        if (cmode >= 5 && !(flags & 4)) {          /* HEAVY: RLE only if this flag is set */
            o->got = m.got < unpklen ? m.got : unpklen;
            if (m.got < unpklen) rc = 1;
        } else {                                   /* RLE over the bytes the first stage wrote */
            dms_out_t r2 = { 0, 0, 0 };
            rc |= dms_unpack_rle(b2, m.got, b1, unpklen, &r2);
            o->got = r2.got;
            o->quirks |= r2.quirks;
            memcpy(b2, b1, o->got);
        }
        break;
    default:
        return 2;                                  /* unknown mode */
    }
    if (o->got < unpklen) memset(b2 + o->got, 0, unpklen - o->got);
    if (rc) return 1;
    if (!(flags & 1)) dms_init_decrunchers(x);
    return 0;
}



static const char *dms_mode_name(unsigned m)
{
    static const char *n[7] = { "NOCOMP", "SIMPLE", "QUICK", "MEDIUM", "DEEP", "HEAVY1", "HEAVY2" };
    return m < 7 ? n[m] : "unknown";
}

/* ---------- archive ---------- */
/* "DMS!", the 56-byte archive header and the header of a first track ("TR"): a file that is only a header
 * (56 bytes: valid header CRC, no track) has no disk in it and is not a DMS */
/* "TR" and sane sizes: the structural checks of a track header that do not need the CRC. */
static bool dms_hdr_sane(const uint8_t *t)
{
    return t[0] == 'T' && t[1] == 'R' && (((unsigned)t[6] << 8) | t[7]) <= DMS_TRACK_MAX &&
           (((unsigned)t[8] << 8) | t[9]) <= DMS_TRACK_MAX && (((unsigned)t[10] << 8) | t[11]) <= DMS_TRACK_MAX;
}

static bool dms_is_signature(const uint8_t *d, size_t avail, uint64_t fsize)
{
    bool sig, tr;
    if (avail < DMS_HEADLEN + 2 || fsize < DMS_HEADLEN + DMS_THLEN) return false;
    sig = d[0] == 'D' && d[1] == 'M' && d[2] == 'S' && d[3] == '!';
    tr = d[DMS_HEADLEN] == 'T' && d[DMS_HEADLEN + 1] == 'R';
    if (sig && tr) return true;
    /* a damaged archive: the signature and the CRC of the archive header are right (the first track header is
     * damaged), or the first track header is right (the signature or the archive header is damaged) */
    if (sig)
        return dms_crc16(d + 4, DMS_HEADLEN - 6) == (uint16_t)(((unsigned)d[DMS_HEADLEN - 2] << 8) | d[DMS_HEADLEN - 1]);
    if (tr && avail >= DMS_HEADLEN + DMS_THLEN)
        return dms_crc16(d + DMS_HEADLEN, DMS_THLEN - 2) ==
                   (uint16_t)(((unsigned)d[DMS_HEADLEN + 18] << 8) | d[DMS_HEADLEN + 19]) &&
               dms_hdr_sane(d + DMS_HEADLEN);
    return false;
}

/* error line, limited to DMS_MAX_LOGGED per archive (a damaged track spoils the following ones) */
static void dms_log_error(unsigned *shown, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void dms_log_error(unsigned *shown, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (*shown >= DMS_MAX_LOGGED) { (*shown)++; return; }
    (*shown)++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_error("%s", buf);
}




/* First valid track header ("TR", header CRC, sane sizes) at or after `from`: where a damaged archive goes on */
/* a valid track header at t (20 bytes): "TR", sane sizes and the right CRC of its first 18 bytes */
static bool dms_header_ok(const uint8_t *t)
{
    return dms_hdr_sane(t) && dms_crc16(t, DMS_THLEN - 2) == (uint16_t)(((unsigned)t[18] << 8) | t[19]);
}

/* Whether d[pos] contains a valid track header; used by dms_resync at each offset. */
static bool dms_header_at(const uint8_t *d, uint64_t size, uint64_t pos)
{
    return pos + DMS_THLEN <= size && dms_header_ok(d + pos);
}

static bool dms_resync(const uint8_t *data, uint64_t size, uint64_t from, uint64_t *found)
{
    uint64_t i;
    for (i = from; i + DMS_THLEN <= size; i++) {
        const uint8_t *t = (const uint8_t *)memchr(data + i, 'T', (size_t)(size - DMS_THLEN + 1 - i));
        if (!t) return false;
        i = (uint64_t)(t - data);
        if (dms_header_at(data, size, i)) { *found = i; return true; }
    }
    return false;
}

/*
 * A track header (20 bytes) that does not start with "TR" or whose CRC is wrong: one damaged byte (or only
 * the CRC field) is searched. A candidate must have the right CRC and sane sizes, and is accepted if the track
 * number is the expected one or if the data it announces end at a valid track header (or at the end of the
 * archive). Among several candidates the one that satisfies both is taken; with one that satisfies only the
 * consistency check, only if it is the only candidate.
 */

/* CRC-16 is linear: dms_hz[p][e] is the CRC of the 18 header bytes that are all zero but e at position p, and
 * dms_hrev inverts it (injective for a position: CRC-16 detects every burst of 16 bits or less) */
static uint16_t dms_hz[DMS_THLEN - 2][256];
/* inverse of dms_hz, as a small open-addressing hash table (a full 18 x 65536 table would be 2.3 MB).
 * An entry packs z in bits 0-15, p in bits 16-20 and e in bits 24-31; 0 means a free slot (e is never 0) */
#define DMS_HREV_SIZE 8192u
static uint32_t dms_hrev_tab[DMS_HREV_SIZE];

static unsigned dms_hrev_slot(unsigned p, unsigned z)
{
    return ((z * 2654435761u) ^ (p * 40503u)) >> 7 & (DMS_HREV_SIZE - 1);
}

/* the byte e (1..255) that put at position p changes the CRC of the zero header by z, or 0 if there is none */
static unsigned dms_hrev(unsigned p, uint16_t z)
{
    unsigned h = dms_hrev_slot(p, z);
    while (dms_hrev_tab[h]) {
        uint32_t v = dms_hrev_tab[h];
        if ((v & 0xFFFFu) == z && ((v >> 16) & 31u) == p) return v >> 24;
        h = (h + 1) & (DMS_HREV_SIZE - 1);
    }
    return 0;
}

static void dms_hz_init(void)
{
    static bool done = false;
    unsigned p, e;
    if (done) return;
    for (p = 0; p < DMS_THLEN - 2; p++)
        for (e = 1; e < 256; e++) {
            uint8_t v[DMS_THLEN - 2];
            uint16_t z;
            unsigned h;
            memset(v, 0, sizeof(v));
            v[p] = (uint8_t)e;
            z = dms_crc16(v, sizeof(v));
            dms_hz[p][e] = z;
            if (dms_hrev(p, z)) continue;                 /* cannot happen: CRC-16 detects every burst of 16 bits */
            h = dms_hrev_slot(p, z);
            while (dms_hrev_tab[h]) h = (h + 1) & (DMS_HREV_SIZE - 1);
            dms_hrev_tab[h] = ((uint32_t)e << 24) | (p << 16) | z;
        }
    done = true;
}

typedef struct { uint8_t best[DMS_THLEN]; int score; unsigned nbest; } dms_hfix_t;

/*
 * Scores a candidate header c (its CRC is right): the track number must be the expected one (else the candidate is
 * dropped); then +1 if the data it announces end at a valid track header (or at the end of the archive), +4 if the
 * CRC of those data is the one in the candidate (the strongest validation condition: it rejects candidates that alter the
 * length or the CRC field). The best candidate is taken only if it is the only one with that score.
 */
static void dms_hfix_consider(dms_hfix_t *f, const uint8_t *c, const uint8_t *data, uint64_t size, uint64_t pos,
                              unsigned expect)
{
    uint64_t end;
    unsigned pk1;
    int score = 2;
    if (!dms_hdr_sane(c) || c[13] > 6 || (((unsigned)c[2] << 8) | c[3]) != expect) return;
    pk1 = ((unsigned)c[6] << 8) | c[7];
    end = pos + DMS_THLEN + pk1;
    if (end == size || (end < size && dms_header_at(data, size, end))) score += 1;
    if (end <= size && dms_crc16(data + pos + DMS_THLEN, pk1) == (uint16_t)(((unsigned)c[16] << 8) | c[17])) score += 4;
    if (score > f->score) { f->score = score; f->nbest = 1; memcpy(f->best, c, DMS_THLEN); }
    else if (score == f->score && memcmp(f->best, c, DMS_THLEN) != 0) f->nbest++;
}

static bool dms_fix_header(const uint8_t *data, uint64_t size, uint64_t pos, unsigned expect, uint8_t out[DMS_THLEN])
{
    uint8_t h[DMS_THLEN], c[DMS_THLEN];
    dms_hfix_t fc, fa;                                    /* only the CRC field damaged / bytes of the content damaged */
    const dms_hfix_t *pick = NULL;
    unsigned p, q, v, stored;
    uint16_t delta, cr;

    dms_hz_init();
    memcpy(h, data + pos, DMS_THLEN);
    stored = ((unsigned)h[18] << 8) | h[19];
    delta = (uint16_t)(dms_crc16(h, DMS_THLEN - 2) ^ stored);
    memset(&fc, 0, sizeof(fc));
    memset(&fa, 0, sizeof(fa));

    memcpy(c, h, DMS_THLEN);
    cr = dms_crc16(c, DMS_THLEN - 2);
    c[18] = (uint8_t)(cr >> 8);
    c[19] = (uint8_t)cr;
    dms_hfix_consider(&fc, c, data, size, pos, expect);
    if (delta != 0) {
        for (p = 0; p < DMS_THLEN - 2; p++) {             /* one damaged byte: the value that satisfies the CRC */
            unsigned e = dms_hrev(p, delta);
            if (!e) continue;
            memcpy(c, h, DMS_THLEN);
            c[p] ^= (uint8_t)e;
            dms_hfix_consider(&fa, c, data, size, pos, expect);
        }
        if (fa.score < 3 && fc.score < 3) {               /* two damaged bytes (the "TR" included) */
            for (p = 0; p < DMS_THLEN - 2; p++)
                for (q = p + 1; q < DMS_THLEN - 2; q++)
                    for (v = 1; v < 256; v++) {
                        unsigned e2 = dms_hrev(q, (uint16_t)(delta ^ dms_hz[p][v]));
                        if (!e2) continue;
                        memcpy(c, h, DMS_THLEN);
                        c[p] ^= (uint8_t)v;
                        c[q] ^= (uint8_t)e2;
                        dms_hfix_consider(&fa, c, data, size, pos, expect);
                    }
        }
    }
    /* a damaged byte of the content that the stored CRC points at is far more likely than a damaged CRC field that
     * happens to leave every other check satisfied; if the content candidates are ambiguous, the one that changes
     * the least (the CRC field alone) is taken */
    if (fa.score && fa.score >= fc.score && fa.nbest == 1) pick = &fa;
    else if (fc.score && fc.score >= fa.score) pick = &fc;
    if (!pick) return false;
    memcpy(out, pick->best, DMS_THLEN);
    return true;
}


/* ---------- decoding of one track with verification ---------- */
typedef struct {
    dms_ctx_t *x;                  /* decompressor state (in/out) */
    dms_ctx_t *xs, *xp, *xr;       /* snapshots: before the track, after the first attempt, before a repair */
    uint8_t   *b1, *b2, *cand;     /* work buffers of DMS_TRACK_MAX + DMS_SLACK bytes */
    uint8_t   *keep;               /* the output of the first attempt while the second one runs */
    bool       unsure;             /* the dictionary may differ from the encoder's */
    unsigned   pklen2, unpklen, cmode, flags, usum;
    size_t     got;                /* result: bytes of b2 really decoded (the rest is zero) */
    unsigned   quirks;             /* result: DMS_Q_* anomalies tolerated by the attempt kept in b2 */
    long       unread;             /* result: bits of the packed data not consumed (see dms_out_t) */
    bool       enc;                /* the packed data is encrypted; k0 is the key at the start of the track */
    uint16_t   k0;
    bool       ambiguous;          /* result of dms_find_key: another key gives the right checksum too */
    bool       weakkey;            /* result of dms_find_key: the key gives the right checksum, but the packed data do not end
                                    * where the unpacked data end (the stream must be fully consumed) */
    bool       rep_skipped;        /* result of dms_repair_multi: a class of candidates with fewer wrong bits than the one that
                                    * gave the correction exceeded the per-track candidate-decode budget and was skipped */
    bool       loud;               /* unpacked as it is, the data lose the end of the packed data (the stream is out of step) */
} dms_trk_t;

/* the anomalies tolerated while decoding (DMS_Q_*), as text for the report */

static void dms_quirk_text(unsigned q, char *buf, size_t n)
{
    static const struct { unsigned bit; const char *text; } tab[] = {
        { DMS_Q_RLE_CLIP,   "the last RLE run is longer than the track: clipped" },
        { DMS_Q_MATCH_CLIP, "the last match goes past the end of the data: clipped" },
        { DMS_Q_PTAB,       "incomplete table of distances: read as \"repeat the last distance\"" },
        { DMS_Q_PTAB_ALT,   "incomplete table of distances: read as a Huffman code, what is not assigned is symbol 0" },
        { DMS_Q_MATCH_PAST, "the last match goes past the end of the data: not clipped" },
    };
    size_t i, len = 0;
    buf[0] = '\0';
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (!(q & tab[i].bit)) continue;
        len += (size_t)snprintf(buf + len, n - len, "%s%s", len ? "; " : "", tab[i].text);
        if (len >= n) { buf[n - 1] = '\0'; return; }
    }
}


/* ---------- encrypted archives ----------
 * The DMS "encryption": plain = cipher ^ (uint8_t)k; k = (uint16_t)((k >> 1) + cipher). The key k runs through the packed
 * data of every track of the archive in file order, track 80 (the file description) excepted; the CRC of the packed data
 * is the one of the encrypted bytes, the checksum the one of the unpacked data. The initial k (a 16-bit hash of the
 * password) only changes the first ~20 bytes decrypted: after them k depends only on the last bytes of cipher text. So a
 * banner, or any block before the first disk track, synchronizes the key, and an archive that begins with disk data needs
 * the 65536 keys tried for its first track only. */

static uint16_t dms_key_advance(uint16_t k, const uint8_t *in, size_t n)
{
    while (n--) k = (uint16_t)((k >> 1) + *in++);
    return k;
}



static void dms_decrypt(uint8_t *out, const uint8_t *in, size_t n, uint16_t k)
{
    while (n--) {
        uint8_t c = *in++;
        *out++ = (uint8_t)(c ^ (uint8_t)k);
        k = (uint16_t)((k >> 1) + c);
    }
}


/* One attempt, in the reading `var` (DMS_V_*): the packed data (decrypted if the archive is encrypted) is unpacked into b2. */

static int dms_attempt(dms_trk_t *t, const uint8_t *packed, size_t len, unsigned var, dms_out_t *o)
{
    if (t->enc) dms_decrypt(t->b1, packed, len, t->k0);
    else memcpy(t->b1, packed, len);
    t->x->noclip = (var & DMS_V_NOCLIP) != 0;
    t->x->ptab_alt = (var & DMS_V_PTAB_ALT) != 0;
    return dms_unpack_track(t->x, t->b1, len, t->b2, (uint16_t)t->pklen2, (uint16_t)t->unpklen, (uint8_t)t->cmode,
                            (uint8_t)t->flags, o);
}


/* Unpacks `packed` (len bytes) and compares the checksum. The attempts, in this order, stop at the first whose checksum is
 * right: the dictionary as it is, in the default reading of the data and, for a HEAVY track that had to tolerate a last
 * match past the end or an incomplete table of distances, in the other readings (DMS_V_*); then, with an uncertain
 * dictionary, the same from an empty dictionary. *r_out: result of the attempt whose output is left in b2 (0 decoded, 1
 * damaged data: b2 holds what could be decoded, 2 unknown method). *retry_ok: the right attempt started from an empty
 * dictionary. When none is right b2 and the state are those of the first attempt (unless only a later one decoded
 * cleanly) and the state is the one left by that attempt. */

static bool dms_decode_verify(dms_trk_t *t, const uint8_t *packed, size_t len, int *r_out, bool *retry_ok)
{
    const bool stateful = t->cmode >= 2, heavy = t->cmode >= 5;
    const unsigned nplan = (stateful && t->unsure) ? 2 : 1;
    const bool snap = stateful && (nplan > 1 || heavy);        /* the attempts start from the same state */
    bool ok = false, have = false;
    int r = 0, best_r = 0;
    size_t best_got = 0;
    unsigned best_q = 0, pi;
    long best_unread = 0;

    *retry_ok = false;
    if (snap) memcpy(t->xs, t->x, sizeof(*t->x));
    for (pi = 0; pi < nplan && !ok && r != 2; pi++) {
        unsigned list[4] = { 0, 0, 0, 0 }, n = 1, i;
        for (i = 0; i < n && !ok; i++) {
            dms_out_t o;
            if (snap && (pi > 0 || i > 0)) {
                memcpy(t->x, t->xs, sizeof(*t->x));
                if (pi > 0) dms_init_decrunchers(t->x);        /* the encoder may have reset the dictionary */
            }
            r = dms_attempt(t, packed, len, list[i], &o);
            if (r == 0 && dms_checksum(t->b2, t->unpklen) == t->usum) {
                ok = true;
                *retry_ok = pi > 0;
                t->got = o.got;
                t->quirks = o.quirks;
                t->unread = o.unread;
                break;
            }
            if (!have || (best_r != 0 && r == 0)) {            /* what is kept if no attempt is right */
                have = true;
                best_r = r;
                best_got = o.got;
                best_q = o.quirks;
                best_unread = o.unread;
                if (snap) {
                    memcpy(t->keep, t->b2, t->unpklen);
                    memcpy(t->xp, t->x, sizeof(*t->x));
                }
            }
            if (r == 2) break;                                 /* unknown method: no other reading helps */
            if (heavy && list[i] == 0) {                       /* the other readings that the tolerance of this one calls for */
                unsigned want = 0;
                if (o.quirks & DMS_Q_MATCH_CLIP) want |= DMS_V_NOCLIP;
                if (o.quirks & DMS_Q_PTAB) want |= DMS_V_PTAB_ALT;
                if (want & DMS_V_NOCLIP) list[n++] = DMS_V_NOCLIP;
                if (want & DMS_V_PTAB_ALT) list[n++] = DMS_V_PTAB_ALT;
                if (want == (DMS_V_NOCLIP | DMS_V_PTAB_ALT)) list[n++] = want;
            }
        }
    }
    if (!ok) {
        if (snap && have) {
            memcpy(t->b2, t->keep, t->unpklen);
            memcpy(t->x, t->xp, sizeof(*t->x));
        }
        t->got = best_got;
        t->quirks = best_q;
        t->unread = best_unread;
        r = best_r;
    } else {
        r = 0;
    }
    *r_out = r;
    return ok;
}


/* A DOS boot block names its root block (880 on a DD disk, 1760 on an HD one) in bytes 8 to 11 */
static bool dms_boot_plausible(const uint8_t *b, size_t n)
{
    uint32_t root;
    if (n < 12) return false;
    root = ((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) | ((uint32_t)b[10] << 8) | b[11];
    return root == 880 || root == 1760;
}

/* The key of the first disk track of an encrypted archive (decoded from an empty dictionary, so the state of the
 * decompressor is reset for every key): the one that makes the checksum of the unpacked data right. Phase 1, for track 0:
 * a cheap probe decodes just the first bytes of every candidate and only the keys whose boot block starts with "DOS" are
 * decoded completely. Phase 2: every key, completely. The search stops after DMS_KEY_SECONDS. Returns true if found
 * (t->k0 = the key; b2, the state and *r, *retry_ok are those of the right decode).
 * A key changes only the first ~20 bytes of the track, and NOCOMP and SIMPLE accept any stream, so for them the checksum
 * is a weak validation criterion: the search goes on through all the keys and counts those that fit. If more than one fits, the one whose
 * boot block names a root block (dms_boot_plausible) is taken when it is the only one; otherwise the first is taken and
 * t->ambiguous says that the key is not certain. The other modes reject nearly every wrong key: the first that fits is
 * taken. A key that gives the right checksum but not the end of the packed data is kept only if no other key fits
 * ("better than none": the data may be damaged at their end), and t->weakkey says that it is not verified: a search
 * through 65536 keys finds about one such key by chance even when the right one is not among them. */

static bool g_dms_key_hit;                             /* the time limit stopped the search for a key */

static bool dms_find_key(dms_trk_t *t, const uint8_t *stored, size_t len, unsigned number, int *r, bool *retry_ok)
{
    const size_t probe_len = len < 512 ? len : 512;
    const bool unsure = t->unsure;
    const bool weakmode = t->cmode <= 1;
    const clock_t limit = clock() + (clock_t)DMS_KEY_SECONDS * CLOCKS_PER_SEC;
    unsigned key, n_match = 0, n_plaus = 0;
    long first = -1, first_plaus = -1;             /* the first key that fits, the first one that fits and names a root */
    long weak = -1;                                /* first key with the right checksum that does not end with the data */
    bool found = false;

    t->enc = true;
    t->unsure = false;
    t->ambiguous = false;
    t->weakkey = false;
    if (number == 0 && t->cmode <= 6) {
        for (key = 0; key < 65536 && !found; key++) {
            dms_out_t o;
            if ((key & 255) == 0 && clock() > limit) { g_dms_key_hit = true; break; }
            dms_decrypt(t->b1, stored, probe_len, (uint16_t)key);
            dms_init_decrunchers(t->x);
            if (dms_unpack_track(t->x, t->b1, probe_len, t->b2, 16, 3, (uint8_t)t->cmode, (uint8_t)t->flags, &o) == 2)
                break;
            if (o.got < 3 || t->b2[0] != 'D' || t->b2[1] != 'O' || t->b2[2] != 'S') continue;
            dms_init_decrunchers(t->x);
            t->k0 = (uint16_t)key;
            if (!dms_decode_verify(t, stored, len, r, retry_ok)) continue;
            if (!weakmode) { found = true; continue; }
            n_match++;
            if (first < 0) first = key;
            if (dms_boot_plausible(t->b2, t->unpklen)) {
                n_plaus++;
                if (first_plaus < 0) first_plaus = key;
            }
        }
    }
    if (!found && n_match == 0) {
        for (key = 0; key < 65536 && !found; key++) {
            if ((key & 255) == 0 && clock() > limit) { g_dms_key_hit = true; break; }
            dms_init_decrunchers(t->x);
            t->k0 = (uint16_t)key;
            if (dms_decode_verify(t, stored, len, r, retry_ok)) {
                /* a wrong key matches the 16-bit checksum with probability 2^-16, i.e. about once in a search; the
                 * data decoded with the right key also ends where the packed data ends */
                if (t->cmode == 0 || dms_tail_ok(t->unread)) {
                    if (!weakmode) { found = true; continue; }
                    n_match++;
                    if (first < 0) first = key;
                    if (dms_boot_plausible(t->b2, t->unpklen)) {
                        n_plaus++;
                        if (first_plaus < 0) first_plaus = key;
                    }
                } else if (weak < 0) {
                    weak = (long)key;
                }
            }
            if (*r == 2) break;                    /* unknown method: no key can help */
        }
    }
    if (!found && n_match > 0) {                   /* NOCOMP, SIMPLE: choose among the keys that fit */
        const long pick = (n_plaus == 1 || (n_match > 1 && first_plaus >= 0)) ? first_plaus : first;
        t->ambiguous = n_match > 1 && n_plaus != 1;
        dms_init_decrunchers(t->x);                /* decode again with the key: the scan went on past it */
        t->k0 = (uint16_t)pick;
        found = dms_decode_verify(t, stored, len, r, retry_ok);
    }
    if (!found && weak >= 0) {                     /* only a key that does not end with the data: better than none */
        dms_init_decrunchers(t->x);
        t->k0 = (uint16_t)weak;
        found = dms_decode_verify(t, stored, len, r, retry_ok);
        t->weakkey = found;
    }
    t->unsure = unsure;
    return found;
}


/* ---------- repair of the packed data of a track ----------
 * The search for repairs is bounded by a number of candidate decodes and by time, so that opening a damaged archive in the
 * panel does not take for ever. */
static long g_dms_budget;                              /* Remaining candidate decodes allowed for this archive. */
static clock_t g_dms_rep_spent;                        /* time that the repairs of this archive have used so far */
static clock_t g_dms_rep_t0;                           /* when the repair that is running began */
static unsigned g_dms_ticks;                           /* calls of dms_budget_over: the clock is read every 64th */
static bool g_dms_budget_hit;                          /* a search was stopped by the limit */

/* The time used by the repairs. The search for a key, the unpacking of the tracks that need no repair and the rest of the
 * work are not counted: DMS_REPAIR_SECONDS is the time of the repairs, as the manual says, and the key has a limit of its own. */
static clock_t dms_rep_clock(void)
{
    return g_dms_rep_spent + (clock() - g_dms_rep_t0);
}

static bool dms_budget_over(void)
{
    if (g_dms_budget_hit) return true;
    if (g_dms_budget <= 0 || ((++g_dms_ticks & 63u) == 0 &&
                              dms_rep_clock() >= (clock_t)DMS_REPAIR_SECONDS * CLOCKS_PER_SEC)) {
        g_dms_budget_hit = true;
        return true;
    }
    return false;
}

/* Decode a repair candidate (counted against the budget); return true if the unpacked track has the expected checksum. */
static bool dms_trial(dms_trk_t *t, const uint8_t *cand, size_t len, bool *retry_ok)
{
    int r;
    g_dms_budget--;
    return dms_decode_verify(t, cand, len, &r, retry_ok);
}

#define DMS_REPAIR_INS_MAX 16384     /* longest track for the search of a missing byte of any value */

/* CRC16 is linear: with M = "process a zero byte", crc(P v S) from state 0 is
 *   M^(|S|+1) crc(P)  ^  M^(|S|) tab[v]  ^  crc(S)
 * so the CRC of all the (position, value) insertions is found without reprocessing the data. */
static uint16_t dms_crc_m(uint16_t s) { return (uint16_t)(g_dms_crc[0][s & 255] ^ (s >> 8)); }

/* B x, where the columns of B are B[i] = B (1 << i) */
static uint16_t dms_crc_apply(const uint16_t *B, uint16_t x)
{
    uint16_t r = 0;
    int i;
    for (i = 0; i < 16; i++) if (x & (1u << i)) r ^= B[i];
    return r;
}

/* One byte lost: every insertion whose CRC equals dcrc is unpacked and accepted only if the checksum of the unpacked
 * data is right too. Values equal to a neighbour (a run made longer) were tried by dms_repair_track already. */
static bool dms_repair_insert_any(dms_trk_t *t, const uint8_t *src, size_t L, size_t pklen1, uint16_t dcrc,
                                  bool *retry_ok, size_t *at, uint8_t *val)
{
    uint16_t *pre, B[16], G = 0;
    size_t k;
    int i;
    bool found = false;

    pre = (uint16_t *)xmalloc((L + 1) * sizeof(uint16_t));
    pre[0] = 0;
    for (k = 0; k < L; k++) pre[k + 1] = (uint16_t)(g_dms_crc[0][(pre[k] ^ src[k]) & 255] ^ (pre[k] >> 8));
    for (i = 0; i < 16; i++) B[i] = (uint16_t)(1u << i);                  /* B = M^(L-k) for k = L */
    for (k = L + 1; k-- > 0;) {                                         /* k = L ... 0; G = crc(src[k..L)) */
        uint16_t A;
        unsigned v;
        if (dms_budget_over()) break;
        A = (uint16_t)(dms_crc_m(dms_crc_apply(B, pre[k])) ^ G);
        for (v = 0; v < 256; v++) {
            if ((uint16_t)(A ^ dms_crc_apply(B, g_dms_crc[0][v])) != dcrc) continue;
            if (k < L && v == src[k]) continue;                           /* run made longer: tried before */
            if (k > 0 && v == src[k - 1]) continue;
            if (dms_budget_over()) break;
            memcpy(t->cand, src, k);
            t->cand[k] = (uint8_t)v;
            memcpy(t->cand + k + 1, src + k, L - k);
            if (dms_trial(t, t->cand, pklen1, retry_ok)) {
                *at = k;
                *val = (uint8_t)v;
                found = true;
                break;
            }
            memcpy(t->x, t->xr, sizeof(*t->x));
        }
        if (found || k == 0) break;
        G = (uint16_t)(dms_crc_apply(B, g_dms_crc[0][src[k - 1]]) ^ G);     /* crc(src[k-1..L)) */
        for (i = 0; i < 16; i++) B[i] = dms_crc_m(B[i]);                 /* M^(L-k+1) */
    }
    free(pre);
    return found;
}

/* One byte of the right length replaced by another value (the length of the track is as declared): same linear
 * computation. For the byte at k the CRC of the data with the value v is
 *   M^(L-k) crc(src[0..k))  ^  M^(L-k-1) tab[v]  ^  crc(src[k+1..L)) */
static bool dms_repair_subst(dms_trk_t *t, const uint8_t *src, size_t L, uint16_t dcrc, bool *retry_ok, size_t *at,
                             uint8_t *val)
{
    uint16_t *pre, B[16], G = 0;
    size_t k;
    int i;
    bool found = false;

    pre = (uint16_t *)xmalloc((L + 1) * sizeof(uint16_t));
    pre[0] = 0;
    for (k = 0; k < L; k++) pre[k + 1] = (uint16_t)(g_dms_crc[0][(pre[k] ^ src[k]) & 255] ^ (pre[k] >> 8));
    for (i = 0; i < 16; i++) B[i] = (uint16_t)(1u << i);                  /* B = M^(L-k-1), k = L-1: M^0 */
    for (k = L; k-- > 0;) {                                             /* G = crc(src[k+1..L)) */
        uint16_t A;
        unsigned v;
        if (dms_budget_over()) break;
        A = (uint16_t)(dms_crc_m(dms_crc_apply(B, pre[k])) ^ G);
        for (v = 0; v < 256; v++) {
            if (v == src[k]) continue;
            if ((uint16_t)(A ^ dms_crc_apply(B, g_dms_crc[0][v])) != dcrc) continue;
            if (dms_budget_over()) break;
            memcpy(t->cand, src, L);
            t->cand[k] = (uint8_t)v;
            if (dms_trial(t, t->cand, L, retry_ok)) {
                *at = k;
                *val = (uint8_t)v;
                found = true;
                break;
            }
            memcpy(t->x, t->xr, sizeof(*t->x));
        }
        if (found) break;
        G = (uint16_t)(dms_crc_apply(B, g_dms_crc[0][src[k]]) ^ G);         /* crc(src[k..L)) */
        for (i = 0; i < 16; i++) B[i] = dms_crc_m(B[i]);
    }
    free(pre);
    return found;
}

/* ---------- repair of several wrong bits ----------
 * The packed data have the declared length and no single wrong byte explains the CRC: the damage may be a few wrong
 * bits. The CRC16 is linear, so the difference between the CRC of the data and the one in the track header (the
 * syndrome) is the CRC of the pattern of wrong bits, and the pattern can be solved for instead of searched:
 *   - a burst of at most 16 bits: for every bit where it can start there is exactly one 16-bit pattern with that
 *     syndrome (moving the start one bit on multiplies the pattern by x, one bit of the CRC register); a pattern
 *     begins with a wrong bit, so every burst is found once;
 *   - two wrong bits more than 15 bits apart: the syndromes of all the bits go into hash chains, and the partner of
 *     a bit is the bit whose syndrome completes the difference.
 * Every candidate has the right CRC by construction and is unpacked. It passes if the checksum of the unpacked track is
 * right and, in the modes with a dictionary, the unpacked data end where the packed data end (a stream that went out
 * of step does not). The checksum is a sum of bytes, so several candidates can pass (the changes of two literals that
 * cancel, a copy of the same bytes from another place); therefore the candidates are tried in classes, the ones that
 * flip the fewest bits first (bursts of 2 to 8 wrong bits, pairs of far bits, bursts of 9 to 16), and a class gives a
 * correction only if exactly one of its candidates passes. NOCOMP and SIMPLE accept any stream and keep the weak
 * checksum alone: there a correction is taken only if it is the only one of all the candidates that passes.
 * When the track unpacked as it is still ends where the packed data end (the damage is in the bits of literals and
 * does not put the stream out of step) the checksum cannot choose between many candidates, and only bursts of up to
 * DMS_QUIET_WEIGHT wrong bits are tried.
 * The search is bounded: a track gets DMS_BITS_TRIALS candidate decodes (a class that exceeds the budget is skipped) and
 * DMS_BITS_SECONDS, and the whole-archive search gets half of the repair time and candidate-decode budget, so
 * that a track that cannot be repaired does not take all of it and the repairs of one byte are never starved. */
static bool g_dms_bits_hit;                            /* the search of wrong bits was stopped by the limit of the archive */
static bool g_dms_bits_cut;                            /* a class of candidates exceeded the per-track decode or time limit */
static unsigned g_dms_bits_ticks;

static bool dms_bits_over(void)
{
    if (g_dms_bits_hit || dms_budget_over()) return true;
    if (g_dms_budget <= DMS_REPAIR_BUDGET / 2 ||
        ((++g_dms_bits_ticks & 63u) == 0 && dms_rep_clock() >= (clock_t)DMS_REPAIR_SECONDS * CLOCKS_PER_SEC / 2)) {
        g_dms_bits_hit = true;
        return true;
    }
    return false;
}

typedef struct { size_t pos[3]; uint8_t mask[3]; unsigned n; int kind; } dms_flips_t;

static uint16_t dms_crc_x(uint16_t c)              /* times x: one bit of zero data through the CRC register */
{
    return (uint16_t)((c >> 1) ^ ((c & 1u) ? 0xA001u : 0u));
}

static uint16_t dms_crc_xinv(uint16_t c)           /* the inverse of dms_crc_x */
{
    return (c & 0x8000u) ? (uint16_t)(((c ^ 0xA001u) << 1) | 1u) : (uint16_t)(c << 1);
}

static unsigned dms_popcount16(unsigned v)
{
    unsigned n = 0;
    for (; v; v &= v - 1u) n++;
    return n;
}

static void dms_flip(uint8_t *buf, const dms_flips_t *f)
{
    unsigned i;
    for (i = 0; i < f->n; i++) buf[f->pos[i]] ^= f->mask[i];
}

static unsigned dms_flips_bits(const dms_flips_t *f)
{
    unsigned i, n = 0;
    for (i = 0; i < f->n; i++) n += dms_popcount16(f->mask[i]);
    return n;
}

/* The bits of the burst that starts at bit i0 with the 16-bit pattern r (bit j of r is bit i0 + j of the data) */
static void dms_burst_flips(dms_flips_t *f, size_t i0, uint16_t r)
{
    uint8_t m[3] = { 0, 0, 0 };
    unsigned j;
    memset(f, 0, sizeof(*f));
    f->kind = 5;
    for (j = 0; j < 16; j++)
        if ((r >> j) & 1u) {
            const unsigned b = (unsigned)(i0 & 7u) + j;
            m[b >> 3] = (uint8_t)(m[b >> 3] | (1u << (b & 7u)));
        }
    for (j = 0; j < 3; j++)
        if (m[j]) { f->pos[f->n] = (i0 >> 3) + j; f->mask[f->n] = m[j]; f->n++; }
}

/* The search in one track */
typedef struct {
    dms_trk_t   *t;
    size_t       L;
    bool        *retry_ok;
    bool         weak;                 /* NOCOMP, SIMPLE: one correction among all the candidates */
    unsigned     trials;               /* Remaining candidate decodes allowed for this track. */
    unsigned     ticks;
    clock_t      deadline;             /* end of the time of this track */
    unsigned     passed;               /* candidates that passed in the class (weak modes: in all of them) */
    bool         skipped;              /* a class exceeded the per-track candidate-decode budget and was skipped */
    dms_flips_t  hit;                  /* the first that passed */
} dms_multi_t;

/* Tries one candidate; the candidate and the decompressor are put back afterwards. False when the search must stop: a
 * limit, or a second candidate that passes in a weak mode */
static bool dms_multi_try(dms_multi_t *m, const dms_flips_t *f)
{
    dms_trk_t *t = m->t;
    bool pass;
    if (dms_bits_over()) return false;
    if ((++m->ticks & 63u) == 0 && clock() >= m->deadline) { g_dms_bits_cut = true; return false; }
    m->trials--;
    dms_flip(t->cand, f);
    pass = dms_trial(t, t->cand, m->L, m->retry_ok) && (m->weak || dms_tail_ok(t->unread));
    dms_flip(t->cand, f);
    if (t->cmode >= 2) memcpy(t->x, t->xr, sizeof(*t->x));
    if (!pass) return true;
    if (m->passed++ == 0) m->hit = *f;
    return !m->weak || m->passed < 2;
}

/* The bursts of w wrong bits that touch two bytes or more (the ones inside a byte were tried by the search for a wrong
 * byte): counted when `count` is set, otherwise evaluated. Returns their number; *go is cleared when a candidate decode hits the limit */
static size_t dms_multi_burst_pass(dms_multi_t *m, uint16_t syn, unsigned w, bool count, bool *go)
{
    const size_t nbits = m->L * 8;
    size_t i0, n = 0;
    uint16_t r = syn;
    for (i0 = 0; i0 < nbits; i0++) r = dms_crc_xinv(r);              /* the pattern of the burst that starts at bit 0 */
    for (i0 = 0; i0 + 1 < nbits; i0++, r = dms_crc_x(r)) {
        unsigned hi;
        dms_flips_t f;
        if (!(r & 1u) || dms_popcount16(r) != w) continue;
        if (nbits - i0 < 16 && (r >> (nbits - i0)) != 0) continue;         /* the pattern goes past the end */
        for (hi = 15; !((r >> hi) & 1u); hi--) { }
        if ((unsigned)(i0 & 7u) + hi < 8u) continue;                        /* inside one byte */
        n++;
        if (count) continue;
        dms_burst_flips(&f, i0, r);
        if (!dms_multi_try(m, &f)) { *go = false; return n; }
    }
    return n;
}

/* The bursts of wlo..whi wrong bits, one weight at a time. 1: a correction, 0: none, -1: ambiguous or stopped. A class
 * that exceeds the remaining per-track candidate-decode budget is skipped (m->skipped): a correction found after that
 * has not been compared with all the candidates that flip fewer bits, and the caller says so */
static int dms_multi_bursts(dms_multi_t *m, uint16_t syn, unsigned wlo, unsigned whi)
{
    unsigned w;
    for (w = wlo; w <= whi; w++) {
        bool go = true;
        if (!m->weak) m->passed = 0;
        if (dms_multi_burst_pass(m, syn, w, true, &go) > m->trials) {
            g_dms_bits_cut = true;
            m->skipped = true;
            if (m->weak) return -1;
            continue;
        }
        dms_multi_burst_pass(m, syn, w, false, &go);
        if (!go) return -1;
        if (!m->weak && m->passed) return m->passed == 1 ? 1 : -1;
    }
    return 0;
}

/* The pairs of wrong bits more than 15 bits apart: same results as dms_multi_bursts */
static int dms_multi_pairs(dms_multi_t *m, uint16_t syn)
{
    const size_t nbits = m->L * 8;
    uint16_t c = 0xA001u;                          /* the syndrome of the last bit of the data */
    uint16_t *syd = (uint16_t *)xmalloc(nbits * sizeof(uint16_t));
    int32_t *next = (int32_t *)xmalloc(nbits * sizeof(int32_t)), *head = (int32_t *)xmalloc(65536 * sizeof(int32_t));
    size_t i, n = 0;
    int rc = 0;

    for (i = 0; i < 65536; i++) head[i] = -1;
    for (i = nbits; i-- > 0;) {
        syd[i] = c;
        next[i] = head[c];
        head[c] = (int32_t)i;
        c = dms_crc_x(c);
    }
    for (i = 0; i < nbits; i++) {
        int32_t q;
        for (q = head[(uint16_t)(syn ^ syd[i])]; q >= 0; q = next[q])
            if ((size_t)q >= i + 16) n++;
    }
    if (n > m->trials) {
        g_dms_bits_cut = true;
        m->skipped = true;
        rc = m->weak ? -1 : 0;
    } else {
        if (!m->weak) m->passed = 0;
        for (i = 0; i < nbits && rc == 0; i++) {
            int32_t q;
            for (q = head[(uint16_t)(syn ^ syd[i])]; q >= 0; q = next[q]) {
                dms_flips_t f;
                if ((size_t)q < i + 16) continue;      /* closer than that: a burst */
                memset(&f, 0, sizeof(f));
                f.n = 2;
                f.kind = 6;
                f.pos[0] = i >> 3;           f.mask[0] = (uint8_t)(1u << (i & 7u));
                f.pos[1] = (size_t)q >> 3;   f.mask[1] = (uint8_t)(1u << ((size_t)q & 7u));
                if (!dms_multi_try(m, &f)) { rc = -1; break; }
            }
        }
        if (rc == 0 && !m->weak && m->passed) rc = m->passed == 1 ? 1 : -1;
    }
    free(syd);
    free(next);
    free(head);
    return rc;
}

/* Returns true on success: t->cand holds the repaired data, b2 and the state are those of the right decode; *what = 5
 * a burst, 6 two bits; *at the byte of the first wrong bit; *val the number of wrong bits. t->loud says whether the
 * data unpacked as they are lost the end of the packed data. t->rep_skipped tells that the correction was found after a
 * class of candidates with fewer wrong bits had been skipped: it is then a correction that nothing contradicts, not
 * the one that the order of the classes would have given. */
static bool dms_repair_multi(dms_trk_t *t, const uint8_t *src, size_t L, uint16_t dcrc, bool *retry_ok, int *what,
                             size_t *at, uint8_t *val)
{
    const uint16_t syn = (uint16_t)(dms_crc16(src, L) ^ dcrc);
    const bool all = t->loud || t->cmode <= 1;
    dms_multi_t m;
    int rc;

    t->rep_skipped = false;
    if (syn == 0 || L < 3 || L > DMS_REPAIR_INS_MAX) return false;
    memcpy(t->cand, src, L);
    memset(&m, 0, sizeof(m));
    m.t = t;
    m.L = L;
    m.retry_ok = retry_ok;
    m.weak = t->cmode <= 1;
    m.trials = DMS_BITS_TRIALS;
    m.deadline = clock() + (clock_t)DMS_BITS_SECONDS * CLOCKS_PER_SEC;
    rc = dms_multi_bursts(&m, syn, 2, all ? 8 : DMS_QUIET_WEIGHT);
    if (rc == 0 && all) rc = dms_multi_pairs(&m, syn);
    if (rc == 0 && all) rc = dms_multi_bursts(&m, syn, 9, 16);
    if (rc == 0 && m.weak && m.passed == 1) rc = 1;
    if (rc != 1) return false;
    /* the one correction that passed: apply it and unpack again, for the data and the state of the right decode */
    dms_flip(t->cand, &m.hit);
    if (!dms_trial(t, t->cand, L, retry_ok) || (t->cmode >= 2 && !dms_tail_ok(t->unread))) {
        dms_flip(t->cand, &m.hit);
        if (t->cmode >= 2) memcpy(t->x, t->xr, sizeof(*t->x));
        return false;
    }
    *what = m.hit.kind;
    *at = m.hit.pos[0];
    *val = (uint8_t)dms_flips_bits(&m.hit);
    t->rep_skipped = m.skipped;
    return true;
}

/*
 * Repair of the packed data of a track. `src` holds the L bytes between the end of the track header and the next valid
 * track header (or the end of the file); pklen1 is the length declared by the header and dcrc its data CRC. The length
 * says which damage to look for: L = pklen1 + 1 an extra byte (every distinct deletion is tried), L = pklen1 - 1 a
 * missing byte (first the runs made longer by one byte, then, for tracks up to DMS_REPAIR_INS_MAX bytes, every value at
 * every position), L = pklen1 one byte with a wrong value and, if none explains the CRC, a few wrong bits (see
 * dms_repair_multi). A candidate whose CRC16 equals dcrc is unpacked and accepted only if the checksum of the unpacked
 * data is right too: in the modes with a dictionary, this gives an independent 16-bit check (about 2^-16 chance of
 * accepting an incorrect candidate); in NOCOMP and SIMPLE
 * the checksum is the sum of the bytes of data that any stream gives, and a wrong byte can be replaced by another one
 * that makes the same sum (the report names the byte that was replaced).
 * Returns true on success (state and b2 valid); *what = 1 extra byte removed, 2 missing byte restored in a run, 3 missing
 * byte restored, 4 one byte with a wrong value corrected, 5 a burst of wrong bits corrected, 6 two wrong bits corrected;
 * *at, *val describe the change.
 */
static bool dms_repair_track_work(dms_trk_t *t, const uint8_t *src, size_t L, size_t pklen1, uint16_t dcrc,
                                  bool *retry_ok, int *what, size_t *at, uint8_t *val)
{
    size_t k;

    memcpy(t->xr, t->x, sizeof(*t->x));
    if (L == pklen1) {                                         /* same length: a wrong byte, or a few wrong bits */
        if (pklen1 <= DMS_REPAIR_INS_MAX && dms_repair_subst(t, src, L, dcrc, retry_ok, at, val)) {
            *what = 4;
            return true;
        }
        return pklen1 <= DMS_REPAIR_INS_MAX && dms_repair_multi(t, src, L, dcrc, retry_ok, what, at, val);
    }
    for (k = 0; k < L; k++) {
        if (k > 0 && src[k] == src[k - 1]) continue;          /* one candidate per run */
        if (dms_budget_over()) return false;
        if (L == pklen1 + 1) {                                 /* an extra byte: remove it from the run */
            memcpy(t->cand, src, k);
            memcpy(t->cand + k, src + k + 1, L - k - 1);
        } else {                                               /* a byte is missing: lengthen the run */
            memcpy(t->cand, src, k);
            t->cand[k] = src[k];
            memcpy(t->cand + k + 1, src + k, L - k);
        }
        if (dms_crc16(t->cand, pklen1) != dcrc) continue;
        if (dms_trial(t, t->cand, pklen1, retry_ok)) {
            *what = (L == pklen1 + 1) ? 1 : 2;
            *at = k;
            *val = src[k];
            return true;
        }
        memcpy(t->x, t->xr, sizeof(*t->x));
    }
    if (L + 1 == pklen1 && pklen1 <= DMS_REPAIR_INS_MAX &&
        dms_repair_insert_any(t, src, L, pklen1, dcrc, retry_ok, at, val)) {
        *what = 3;
        return true;
    }
    return false;
}

/* dms_repair_track_work, timed: the time of the repairs of the archive (DMS_REPAIR_SECONDS) is the time spent in here */
static bool dms_repair_track(dms_trk_t *t, const uint8_t *src, size_t L, size_t pklen1, uint16_t dcrc,
                             bool *retry_ok, int *what, size_t *at, uint8_t *val)
{
    bool done;
    g_dms_rep_t0 = clock();
    done = dms_repair_track_work(t, src, L, pklen1, dcrc, retry_ok, what, at, val);
    g_dms_rep_spent += clock() - g_dms_rep_t0;
    return done;
}

/* ---------------------------------------------------------------------------------------------------- */
/* DMS archive -> disk image, in memory                                                                     */
/* ---------------------------------------------------------------------------------------------------- */

/* what became of each track (cylinder) of the disk */
enum {
    DMT_ABSENT = 0,      /* not in the archive: zeros */
    DMT_OK,              /* CRC of the packed data and checksum of the unpacked track are right */
    DMT_REPAIRED,        /* the header or the packed data were damaged and repaired: CRC and checksum are right */
    DMT_UNVERIFIED,      /* kept, not verified: the checksum of the track is not the expected one (the packed data are intact),
                          * or the key (several keys fit, or one fits weakly) or the repair (a class of candidates was skipped)
                          * is not completely verified */
    DMT_BEST,            /* kept as unpacked, not verified: damaged data, or copies from a damaged track */
    DMT_LOST             /* in the archive, not recovered: zeros */
};

/* why a file is not opened: set by the readers, shown by OpenFilePluginW */
enum {
    REFUSE_NONE = 0,
    REFUSE_READ,                           /* the file cannot be read, or its size is not valid */
    REFUSE_MEMORY,                         /* not enough memory */
    REFUSE_NODISK,                         /* a DMS archive without a disk that can be read */
    REFUSE_DISKTYPE,                       /* disk type 7: the archive does not hold a disk image */
    REFUSE_ENCRYPTED                       /* an encrypted DMS archive that cannot be decrypted */
};

typedef struct {
    uint8_t *image;                        /* malloc'ed disk image (zeros where there is no data) */
    size_t   image_size;
    unsigned tsize;                        /* bytes per track */
    unsigned maxnum;                       /* highest track found */
    uint8_t  st[DMS_MAX_TRACKS];           /* DMT_* per track */
    unsigned n_ok, n_repaired, n_unverified, n_best, n_lost, n_absent;
    unsigned hdr_repaired;                 /* track headers repaired */
    bool     hdr_ok;                       /* the archive header was right */
    bool     sig_ok;                       /* the signature "DMS!" was right */
    bool     budget_hit;                   /* the search for repairs was stopped by the effort limit */
    bool     encrypted;                    /* the archive is encrypted (and decrypted, if the key was found) */
    bool     key_hit;                      /* the search for the key was stopped by the time limit */
    bool     any_error;
    unsigned from, to, version;
    const char *mode;
    int      refuse;                       /* REFUSE_*: why dms_unpack_image returned -1 */
} DmsOut;

static void dms_out_free(DmsOut *o)
{
    free(o->image);
    memset(o, 0, sizeof(*o));
}

/* Number of disk tracks that a track size can hold: 160 half-cylinder tracks, else 80 cylinders */
static unsigned dms_tracks_for(unsigned tsize)
{
    return tsize == DMS_TRACK_HALF ? DMS_MAX_TRACKS : DMS_STD_TRACKS;
}

/* a note of the report, limited like the errors of dms_log_error (a damaged track spoils the following ones) */
static void dms_log_note(unsigned *shown, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void dms_log_note(unsigned *shown, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (*shown >= DMS_MAX_LOGGED) { (*shown)++; return; }
    (*shown)++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_note("%s", buf);
}

/* How far a stored track can be trusted: 0 nothing (absent or lost), 1 kept with errors, 2 verified */
static unsigned dms_rank(unsigned st)
{
    if (st == DMT_OK || st == DMT_REPAIRED) return 2;
    return (st == DMT_UNVERIFIED || st == DMT_BEST) ? 1 : 0;
}

/*
 * Unpacks a DMS archive held in memory. Returns 0 if a disk image was produced (o->image, also when tracks are
 * missing or damaged: see o->st), -1 if the archive cannot be used (the report says why).
 *   - A track header is repaired when it is damaged in one or two bytes or only in its CRC field; a header that cannot
 *     be repaired is skipped by searching the next valid one (the dictionary of the following tracks is then uncertain).
 *   - The packed data of a track end at the next valid track header, so a lost or an added byte is seen from the length;
 *     the file may end inside the last track, which is then unpacked as far as it goes.
 *   - Every track is decoded and judged by its own checksum. The modes that keep a dictionary from one track to the next
 *     continue from the dictionary left by the previous tracks; when that dictionary is uncertain (a track was lost or
 *     damaged, a header was skipped) a track that fails is decoded again from an empty one. A
 *     HEAVY track that fails after tolerating a last match past its end or an incomplete table of distances is also decoded
 *     in the other readings of those two points.
 *   - A track whose packed data have a wrong CRC is first unpacked as it is (the damage may be in bytes that the decoder
 *     does not need) and kept if the checksum of the unpacked track is right. If not, it is repaired when the length and
 *     the CRC point at one lost, added or wrong byte, or at a few wrong bits (see dms_repair_multi), and the checksum
 *     of the unpacked track is then right. The last track has no next header to give its length: the end of the file
 *     does, when it is one byte off. A track that cannot be verified is kept as decoded (the part before the damage is
 *     right); a track that decodes to nothing is lost.
 *   - An encrypted archive is decrypted: the key is found from the checksum of the first disk track (a first track
 *     that more than one key fits, or that only a weak key fits, is shown as not verified). If the search for the key is
 *     stopped by the time limit the archive is shown as damaged; if no track can be decrypted it is refused.
 *   - A track that appears twice is stored once: the one that is verified, or else the first.
 */
static int dms_unpack_image(const uint8_t *data, uint64_t size, DmsOut *o)
{
    uint8_t *b1 = NULL, *b2 = NULL, *img = NULL, *cand = NULL, *keep = NULL;
    dms_ctx_t *x = NULL, *xs = NULL, *xp = NULL, *xr = NULL;
    uint64_t pos;
    unsigned geninfo, from, to, disktype, cmode_hdr, n_err = 0, n_stored = 0;
    unsigned tsize = 0, tmax = DMS_STD_TRACKS, maxnum = 0, last_num = 0, exp_num;
    unsigned n_shown = 0, n_ver = 0, n_kept = 0, t;
    uint8_t hfix[DMS_THLEN];
    bool enc, key_search = true, any_error = false, hdr_ok = true, state_unsure = false;
    uint16_t kcur = 0;                         /* the running key: the key at the start of the next track */
    size_t enc_bytes = 0;                      /* encrypted bytes that the key has gone through */
    dms_trk_t trk;
    int rc = 0;

    memset(o, 0, sizeof(*o));
    dms_crc_init();
    g_dms_budget = DMS_REPAIR_BUDGET;
    g_dms_rep_spent = 0;
    g_dms_rep_t0 = clock();
    g_dms_bits_hit = false;
    g_dms_bits_cut = false;
    g_dms_bits_ticks = 0;
    g_dms_key_hit = false;
    g_dms_ticks = 0;
    g_dms_budget_hit = false;
    if (!dms_is_signature(data, size < 512 ? (size_t)size : 512, size)) {
        log_error("DMS: invalid signature");
        o->refuse = REFUSE_NODISK;
        return -1;
    }

    /* archive header (56 bytes): CRC over bytes 4..53 */
    if (dms_crc16(data + 4, DMS_HEADLEN - 6) != (uint16_t)(((unsigned)data[DMS_HEADLEN - 2] << 8) | data[DMS_HEADLEN - 1])) {
        dms_log_error(&n_shown, "DMS: INVALID HEADER: archive header CRC mismatch (disk information ignored, the flag of encryption is kept)");
        any_error = true;
        n_err++;
        hdr_ok = false;
    }
    o->sig_ok = memcmp(data, "DMS!", 4) == 0;
    if (!o->sig_ok) {                          /* accepted by the first track header */
        dms_log_error(&n_shown, "DMS: INVALID HEADER: signature \"DMS!\" damaged");
        any_error = true;
        n_err++;
    }
    geninfo   = ((unsigned)data[10] << 8) | data[11];
    from      = ((unsigned)data[16] << 8) | data[17];
    to        = ((unsigned)data[18] << 8) | data[19];
    disktype  = ((unsigned)data[50] << 8) | data[51];
    cmode_hdr = ((unsigned)data[52] << 8) | data[53];
    if (!hdr_ok) { geninfo &= 2; disktype = 0; to = 0xFFFF; from = 0; }     /* the flag of encryption stays: the first track decides */
    exp_num = from;
    o->hdr_ok = hdr_ok;
    o->from = from;
    o->to = to;
    o->mode = dms_mode_name(cmode_hdr);
    o->version = ((unsigned)data[46] << 8) | data[47];
    if (hdr_ok)
        log_msg("  DMS: tracks %u-%u, main mode %s, created with DMS %u.%02u%s", from, to,
                dms_mode_name(cmode_hdr), o->version / 100, o->version % 100, (geninfo & 2) ? ", encrypted" : "");

    if (disktype == 7) {
        /* disk type 7: the archive holds files, not a disk image */
        log_error("DMS: UNSUPPORTED METHOD (disk type 7, not a disk image)");
        o->refuse = REFUSE_DISKTYPE;
        return -1;
    }
    enc = (geninfo & 2) != 0;
    o->encrypted = enc;

    x = (dms_ctx_t *)xcalloc(1, sizeof(*x));
    xs = (dms_ctx_t *)xmalloc(sizeof(*x));
    xp = (dms_ctx_t *)xmalloc(sizeof(*x));
    xr = (dms_ctx_t *)xmalloc(sizeof(*x));
    b1 = (uint8_t *)xcalloc(1, DMS_TRACK_MAX + DMS_SLACK);
    b2 = (uint8_t *)xcalloc(1, DMS_TRACK_MAX + DMS_SLACK);
    cand = (uint8_t *)xmalloc(DMS_TRACK_MAX + DMS_SLACK);
    keep = (uint8_t *)xmalloc(DMS_TRACK_MAX + DMS_SLACK);
    dms_init_decrunchers(x);
    memset(&trk, 0, sizeof(trk));
    trk.x = x; trk.xs = xs; trk.xp = xp; trk.xr = xr; trk.b1 = b1; trk.b2 = b2; trk.cand = cand; trk.keep = keep;

    pos = DMS_HEADLEN;
    for (;;) {
        unsigned dcrc, usum, number, pklen1, pklen2, unpklen, cmode, flags, use_len, stat_new, rank_new;
        const uint8_t *th;
        uint64_t data_start, next_pos;
        bool crc_ok, ok, retry_ok = false, stateful, repaired = false, trunc, decoded = false, hdr_fixed = false;
        bool key_unsure = false;               /* the key of this track is one of several that match its checksum, or fits weakly */
        bool rep_unsure = false;               /* the repair of this track skipped a class of candidates with fewer wrong bits */
        int r = 0;
        size_t rep_L = 0;                      /* the length of the track data when it is one byte off the declared one */
        size_t rep_L2 = 0;                     /* a second length to try when the first repair fails */
        uint16_t k_start;
        size_t enc_before;
        /* the highest track that the archive header announces (or, if more tracks were found, the last track of a
         * standard disk) was found: what follows is not a track */
        const bool all_found = n_stored > 0 && last_num >= (maxnum > to ? tmax - 1 : to);

        if (pos >= size) break;                /* clean end of the data */
        th = data + pos;
        if (size - pos >= DMS_THLEN && !dms_header_at(data, size, pos) && !all_found &&
            dms_fix_header(data, size, pos, exp_num, hfix)) {
            /* a damaged byte in the header: the track is not lost any more */
            dms_log_note(&n_shown, "DMS: track header at offset %llu damaged and repaired (track %u)",
                         (unsigned long long)pos, ((unsigned)hfix[2] << 8) | hfix[3]);
            th = hfix;
            hdr_fixed = true;
            o->hdr_repaired++;
        }
        if (size - pos < DMS_THLEN || !dms_header_ok(th)) {
            uint64_t q;
            if (size - pos < DMS_THLEN || th[0] != 'T' || th[1] != 'R') {
                /* whatever follows the last track (text, zeros) is not an error: the highest track number of the
                 * archive header decides, if it was reached the rest is ignored */
                if (all_found) break;
                dms_log_error(&n_shown, "DMS: INVALID HEADER: track header expected at offset %llu (%llu bytes left)",
                              (unsigned long long)pos, (unsigned long long)(size - pos));
            } else if (dms_crc16(th, DMS_THLEN - 2) != (uint16_t)(((unsigned)th[18] << 8) | th[19])) {
                dms_log_error(&n_shown, "DMS: INVALID HEADER: track header CRC mismatch at offset %llu",
                              (unsigned long long)pos);
            } else {
                dms_log_error(&n_shown, "DMS: INVALID HEADER: track %u too big at offset %llu",
                              ((unsigned)th[2] << 8) | th[3], (unsigned long long)pos);
            }
            any_error = true;
            n_err++;
            /* not the end of the archive: go on at the next valid track header */
            if (size - pos < DMS_THLEN || !dms_resync(data, size, pos + 1, &q)) break;
            log_msg("  DMS: next track header found at offset %llu (%llu bytes skipped)", (unsigned long long)q,
                    (unsigned long long)(q - pos));
            pos = q;
            state_unsure = true;               /* the skipped bytes may hold tracks */
            continue;
        }
        number  = ((unsigned)th[2] << 8) | th[3];
        exp_num = number + 1;
        pklen1  = ((unsigned)th[6] << 8) | th[7];
        pklen2  = ((unsigned)th[8] << 8) | th[9];
        unpklen = ((unsigned)th[10] << 8) | th[11];
        flags   = th[12];
        cmode   = th[13];
        usum    = ((unsigned)th[14] << 8) | th[15];
        dcrc    = ((unsigned)th[16] << 8) | th[17];

        data_start = pos + DMS_THLEN;
        trunc = size - data_start < pklen1;    /* the file ends inside the track: decode what is there */
        if (trunc) {
            next_pos = size;
            use_len = (unsigned)(size - data_start);
            crc_ok = false;
            if (use_len + 1 == pklen1) rep_L = use_len;      /* the file ends one byte early: a byte lost, or a cut */
        } else {
            next_pos = data_start + pklen1;
            use_len = pklen1;
            crc_ok = dms_crc16(data + data_start, pklen1) == dcrc;
        }
        if (!crc_ok && !trunc) {
            /* the length in the header cannot be trusted (a byte lost or added): the next valid track header,
             * searched from the start of the data, tells where the track ends */
            uint64_t q = 0, lq = pklen1;       /* no later header: as declared */
            bool found_q = dms_resync(data, size, data_start, &q);
            if (found_q) lq = q - data_start;
            if (!found_q && size - next_pos == 1) {
                rep_L = pklen1 + 1;            /* the file ends one byte late: a byte added in the data, or a stray byte */
                rep_L2 = pklen1;
            } else if (!found_q || q > next_pos + 1) rep_L = pklen1;    /* nothing says that the length is not the declared one */
            else if (lq == (uint64_t)pklen1 + 1 || lq + 1 == (uint64_t)pklen1 || lq == (uint64_t)pklen1) rep_L = (size_t)lq;
            /* The data end at the next header when it is earlier than declared (bytes lost) or one byte later (a byte
             * added). A header found farther on is not a longer track: the header at the declared end is damaged, and
             * the next track is repaired or skipped when the loop gets there. */
            if (found_q && q != next_pos && (q < next_pos || q == next_pos + 1)) {
                if (q < next_pos) use_len = (unsigned)(q - data_start);
                log_msg("  DMS: track %u: the next track header is at offset %llu instead of %llu", number,
                        (unsigned long long)q, (unsigned long long)next_pos);
                next_pos = q;
            }
        }
        pos = next_pos;

        /* the key at the start of this track; the running key goes through its packed data */
        k_start = kcur;
        enc_before = enc_bytes;
        if (enc && number != 80) {
            kcur = dms_key_advance(kcur, data + data_start, (size_t)(next_pos - data_start));
            enc_bytes += (size_t)(next_pos - data_start);
        }

        /* tracks 80 (the file description), 0xFFFF (the banner) and the 1 KiB fake boot block are not disk data; the
         * 160 tracks of 5632 bytes are. They leave an empty dictionary. */
        if (unpklen <= 2048 || number >= DMS_MAX_TRACKS || (number >= DMS_STD_TRACKS && unpklen != DMS_TRACK_HALF)) {
            dms_init_decrunchers(x);
            continue;
        }

        stateful = cmode >= 2;
        trk.unsure = state_unsure;
        trk.loud = false;
        trk.pklen2 = pklen2; trk.unpklen = unpklen; trk.cmode = cmode; trk.flags = flags; trk.usum = usum;
        trk.enc = enc;
        trk.k0 = k_start;
        if (enc && key_search && crc_ok) {
            /* the first disk track of an encrypted archive: (1) the key that the data before it gave (a banner),
             * (2) not encrypted after all (the flag set by mistake), (3) the 65536 keys. The later tracks just follow
             * the running key, which synchronizes itself after a few bytes. */
            key_search = false;
            decoded = true;
            dms_init_decrunchers(x);
            ok = dms_decode_verify(&trk, data + data_start, use_len, &r, &retry_ok);
            if (!ok) {
                dms_init_decrunchers(x);
                trk.enc = false;
                ok = dms_decode_verify(&trk, data + data_start, use_len, &r, &retry_ok);
                if (ok) {
                    enc = false;
                    o->encrypted = false;
                    dms_log_note(&n_shown, "DMS: the archive is marked as encrypted but its data is not: no decryption");
                } else {
                    trk.enc = true;
                }
            }
            if (!ok && enc_before < 64) {
                log_msg("  DMS: encrypted archive: searching the decryption key of track %u (65536 candidates)", number);
                if (dms_find_key(&trk, data + data_start, use_len, number, &r, &retry_ok)) {
                    ok = true;
                    kcur = dms_key_advance(trk.k0, data + data_start, use_len);
                    dms_log_note(&n_shown, "DMS: encrypted archive: decryption key 0x%04X found (the checksum of track %u is right)",
                                 (unsigned)trk.k0, number);
                    if (trk.ambiguous) {
                        key_unsure = true;
                        dms_log_note(&n_shown, "DMS: encrypted archive: another key gives the right checksum for track %u too: "
                                     "its first bytes (up to about 20) are not certain", number);
                    }
                    if (trk.weakkey) {
                        key_unsure = true;
                        dms_log_note(&n_shown, "DMS: encrypted archive: the key gives the right checksum for track %u but the "
                                     "unpacked data do not end where the packed data end: the key is not verified (a key that "
                                     "fits by chance is found about once in a search), the track is shown as not verified",
                                     number);
                    }
                } else {
                    dms_init_decrunchers(x);
                    trk.k0 = k_start;
                    ok = dms_decode_verify(&trk, data + data_start, use_len, &r, &retry_ok);
                    dms_log_error(&n_shown, "DMS: encrypted archive: no decryption key gives the right checksum for track %u", number);
                }
            }
        }
        if (!decoded && !crc_ok && !(enc && key_search && enc_before < 64)) {
            /* the CRC of the packed data is wrong: unpack them as they are first, the damage may be in bytes that the
             * decoder does not need. If the checksum of the unpacked track is right the track is kept; else the
             * decompressor goes back to its state and the repairs are tried */
            memcpy(trk.xr, x, sizeof(*x));
            ok = dms_decode_verify(&trk, data + data_start, use_len, &r, &retry_ok);
            decoded = true;
            trk.loud = r != 0 || !dms_tail_ok(trk.unread);
            if (!ok && rep_L != 0) {
                memcpy(x, trk.xr, sizeof(*x));
                decoded = false;
            }
        }
        if (!decoded && !crc_ok && rep_L != 0 && !(enc && key_search && enc_before < 64)) {   /* a lost, added or wrong byte or bit? */
            int what = 0;
            size_t at = 0;
            uint8_t val = 0;
            size_t used_L = rep_L;
            bool done = dms_repair_track(&trk, data + data_start, rep_L, pklen1, (uint16_t)dcrc, &retry_ok, &what, &at, &val);
            if (!done && rep_L2 != 0) {
                used_L = rep_L2;
                done = dms_repair_track(&trk, data + data_start, rep_L2, pklen1, (uint16_t)dcrc, &retry_ok, &what, &at, &val);
            }
            if (done) {
                repaired = true;
                if (used_L > pklen1 && next_pos < size && data_start + used_L == size) pos = next_pos = size;   /* the byte that ended the file was part of the track */
                if (what >= 5) {
                    dms_log_note(&n_shown, "DMS: track %u repaired: %s (%u bit%s) at offset %llu of the track data (the CRC of the "
                                 "packed data and the checksum of the unpacked data are both right)", number,
                                 what == 5 ? "a burst of wrong bits corrected" : "two wrong bits corrected", (unsigned)val,
                                 val == 1 ? "" : "s", (unsigned long long)at);
                    if (trk.rep_skipped) {
                        rep_unsure = true;
                        dms_log_note(&n_shown, "DMS: track %u: a class of candidates with fewer wrong bits was too large to be "
                                     "tried before this correction was found: the correction is not completely verified, the "
                                     "track is shown as not verified", number);
                    }
                } else
                    dms_log_note(&n_shown, "DMS: track %u repaired: %s 0x%02X at offset %llu of the track data (the CRC of the "
                                 "packed data and the checksum of the unpacked data are both right)", number,
                                 what == 1 ? "one extra byte removed from a run of" : what == 2 ? "one missing byte restored in a run of"
                                           : what == 3 ? "one missing byte restored, value" : "one wrong byte replaced, new value",
                                 (unsigned)val, (unsigned long long)at);
            }
        }
        if (repaired) {
            ok = true;
            if (enc) kcur = dms_key_advance(k_start, cand, pklen1);   /* the key goes through the repaired data */
        } else if (!decoded) ok = dms_decode_verify(&trk, data + data_start, use_len, &r, &retry_ok);
        if (!ok) {
            /* what could be decoded of a damaged track is kept (it is better than a hole in the disk) and the report
             * says so; nothing is kept when there is nothing to keep (an unknown method, no byte decoded) */
            bool keep_it = r != 2 && trk.got > 0;
            char fate[112] = "discarded";
            if (keep_it) {
                if (trk.got < unpklen)
                    snprintf(fate, sizeof(fate), "kept with errors (%lu of %u bytes decoded, the rest is zero)",
                             (unsigned long)trk.got, unpklen);
                else
                    snprintf(fate, sizeof(fate), "kept with errors");
            }
            if (trunc)
                dms_log_error(&n_shown, "DMS: TRUNCATED, track %u %s", number, fate);
            else if (!crc_ok)
                dms_log_error(&n_shown, "DMS: CRC mismatch, track %u %s", number, fate);
            else if (r == 2)
                dms_log_error(&n_shown, "DMS: UNSUPPORTED METHOD %u, track %u %s", cmode, number, fate);
            else if (r != 0)
                dms_log_error(&n_shown, "DMS: CRC (decompression failed, mode %s), track %u %s", dms_mode_name(cmode),
                              number, fate);
            else
                dms_log_error(&n_shown, "DMS: CRC mismatch (track checksum, mode %s), track %u %s", dms_mode_name(cmode),
                              number, fate);
            any_error = true;
            if (stateful) {
                if (flags & 1) state_unsure = true;
                else { dms_init_decrunchers(x); state_unsure = false; }   /* the encoder reset it after this track */
            }
            if (!keep_it) {
                n_err++;
                if (o->st[number] == DMT_ABSENT) o->st[number] = DMT_LOST;
                continue;
            }
            stat_new = (crc_ok && r == 0) ? DMT_UNVERIFIED : DMT_BEST;
            rank_new = 1;
        } else {
            if (trunc) {
                if (!repaired)
                    dms_log_note(&n_shown, "DMS: TRUNCATED, track %u: the file ends %u bytes early but the checksum of the unpacked "
                                 "data is right: track kept", number, pklen1 - use_len);
            } else if (!crc_ok && !repaired)
                dms_log_note(&n_shown, "DMS: the packed data CRC of track %u does not match but the checksum of the unpacked "
                             "data does: track kept", number);
            if (retry_ok)
                log_msg("  DMS: track %u unpacked from an empty dictionary (the previous tracks are damaged or missing)",
                        number);
            if (trk.quirks) {
                char qt[160];
                dms_quirk_text(trk.quirks, qt, sizeof(qt));
                dms_log_note(&n_shown, "DMS: track %u decoded with tolerance (%s): the checksum of the unpacked data is right",
                             number, qt);
            }
            if (state_unsure && (!(flags & 1) || (cmode >= 5 && pklen2 >= 8192))) state_unsure = false;
            stat_new = (key_unsure || rep_unsure) ? DMT_UNVERIFIED : (repaired || hdr_fixed) ? DMT_REPAIRED : DMT_OK;
            rank_new = (key_unsure || rep_unsure) ? 1 : 2;
        }
        if (tsize == 0) {
            tsize = unpklen;
            tmax = dms_tracks_for(tsize);
            img = (uint8_t *)xcalloc(tmax, tsize);
        } else if (unpklen != tsize) {
            dms_log_error(&n_shown, "DMS: track %u has size %u instead of %u, discarded", number, unpklen, tsize);
            any_error = true;
            n_err++;
            if (o->st[number] == DMT_ABSENT) o->st[number] = DMT_LOST;
            continue;
        }
        if (number >= tmax) continue;          /* a track the image cannot hold (160-track numbers in a 80-track disk) */
        if (dms_rank(o->st[number]) > 0) {     /* a second copy of the track: only a better one replaces the first */
            if (dms_rank(o->st[number]) >= rank_new) {
                log_msg("  DMS: track %u appears twice: the first copy is kept", number);
                continue;
            }
            n_stored--;
        }
        memcpy(img + (size_t)number * tsize, b2, tsize);
        o->st[number] = (uint8_t)stat_new;
        n_stored++;
        if (number > maxnum) maxnum = number;
        last_num = number;
    }

    for (t = 0; t < DMS_MAX_TRACKS; t++) {
        unsigned rk = dms_rank(o->st[t]);
        if (rk == 2) n_ver++;
        else if (rk == 1) n_kept++;
    }
    if (n_stored == 0 || (enc && n_ver == 0 && !g_dms_key_hit)) {
        /* nothing to show: no track, or an encrypted archive of which no track could be verified (decoded with a
         * wrong key it gives noise). If the search for the key was stopped by the time limit the archive is shown as
         * damaged, so that the report can say why */
        log_error(n_stored == 0 ? "DMS: no disk track found in the archive"
                                : "DMS: encrypted archive: no track could be decrypted");
        any_error = true;
        o->refuse = enc ? REFUSE_ENCRYPTED : REFUSE_NODISK;
        rc = -1;
    } else {
        /* a standard DD or HD disk is always written at its full size (80 cylinders), missing tracks are zero; any
         * other geometry is written up to its highest track */
        size_t imgsize = (tsize == DMS_TRACK_DD || tsize == DMS_TRACK_HD || tsize == DMS_TRACK_HALF)
                             ? (size_t)tmax * tsize
                             : (size_t)(maxnum + 1) * tsize;
        unsigned missing = 0;

        if (tsize == DMS_TRACK_DD || tsize == DMS_TRACK_HD) imgsize = (size_t)DMS_STD_TRACKS * tsize;
        for (t = 0; t <= maxnum; t++) if (dms_rank(o->st[t]) == 0) missing++;
        if (missing > 0)
            log_msg("  DMS: %u tracks of the disk are missing (written as zeros)", missing);
        o->image = img;
        img = NULL;
        o->image_size = imgsize;
        o->tsize = tsize;
        o->maxnum = maxnum;
        for (t = 0; t < tmax; t++) {
            switch (o->st[t]) {
            case DMT_OK:         o->n_ok++; break;
            case DMT_REPAIRED:   o->n_repaired++; break;
            case DMT_UNVERIFIED: o->n_unverified++; break;
            case DMT_BEST:       o->n_best++; break;
            case DMT_LOST:       o->n_lost++; break;
            default:            o->n_absent++; break;
            }
        }
    }
    if (n_shown > DMS_MAX_LOGGED)
        log_msg("  DMS: %u further messages not shown (only the first %d are listed)", n_shown - DMS_MAX_LOGGED,
                DMS_MAX_LOGGED);
    if (g_dms_budget_hit) {
        o->budget_hit = true;
        log_msg("  DMS: the search for repairs was stopped by the effort limit (%d trial unpacks or %d seconds)",
                DMS_REPAIR_BUDGET, DMS_REPAIR_SECONDS);
    }
    if (g_dms_bits_hit)
        log_msg("  DMS: the search for repairs of wrong bits was stopped by the limit of the archive (half of the time and of "
                "the trials of the repairs)");
    if (g_dms_bits_cut)
        log_msg("  DMS: in some track the search for repairs of wrong bits did not try every candidate: a class was too large "
                "for the limit of a track (%d trial unpacks, %d s)", DMS_BITS_TRIALS, DMS_BITS_SECONDS);
    if (g_dms_key_hit) {
        o->key_hit = true;
        log_msg("  DMS: the search for the decryption key was stopped by the time limit (%d seconds)", DMS_KEY_SECONDS);
    }
    o->any_error = any_error;
    if (n_kept)
        log_msg("  DMS: %u tracks extracted (%u of them not verified or with errors, kept as decoded), %u in error",
                n_ver + n_kept, n_kept, n_err);
    else
        log_msg("  DMS: %u tracks extracted, %u in error", n_ver, n_err);
    free(img);
    free(b1);
    free(b2);
    free(cand);
    free(keep);
    free(x);
    free(xs);
    free(xp);
    free(xr);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------- */
/* ADF file system reader (OFS/FFS) with the recovery of damaged disks                                    */
/* ---------------------------------------------------------------------------------------------------- */

#define ADF_BLOCK_SIZE  512
#define T_HEADER   2
#define T_DATA     8
#define T_LIST     16
#define T_DIRC     33                         /* directory cache block (DC-OFS / DC-FFS) */
#define ST_ROOT    1
#define ST_USERDIR 2
#define ST_FILE    (-3)
#define HT_SIZE    72
#define ADF_MAX_DEPTH 256
#define ADF_PATH_MAX  48                      /* levels of a path rebuilt for a recovered file */
#define read_be32 rd32

typedef struct { uint64_t key; uint32_t blk; } adf_oent_t;    /* OFS data block: key = header_key << 32 | seq_num */
typedef struct { uint32_t blk; bool strict; } adf_hdr_t;      /* header block found by the scan (strict: checksum right) */
typedef struct { uint32_t blk; uint8_t name[32]; size_t nlen; } adf_ent_t;

/* Recovery: the files and the data blocks that the directory tree does not reach (folder _RECOVERED) */
typedef struct { uint32_t dirblk; int32_t parent; uint8_t name[32]; size_t nlen; } adf_rnode_t;   /* node 0: _RECOVERED */
typedef struct { uint32_t blk; int32_t node; bool strict; } adf_rfile_t;
typedef struct { uint32_t hk; uint32_t maxseq; unsigned missing; uint64_t total; } adf_rdata_t;

typedef struct {
    uint8_t  *data;                 /* the image (owned by the caller) */
    uint64_t  size;
    uint32_t  nblk;
    uint32_t  root_blk;
    int       is_ffs;
    uint8_t  *visited;              /* header blocks reached by the walk of the whole disk */
    uint32_t *seen;                 /* cycle detection of one directory listing (stamp = gen) */
    uint32_t  gen;
    int       has_error;            /* block or chain error */
    int       quiet;                /* > 0: errors are noted but not written to the report (a repeated read) */
    bool      boot_ok;              /* the bootblock is "DOS" + flags 0-7 */
    bool      root_damaged;         /* root block with a wrong checksum, accepted */
    bool      rootless;             /* no usable root block: only the recovery can show files */
    adf_oent_t *oidx;               /* OFS data blocks sorted by (header, sequence): built when first needed */
    size_t    noidx;
    bool      oidx_done;
    adf_hdr_t *hf, *hd;             /* file headers and user directory headers found by the scan */
    size_t    nhf, nhd;
    bool      hscan_done;
    bool      strict_oidx;          /* index only the OFS data blocks whose checksum is right (no root block, no file header) */
    bool      headerless;           /* no root block and no file header: the files are rebuilt from the OFS data blocks */
    uint32_t  n_dup;                /* OFS data blocks left out of the index: same (header, sequence) as another block */
    /* statistics of the walk of the whole disk */
    uint32_t  n_files, n_dirs, n_part, n_lost;
    /* damaged blocks, by track (a track = a cylinder: 22 blocks of a DD disk, 44 of an HD disk) */
    uint32_t  per_cyl, ntracks;
    uint8_t  *badtrk;
    uint32_t  nbad;
    /* recovery */
    adf_rnode_t *rnode;  size_t nrnode;
    adf_rfile_t *rfile;  size_t nrfile;
    adf_rdata_t *rdata;  size_t nrdata;
} adf_img_t;

static void adf_free(adf_img_t *img)
{
    free(img->visited);
    free(img->seen);
    free(img->oidx);
    free(img->hf);
    free(img->hd);
    free(img->badtrk);
    free(img->rnode);
    free(img->rfile);
    free(img->rdata);
    memset(img, 0, sizeof(*img));
}

/* A structural error of the image: flags it and, unless the read is a repeat, writes it to the report */
static void adf_fail(adf_img_t *img, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void adf_fail(adf_img_t *img, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    img->has_error = 1;
    if (!img->quiet) log_error("ADF: %s", msg);
}

/* Marks the track of a damaged block */
static void adf_note_bad(adf_img_t *img, uint32_t blk)
{
    if (!img->badtrk || blk >= img->nblk) return;
    if (!img->quiet) img->nbad++;
    if (blk / img->per_cyl < img->ntracks) img->badtrk[blk / img->per_cyl] = 1;
}

/* Name of a header for a message: Latin-1 turned into printable ASCII */
static void adf_log_name(const uint8_t *n, size_t len, char *out, size_t cap)
{
    size_t i, k = 0;
    for (i = 0; i < len && k + 1 < cap; i++) out[k++] = (n[i] >= 32 && n[i] < 127) ? (char)n[i] : '?';
    out[k] = 0;
}


static const uint8_t *adf_block(const adf_img_t *img, uint32_t b)
{
    if (b == 0 || b >= img->nblk) return NULL;
    return img->data + (size_t)b * ADF_BLOCK_SIZE;
}

static uint32_t adf_sum(const uint8_t *blk)
{
    uint32_t sum = 0;
    int i;
    for (i = 0; i < 128; i++) sum += read_be32(blk + i * 4);
    return sum;
}

/* Valid root block (4.4 point 5): type 2, sec_type 1, sum of the 128 longs = 0 */
static bool adf_is_root_block(const uint8_t *blk)
{
    return read_be32(blk) == T_HEADER && read_be32(blk + 508) == ST_ROOT && adf_sum(blk) == 0;
}

/* Root block with a wrong checksum: type 2, sec_type 1 and at least one hash table entry that points
 * at a header block of the image (a damaged root of a disk that still has its files) */
static bool adf_root_plausible(const adf_img_t *img, uint32_t b)
{
    const uint8_t *blk = img->data + (size_t)b * ADF_BLOCK_SIZE;
    int h;
    if (read_be32(blk) != T_HEADER || read_be32(blk + 508) != ST_ROOT) return false;
    for (h = 0; h < HT_SIZE; h++) {
        uint32_t p = read_be32(blk + 24 + h * 4);
        if (p >= 2 && p < img->nblk && p != b && read_be32(img->data + (size_t)p * ADF_BLOCK_SIZE) == T_HEADER)
            return true;
    }
    return false;
}

/*
 * Finds the root block. A size that is not a multiple of 512 is accepted (the whole blocks are used), a
 * bootblock that is not "DOS" is accepted when the root block is valid (custom bootblocks of games and demos on
 * a disk that has a normal file system), and a root block whose checksum is wrong is accepted when its hash table
 * still points at header blocks. Returns 0 if the image is
 * handled as DOS; otherwise -1 and *why gives the reason (img->nblk is set whenever the image has 8 blocks).
 */
static int adf_find_geometry(adf_img_t *img, const char **why)
{
    uint32_t cand, best = UINT32_MAX, best_dist = UINT32_MAX, center, b, k;
    static const uint32_t std_root[2] = { 880, 1760 };      /* DD and HD: the root of a truncated image stays here */

    if (img->size < 8 * ADF_BLOCK_SIZE) { *why = "image below 8 blocks"; return -1; }
    if (img->size / ADF_BLOCK_SIZE > 0x7FFFFFFFu) { *why = "image too large"; return -1; }
    img->nblk = (uint32_t)(img->size / ADF_BLOCK_SIZE);
    img->boot_ok = img->data[0] == 'D' && img->data[1] == 'O' && img->data[2] == 'S' && img->data[3] <= 7;
    img->is_ffs = img->boot_ok ? (img->data[3] & 1) != 0 : 0;
    cand = (img->nblk + 1) / 2;
    if (adf_is_root_block(img->data + (size_t)cand * ADF_BLOCK_SIZE)) { img->root_blk = cand; return 0; }

    center = img->nblk / 2;
    for (b = 0; b < img->nblk; b++) {
        uint32_t dist;
        if (!adf_is_root_block(img->data + (size_t)b * ADF_BLOCK_SIZE)) continue;
        dist = b > center ? b - center : center - b;
        if (dist < best_dist) { best_dist = dist; best = b; }   /* on ties: lower block */
    }
    if (best != UINT32_MAX) { img->root_blk = best; return 0; }

    /* damaged root: the usual places first, then the nearest to the center */
    if (cand < img->nblk && adf_root_plausible(img, cand)) { img->root_blk = cand; img->root_damaged = true; return 0; }
    for (k = 0; k < 2; k++)
        if (std_root[k] < img->nblk && adf_root_plausible(img, std_root[k])) {
            img->root_blk = std_root[k];
            img->root_damaged = true;
            return 0;
        }
    for (b = 2; b < img->nblk; b++) {
        uint32_t dist;
        if (!adf_root_plausible(img, b)) continue;
        dist = b > center ? b - center : center - b;
        if (dist < best_dist) { best_dist = dist; best = b; }
    }
    if (best != UINT32_MAX) { img->root_blk = best; img->root_damaged = true; return 0; }
    *why = img->boot_ok ? "no valid root block" : "bootblock not recognized and no valid root block";
    return -1;
}

/* A header block (type 2) that points at itself: a file header (ST_FILE) or a user directory
 * (ST_USERDIR). Returns the sec_type, 0 if the block is neither. *strict: the checksum is right. */
static int adf_self_header(const adf_img_t *img, uint32_t b, bool *strict)
{
    const uint8_t *blk;
    int32_t st;
    if (b < 2 || b >= img->nblk) return 0;
    blk = img->data + (size_t)b * ADF_BLOCK_SIZE;
    if (read_be32(blk) != T_HEADER || read_be32(blk + 4) != b) return 0;
    st = (int32_t)read_be32(blk + 508);
    if (st != ST_FILE && st != ST_USERDIR) return 0;
    if (blk[432] == 0 || blk[432] > 30) return 0;
    if (st == ST_FILE && (read_be32(blk + 8) > HT_SIZE || read_be32(blk + 324) > img->size)) return 0;
    if (strict) *strict = adf_sum(blk) == 0;
    return st;
}

static void adf_hdr_push(adf_hdr_t **v, size_t *n, size_t *cap, uint32_t blk, bool strict)
{
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *v = (adf_hdr_t *)xrealloc(*v, *cap * sizeof(**v));
    }
    (*v)[*n].blk = blk;
    (*v)[*n].strict = strict;
    (*n)++;
}

/* One pass over the image: every file header and user directory header block (strict or not) */
static void adf_scan_headers(adf_img_t *img)
{
    uint32_t b;
    size_t capf = 0, capd = 0;
    if (img->hscan_done) return;
    img->hscan_done = true;
    for (b = 2; b < img->nblk; b++) {
        bool strict = false;
        int st;
        if (read_be32(img->data + (size_t)b * ADF_BLOCK_SIZE) != T_HEADER) continue;
        st = adf_self_header(img, b, &strict);
        if (st == ST_FILE) adf_hdr_push(&img->hf, &img->nhf, &capf, b, strict);
        else if (st == ST_USERDIR) adf_hdr_push(&img->hd, &img->nhd, &capd, b, strict);
    }
}

/*
 * OFS or FFS from the data. The first data block of an OFS file is a T_DATA block that names the file
 * header and has sequence number 1: an FFS block of file data does not look like that. A bootblock that says
 * FFS on a disk whose files have OFS blocks is read as OFS; without a DOS bootblock, no OFS block at all
 * means FFS.
 */
static void adf_detect_flavor(adf_img_t *img)
{
    size_t i;
    unsigned hits = 0, seen = 0;
    adf_scan_headers(img);
    for (i = 0; i < img->nhf; i++) {
        const uint8_t *h, *d;
        if (!img->hf[i].strict) continue;
        h = img->data + (size_t)img->hf[i].blk * ADF_BLOCK_SIZE;
        if (read_be32(h + 324) == 0) continue;
        d = adf_block(img, read_be32(h + 24 + (HT_SIZE - 1) * 4));
        if (!d) continue;
        seen++;
        if (read_be32(d) == T_DATA && read_be32(d + 4) == img->hf[i].blk && read_be32(d + 8) == 1) hits++;
    }
    if (hits > 0) {
        if (img->is_ffs) log_msg("  ADF: the bootblock says FFS but %u file(s) have OFS data blocks: read as OFS", hits);
        img->is_ffs = 0;
    } else if (seen > 0 && !img->boot_ok) {
        img->is_ffs = 1;
    }
}

static int adf_oent_cmp(const void *pa, const void *pb)
{
    const adf_oent_t *a = (const adf_oent_t *)pa, *b = (const adf_oent_t *)pb;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return (a->blk > b->blk) - (a->blk < b->blk);
}

/* Index of the OFS data blocks by (file header block, sequence number) */
static void adf_oidx_build(adf_img_t *img)
{
    uint32_t b;
    size_t cap = 0;
    if (img->oidx_done) return;
    img->oidx_done = true;
    for (b = 2; b < img->nblk; b++) {
        const uint8_t *blk = img->data + (size_t)b * ADF_BLOCK_SIZE;
        uint32_t hk, seq;
        if (read_be32(blk) != T_DATA) continue;
        if (img->strict_oidx && adf_sum(blk) != 0) continue;
        hk = read_be32(blk + 4);
        seq = read_be32(blk + 8);
        if (hk < 2 || hk >= img->nblk || seq == 0 || read_be32(blk + 12) > ADF_BLOCK_SIZE - 24) continue;
        if (img->noidx >= cap) {
            cap = cap ? cap * 2 : 256;
            img->oidx = (adf_oent_t *)xrealloc(img->oidx, cap * sizeof(*img->oidx));
        }
        img->oidx[img->noidx].key = ((uint64_t)hk << 32) | seq;
        img->oidx[img->noidx].blk = b;
        img->noidx++;
    }
    if (img->noidx > 1) {
        size_t i, j, k = 0;
        qsort(img->oidx, img->noidx, sizeof(*img->oidx), adf_oent_cmp);
        /* The same (header, sequence) in more than one block (an old copy that was never cleared, a copy of a block):
         * one entry stays, so that every lookup gives the same block. A block with a right checksum goes before one
         * with a wrong checksum; among equals the lowest block number wins (the blocks of a key are in that order). */
        for (i = 0; i < img->noidx; i = j) {
            size_t best = i;
            j = i + 1;
            while (j < img->noidx && img->oidx[j].key == img->oidx[i].key) j++;
            if (j - i > 1) {
                size_t m;
                for (m = i; m < j; m++)
                    if (adf_sum(img->data + (size_t)img->oidx[m].blk * ADF_BLOCK_SIZE) == 0) {
                        best = m;
                        break;
                    }
                img->n_dup += (uint32_t)(j - i - 1);
            }
            img->oidx[k++] = img->oidx[best];
        }
        img->noidx = k;
    }
}

/* OFS data blocks with a right checksum and a plausible (file header, sequence number). A disk that has neither a root
 * block nor any file header block but still has them holds files that can be rebuilt from their data blocks */
static unsigned adf_valid_data_blocks(const adf_img_t *img)
{
    uint32_t b;
    unsigned n = 0;
    for (b = 2; b < img->nblk; b++) {
        const uint8_t *blk = img->data + (size_t)b * ADF_BLOCK_SIZE;
        uint32_t hk, seq;
        if (read_be32(blk) != T_DATA || adf_sum(blk) != 0) continue;
        hk = read_be32(blk + 4);
        seq = read_be32(blk + 8);
        if (hk < 2 || hk >= img->nblk || seq == 0 || seq > img->nblk || read_be32(blk + 12) > ADF_BLOCK_SIZE - 24) continue;
        n++;
    }
    return n;
}

/* first entry with key >= `key` (index in oidx), noidx if none */
static size_t adf_oidx_lower(const adf_img_t *img, uint64_t key)
{
    size_t lo = 0, hi = img->noidx;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (img->oidx[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* The data block number `seq` (from 1) of the OFS file whose header is `hk`, or 0 */
static uint32_t adf_oidx_find(adf_img_t *img, uint32_t hk, uint32_t seq)
{
    uint64_t key = ((uint64_t)hk << 32) | seq;
    size_t i;
    if (img->is_ffs) return 0;
    adf_oidx_build(img);
    i = adf_oidx_lower(img, key);
    return (i < img->noidx && img->oidx[i].key == key) ? img->oidx[i].blk : 0;
}

/* Size of an OFS file from its data blocks: (highest sequence - 1) * 488 + the size of that block; 0 if none */
static uint64_t adf_oidx_size(adf_img_t *img, uint32_t hk)
{
    size_t i;
    const adf_oent_t *last = NULL;
    if (img->is_ffs) return 0;
    adf_oidx_build(img);
    for (i = adf_oidx_lower(img, ((uint64_t)hk << 32) | 1); i < img->noidx && (img->oidx[i].key >> 32) == hk; i++)
        last = &img->oidx[i];
    if (!last) return 0;
    return ((uint64_t)(last->key & 0xFFFFFFFFu) - 1) * (ADF_BLOCK_SIZE - 24) +
           read_be32(img->data + (size_t)last->blk * ADF_BLOCK_SIZE + 12);
}

/* How a data block of an OFS file turned out (adf_ofs_pick) */
#define OB_INDEX    0x01u   /* not the block that the table names: found through the index (header, sequence) */
#define OB_SUMBAD   0x02u   /* wrong checksum: the data are used, but nothing says that they are right */
#define OB_SHORT    0x04u   /* the block holds fewer bytes than the file needs at this position: the rest is zero */
#define OB_HDRBAD   0x08u   /* damaged header: the body is used as it is */
#define OB_FOREIGN  0x10u   /* (not used) a right data block of another file or of another position */
#define OB_UNSURE   (OB_SUMBAD | OB_SHORT | OB_HDRBAD)   /* kept, but the file can no longer be called right */

/*
 * How well block `d` can stand for data block number `seq` (from 1) of the OFS file whose header is `hdr_blk`, which
 * needs `expect` bytes there. 4: verified (it names this file and this position, its checksum is right and it holds all
 * the bytes); 3: right but too short; 2: the checksum is wrong; 1: the header is damaged, the body is used as it is;
 * 0: not usable. *len is how many bytes to copy and *st the OB_* flag that goes with the grade.
 * A data block that names another file or position is used only if its checksum is wrong, which is how a damaged
 * header looks. With a right checksum it is a right block of something else, and it is never copied into this file.
 */
static int adf_ofs_grade(const uint8_t *d, uint32_t hdr_blk, uint32_t seq, size_t expect, size_t *len, unsigned *st)
{
    uint32_t t = read_be32(d);
    size_t i;

    *len = 0;
    *st = 0;
    if (t == T_DATA) {
        uint32_t dsz = read_be32(d + 12);
        bool sum_ok = adf_sum(d) == 0;
        if (read_be32(d + 4) != hdr_blk || read_be32(d + 8) != seq) {
            if (sum_ok) {
                *st = OB_FOREIGN;
                return 0;
            }
            *st = OB_HDRBAD;
            *len = expect;
            return 1;
        }
        if (dsz > ADF_BLOCK_SIZE - 24) {
            *st = OB_HDRBAD;
            *len = expect;
            return 1;
        }
        *len = dsz < expect ? dsz : expect;
        if (!sum_ok) {
            *st = OB_SUMBAD;
            return 2;
        }
        if (dsz < expect) {
            *st = OB_SHORT;
            return 3;
        }
        return 4;
    }
    if (t == T_HEADER || t == T_LIST || t == T_DIRC) return 0;
    for (i = 24; i < ADF_BLOCK_SIZE; i++)
        if (d[i]) {                      /* no structure of the file system, but data: only its header is damaged */
            *st = OB_HDRBAD;
            *len = expect;
            return 1;
        }
    return 0;
}

/*
 * One data block of an OFS file: `db` is what the block table says (0 if the table is lost), `seq` the position (from 1)
 * in the file, `expect` the bytes that the file needs there. The block of the table is used if it is verified. Else the
 * block that names this file and this position (adf_oidx_find) is used if it is better. Else the table's block is used
 * as well as it can be (OB_SUMBAD, OB_SHORT, OB_HDRBAD in *st: the file is then "incomplete or unverified"), unless it is
 * a right block of another file (OB_FOREIGN) or no data block at all.
 * Returns 0 (src and len are set, *blk is the block used, *st how it turned out) or -1 (nothing can be used).
 */
static int adf_ofs_pick(adf_img_t *img, uint32_t hdr_blk, uint32_t seq, uint32_t db, size_t expect,
                        const uint8_t **src, size_t *len, unsigned *st, uint32_t *blk)
{
    const uint8_t *d = adf_block(img, db), *c = NULL;
    uint32_t ib;
    size_t dlen = 0, clen = 0;
    unsigned dst = 0, cst = 0;
    int dq = 0, cq = 0;

    *src = NULL;
    *len = 0;
    *st = 0;
    *blk = db;
    if (d) {
        dq = adf_ofs_grade(d, hdr_blk, seq, expect, &dlen, &dst);
        if (dq == 4) {                                   /* the usual case */
            *src = d + 24;
            *len = dlen;
            return 0;
        }
    }
    ib = adf_oidx_find(img, hdr_blk, seq);
    if (ib != 0 && ib != db && (c = adf_block(img, ib)) != NULL) cq = adf_ofs_grade(c, hdr_blk, seq, expect, &clen, &cst);
    if (cq > dq) {
        *src = c + 24;
        *len = clen;
        *st = cst | OB_INDEX;
        *blk = ib;
        return 0;
    }
    if (dq > 0) {
        *src = d + 24;
        *len = dlen;
        *st = dst;
        return 0;
    }
    *st = dst;
    return -1;
}

/*
 * Contents of a file. The data block table is stored backwards: the i-th
 * block (from 0) of the header/extension block sits in slot (71 - i), i.e.
 * at offset 24 + (71 - i) * 4; extension blocks (T_LIST) are chained from
 * offset 504. Every block is placed at its position (i * block size, as the
 * file system does). A block that cannot be read is replaced by zeros of the
 * length it should have had, and the file is reported as incomplete; the file
 * is deleted only if no block is readable. In an OFS file every data block is
 * verified (adf_ofs_pick): the blocks that the table cannot give or that fail the
 * checks (broken chain, damaged block header, block of another file, wrong checksum,
 * too few bytes) are looked up by (header, sequence number), and a size field
 * that makes no sense is taken from the data blocks.
 * What is kept although it is not verified (a data block with a wrong checksum, a
 * damaged header or too few bytes; a header or an extension block with a wrong
 * checksum) makes the file incomplete too: *bad_out counts those blocks.
 * Returns 0 (with *out malloc'ed, also for an empty file), 1 (incomplete or
 * unverified) or -1. out == NULL: a dry run that only reports the state of the file.
 */
static int adf_file_data(adf_img_t *img, uint32_t hdr_blk, const char *nm,
                         uint8_t **out, size_t *out_size, unsigned *bad_out)
{
    const uint8_t *hdr = adf_block(img, hdr_blk), *cur;
    uint32_t byte_size, per_blk, need, i, got = 0, ext_steps = 0, *ptr;
    uint8_t *buf;
    unsigned bad = 0, good = 0, found = 0;
    bool noted = false;

    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    *bad_out = 0;
    if (adf_sum(hdr) != 0) {                            /* kept (best effort), but the table and the size are not verified */
        adf_fail(img, "file '%s': header block %u has a wrong checksum: its data are used but not verified", nm,
                 hdr_blk);
        adf_note_bad(img, hdr_blk);
        noted = true;
        bad++;
    }
    byte_size = read_be32(hdr + 324);
    if ((uint64_t)byte_size > img->size) {
        uint64_t sz = adf_oidx_size(img, hdr_blk);
        if (sz == 0 || sz > img->size) {
            adf_fail(img, "file '%s': size %u inconsistent with the image", nm, byte_size);
            return -1;
        }
        adf_fail(img, "file '%s': size %u inconsistent with the image: %llu bytes from the data blocks", nm,
                 byte_size, (unsigned long long)sz);
        noted = true;
        byte_size = (uint32_t)sz;
        bad++;
    }
    per_blk = img->is_ffs ? ADF_BLOCK_SIZE : ADF_BLOCK_SIZE - 24;
    need = (uint32_t)(((uint64_t)byte_size + per_blk - 1) / per_blk);
    if (need == 0) {
        if (read_be32(hdr + 8) != 0) {
            adf_fail(img, "file '%s': size 0 but data blocks present", nm);
            return -1;
        }
        if (out) *out = (uint8_t *)xmalloc(1);
        *bad_out = bad;
        return bad ? 1 : 0;
    }
#define ADF_NOTE(...)                                                               \
    do {                                                                            \
        if (!noted) adf_fail(img, __VA_ARGS__); else img->has_error = 1;            \
        noted = true;                                                               \
    } while (0)
    ptr = (uint32_t *)xcalloc(need, sizeof(uint32_t));        /* the block numbers the table gives (0: unknown) */
    cur = hdr;
    for (;;) {
        uint32_t high_seq = read_be32(cur + 8), take, ext;
        if (high_seq > HT_SIZE) {
            ADF_NOTE("file '%s': high_seq %u out of range", nm, high_seq);
            break;
        }
        take = need - got < high_seq ? need - got : high_seq;
        for (i = 0; i < take; i++) ptr[got++] = read_be32(cur + 24 + (HT_SIZE - 1 - i) * 4);
        if (got >= need) break;
        ext = read_be32(cur + 504);
        if (ext == 0) {
            ADF_NOTE("file '%s': block chain too short (%u of %u)", nm, got, need);
            break;
        }
        cur = adf_block(img, ext);
        if (!cur) {
            ADF_NOTE("file '%s': extension block %u outside the image", nm, ext);
            break;
        }
        if (read_be32(cur) != T_LIST) {
            ADF_NOTE("file '%s': block %u is not an extension block", nm, ext);
            break;
        }
        if (adf_sum(cur) != 0) {
            ADF_NOTE("file '%s': extension block %u has a wrong checksum: its data are used but not verified", nm, ext);
            adf_note_bad(img, ext);
            bad++;
        }
        if (++ext_steps > img->nblk) {
            ADF_NOTE("file '%s': cycle in the block chain", nm);
            break;
        }
    }
    buf = out ? (uint8_t *)xcalloc(byte_size, 1) : NULL;        /* out == NULL: only check the file (dry run) */
    for (i = 0; i < need; i++) {
        size_t off = (size_t)i * per_blk, left = (size_t)byte_size - off;
        size_t expect = left < per_blk ? left : per_blk, len = 0;
        uint32_t db = i < got ? ptr[i] : 0;
        const uint8_t *d = adf_block(img, db), *src = NULL;
        unsigned st = 0;
        uint32_t ub = db;

        if (img->is_ffs) {
            if (d) {
                if (buf) memcpy(buf + off, d, expect);
                good++;
            } else {
                if (i < got) ADF_NOTE("file '%s': data block pointer %u outside the image", nm, db);
                bad++;                                  /* i >= got: the chain error is already reported */
            }
            continue;
        }
        if (adf_ofs_pick(img, hdr_blk, i + 1, db, expect, &src, &len, &st, &ub) < 0) {
            if (i < got) {
                if (!d) {
                    ADF_NOTE("file '%s': data block pointer %u outside the image", nm, db);
                } else {
                    if (st & OB_FOREIGN)
                        ADF_NOTE("file '%s': block %u belongs to another file or position: it is not used", nm, db);
                    else
                        ADF_NOTE("file '%s': block %u is not a data block", nm, db);
                    adf_note_bad(img, db);
                }
            }
            bad++;
            continue;
        }
        if (buf) memcpy(buf + off, src, len);
        good++;
        if (st & OB_INDEX) found++;
        if (st & OB_UNSURE) {                           /* kept, but not verified: the file is not "right" */
            if (st & OB_SUMBAD)
                ADF_NOTE("file '%s': block %u has a wrong checksum: its data are used but not verified", nm, ub);
            else if (st & OB_SHORT)
                ADF_NOTE("file '%s': block %u holds %u of the %u bytes that the file needs there: the rest is zero", nm,
                         ub, (unsigned)len, (unsigned)expect);
            else
                ADF_NOTE("file '%s': block %u has a damaged header: its data are used as they are", nm, ub);
            adf_note_bad(img, ub);
            bad++;
        }
    }
    if (found) {
        img->has_error = 1;
        if (!img->quiet)
            log_msg("  ADF: file '%s': %u block(s) found through the data block index (header and sequence number)",
                    nm, found);
    }
#undef ADF_NOTE
    free(ptr);
    if (good == 0) {                                    /* nothing readable: the file is lost */
        free(buf);
        return -1;
    }
    if (out) *out = buf;
    if (out_size) *out_size = byte_size;
    *bad_out = bad;
    return bad ? 1 : 0;
}

/* ---- directories ---------------------------------------------------------------------------------- */

/*
 * The entries of directory dir_blk: block and name, from the hash table and its chains. A pointer outside the
 * image, a block that is not a header and a cycle end that chain and are noted; the other chains go on.
 * mark: the header blocks are also marked as reached (the walk of the whole disk, which later tells what the tree
 * did not reach); otherwise a cycle is detected within this listing only.
 */
static void adf_dir_read(adf_img_t *img, uint32_t dir_blk, bool mark, adf_ent_t **out, size_t *count)
{
    const uint8_t *hdr = adf_block(img, dir_blk);
    adf_ent_t *list = NULL;
    size_t n = 0, cap = 0;
    uint32_t stamp = ++img->gen;
    int h;

    *out = NULL;
    *count = 0;
    if (!hdr) return;
    for (h = 0; h < HT_SIZE; h++) {
        uint32_t b = read_be32(hdr + 24 + h * 4);
        while (b != 0) {
            const uint8_t *eh = adf_block(img, b);
            size_t nlen;
            if (!eh) {
                adf_fail(img, "block pointer %u outside the image", b);
                break;
            }
            if (mark ? img->visited[b] != 0 : img->seen[b] == stamp) {
                adf_fail(img, "cycle in the block chains (block %u)", b);
                adf_note_bad(img, b);
                break;
            }
            if (mark) img->visited[b] = 1; else img->seen[b] = stamp;
            if (read_be32(eh) != T_HEADER) {
                adf_fail(img, "block %u is not a header block (type %u)", b, read_be32(eh));
                adf_note_bad(img, b);
                break;
            }
            /* a header with a wrong checksum is kept (best effort) but flagged; a file says so itself (adf_file_data) */
            if ((int32_t)read_be32(eh + 508) != ST_FILE && adf_sum(eh) != 0) {
                adf_fail(img, "header block %u has a wrong checksum", b);
                adf_note_bad(img, b);
            }
            if (n >= cap) {
                cap = cap ? cap * 2 : 32;
                list = (adf_ent_t *)xrealloc(list, cap * sizeof(*list));
            }
            list[n].blk = b;
            nlen = eh[432];
            if (nlen > 30) nlen = 30;
            memcpy(list[n].name, eh + 433, nlen);
            list[n].nlen = nlen;
            n++;
            b = read_be32(eh + 496);                 /* hash chain */
        }
    }
    *out = list;
    *count = n;
}

/* The walk of the whole disk: reaches every header, reads every file in a dry run (nothing is copied) and so finds
 * out whether the disk has errors; the headers that it marked are the ones that the tree reaches. */
static void adf_walk_dir(adf_img_t *img, uint32_t dir_blk, int depth)
{
    adf_ent_t *list;
    size_t n, a;

    if (depth > ADF_MAX_DEPTH) {
        adf_fail(img, "directories nested too deeply");
        return;
    }
    adf_dir_read(img, dir_blk, true, &list, &n);
    for (a = 0; a < n; a++) {
        const uint8_t *eh = adf_block(img, list[a].blk);
        int32_t st = (int32_t)read_be32(eh + 508);
        char nm[48];

        if (st != ST_USERDIR && st != ST_FILE) continue;    /* links: not shown */
        adf_log_name(list[a].name, list[a].nlen, nm, sizeof(nm));
        if (st == ST_USERDIR) {
            img->n_dirs++;
            adf_walk_dir(img, list[a].blk, depth + 1);
        } else {
            unsigned bad = 0;
            int rc = adf_file_data(img, list[a].blk, nm, NULL, NULL, &bad);
            if (rc < 0) img->n_lost++;
            else if (rc == 1) img->n_part++;
            else img->n_files++;
        }
    }
    free(list);
}

/* A directory of the disk for the panel: the entries with their kind, size and state. */
typedef struct {
    uint32_t blk;
    bool     is_dir;
    uint8_t  name[32];
    size_t   nlen;
    uint32_t size;
    unsigned flags;                 /* AI_* */
    uint32_t days, mins, ticks;     /* date of the header block: days since 1978-01-01, minutes since midnight, 1/50 s */
    bool     has_date;              /* the three fields above are plausible */
} adf_item_t;

/* The date of a file or directory header block (the last modification, at BSIZE-92: days, minutes, ticks).
 * Plausible: minutes < 1440, ticks < 3000, days within 400 years of 1978 (a damaged header gives no date). */
static bool adf_header_date(const uint8_t *h, uint32_t *days, uint32_t *mins, uint32_t *ticks)
{
    *days  = read_be32(h + 420);
    *mins  = read_be32(h + 424);
    *ticks = read_be32(h + 428);
    return *days < 146000u && *mins < 1440u && *ticks < 3000u;
}
#define AI_PARTIAL  1               /* incomplete: some blocks cannot be read */
#define AI_LOST     2               /* nothing of the file can be read */
#define AI_SIZEFIX  4               /* the size field was wrong: the size comes from the data blocks */

static void adf_list_dir(adf_img_t *img, uint32_t dir_blk, adf_item_t **out, size_t *count)
{
    adf_ent_t *list;
    adf_item_t *it = NULL;
    size_t n, a, k = 0;

    img->quiet++;
    adf_dir_read(img, dir_blk, false, &list, &n);
    if (n) it = (adf_item_t *)xcalloc(n, sizeof(*it));
    for (a = 0; a < n; a++) {
        const uint8_t *eh = adf_block(img, list[a].blk);
        int32_t st = (int32_t)read_be32(eh + 508);
        char nm[48];
        adf_item_t *p;

        if (st != ST_USERDIR && st != ST_FILE) continue;
        p = &it[k++];
        p->blk = list[a].blk;
        p->is_dir = st == ST_USERDIR;
        memcpy(p->name, list[a].name, list[a].nlen);
        p->nlen = list[a].nlen;
        p->has_date = adf_header_date(eh, &p->days, &p->mins, &p->ticks);
        if (!p->is_dir) {
            unsigned bad = 0;
            uint32_t bs = read_be32(eh + 324);
            int rc;
            adf_log_name(list[a].name, list[a].nlen, nm, sizeof(nm));
            if ((uint64_t)bs > img->size) {
                uint64_t sz = adf_oidx_size(img, list[a].blk);
                p->size = sz && sz <= img->size ? (uint32_t)sz : 0;
                p->flags |= AI_SIZEFIX;
            } else {
                p->size = bs;
            }
            rc = adf_file_data(img, list[a].blk, nm, NULL, NULL, &bad);
            if (rc < 0) p->flags |= AI_LOST;
            else if (rc == 1) p->flags |= AI_PARTIAL;
        }
    }
    free(list);
    img->quiet--;
    *out = it;
    *count = k;
}

/* ---- recovery of what the tree does not reach ---------------------------------------------------- */

static int32_t adf_rnode_find(adf_img_t *img, size_t *cap, int32_t parent, uint32_t dirblk)
{
    size_t i;
    for (i = 0; i < img->nrnode; i++)
        if (img->rnode[i].parent == parent && img->rnode[i].dirblk == dirblk) return (int32_t)i;
    if (img->nrnode >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        img->rnode = (adf_rnode_t *)xrealloc(img->rnode, *cap * sizeof(*img->rnode));
    }
    memset(&img->rnode[img->nrnode], 0, sizeof(img->rnode[0]));
    img->rnode[img->nrnode].parent = parent;
    img->rnode[img->nrnode].dirblk = dirblk;
    if (dirblk) {
        const uint8_t *dh = img->data + (size_t)dirblk * ADF_BLOCK_SIZE;
        size_t nl = dh[432] > 30 ? 30 : dh[432];
        memcpy(img->rnode[img->nrnode].name, dh + 433, nl);
        img->rnode[img->nrnode].nlen = nl;
    }
    return (int32_t)img->nrnode++;
}

/*
 * Recovery of what the tree does not reach. Run after a walk with errors, or when there is no usable root block:
 *   - every file header (self-pointing header block, plausible fields) that the walk did not reach is a file; its
 *     path is rebuilt from the parent pointers through the directory headers that are still there;
 *   - in an OFS disk, the data blocks of a file whose header is lost, grouped by header_key and ordered by sequence
 *     number, are a file (DATA_<block>.bin); missing sequence numbers are zeros.
 * A disk where a file was deleted gives such files back too: a lost file cannot be told from a deleted one.
 */
static void adf_recovery_build(adf_img_t *img)
{
    size_t i, capn = 0, capf = 0, capd = 0;

    adf_scan_headers(img);
    adf_rnode_find(img, &capn, -1, 0);                     /* node 0 */
    for (i = 0; i < img->nhf; i++) {
        uint32_t blk = img->hf[i].blk, p, guard;
        const uint8_t *h = img->data + (size_t)blk * ADF_BLOCK_SIZE;
        uint32_t chain[ADF_PATH_MAX];
        size_t nd = 0, k;
        int32_t node = 0;

        if (img->visited && img->visited[blk]) continue;
        if (img->visited) img->visited[blk] = 1;
        p = read_be32(h + 500);
        for (guard = 0; p != 0 && p != img->root_blk && nd < ADF_PATH_MAX && guard < ADF_PATH_MAX; guard++) {
            if (adf_self_header(img, p, NULL) != ST_USERDIR) break;
            chain[nd++] = p;
            p = read_be32(img->data + (size_t)p * ADF_BLOCK_SIZE + 500);
        }
        for (k = nd; k-- > 0; ) node = adf_rnode_find(img, &capn, node, chain[k]);
        if (img->nrfile >= capf) {
            capf = capf ? capf * 2 : 32;
            img->rfile = (adf_rfile_t *)xrealloc(img->rfile, capf * sizeof(*img->rfile));
        }
        img->rfile[img->nrfile].blk = blk;
        img->rfile[img->nrfile].node = node;
        img->rfile[img->nrfile].strict = img->hf[i].strict;
        img->nrfile++;
    }
    if (!img->is_ffs) {
        size_t a, e, j;
        adf_oidx_build(img);
        for (a = 0; a < img->noidx; a = e) {
            uint32_t hk = (uint32_t)(img->oidx[a].key >> 32), seq, maxseq = 0, last = 0, want = 1;
            unsigned missing = 0;
            uint64_t total;
            for (e = a; e < img->noidx && (uint32_t)(img->oidx[e].key >> 32) == hk; e++) {
                seq = (uint32_t)(img->oidx[e].key & 0xFFFFFFFFu);
                if (seq >= maxseq) { maxseq = seq; last = img->oidx[e].blk; }
            }
            if (adf_self_header(img, hk, NULL) == ST_FILE) continue;        /* its header is there: handled above */
            if (maxseq > img->nblk) continue;
            total = (uint64_t)(maxseq - 1) * (ADF_BLOCK_SIZE - 24) +
                    read_be32(img->data + (size_t)last * ADF_BLOCK_SIZE + 12);
            if (total == 0 || total > img->size) continue;
            for (j = a; j < e; j++) {
                uint32_t sq = (uint32_t)(img->oidx[j].key & 0xFFFFFFFFu);
                if (sq > want) missing += sq - want;
                if (sq >= want) want = sq + 1;
            }
            if (img->nrdata >= capd) {
                capd = capd ? capd * 2 : 16;
                img->rdata = (adf_rdata_t *)xrealloc(img->rdata, capd * sizeof(*img->rdata));
            }
            img->rdata[img->nrdata].hk = hk;
            img->rdata[img->nrdata].maxseq = maxseq;
            img->rdata[img->nrdata].missing = missing;
            img->rdata[img->nrdata].total = total;
            img->nrdata++;
        }
    }
    if (img->nrfile || img->nrdata)
        log_msg("  ADF: %llu file(s) and %llu group(s) of data blocks recovered outside the directory tree "
                "(folder _RECOVERED)", (unsigned long long)img->nrfile, (unsigned long long)img->nrdata);
}

/* The data of a headerless group of OFS data blocks: the blocks in sequence order, missing ones are zeros */
static uint8_t *adf_group_data(adf_img_t *img, const adf_rdata_t *g, size_t *size)
{
    uint8_t *buf = (uint8_t *)xcalloc((size_t)g->total, 1);
    size_t j = adf_oidx_lower(img, ((uint64_t)g->hk << 32) | 1);
    for (; j < img->noidx && (uint32_t)(img->oidx[j].key >> 32) == g->hk; j++) {
        uint32_t sq = (uint32_t)(img->oidx[j].key & 0xFFFFFFFFu);
        const uint8_t *d = img->data + (size_t)img->oidx[j].blk * ADF_BLOCK_SIZE;
        size_t off = (size_t)(sq - 1) * (ADF_BLOCK_SIZE - 24), nn = read_be32(d + 12);
        if (off >= g->total) continue;
        if (off + nn > g->total) nn = (size_t)g->total - off;
        memcpy(buf + off, d + 24, nn);
    }
    *size = (size_t)g->total;
    return buf;
}

/* ---- opening ---------------------------------------------------------------------------------------- */

/*
 * Opens the image: geometry (a size that is not a multiple of 512, a bootblock that is not DOS with a valid root, a
 * root with a wrong checksum, no root at all), the OFS/FFS flavor read from the data, the walk of the whole disk
 * and, if it had errors or there is no root, the recovery. A disk with neither a root block nor a file header is
 * still opened when it has OFS data blocks with a right checksum: the recovery rebuilds its files from them.
 * Returns 0 if the image can be shown, -1 if it is not a DOS disk at all (img is then freed).
 */
static int adf_open(adf_img_t *img, uint8_t *data, uint64_t size)
{
    const char *why = "";
    bool rootless;

    memset(img, 0, sizeof(*img));
    img->data = data;
    img->size = size;
    rootless = adf_find_geometry(img, &why) != 0;
    if (rootless && img->nblk >= 8) adf_scan_headers(img);
    if (rootless && img->nhf == 0 && img->nblk >= 8 && adf_valid_data_blocks(img) >= 2) {
        img->is_ffs = 0;
        img->strict_oidx = true;
        img->headerless = true;
    }
    if (rootless && img->nhf == 0 && !img->headerless) {
        log_error("ADF: not a DOS disk: %s", why);
        adf_free(img);
        return -1;
    }
    adf_detect_flavor(img);
    img->rootless = rootless;
    img->per_cyl = img->nblk >= 2900 ? 44 : 22;
    img->ntracks = (img->nblk + img->per_cyl - 1) / img->per_cyl;
    img->badtrk = (uint8_t *)xcalloc(img->ntracks ? img->ntracks : 1, 1);
    img->visited = (uint8_t *)xcalloc(img->nblk, 1);
    img->seen = (uint32_t *)xcalloc(img->nblk, sizeof(uint32_t));
    if (img->headerless)
        log_msg("  ADF without a usable root block and without file headers (%s): files rebuilt from the OFS data blocks",
                why);
    else if (rootless)
        log_msg("  ADF without a usable root block (%s): %llu file header block(s) found, %s", why, (unsigned long long)img->nhf,
                img->is_ffs ? "FFS" : "OFS");
    else
        log_msg("  ADF DOS recognized: %u blocks, root %u, %s%s%s", img->nblk, img->root_blk,
                img->is_ffs ? "FFS" : "OFS", img->boot_ok ? "" : " (bootblock not DOS)",
                img->root_damaged ? " (root block checksum wrong)" : "");
    if (!rootless) {
        img->visited[img->root_blk] = 1;
        if (img->root_damaged) {
            img->has_error = 1;
            adf_note_bad(img, img->root_blk);
        }
        adf_walk_dir(img, img->root_blk, 0);
    }
    if (rootless || img->has_error) adf_recovery_build(img);
    if (img->headerless && img->nrdata == 0) {         /* nothing could be rebuilt */
        log_error("ADF: not a DOS disk: %s", why);
        adf_free(img);
        return -1;
    }
    if (img->n_dup)
        log_msg("  ADF: %u OFS data block(s) name the same file and position as another block: the one with a right "
                "checksum is used, else the one with the lowest block number", img->n_dup);
    return 0;
}


/* ====================================================================================================== */
/* FAR MANAGER PLUG-IN                                                                                      */
/* ====================================================================================================== */
#ifndef ADF_CORE_ONLY

/* ==================== GUID & Constants ==================== */

/* The item identifiers are packed into the user data pointer of the panel item: kind in the top bits (bits 28-30),
 * id in the low 28 bits (a block number is below 2^20, MAX_FILE_SIZE / 512). Works for 32 and 64 bit builds. */
typedef char adf_plugin_needs_32bit_pointers[sizeof(void *) >= 4 ? 1 : -1];

static const GUID MainGuid =
{ 0xb1ad9402, 0xadbd, 0x4216, { 0xaf, 0x3c, 0x60, 0x61, 0x13, 0x37, 0xc4, 0xa8 } };   /* B1AD9402-ADBD-4216-AF3C-60611337C4A8 */

#define PLUGIN_NAME   L"ADFDMSView"
#define PLUGIN_DESC   L"Amiga ADF & DMS disk images, with recovery of damaged disks"
#define PLUGIN_AUTHOR L"Marco Pistella"

#define MAX_NAME       64
#define MAX_PATH_DEPTH 64
#define MAX_TRACKS     160
#define MAX_INFO_LINES 20                       /* the panel uses at most 17 */
#define MAX_FILE_SIZE  (512u * 1024u * 1024u)   /* an image or an archive bigger than this is not opened */

#define NAME_RECOVERED L"_RECOVERED"
#define NAME_REPORT    L"_RECOVERY_REPORT.txt"

/* ==================== Data Structures ==================== */

enum {                              /* what a panel item is */
    EK_FILE = 0,                    /* a file of the disk */
    EK_DIR,                         /* a directory of the disk */
    EK_RECDIR,                      /* a directory of _RECOVERED (Id: node; 0 is _RECOVERED itself) */
    EK_RECFILE,                     /* a file whose header is not reached by the tree (Id: header block) */
    EK_RECDATA,                     /* data blocks of a file whose header is lost (Id: index of the group) */
    EK_IMAGE,                       /* the disk image unpacked from a DMS archive (<name>.adf) */
    EK_REPORT                       /* _RECOVERY_REPORT.txt */
};

typedef struct {
    wchar_t  Name[MAX_NAME];
    DWORD    Size;
    DWORD    Id;
    int      Kind;
    unsigned Flags;                 /* AI_* */
    FILETIME Time;                  /* date of the Amiga file or directory, as a UTC FILETIME */
    int      HasTime;               /* Time is valid */
} ADFEntry;

/* Amiga date (local time: days since 1978-01-01, minutes since midnight, ticks of 1/50 s) -> UTC FILETIME.
 * The Amiga has no time zone: the clock value is taken as local time, so that Windows shows the same value. */
static void amiga_to_filetime(uint32_t days, uint32_t mins, uint32_t ticks, FILETIME *out)
{
    ULARGE_INTEGER u;
    FILETIME local;
    SYSTEMTIME st, utc;

    /* 137696 days from 1601-01-01 (the FILETIME epoch) to 1978-01-01 */
    u.QuadPart = ((ULONGLONG)(days + 137696u) * 86400ull + (ULONGLONG)mins * 60ull) * 10000000ull;
    local.dwLowDateTime  = u.LowPart;
    local.dwHighDateTime = u.HighPart;
    if (FileTimeToSystemTime(&local, &st)) {
        st.wSecond       = (WORD)(ticks / 50u);
        st.wMilliseconds = (WORD)((ticks % 50u) * 20u);
        if (TzSpecificLocalTimeToSystemTime(NULL, &st, &utc) && SystemTimeToFileTime(&utc, out)) return;   /* DST of that date */
        if (SystemTimeToFileTime(&st, &local) && LocalFileTimeToFileTime(&local, out)) return;
    }
    out->dwLowDateTime = out->dwHighDateTime = 0;
}

static void entry_set_time(ADFEntry *e, const uint8_t *hdr)
{
    uint32_t d, m, t;
    if (hdr && adf_header_date(hdr, &d, &m, &t)) {
        amiga_to_filetime(d, m, t, &e->Time);
        e->HasTime = e->Time.dwLowDateTime || e->Time.dwHighDateTime;
    }
}

typedef struct {
    int      Kind;                  /* EK_DIR or EK_RECDIR (the root is EK_DIR with the root block) */
    DWORD    Id;
    wchar_t  Name[MAX_NAME];
} DirStackItem;

typedef struct {
    wchar_t      FileName[MAX_PATH];
    BYTE        *Image;             /* the disk image (for a DMS archive: the unpacked one) */
    size_t       ImageSize;
    int          IsDMS;
    DmsOut       Dms;
    adf_img_t    Adf;
    int          AdfOk;             /* the image could be read as an Amiga DOS disk (also with recovery) */
    CoreLog      Log;               /* what the readers found and repaired */
    int          HasReport;
    int          Damaged;           /* the image or the archive had damage (reported by the title) */
    char        *Report;            /* text of _RECOVERY_REPORT.txt (CRLF) */
    size_t       ReportLen;

    ADFEntry    *Entries;
    int          EntryCount;
    int          EntryCap;

    BYTE         BadTracks[MAX_TRACKS];
    DWORD        BadSectorsCount;
    DWORD        TotalBadTracksCount;
    wchar_t      BadTracksSummary[160];

    DirStackItem DirStack[MAX_PATH_DEPTH];
    int          StackDepth;
    wchar_t      CurrentPath[MAX_PATH];

    struct InfoPanelLine InfoLines[MAX_INFO_LINES];
    wchar_t      InfoLabel[MAX_INFO_LINES][48];     /* labels of the information panel (copies of messages) */
    wchar_t      InfoText[MAX_INFO_LINES][112];
    wchar_t      Title[MAX_PATH + 48];
} PluginData;

static struct PluginStartupInfo Psi;

/* ==================== Messages (translatable texts) ==================== */

/*
 * Every text that the plug-in shows in Far comes from a Far language file: ADFDMSViewEng.lng (English),
 * ADFDMSView<Language>.lng, ... in the folder of the DLL. Far loads the file of its interface language (the English
 * one if there is none) and its GetMsg() returns a message by number: the number of a message is its position in the
 * file, counting the lines that start with a double quote.
 *
 * ADF_MESSAGES lists the identifiers, in the order of the files, with the built-in English texts. The built-in text
 * is used when Far gives no text (no language file) and when a translated format takes other arguments than the
 * English one (a wrong placeholder in a translation must not crash Far). ADFDMSViewEng.lng holds the same texts.
 * To add a message: add it at the end of the list and at the end of every .lng file (a language file shorter than the
 * list makes Far report a bad language file).
 * Only C comments (not //) can go inside the list: its lines end with a backslash.
 *
 * A message that is a printf format (it contains %) is read with GetMsgFmt(), any other one with GetMsg().
 * The .lng files say, above each format, what its arguments are.
 */
#define ADF_MESSAGES(X) \
    /* suffix of the panel title */ \
    X(MTitleDamaged,         L"[damaged]") \
    /* information panel (Ctrl+L): labels */ \
    X(MInfoContainer,        L"Container Format") \
    X(MInfoFileSystem,       L"File System") \
    X(MInfoCapacity,         L"Disk Capacity") \
    X(MInfoBadSectors,       L"Bad Sectors") \
    X(MInfoBadTracks,        L"Bad Tracks") \
    X(MInfoBadTrackList,     L"Bad Track List") \
    X(MInfoRootBlock,        L"Root Block") \
    X(MInfoRecovered,        L"Recovered") \
    X(MInfoIncomplete,       L"Incomplete Files") \
    X(MInfoDmsTracks,        L"DMS Tracks") \
    X(MInfoDmsMissing,       L"DMS Tracks Missing") \
    X(MInfoDmsHeaders,       L"DMS Headers") \
    X(MInfoDetails,          L"Details") \
    X(MInfoDirBlock,         L"Dir Block / Items") \
    /* information panel: values */ \
    X(MValContainerDms,      L"DMS Compressed") \
    X(MValContainerAdf,      L"ADF Image") \
    X(MValFsAmiga,           L"Amiga %ls") \
    X(MValFsAmigaCustomBoot, L"Amiga %ls (non-DOS bootblock)") \
    X(MValFsUnknown,         L"not recognized") \
    X(MValCapacity,          L"%u KB") \
    X(MValNoBadTracks,       L"None (disk intact)") \
    X(MValRootMissing,       L"missing: files recovered from headers") \
    X(MValRootBadSum,        L"bad checksum (accepted)") \
    X(MValRootValid,         L"valid") \
    X(MValRootNoFs,          L"no readable DOS file system") \
    X(MValRecovered,         L"files: %u, block groups: %u (%ls)") \
    X(MValIncomplete,        L"%u incomplete, %u unreadable") \
    X(MValDmsTracks,         L"%u OK, %u repaired, %u unverified") \
    X(MValDmsMissing,        L"%u lost, %u absent") \
    X(MValDmsHeaders,        L"%u repaired") \
    X(MValDetails,           L"see %ls") \
    X(MValDirBlock,          L"Block %u (items: %d)") \
    /* descriptions of the items of the panel */ \
    X(MDescUnreadable,       L"unreadable") \
    X(MDescIncomplete,       L"incomplete or unverified blocks") \
    X(MDescSizeFixed,        L"size taken from the data blocks") \
    X(MDescRecFile,          L"recovered from the header") \
    X(MDescRecData,          L"data blocks without a header") \
    X(MDescRecDir,           L"files and data not reached by the directory tree") \
    X(MDescImage,            L"unpacked disk image") \
    X(MDescReport,           L"what was found and repaired") \
    /* the box shown when a file that the plug-in recognizes cannot be opened: the name of the file, then one of these */ \
    X(MErrDiskType,          L"This archive does not hold a disk image (disk type 7)") \
    X(MErrEncrypted,         L"This DMS archive is encrypted and its decryption key was not found") \
    X(MErrNoDisk,            L"No disk that can be read was found in this file") \
    X(MErrRead,              L"The file cannot be read, or its size is not valid") \
    X(MErrMemory,            L"Not enough memory to open the file") \
    /* the box shown when something would change the image: the panel is read-only */ \
    X(MReadOnly,             L"This panel is read-only: the image cannot be changed") \
    /* the copy dialog (F5) and the boxes that go with it */ \
    X(MCopyTitle,            L"Copy") \
    X(MCopyOne,              L"Copy \"%ls\" to:") \
    X(MCopyMany,             L"Copy %u items to:") \
    X(MCopyExisting,         L"Already existing files:") \
    X(MCopyAsk,              L"Ask") \
    X(MCopyOverwrite,        L"Overwrite") \
    X(MCopySkip,             L"Skip") \
    X(MBtnCopy,              L"&Copy") \
    X(MBtnCancel,            L"Cancel") \
    X(MExists,               L"File already exists:") \
    X(MBtnOverwrite,         L"&Overwrite") \
    X(MBtnSkip,              L"&Skip") \
    X(MBtnOverwriteAll,      L"Overwrite &all") \
    X(MBtnSkipAll,           L"S&kip all") \
    X(MErrDest,              L"Cannot create the destination folder:") \
    X(MCopyFailed,           L"Items that could not be copied: %u") \
    /* information panel, descriptions of the items and summary of the copy: more texts */ \
    X(MValContainerDmsEnc,   L"DMS Compressed, encrypted") \
    X(MValRootMissingData,   L"missing: files rebuilt from data blocks") \
    X(MCopyPartial,          L"Items copied incomplete or unverified: %u") \
    X(MErrDialog,            L"The copy dialog cannot be shown: nothing was copied.")

enum {
#define ADF_MSG_ID(id, text) id,
    ADF_MESSAGES(ADF_MSG_ID)
#undef ADF_MSG_ID
    MMessageCount
};

static const wchar_t *const g_msg_default[MMessageCount] = {
#define ADF_MSG_TEXT(id, text) text,
    ADF_MESSAGES(ADF_MSG_TEXT)
#undef ADF_MSG_TEXT
};

/* The text of a message: the one of the language file, or the built-in English one if Far gives none */
static const wchar_t *GetMsg(int id)
{
    const wchar_t *s;
    if (id < 0 || id >= MMessageCount) return L"";
    s = Psi.GetMsg ? Psi.GetMsg(&MainGuid, id) : NULL;
    return (s && *s) ? s : g_msg_default[id];
}

/* The conversions of a printf-style format as a text of codes: "Block %u (items: %d)" gives "u,d,". False if the
 * format has something that the plug-in does not pass (a width taken from an argument, %n, numbered arguments, a
 * conversion other than d i u x X c s) or too many conversions. */
static bool fmt_signature(const wchar_t *f, wchar_t *sig, size_t cap)
{
    size_t k = 0;
    for (; *f; f++) {
        if (*f != L'%') continue;
        f++;
        if (*f == L'%') continue;                                  /* a literal percent sign */
        while (*f && wcschr(L"-0123456789.", *f)) f++;             /* flags, width, precision (never '*') */
        while ((*f == L'l' || *f == L'h') && k + 4 < cap) sig[k++] = *f++;
        if (!*f || !wcschr(L"diuxXcs", *f) || k + 3 >= cap) return false;
        sig[k++] = *f;
        sig[k++] = L',';
    }
    sig[k] = 0;
    return true;
}

/* A message that is a printf format: the translated one, if it takes the same arguments as the English one,
 * otherwise the English one (a translation with a wrong placeholder would make the printf read what is not there) */
static const wchar_t *GetMsgFmt(int id)
{
    wchar_t a[32], b[32];
    const wchar_t *s = GetMsg(id), *d = (id >= 0 && id < MMessageCount) ? g_msg_default[id] : L"";
    if (s == d) return s;
    if (fmt_signature(s, a, 32) && fmt_signature(d, b, 32) && wcscmp(a, b) == 0) return s;
    return d;
}

/* ==================== Names ==================== */

static void wfmt(wchar_t *buf, size_t n, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, n, fmt, ap);
    va_end(ap);
    buf[n - 1] = 0;
}

static int is_reserved_name(const wchar_t *n)
{
    static const wchar_t *const dev[] = { L"CON", L"PRN", L"AUX", L"NUL", NULL };
    size_t len = 0, i;
    while (n[len] && n[len] != L'.') len++;        /* the name without its extension */
    while (len > 0 && n[len - 1] == L' ') len--;   /* "CON .txt" is the device CON too */
    for (i = 0; dev[i]; i++)
        if (len == 3 && _wcsnicmp(n, dev[i], 3) == 0) return 1;
    /* COM1-9 and LPT1-9, and with the superscript digits of ISO 8859-1 (U+00B9, U+00B2, U+00B3), which Windows
     * takes as digits too: "COM\xB9" is a device */
    if (len == 4 && (_wcsnicmp(n, L"COM", 3) == 0 || _wcsnicmp(n, L"LPT", 3) == 0) &&
        ((n[3] >= L'1' && n[3] <= L'9') || n[3] == 0xB9 || n[3] == 0xB2 || n[3] == 0xB3))
        return 1;
    return 0;
}

/* An Amiga name (Latin-1) as a Windows file name: characters that Windows does not allow, the control characters
 * (0-31, 127-159: the second range is not printable either) and trailing dots and spaces (Windows would drop them,
 * and "a." and "a" would clash) are replaced by '_', an empty name becomes "_", and the names of devices get a '_'
 * in front, so that the item can be created when it is extracted */
static void name_to_wide(const uint8_t *n, size_t len, wchar_t *out, size_t cap)
{
    size_t i, k = 0;
    for (i = 0; i < len && k + 2 < cap; i++) {
        wchar_t c = n[i];
        if (c < 32 || (c >= 127 && c <= 159) || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        out[k++] = c;
    }
    {
        size_t j = k;                          /* (k is not changed: the length stays the same) */
        while (j > 0 && (out[j - 1] == L'.' || out[j - 1] == L' ')) out[--j] = L'_';
    }
    if (k == 0) out[k++] = L'_';             /* an empty name */
    out[k] = 0;
    if (is_reserved_name(out) && k + 1 < cap) {
        memmove(out + 1, out, (k + 1) * sizeof(wchar_t));
        out[0] = L'_';
    }
}

/* The case folding of the Windows file systems for the characters of an Amiga name (Latin-1): A-Z, and the letters
 * U+00C0-U+00DE (not U+00D7) with U+00E0-U+00FE. _wcsicmp does not do the second part (in the "C" locale, which the
 * plug-in must not change, the lower and the upper case a-umlaut are different), but NTFS and FAT see one name:
 * two such files would overwrite each other when copied */
static wchar_t fold_name_char(wchar_t c)
{
    if ((c >= L'A' && c <= L'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return (wchar_t)(c + 32);
    return c;
}

static int name_equal(const wchar_t *a, const wchar_t *b)
{
    for (; *a && *b; a++, b++)
        if (fold_name_char(*a) != fold_name_char(*b)) return 0;
    return *a == *b;
}

static unsigned name_hash(const wchar_t *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned)fold_name_char(*s); h *= 16777619u; }
    return h;
}

/* Gives every item of the list a name that no previous item has (case does not count): the first keeps its name, the
 * next ones get "~2", "~3", ... after the name that they had. A hash table of the names given so far finds a clash
 * at once: a damaged disk can hold thousands of items with the same name, and comparing every try with all the
 * previous items would be quadratic. The tries always start from the name that the item had ("foo~2", "foo~3"),
 * not from the last try ("foo~2~3") */
static void make_names_unique(ADFEntry *e, int n)
{
    int i, cap = 16, *tab;
    if (n < 2) return;
    while (cap < 2 * n) cap <<= 1;
    tab = (int *)xcalloc((size_t)cap, sizeof(int));           /* 0: free; else the index of an item, plus 1 */
    for (i = 0; i < n; i++) {
        wchar_t base[MAX_NAME];
        int seq = 1;
        wcsncpy(base, e[i].Name, MAX_NAME - 8);               /* room for "~N" */
        base[MAX_NAME - 8] = 0;
        for (;;) {
            unsigned h = name_hash(e[i].Name) & (unsigned)(cap - 1);
            while (tab[h] && !name_equal(e[tab[h] - 1].Name, e[i].Name)) h = (h + 1) & (unsigned)(cap - 1);
            if (!tab[h]) { tab[h] = i + 1; break; }           /* nobody has it: it is this item's */
            if (seq >= 999999) break;                         /* cannot happen (not that many items): never loop for ever */
            wfmt(e[i].Name, MAX_NAME, L"%ls~%d", base, ++seq);
        }
    }
    free(tab);
}

/* ==================== Entry lists ==================== */

static ADFEntry *entry_add(ADFEntry **v, int *n, int *cap)
{
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *v = (ADFEntry *)xrealloc(*v, (size_t)*cap * sizeof(ADFEntry));
    }
    memset(&(*v)[*n], 0, sizeof(ADFEntry));
    return &(*v)[(*n)++];
}

/* The items of a directory of the disk */
static void build_real(PluginData *pd, uint32_t blk, ADFEntry **v, int *n, int *cap)
{
    adf_item_t *it = NULL;
    size_t cnt = 0, i;
    int first = *n;

    if (!pd->AdfOk || pd->Adf.rootless || blk == 0) return;
    adf_list_dir(&pd->Adf, blk, &it, &cnt);
    for (i = 0; i < cnt; i++) {
        ADFEntry *e = entry_add(v, n, cap);
        name_to_wide(it[i].name, it[i].nlen, e->Name, MAX_NAME);
        e->Kind = it[i].is_dir ? EK_DIR : EK_FILE;
        e->Id = it[i].blk;
        e->Size = it[i].size;
        e->Flags = it[i].flags;
        if (it[i].has_date) {
            amiga_to_filetime(it[i].days, it[i].mins, it[i].ticks, &e->Time);
            e->HasTime = e->Time.dwLowDateTime || e->Time.dwHighDateTime;
        }
    }
    free(it);
    make_names_unique(*v + first, *n - first);
}

/* The items of a directory of _RECOVERED (node 0 is _RECOVERED itself) */
static void build_rec(PluginData *pd, int32_t node, ADFEntry **v, int *n, int *cap)
{
    adf_img_t *a = &pd->Adf;
    size_t i;
    int first = *n;

    if (!pd->AdfOk) return;
    for (i = 1; i < a->nrnode; i++)
        if (a->rnode[i].parent == node) {
            ADFEntry *e = entry_add(v, n, cap);
            name_to_wide(a->rnode[i].name, a->rnode[i].nlen, e->Name, MAX_NAME);
            e->Kind = EK_RECDIR;
            e->Id = (DWORD)i;
            if (a->rnode[i].dirblk) entry_set_time(e, adf_block(a, a->rnode[i].dirblk));
        }
    for (i = 0; i < a->nrfile; i++)
        if (a->rfile[i].node == node) {
            const uint8_t *h = a->data + (size_t)a->rfile[i].blk * ADF_BLOCK_SIZE;
            uint32_t bs = rd32(h + 324);
            ADFEntry *e = entry_add(v, n, cap);
            name_to_wide(h + 433, h[432] > 30 ? 30 : h[432], e->Name, MAX_NAME);
            e->Kind = EK_RECFILE;
            e->Id = a->rfile[i].blk;
            entry_set_time(e, h);
            if ((uint64_t)bs > a->size) {
                uint64_t sz = adf_oidx_size(a, a->rfile[i].blk);
                e->Size = sz && sz <= a->size ? (DWORD)sz : 0;
                e->Flags |= AI_SIZEFIX;
            } else {
                e->Size = bs;
            }
            {
                unsigned bad = 0;
                char nm[48];
                int rc;
                adf_log_name(h + 433, h[432] > 30 ? 30 : h[432], nm, sizeof(nm));
                a->quiet++;
                rc = adf_file_data(a, a->rfile[i].blk, nm, NULL, NULL, &bad);       /* dry run: the state of the file */
                a->quiet--;
                if (rc < 0) e->Flags |= AI_LOST;
                else if (rc == 1) e->Flags |= AI_PARTIAL;
            }
        }
    if (node == 0)
        for (i = 0; i < a->nrdata; i++) {
            ADFEntry *e = entry_add(v, n, cap);
            wfmt(e->Name, MAX_NAME, L"DATA_%u.bin", (unsigned)a->rdata[i].hk);
            e->Kind = EK_RECDATA;
            e->Id = (DWORD)i;
            e->Size = (DWORD)a->rdata[i].total;
            if (a->rdata[i].missing) e->Flags |= AI_PARTIAL;
        }
    make_names_unique(*v + first, *n - first);
}

/* The file name part of a path (either kind of separator) */
static const wchar_t *path_base(const wchar_t *p)
{
    const wchar_t *b = p, *q;
    for (q = p; *q; q++)
        if (*q == L'\\' || *q == L'/') b = q + 1;
    return b;
}

/* Name of the unpacked image shown for a DMS archive: <archive name>.adf */
static void image_name(const PluginData *pd, wchar_t *out)
{
    const wchar_t *base = path_base(pd->FileName);
    wchar_t tmp[MAX_NAME];
    wchar_t *dot;
    wcsncpy(tmp, base, MAX_NAME - 8);
    tmp[MAX_NAME - 8] = 0;
    dot = wcsrchr(tmp, L'.');
    if (dot) *dot = 0;
    wfmt(out, MAX_NAME, L"%ls.adf", tmp);
}

/* The items of the directory on top of the stack; the root also gets _RECOVERED, the report and the unpacked image */
static void load_dir(PluginData *pd)
{
    DirStackItem *top = &pd->DirStack[pd->StackDepth];
    int cap = pd->EntryCap;

    pd->EntryCount = 0;
    if (top->Kind == EK_RECDIR) {
        build_rec(pd, (int32_t)top->Id, &pd->Entries, &pd->EntryCount, &cap);
    } else {
        build_real(pd, top->Id, &pd->Entries, &pd->EntryCount, &cap);
        if (pd->StackDepth == 0) {
            if (pd->AdfOk && (pd->Adf.nrfile || pd->Adf.nrdata)) {
                ADFEntry *e = entry_add(&pd->Entries, &pd->EntryCount, &cap);
                wcscpy(e->Name, NAME_RECOVERED);
                e->Kind = EK_RECDIR;
                e->Id = 0;
            }
            if (pd->IsDMS && pd->Image) {
                ADFEntry *e = entry_add(&pd->Entries, &pd->EntryCount, &cap);
                image_name(pd, e->Name);
                e->Kind = EK_IMAGE;
                e->Size = (DWORD)pd->ImageSize;
            }
            if (pd->HasReport) {
                ADFEntry *e = entry_add(&pd->Entries, &pd->EntryCount, &cap);
                wcscpy(e->Name, NAME_REPORT);
                e->Kind = EK_REPORT;
                e->Size = (DWORD)pd->ReportLen;
            }
            make_names_unique(pd->Entries, pd->EntryCount);
        }
    }
    pd->EntryCap = cap;
}

static void update_current_path(PluginData *pd)
{
    int i;
    pd->CurrentPath[0] = L'\\';
    pd->CurrentPath[1] = L'\0';
    for (i = 1; i <= pd->StackDepth; i++) {
        size_t l = wcslen(pd->CurrentPath);
        if (i > 1 && l + 1 < MAX_PATH) wcscat(pd->CurrentPath, L"\\");
        l = wcslen(pd->CurrentPath);
        wcsncat(pd->CurrentPath, pd->DirStack[i].Name, MAX_PATH - l - 1);
    }
}


/* ==================== Guard against a failed allocation ==================== */

/* The core jumps back here if an allocation fails (it must not terminate Far): the call is lost, Far goes on. */
#define OOM_GUARD_BEGIN                                                         \
    jmp_buf oom_jb__;                                                           \
    jmp_buf *oom_prev__ = g_oom_jmp;                                            \
    g_oom_jmp = &oom_jb__;                                                      \
    if (setjmp(oom_jb__) == 0) {
#define OOM_GUARD_END(failed)                                                   \
    } else {                                                                    \
        (failed) = 1;                                                           \
    }                                                                           \
    g_oom_jmp = oom_prev__;

/* The items of a directory of the disk or of _RECOVERED, guarded against a failed allocation; 0 on failure */
static int dir_items(PluginData *pd, int kind, DWORD id, ADFEntry **out, int *count)
{
    ADFEntry *v = NULL;
    int n = 0, cap = 0;
    volatile int failed = 0;

    OOM_GUARD_BEGIN
        if (kind == EK_RECDIR) build_rec(pd, (int32_t)id, &v, &n, &cap);
        else build_real(pd, id, &v, &n, &cap);
    OOM_GUARD_END(failed)
    pd->Adf.quiet = 0;
    if (failed) return 0;
    *out = v;
    *count = n;
    return 1;
}

/* load_dir, guarded against a failed allocation; 0 on failure (the listing is then empty) */
static int reload_dir(PluginData *pd)
{
    volatile int failed = 0;
    OOM_GUARD_BEGIN
        load_dir(pd);
    OOM_GUARD_END(failed)
    pd->Adf.quiet = 0;
    if (failed) pd->EntryCount = 0;
    return !failed;
}

/* ==================== Opening ==================== */

static BYTE *read_whole_file(const wchar_t *name, size_t *out_size)
{
    HANDLE h;
    LARGE_INTEGER sz;
    BYTE *buf;
    size_t got = 0, total;

    h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 56 || sz.QuadPart > (LONGLONG)MAX_FILE_SIZE) {
        CloseHandle(h);
        return NULL;
    }
    total = (size_t)sz.QuadPart;
    buf = (BYTE *)malloc(total);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }
    while (got < total) {
        DWORD want = (DWORD)(total - got > (16u << 20) ? (16u << 20) : total - got), rd = 0;
        if (!ReadFile(h, buf + got, want, &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    if (got < 56) {
        free(buf);
        return NULL;
    }
    *out_size = got;
    return buf;
}

/* A DMS archive: the signature, or an archive whose signature or first track header is damaged */
static int looks_like_dms(const unsigned char *b, size_t n)
{
    if (n >= 4 && memcmp(b, "DMS!", 4) == 0) return 1;
    return n >= DMS_HEADLEN + DMS_THLEN && dms_is_signature(b, n < 512 ? n : 512, n);
}

/* Bad tracks (cylinders): blocks that could not be read and DMS tracks that were not recovered or not verified */
static void compute_bad_tracks(PluginData *pd)
{
    unsigned t, n = 0, i;
    memset(pd->BadTracks, 0, sizeof(pd->BadTracks));
    if (pd->AdfOk)
        for (t = 0; t < pd->Adf.ntracks && t < MAX_TRACKS; t++)
            if (pd->Adf.badtrk[t]) pd->BadTracks[t] = 1;
    if (pd->IsDMS) {
        unsigned tmax = dms_tracks_for(pd->Dms.tsize ? pd->Dms.tsize : DMS_TRACK_DD);
        /* an absent track is bad only if the archive was supposed to hold it: inside [from, to] of the archive
         * header (0-based track numbers); if the header is damaged that range is unknown, and only the holes
         * below the highest track found are certain */
        unsigned first = pd->Dms.hdr_ok ? pd->Dms.from : 0;
        unsigned last  = pd->Dms.hdr_ok ? (pd->Dms.to < tmax ? pd->Dms.to : tmax - 1) : pd->Dms.maxnum;
        for (t = 0; t < tmax && t < MAX_TRACKS; t++) {
            int st = pd->Dms.st[t];
            if (st == DMT_LOST || st == DMT_BEST || st == DMT_UNVERIFIED ||
                (st == DMT_ABSENT && t >= first && t <= last))
                pd->BadTracks[t] = 1;
        }
    }
    for (t = 0; t < MAX_TRACKS; t++) n += pd->BadTracks[t] ? 1 : 0;
    pd->TotalBadTracksCount = n;
    pd->BadSectorsCount = pd->AdfOk ? pd->Adf.nbad : 0;
    pd->BadTracksSummary[0] = 0;               /* no bad track: the panel shows the message "none" instead of the list */
    if (n == 0) return;
    for (t = 0, i = 0; t < MAX_TRACKS; t++) {
        wchar_t tmp[16];
        if (!pd->BadTracks[t]) continue;
        if (i++ > 0) wcscat(pd->BadTracksSummary, L", ");
        wfmt(tmp, 16, L"%u", t);
        wcscat(pd->BadTracksSummary, tmp);
        if (wcslen(pd->BadTracksSummary) > 100) {
            wcscat(pd->BadTracksSummary, L"...");
            break;
        }
    }
}

/* The text of _RECOVERY_REPORT.txt: a heading and what the readers found and repaired, with CRLF line ends */
static void build_report(PluginData *pd)
{
    char head[1024];
    char fname[MAX_PATH];
    size_t hl, i, extra = 0, o = 0;
    const char *t = pd->Log.text ? pd->Log.text : "";
    size_t tl = strlen(t);

    {
        const wchar_t *b = path_base(pd->FileName);
        size_t k = 0;
        for (; b[k] && k + 1 < sizeof(fname); k++) fname[k] = (b[k] >= 32 && b[k] < 127) ? (char)b[k] : '?';
        fname[k] = 0;
    }
    hl = (size_t)sprintf(head, "ADFDMSView - recovery report\n%s (%s)\n\n",
                         fname, pd->IsDMS ? "DMS archive" : "ADF image");
    for (i = 0; i < tl; i++) if (t[i] == '\n') extra++;
    pd->Report = (char *)xmalloc(hl + tl + extra + hl + 1);
    for (i = 0; i < hl; i++) {
        if (head[i] == '\n') pd->Report[o++] = '\r';
        pd->Report[o++] = head[i];
    }
    for (i = 0; i < tl; i++) {
        if (t[i] == '\n') pd->Report[o++] = '\r';
        pd->Report[o++] = t[i];
    }
    pd->Report[o] = 0;
    pd->ReportLen = o;
}

/* Reads the file and builds everything the panel shows. NULL: not an image that can be shown; *why (REFUSE_*) says
 * why */
static PluginData *pd_open(const wchar_t *name, int *why)
{
    size_t rawsize = 0;
    BYTE *volatile raw = read_whole_file(name, &rawsize);
    volatile size_t rawsz = rawsize;
    PluginData *pd;
    volatile int failed = 0, usable = 0;

    *why = REFUSE_NONE;
    if (!raw) {
        *why = REFUSE_READ;
        return NULL;
    }
    pd = (PluginData *)calloc(1, sizeof(PluginData));
    if (!pd) {
        free(raw);
        *why = REFUSE_MEMORY;
        return NULL;
    }
    wcsncpy(pd->FileName, name, MAX_PATH - 1);
    g_clog = &pd->Log;
    OOM_GUARD_BEGIN
        if (looks_like_dms(raw, rawsz)) {
            pd->IsDMS = 1;
            if (dms_unpack_image(raw, rawsz, &pd->Dms) == 0) {
                pd->Image = pd->Dms.image;
                pd->ImageSize = pd->Dms.image_size;
                usable = 1;
            }
            free(raw);
            raw = NULL;
        } else {
            pd->Image = raw;
            pd->ImageSize = rawsz;
            raw = NULL;
            usable = 1;
        }
        if (usable) {
            pd->AdfOk = adf_open(&pd->Adf, pd->Image, pd->ImageSize) == 0;
            compute_bad_tracks(pd);
            pd->Damaged = !pd->AdfOk || (pd->AdfOk && (pd->Adf.has_error || pd->Adf.root_damaged || pd->Adf.rootless)) ||
                          (pd->IsDMS && (pd->Dms.any_error || pd->Dms.n_repaired || pd->Dms.hdr_repaired ||
                                         !pd->Dms.sig_ok || pd->Dms.budget_hit || pd->Dms.key_hit || pd->TotalBadTracksCount));
            pd->HasReport = pd->Damaged && pd->Log.lines > 0;
            if (pd->HasReport) build_report(pd);
        }
    OOM_GUARD_END(failed)
    g_clog = NULL;
    if (failed || !usable) {
        /* (memory of an aborted call is lost; what was set up is released) */
        *why = failed ? REFUSE_MEMORY : (pd->IsDMS && pd->Dms.refuse != REFUSE_NONE ? pd->Dms.refuse : REFUSE_NODISK);
        adf_free(&pd->Adf);                    /* an adf_open that was cut short by the lack of memory leaves its tables */
        if (pd->IsDMS) dms_out_free(&pd->Dms); else free(pd->Image);
        free(raw);
        free(pd->Log.text);
        free(pd->Report);
        free(pd);
        return NULL;
    }
    pd->DirStack[0].Kind = EK_DIR;
    pd->DirStack[0].Id = pd->AdfOk && !pd->Adf.rootless ? pd->Adf.root_blk : 0;
    wcscpy(pd->DirStack[0].Name, L"\\");
    pd->StackDepth = 0;
    update_current_path(pd);
    reload_dir(pd);
    return pd;
}

static void pd_free(PluginData *pd)
{
    if (!pd) return;
    if (pd->AdfOk) adf_free(&pd->Adf);
    if (pd->IsDMS) dms_out_free(&pd->Dms); else free(pd->Image);
    free(pd->Entries);
    free(pd->Log.text);
    free(pd->Report);
    free(pd);
}


/* ==================== Extraction ==================== */

static int write_all(HANDLE h, const BYTE *p, size_t n)
{
    while (n > 0) {
        DWORD want = (DWORD)(n > (16u << 20) ? (16u << 20) : n), wr = 0;
        if (!WriteFile(h, p, want, &wr, NULL) || wr == 0) return 0;
        p += wr;
        n -= wr;
    }
    return 1;
}

/*
 * The content of a panel item. Returns 0 (complete), 1 (written, but some blocks could not be read: they are
 * zeros), -1 (nothing can be read). *owned: the buffer must be freed.
 */
static int item_data(PluginData *pd, int kind, DWORD id, const BYTE **buf, size_t *size, int *owned)
{
    int rc = 0;
    unsigned bad = 0;
    char nm[48];
    uint8_t *b = NULL;
    const uint8_t *h;

    *buf = NULL;
    *size = 0;
    *owned = 0;
    switch (kind) {
    case EK_FILE:
    case EK_RECFILE:
        if (!pd->AdfOk) return -1;
        h = adf_block(&pd->Adf, id);
        if (!h) return -1;
        adf_log_name(h + 433, h[432] > 30 ? 30 : h[432], nm, sizeof(nm));
        pd->Adf.quiet++;
        rc = adf_file_data(&pd->Adf, id, nm, &b, size, &bad);
        pd->Adf.quiet--;
        if (rc < 0) return -1;
        *buf = b;
        *owned = 1;
        return rc;
    case EK_RECDATA:
        if (!pd->AdfOk || id >= pd->Adf.nrdata) return -1;
        *buf = adf_group_data(&pd->Adf, &pd->Adf.rdata[id], size);
        *owned = 1;
        return pd->Adf.rdata[id].missing ? 1 : 0;
    case EK_IMAGE:
        if (!pd->Image) return -1;
        *buf = pd->Image;
        *size = pd->ImageSize;
        return 0;
    case EK_REPORT:
        if (!pd->Report) return -1;
        *buf = (const BYTE *)pd->Report;
        *size = pd->ReportLen;
        return 0;
    default:
        return -1;
    }
}

/* ---- Copying out of the panel: the dialog of F5, and what to do with the files that exist ---- */

enum { XM_ASK = 0, XM_OVERWRITE, XM_SKIP };                    /* what to do with a file that already exists */
enum { XA_OVERWRITE = 0, XA_SKIP, XA_OVERWRITE_ALL, XA_SKIP_ALL, XA_CANCEL };   /* the buttons of the box that asks */

typedef struct {
    int      mode;                                             /* XM_* */
    int      silent;                                           /* no box at all (Far works silently, or a viewer or an editor asked) */
    int      cancelled;                                        /* Cancel in the box about a file that exists */
    unsigned copied, skipped, failed;
    unsigned partial;                                          /* of the copied ones: written, but with blocks missing or not verified */
} XCtx;

static int g_copy_mode = XM_ASK;                               /* what the dialog proposes: the last choice made */

/* a path for a box: at most max characters, the middle replaced by "..." (out holds max + 1) */
static void shorten_path(const wchar_t *p, wchar_t *out, size_t max)
{
    size_t n = wcslen(p), head, tail;
    if (n <= max) {
        wcscpy(out, p);
        return;
    }
    head = max / 3;
    tail = max - head - 3;
    wcsncpy(out, p, head);
    out[head] = 0;
    wcscat(out, L"...");
    wcscat(out, p + n - tail);
}

/* dir\name; 0 if it is not shorter than MAX_PATH */
static int join_path(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    size_t d = wcslen(dir);
    int bs = d > 0 && dir[d - 1] == L'\\';
    if (d + (bs ? 0 : 1) + wcslen(name) >= MAX_PATH) return 0;
    wfmt(out, MAX_PATH, bs ? L"%ls%ls" : L"%ls\\%ls", dir, name);
    return 1;
}

/* a box with a title, a line of text and, if given, a path under it */
static void box_text(int title_id, const wchar_t *text, const wchar_t *path)
{
    static const GUID TextGuid =
    { 0x2975352e, 0xd80a, 0x41a1, { 0x9c, 0xa2, 0x06, 0xbe, 0x25, 0x40, 0xdd, 0x43 } };   /* 2975352E-D80A-41A1-9CA2-06BE2540DD43 */
    wchar_t shown[72];
    const wchar_t *items[3];
    int n = 0;

    items[n++] = GetMsg(title_id);
    items[n++] = text;
    if (path) {
        shorten_path(path, shown, 66);
        items[n++] = shown;
    }
    if (Psi.Message) Psi.Message(&MainGuid, &TextGuid, FMSG_WARNING | FMSG_MB_OK, NULL, items, n, 0);
}

/* "File already exists": Overwrite, Skip, Overwrite all, Skip all, Cancel (Esc is Cancel) */
static int ask_overwrite(const wchar_t *path)
{
    static const GUID AskGuid =
    { 0x4f56f101, 0x22d6, 0x47f1, { 0xae, 0x29, 0xa0, 0x39, 0x3c, 0xf0, 0x4f, 0x91 } };   /* 4F56F101-22D6-47F1-AE29-A0393CF04F91 */
    wchar_t shown[72];
    const wchar_t *items[8];
    intptr_t r;

    if (!Psi.Message) return XA_OVERWRITE;
    shorten_path(path, shown, 66);
    items[0] = GetMsg(MCopyTitle);
    items[1] = GetMsg(MExists);
    items[2] = shown;
    items[3] = GetMsg(MBtnOverwrite);
    items[4] = GetMsg(MBtnSkip);
    items[5] = GetMsg(MBtnOverwriteAll);
    items[6] = GetMsg(MBtnSkipAll);
    items[7] = GetMsg(MBtnCancel);
    r = Psi.Message(&MainGuid, &AskGuid, FMSG_WARNING, NULL, items, 8, 5);
    return (r >= XA_OVERWRITE && r <= XA_SKIP_ALL) ? (int)r : XA_CANCEL;
}

/* 1: write the file; 0: do not (it exists and is skipped, or the user cancelled) */
static int may_write(const wchar_t *outpath, XCtx *x)
{
    DWORD a;

    if (x->cancelled) return 0;
    a = GetFileAttributesW(outpath);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return 1;   /* nothing there (a folder in the way fails later) */
    if (x->silent || x->mode == XM_OVERWRITE) return 1;
    if (x->mode == XM_ASK) {
        switch (ask_overwrite(outpath)) {
        case XA_OVERWRITE:     return 1;
        case XA_OVERWRITE_ALL: x->mode = XM_OVERWRITE; return 1;
        case XA_SKIP_ALL:      x->mode = XM_SKIP; break;                           /* and this one is skipped too */
        case XA_SKIP:          break;
        default:               x->cancelled = 1; return 0;
        }
    }
    x->skipped++;
    return 0;
}

/* the date of the Amiga item on a file or folder that was created (ft: NULL or zero = no date known) */
static int has_time(const FILETIME *ft)
{
    return ft && (ft->dwLowDateTime || ft->dwHighDateTime);
}

static void set_folder_time(const wchar_t *path, const FILETIME *ft)
{
    HANDLE h;
    if (!has_time(ft)) return;
    h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFileTime(h, ft, NULL, ft);              /* a failure here is not an error of the copy */
    CloseHandle(h);
}

static void extract_file(PluginData *pd, int kind, DWORD id, const wchar_t *outpath, XCtx *x, const FILETIME *ft)
{
    const BYTE *buf;
    size_t size;
    int owned, rc;
    volatile int ok = 0, failed = 0, partial = 0;
    HANDLE h;

    if (!may_write(outpath, x)) return;
    OOM_GUARD_BEGIN
        rc = item_data(pd, kind, id, &buf, &size, &owned);
        if (rc >= 0) {
            h = CreateFileW(outpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                ok = write_all(h, buf, size);
                if (ok && has_time(ft)) SetFileTime(h, ft, NULL, ft);     /* original date and time of the Amiga file */
                CloseHandle(h);
                if (!ok) DeleteFileW(outpath);       /* a file cut short by a write error must not pass for a copy */
            }
            if (rc == 1) partial = 1;
            if (owned) free((void *)buf);
        }
    OOM_GUARD_END(failed)
    pd->Adf.quiet = 0;
    if (ok && !failed) {
        x->copied++;
        if (partial) x->partial++;
    } else {
        x->failed++;
    }
}

/* creates a folder of the destination; 1 if it is there afterwards (a file with that name is in the way: 0) */
static int make_folder(const wchar_t *path)
{
    DWORD a;
    CreateDirectoryW(path, NULL);
    a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void extract_dir(PluginData *pd, int kind, DWORD id, const wchar_t *dest, int depth, XCtx *x)
{
    ADFEntry *v = NULL;
    int n = 0, i;

    if (depth > 64 || !dir_items(pd, kind, id, &v, &n)) {
        x->failed++;
        return;
    }
    for (i = 0; i < n && !x->cancelled; i++) {
        wchar_t path[MAX_PATH];
        if (!join_path(path, dest, v[i].Name)) {
            x->failed++;
            continue;
        }
        if (v[i].Kind == EK_DIR || v[i].Kind == EK_RECDIR) {
            if (make_folder(path)) {
                extract_dir(pd, v[i].Kind, v[i].Id, path, depth + 1, x);
                if (v[i].HasTime) set_folder_time(path, &v[i].Time);      /* after the contents: they change the folder's date */
            } else {
                x->failed++;
            }
        } else {
            extract_file(pd, v[i].Kind, v[i].Id, path, x, v[i].HasTime ? &v[i].Time : NULL);
        }
    }
    free(v);
}

/* the folder typed in the dialog as a full path: environment variables are expanded, a relative path is taken from
 * the folder that Far proposed (base), "." and ".." are resolved, a trailing backslash goes away; 0 if it is not valid */
static int resolve_dest(const wchar_t *base, const wchar_t *typed, wchar_t *out, size_t cap)
{
    wchar_t t[MAX_PATH * 2], j[MAX_PATH * 2];
    size_t n;
    DWORD r;

    while (*typed == L' ') typed++;
    n = wcslen(typed);
    while (n > 0 && typed[n - 1] == L' ') n--;
    if (n == 0) {                                                  /* nothing typed: the folder that Far proposed */
        if (wcslen(base) >= cap) return 0;
        wcscpy(out, base);
        return 1;
    }
    if (n >= MAX_PATH) return 0;
    wcsncpy(t, typed, n);
    t[n] = 0;
    r = ExpandEnvironmentStringsW(t, j, MAX_PATH * 2);
    if (r == 0 || r > MAX_PATH * 2) return 0;
    wcscpy(t, j);
    if (t[0] == L'\\' || (t[0] && t[1] == L':')) {                 /* "C:...", "\\server\share", "\dir": not relative to base */
        wcscpy(j, t);
    } else {
        if (wcslen(base) + 1 + wcslen(t) >= MAX_PATH * 2) return 0;
        wfmt(j, MAX_PATH * 2, L"%ls\\%ls", base, t);
    }
    r = GetFullPathNameW(j, (DWORD)cap, out, NULL);
    if (r == 0 || r >= cap) return 0;
    n = wcslen(out);
    if (n > 3 && out[n - 1] == L'\\') out[n - 1] = 0;              /* "C:\" keeps its backslash */
    return 1;
}

/* creates the folder and the missing folders above it; 1 if the folder is there afterwards */
static int make_dirs(const wchar_t *path)
{
    wchar_t t[MAX_PATH];
    wchar_t *p;
    size_t root = 0;
    DWORD a = GetFileAttributesW(path);

    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (wcslen(path) >= MAX_PATH - 2) return 0;
    wcscpy(t, path);
    if (t[0] == L'\\' && t[1] == L'\\') {                          /* \\server\share\...: the server and the share are not folders to make */
        int skip = 2;
        for (p = t + 2; *p && skip; p++)
            if (*p == L'\\') skip--;
        root = (size_t)(p - t);
    } else if (t[0] && t[1] == L':') {
        root = t[2] == L'\\' ? 3 : 2;
    }
    for (p = t + root; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(t, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(t, NULL);
    a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The dialog of F5, like the copy dialog of Far or the extraction dialog of ArcLite: the folder to copy to (the folder
 * of the other panel is proposed) and what to do with the files that exist. 1: go on (typed and *mode are set), 0:
 * cancelled, -1: the dialog could not be shown */
static int copy_dialog(const struct GetFilesInfo *Info, wchar_t *typed, size_t typedcap, int *mode)
{
    enum { D_BOX, D_TEXT, D_EDIT, D_SEP1, D_LABEL, D_COMBO, D_SEP2, D_OK, D_CANCEL, D_COUNT };
    static const GUID CopyGuid =
    { 0xbd9f99d7, 0xe884, 0x41b6, { 0xba, 0x7a, 0x06, 0x90, 0x62, 0x8e, 0xf5, 0x38 } };   /* BD9F99D7-E884-41B6-BA7A-0690628EF538 */
    struct FarDialogItem it[D_COUNT];
    struct FarListItem li[3];
    struct FarList fl;
    struct FarListPos lp;
    wchar_t text[128], name[56];
    const wchar_t *label = GetMsg(MCopyExisting), *tx;
    size_t lw = wcslen(label), cw = 0, k;
    HANDLE h;
    intptr_t ret;
    int result = 0;

    if (!Psi.DialogInit || !Psi.DialogRun || !Psi.DialogFree || !Psi.SendDlgMessage) return -1;
    if (Info->ItemsNumber == 1) {
        shorten_path(Info->PanelItem[0].FileName, name, 48);
        wfmt(text, 128, GetMsgFmt(MCopyOne), name);
    } else {
        wfmt(text, 128, GetMsgFmt(MCopyMany), (unsigned)Info->ItemsNumber);
    }
    memset(li, 0, sizeof li);
    li[XM_ASK].Text = GetMsg(MCopyAsk);
    li[XM_OVERWRITE].Text = GetMsg(MCopyOverwrite);
    li[XM_SKIP].Text = GetMsg(MCopySkip);
    li[*mode >= XM_ASK && *mode <= XM_SKIP ? *mode : XM_ASK].Flags = LIF_SELECTED;
    for (k = 0; k < 3; k++)
        if (wcslen(li[k].Text) > cw) cw = wcslen(li[k].Text);
    fl.StructSize = sizeof fl;
    fl.ItemsNumber = 3;
    fl.Items = li;

    memset(it, 0, sizeof it);
    it[D_BOX].Type = DI_DOUBLEBOX;  it[D_BOX].X1 = 3;  it[D_BOX].Y1 = 1;  it[D_BOX].X2 = 72;  it[D_BOX].Y2 = 8;
    it[D_BOX].Data = GetMsg(MCopyTitle);
    it[D_TEXT].Type = DI_TEXT;      it[D_TEXT].X1 = 5; it[D_TEXT].Y1 = 2;
    it[D_TEXT].Data = text;
    it[D_EDIT].Type = DI_EDIT;      it[D_EDIT].X1 = 5; it[D_EDIT].Y1 = 3; it[D_EDIT].X2 = 70;
    it[D_EDIT].History = L"ADFDMSView.Copy";
    it[D_EDIT].Flags = DIF_FOCUS | DIF_HISTORY | DIF_EDITEXPAND | DIF_EDITPATH;
    it[D_EDIT].Data = Info->DestPath;
    it[D_EDIT].MaxLength = MAX_PATH - 1;
    it[D_SEP1].Type = DI_TEXT;      it[D_SEP1].X1 = -1; it[D_SEP1].Y1 = 4;
    it[D_SEP1].Flags = DIF_SEPARATOR;
    it[D_LABEL].Type = DI_TEXT;     it[D_LABEL].X1 = 5; it[D_LABEL].Y1 = 5;
    it[D_LABEL].Data = label;
    it[D_COMBO].Type = DI_COMBOBOX; it[D_COMBO].X1 = (intptr_t)(6 + lw); it[D_COMBO].Y1 = 5; it[D_COMBO].X2 = (intptr_t)(6 + lw + cw + 3);
    it[D_COMBO].Param.ListItems = &fl;
    it[D_COMBO].Flags = DIF_DROPDOWNLIST;
    it[D_SEP2].Type = DI_TEXT;      it[D_SEP2].X1 = -1; it[D_SEP2].Y1 = 6;
    it[D_SEP2].Flags = DIF_SEPARATOR;
    it[D_OK].Type = DI_BUTTON;      it[D_OK].Y1 = 7;
    it[D_OK].Flags = DIF_CENTERGROUP | DIF_DEFAULTBUTTON;
    it[D_OK].Data = GetMsg(MBtnCopy);
    it[D_CANCEL].Type = DI_BUTTON;  it[D_CANCEL].Y1 = 7;
    it[D_CANCEL].Flags = DIF_CENTERGROUP;
    it[D_CANCEL].Data = GetMsg(MBtnCancel);

    h = Psi.DialogInit(&MainGuid, &CopyGuid, -1, -1, 76, 10, NULL, it, D_COUNT, 0, FDLG_NONE, NULL, NULL);   /* centered: width and height */
    if (h == NULL || h == INVALID_HANDLE_VALUE) return -1;
    ret = Psi.DialogRun(h);
    if (ret == D_OK) {
        tx = (const wchar_t *)Psi.SendDlgMessage(h, DM_GETCONSTTEXTPTR, D_EDIT, NULL);
        wcsncpy(typed, tx ? tx : L"", typedcap - 1);
        typed[typedcap - 1] = 0;
        memset(&lp, 0, sizeof lp);
        lp.StructSize = sizeof lp;
        Psi.SendDlgMessage(h, DM_LISTGETCURPOS, D_COMBO, &lp);
        *mode = (lp.SelectPos >= XM_ASK && lp.SelectPos <= XM_SKIP) ? (int)lp.SelectPos : XM_ASK;
        result = 1;
    }
    Psi.DialogFree(h);
    return result;
}

/* ==================== Far Manager Exported Functions ==================== */

void WINAPI GetGlobalInfoW(struct GlobalInfo *Info) {
    Info->StructSize    = sizeof(struct GlobalInfo);
    Info->MinFarVersion = MAKEFARVERSION(3, 0, 0, 3000, VS_RELEASE);
    Info->Version       = MAKEFARVERSION(2, 4, 3, 0, VS_RELEASE);     /* Far wants four numbers, the build is 0 */
    Info->Guid          = MainGuid;
    Info->Title         = PLUGIN_NAME;
    Info->Description   = PLUGIN_DESC;
    Info->Author        = PLUGIN_AUTHOR;
}

void WINAPI SetStartupInfoW(const struct PluginStartupInfo *Info) {
    Psi = *Info;
}

void WINAPI GetPluginInfoW(struct PluginInfo *Info) {
    Info->StructSize = sizeof(struct PluginInfo);
    Info->Flags      = 0;
}

HANDLE WINAPI AnalyseW(const struct AnalyseInfo *Info) {
    const wchar_t *ext;
    if (!Info || !Info->FileName) return NULL;
    ext = wcsrchr(Info->FileName, L'.');
    if (!ext) return NULL;

    if (_wcsicmp(ext, L".adf") == 0) return (HANDLE)1;
    if (_wcsicmp(ext, L".dms") == 0) {
        /* also an archive with a damaged signature or first track header (the recovery opens it) */
        if (Info->BufferSize >= 4 && looks_like_dms((const unsigned char *)Info->Buffer, (size_t)Info->BufferSize))
            return (HANDLE)1;
    }
    return NULL;
}

void WINAPI CloseAnalyseW(const struct CloseAnalyseInfo *Info) { (void)Info; }

/* The box that says why a file that the plug-in recognizes is not opened: the name of the file and the reason */
static void show_refusal(const wchar_t *file, int why)
{
    static const GUID RefuseGuid =
    { 0x912fe51d, 0x6097, 0x4a87, { 0xa0, 0x5b, 0xc6, 0xf7, 0x11, 0x4f, 0xf7, 0x62 } };   /* 912FE51D-6097-4A87-A05B-C6F7114FF762 */
    wchar_t name[72];
    const wchar_t *items[4];
    const wchar_t *base = path_base(file);
    size_t n = wcslen(base);
    int id;

    switch (why) {
    case REFUSE_DISKTYPE:  id = MErrDiskType;  break;
    case REFUSE_ENCRYPTED: id = MErrEncrypted; break;
    case REFUSE_READ:      id = MErrRead;      break;
    case REFUSE_MEMORY:    id = MErrMemory;    break;
    default:               id = MErrNoDisk;    break;
    }
    if (n > 64) {                              /* a long name: its first characters, so that the box stays narrow */
        wcsncpy(name, base, 61);
        wcscpy(name + 61, L"...");
    } else {
        wcscpy(name, base);
    }
    items[0] = PLUGIN_NAME;                    /* the title of the box */
    items[1] = name;
    items[2] = L"";
    items[3] = GetMsg(id);
    if (Psi.Message) Psi.Message(&MainGuid, &RefuseGuid, FMSG_WARNING | FMSG_MB_OK, NULL, items, 4, 0);
}

/* A file that the plug-in cannot show: the reason is explained and Far is told that the file was dealt with
 * (PANEL_STOP, which is INVALID_HANDLE_VALUE in Far 3); while Far searches in archives or works silently nothing is
 * shown and the file is declined (NULL), so that the other plug-ins can try */
HANDLE WINAPI OpenFilePluginW(const wchar_t *Name, const unsigned char *Data, int DataSize, int OpMode) {
    PluginData *pd;
    int why = REFUSE_NONE;
    (void)Data; (void)DataSize;
    if (!Name) return NULL;
    pd = pd_open(Name, &why);
    if (pd) return (HANDLE)pd;
    if (why == REFUSE_NONE || ((unsigned long long)OpMode & ((unsigned long long)OPM_FIND | (unsigned long long)OPM_SILENT)))
        return NULL;
    show_refusal(Name, why);
    return PANEL_STOP;
}

void WINAPI ClosePanelW(const struct ClosePanelInfo *Info) {
    pd_free((PluginData *)Info->hPanel);
}

/* One line of the information panel (Ctrl+L): the label is a message, fmt a printf-style format. The label is copied
 * into the panel data like the value, so what Far reads stays valid as long as the panel is open */
static int info_line(PluginData *pd, int n, int label_id, const wchar_t *fmt, ...)
{
    va_list ap;
    if (n >= MAX_INFO_LINES) return n;
    wcsncpy(pd->InfoLabel[n], GetMsg(label_id), 47);
    pd->InfoLabel[n][47] = 0;
    pd->InfoLines[n].Text = pd->InfoLabel[n];
    if (fmt) {
        va_start(ap, fmt);
        _vsnwprintf(pd->InfoText[n], 112, fmt, ap);
        va_end(ap);
        pd->InfoText[n][111] = 0;
        pd->InfoLines[n].Data = pd->InfoText[n];
    } else {
        pd->InfoLines[n].Data = L"";
    }
    pd->InfoLines[n].Flags = 0;
    return n + 1;
}

static int info_sep(PluginData *pd, int n)
{
    if (n >= MAX_INFO_LINES) return n;
    pd->InfoLines[n].Text = L"";
    pd->InfoLines[n].Data = L"";
    pd->InfoLines[n].Flags = IPLFLAGS_SEPARATOR;
    return n + 1;
}

void WINAPI GetOpenPanelInfoW(struct OpenPanelInfo *Info) {
    PluginData *pd = (PluginData *)Info->hPanel;
    const wchar_t *kind = pd->IsDMS ? L"DMS" : L"ADF";
    int n = 0;

    Info->StructSize = sizeof(struct OpenPanelInfo);
    Info->Flags      = OPIF_ADDDOTS | OPIF_SHOWNAMESONLY;
    if (pd->Damaged)
        wfmt(pd->Title, MAX_PATH + 48, L"%ls: %ls %ls", kind, pd->FileName, GetMsg(MTitleDamaged));
    else
        wfmt(pd->Title, MAX_PATH + 48, L"%ls: %ls", kind, pd->FileName);
    Info->PanelTitle = pd->Title;
    Info->CurDir     = pd->CurrentPath;

    n = info_line(pd, n, MInfoContainer, L"%ls",
                  GetMsg(!pd->IsDMS ? MValContainerAdf : pd->Dms.encrypted ? MValContainerDmsEnc : MValContainerDms));
    if (pd->AdfOk)
        n = info_line(pd, n, MInfoFileSystem, GetMsgFmt(pd->Adf.boot_ok ? MValFsAmiga : MValFsAmigaCustomBoot),
                      pd->Adf.is_ffs ? L"FFS" : L"OFS");
    else
        n = info_line(pd, n, MInfoFileSystem, L"%ls", GetMsg(MValFsUnknown));
    n = info_line(pd, n, MInfoCapacity, GetMsgFmt(MValCapacity), (unsigned)(pd->ImageSize / 1024));
    n = info_sep(pd, n);
    n = info_line(pd, n, MInfoBadSectors, L"%u", (unsigned)pd->BadSectorsCount);
    n = info_line(pd, n, MInfoBadTracks, L"%u", (unsigned)pd->TotalBadTracksCount);
    n = info_line(pd, n, MInfoBadTrackList, L"%ls",
                  pd->TotalBadTracksCount ? pd->BadTracksSummary : GetMsg(MValNoBadTracks));
    n = info_sep(pd, n);
    /* the state of the disk and what the recovery did */
    if (pd->AdfOk) {
        if (pd->Adf.rootless)
            n = info_line(pd, n, MInfoRootBlock, L"%ls", GetMsg(pd->Adf.headerless ? MValRootMissingData : MValRootMissing));
        else if (pd->Adf.root_damaged)
            n = info_line(pd, n, MInfoRootBlock, L"%ls", GetMsg(MValRootBadSum));
        else
            n = info_line(pd, n, MInfoRootBlock, L"%ls", GetMsg(MValRootValid));
        if (pd->Adf.nrfile || pd->Adf.nrdata)
            n = info_line(pd, n, MInfoRecovered, GetMsgFmt(MValRecovered), (unsigned)pd->Adf.nrfile,
                          (unsigned)pd->Adf.nrdata, NAME_RECOVERED);
        if (pd->Adf.n_part || pd->Adf.n_lost)
            n = info_line(pd, n, MInfoIncomplete, GetMsgFmt(MValIncomplete), (unsigned)pd->Adf.n_part,
                          (unsigned)pd->Adf.n_lost);
    } else {
        n = info_line(pd, n, MInfoRootBlock, L"%ls", GetMsg(MValRootNoFs));
    }
    if (pd->IsDMS) {
        n = info_line(pd, n, MInfoDmsTracks, GetMsgFmt(MValDmsTracks), (unsigned)pd->Dms.n_ok,
                      (unsigned)pd->Dms.n_repaired, (unsigned)(pd->Dms.n_unverified + pd->Dms.n_best));
        n = info_line(pd, n, MInfoDmsMissing, GetMsgFmt(MValDmsMissing), (unsigned)pd->Dms.n_lost,
                      (unsigned)pd->Dms.n_absent);
        if (pd->Dms.hdr_repaired)
            n = info_line(pd, n, MInfoDmsHeaders, GetMsgFmt(MValDmsHeaders), (unsigned)pd->Dms.hdr_repaired);
    }
    if (pd->HasReport)
        n = info_line(pd, n, MInfoDetails, GetMsgFmt(MValDetails), NAME_REPORT);
    n = info_sep(pd, n);
    n = info_line(pd, n, MInfoDirBlock, GetMsgFmt(MValDirBlock),
                  (unsigned)pd->DirStack[pd->StackDepth].Id, pd->EntryCount);

    Info->InfoLinesNumber = (size_t)n;
    Info->InfoLines       = pd->InfoLines;
}

intptr_t WINAPI GetFindDataW(struct GetFindDataInfo *Info) {
    PluginData *pd = (PluginData *)Info->hPanel;
    int n = pd->EntryCount, i;
    struct PluginPanelItem *items;

    if (n == 0) {
        Info->PanelItem   = NULL;
        Info->ItemsNumber = 0;
        return 1;
    }

    items = (struct PluginPanelItem *)calloc((size_t)n, sizeof(struct PluginPanelItem));
    if (!items) return 0;

    for (i = 0; i < n; i++) {
        const ADFEntry *e = &pd->Entries[i];
        int is_dir = e->Kind == EK_DIR || e->Kind == EK_RECDIR;
        items[i].FileName = _wcsdup(e->Name);
        if (!items[i].FileName) {               /* out of memory: release what was allocated and fail */
            int j;
            for (j = 0; j < i; j++) free((void *)items[j].FileName);
            free(items);
            Info->PanelItem   = NULL;
            Info->ItemsNumber = 0;
            return 0;
        }
        items[i].FileAttributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
        items[i].FileSize = e->Size;
        items[i].NumberOfLinks = 1;
        if (e->HasTime) {
            items[i].LastWriteTime = e->Time;
            items[i].CreationTime  = e->Time;
            items[i].LastAccessTime = e->Time;
        }
        items[i].UserData.Data = (void *)(((uintptr_t)e->Kind << 28) | (uintptr_t)(e->Id & 0x0FFFFFFFu));
        if (e->Flags & AI_LOST)             items[i].Description = GetMsg(MDescUnreadable);
        else if (e->Flags & AI_PARTIAL)     items[i].Description = GetMsg(MDescIncomplete);
        else if (e->Flags & AI_SIZEFIX)     items[i].Description = GetMsg(MDescSizeFixed);
        else if (e->Kind == EK_RECFILE)     items[i].Description = GetMsg(MDescRecFile);
        else if (e->Kind == EK_RECDATA)     items[i].Description = GetMsg(MDescRecData);
        else if (e->Kind == EK_RECDIR && e->Id == 0) items[i].Description = GetMsg(MDescRecDir);
        else if (e->Kind == EK_IMAGE)       items[i].Description = GetMsg(MDescImage);
        else if (e->Kind == EK_REPORT)      items[i].Description = GetMsg(MDescReport);
    }

    Info->PanelItem   = items;
    Info->ItemsNumber = (size_t)n;
    return 1;
}

void WINAPI FreeFindDataW(const struct FreeFindDataInfo *Info) {
    size_t i;
    for (i = 0; i < Info->ItemsNumber; i++) free((void *)Info->PanelItem[i].FileName);
    free(Info->PanelItem);
}

/* The panel is read-only: whatever would change the image is refused with a box, and Far is told that it failed
 * (0). Far calls PutFilesW for F5 and F6 into the panel (and when a file edited with F4 is saved), DeleteFilesW for
 * F8 and Del, MakeDirectoryW for F7. A function that is not exported is taken by Far as "cancelled" (PutFiles,
 * MakeDirectory) or "failed" (DeleteFiles) without a message, so they must exist. Returning 0 matters for a file
 * edited with F4: Far then keeps the edited temporary copy and says where it is, while "cancelled" (-1) makes Far
 * delete that copy. No box while Far works silently */
static intptr_t refuse_write(unsigned long long opmode)
{
    static const GUID ReadOnlyGuid =
    { 0x0851a8f1, 0x1c98, 0x4c48, { 0xa1, 0x57, 0xa1, 0x86, 0x84, 0x58, 0xf4, 0x75 } };   /* 0851A8F1-1C98-4C48-A157-A1868458F475 */
    const wchar_t *items[2];

    if (opmode & (unsigned long long)OPM_SILENT) return 0;
    items[0] = PLUGIN_NAME;                    /* the title of the box */
    items[1] = GetMsg(MReadOnly);
    if (Psi.Message) Psi.Message(&MainGuid, &ReadOnlyGuid, FMSG_WARNING | FMSG_MB_OK, NULL, items, 2, 0);
    return 0;
}

intptr_t WINAPI PutFilesW(const struct PutFilesInfo *Info) {
    return refuse_write(Info ? (unsigned long long)Info->OpMode : 0);
}

intptr_t WINAPI DeleteFilesW(const struct DeleteFilesInfo *Info) {
    return refuse_write(Info ? (unsigned long long)Info->OpMode : 0);
}

intptr_t WINAPI MakeDirectoryW(struct MakeDirectoryInfo *Info) {
    refuse_write(Info ? (unsigned long long)Info->OpMode : 0);
    return -1;                                 /* "cancelled": after 0 Far adds a box of its own, "Cannot create the folder", with no name */
}

intptr_t WINAPI GetFilesW(struct GetFilesInfo *Info) {
    PluginData *pd = (PluginData *)Info->hPanel;
    static wchar_t dest[MAX_PATH];             /* the folder that Far is told about: it reads it after the call */
    wchar_t typed[MAX_PATH * 2];
    XCtx x;
    size_t i;

    if (!pd || !Info->DestPath) return 0;
    if (Info->Move) return refuse_write((unsigned long long)Info->OpMode);   /* F6: a move would delete from the image */
    if (Info->ItemsNumber == 0) return 1;
    if (wcslen(Info->DestPath) >= MAX_PATH) return 0;
    memset(&x, 0, sizeof x);
    x.mode = XM_ASK;                           /* what an existing file gets without a dialog is never decided here */
    x.silent = ((unsigned long long)Info->OpMode & ((unsigned long long)OPM_SILENT | (unsigned long long)OPM_VIEW |
                (unsigned long long)OPM_EDIT | (unsigned long long)OPM_QUICKVIEW | (unsigned long long)OPM_FIND |
                (unsigned long long)OPM_DESCR)) != 0;
    wcscpy(dest, Info->DestPath);
    if (!x.silent) {                           /* F5: the dialog */
        int mode = g_copy_mode;
        int r = copy_dialog(Info, typed, MAX_PATH * 2, &mode);
        if (r == 0) return -1;                 /* cancelled: nothing is done, Far keeps the selection */
        if (r < 0) {                           /* no dialog: nothing is copied, no file is overwritten unasked */
            box_text(MCopyTitle, GetMsg(MErrDialog), NULL);
            return 0;
        }
        g_copy_mode = mode;
        x.mode = mode;
        if (!resolve_dest(Info->DestPath, typed, dest, MAX_PATH) || !make_dirs(dest)) {
            box_text(MCopyTitle, GetMsg(MErrDest), typed);
            return 0;
        }
    }
    for (i = 0; i < Info->ItemsNumber && !x.cancelled; i++) {
        struct PluginPanelItem *item = &Info->PanelItem[i];
        uintptr_t ud = (uintptr_t)item->UserData.Data;
        int kind = (int)(ud >> 28);
        DWORD id = (DWORD)(ud & 0x0FFFFFFFu);
        wchar_t outpath[MAX_PATH];

        if (!join_path(outpath, dest, item->FileName)) {
            x.failed++;
            continue;
        }
        if (kind == EK_DIR || kind == EK_RECDIR) {
            if (make_folder(outpath)) {
                extract_dir(pd, kind, id, outpath, 0, &x);
                set_folder_time(outpath, &item->LastWriteTime);           /* after the contents: they change the folder's date */
            } else {
                x.failed++;
            }
        } else {
            extract_file(pd, kind, id, outpath, &x, &item->LastWriteTime);
        }
    }
    Info->DestPath = dest;
    if (x.cancelled) return -1;
    if ((x.failed || x.partial) && !x.silent) {
        wchar_t fail[96], part[96];
        wfmt(fail, 96, GetMsgFmt(MCopyFailed), x.failed);
        wfmt(part, 96, GetMsgFmt(MCopyPartial), x.partial);
        if (x.failed && x.partial) box_text(MCopyTitle, fail, part);
        else box_text(MCopyTitle, x.failed ? fail : part, NULL);
    }
    return (x.copied > 0 || x.skipped > 0 || x.failed == 0) ? 1 : 0;
}

/* One step of a directory change: "..", a name of the current listing */
static int enter_dir(PluginData *pd, const wchar_t *name)
{
    int i;
    if (wcscmp(name, L".") == 0) return 1;      /* the current directory: nothing to do */
    if (wcscmp(name, L"..") == 0) {
        if (pd->StackDepth > 0) pd->StackDepth--;
        return 1;
    }
    for (i = 0; i < pd->EntryCount; i++) {
        const ADFEntry *e = &pd->Entries[i];
        if ((e->Kind == EK_DIR || e->Kind == EK_RECDIR) && name_equal(e->Name, name)) {
            if (pd->StackDepth + 1 >= MAX_PATH_DEPTH) return 0;
            pd->StackDepth++;
            pd->DirStack[pd->StackDepth].Kind = e->Kind;
            pd->DirStack[pd->StackDepth].Id = e->Id;
            wcsncpy(pd->DirStack[pd->StackDepth].Name, e->Name, MAX_NAME - 1);
            pd->DirStack[pd->StackDepth].Name[MAX_NAME - 1] = 0;
            return 1;
        }
    }
    return 0;
}

intptr_t WINAPI SetDirectoryW(const struct SetDirectoryInfo *Info) {
    PluginData *pd = (PluginData *)Info->hPanel;
    wchar_t path[MAX_PATH], *p, *tok;
    int ok = 1;

    if (!pd || !Info->Dir) return 0;
    wcsncpy(path, Info->Dir, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    p = path;
    if (*p == L'\\' || *p == 0) {                       /* the root (and what follows it, if any) */
        pd->StackDepth = 0;
        while (*p == L'\\') p++;
    }
    /* one or more names separated by backslashes; the listing is reloaded after each step */
    while (*p && ok) {
        tok = p;
        while (*p && *p != L'\\') p++;
        if (*p) *p++ = 0;
        if (*tok) {
            if (!reload_dir(pd)) return 0;
            ok = enter_dir(pd, tok);
        }
    }
    update_current_path(pd);
    if (!reload_dir(pd)) return 0;
    return ok ? 1 : 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) { (void)hinst; (void)reason; (void)reserved; return TRUE; }

HANDLE WINAPI OpenW(const struct OpenInfo *Info) {
    struct OpenAnalyseInfo *oai;
    if (Info->OpenFrom != OPEN_ANALYSE) return NULL;
    oai = (struct OpenAnalyseInfo *)Info->Data;
    if (!oai || !oai->Info) return NULL;

    return OpenFilePluginW(oai->Info->FileName,
                           (const unsigned char *)oai->Info->Buffer,
                           (int)oai->Info->BufferSize,
                           (int)oai->Info->OpMode);
}

#endif /* !ADF_CORE_ONLY */
