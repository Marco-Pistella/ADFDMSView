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
 *     each track is kept: right, repaired, kept but not verified, lost, absent (DMT_*);
 *   - a track header damaged in one or two bytes (or only its CRC field) is repaired; the archive is recognized also
 *     with a damaged signature or first track header;
 *   - damaged packed data are repaired when the damage is a burst of up to 16 bits, two flipped bits, or one byte
 *     lost or added; a repair is accepted only if the track checksum is then right. The search is bounded by
 *     DMS_REPAIR_SECONDS and DMS_REPAIR_BUDGET;
 *   - the data of a track end at the next valid track header; a truncated last track is unpacked as far as it goes;
 *     a track that cannot be verified is kept as unpacked (best effort) so that the tracks that copy from its
 *     dictionary (HEAVY) are not lost too;
 *   - the decoders are bounds-checked; incomplete Huffman tables and matches that run past the end are handled;
 *   - the unpacked image is also shown as <archive>.adf, so that it can be copied out;
 *   - archives that do not hold a disk image (disk type 7) and encrypted archives are refused.
 *
 * ADF reader:
 *   - the image size need not be a multiple of 512 (the whole blocks are used); the bootblock need not be "DOS" when
 *     a valid root block exists; a root block with a wrong checksum is accepted if its hash table still points at
 *     header blocks; with no usable root block the files are found by scanning for file headers;
 *   - OFS or FFS is detected from the data blocks, not only from the bootblock;
 *   - in an OFS disk the data blocks of a file are looked up by (file header, sequence number) when the block table is
 *     damaged, lost, or points at a block of another file; a size field that makes no sense is taken from the data
 *     blocks; a block with a damaged header gives its data as they are;
 *   - files and data blocks that the directory tree does not reach appear in the folder _RECOVERED (paths rebuilt
 *     from the parent pointers; DATA_<block>.bin for data blocks whose file header is lost);
 *   - items that could not be read completely say so in the Description column; _RECOVERY_REPORT.txt (shown only for a
 *     damaged disk) lists what the readers found and repaired.
 *
 * Effort limits at the top of the DMS engine: DMS_REPAIR_SECONDS and DMS_REPAIR_BUDGET (search for repairs), and
 * MAX_FILE_SIZE (biggest image or archive that is opened).
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
#define DMS_REPAIR_BUDGET  150000         /* trial unpacks for the repairs of one archive (bounds the opening time) */
#define DMS_TEXT_SIZE      0x4000         /* largest dictionary (mask 0x3FFF) */
#define DMS_TRACK_DD       11264          /* one cylinder, DD: 2 x 11 x 512 */
#define DMS_TRACK_HD       22528
#define DMS_MAX_LOGGED     20             /* detailed error lines per archive */

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

/* CRC-16/ARC (polynomial 0xA001 reflected, initial value 0): the table is shared by the checks and by the repairs */
static const uint16_t *dms_crc_tab(void)
{
    static uint16_t tab[256];
    static bool init = false;
    if (!init) {
        unsigned i, k;
        for (i = 0; i < 256; i++) {
            unsigned c = i;
            for (k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0xA001u : (c >> 1);
            tab[i] = (uint16_t)c;
        }
        init = true;
    }
    return tab;
}

static uint16_t dms_crc16(const uint8_t *m, size_t n)
{
    const uint16_t *tab = dms_crc_tab();
    uint16_t crc = 0;
    while (n--) crc = (uint16_t)(tab[(crc ^ *m++) & 255] ^ ((crc >> 8) & 255));
    return crc;
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
static void dms_init_decrunchers(dms_ctx_t *x)
{
    x->quick_loc = 251;
    x->medium_loc = 0x3fbe;
    x->heavy_loc = 0;
    x->deep_loc = 0x3fc4;
    x->deep_init = 1;
    memset(x->text, 0, 0x3fc8);
}

/* ---------- RLE (used by every mode but NOCOMP) ---------- */
/*
 * Returns 0, or 2 if the input ended before the output was complete (what is missing is zero; *done = bytes
 * really decoded). A run that is longer than what is left of the output is cut to fit: the Amiga program
 * does it and the track checksum then matches.
 */
static int dms_unpack_rle(const uint8_t *in, size_t inlen, uint8_t *out, uint16_t origsize, size_t *done)
{
    const uint8_t *ie = in + inlen;
    uint8_t *out0 = out, *outend = out + origsize;

    while (out < outend) {
        uint8_t a, b;
        if (in >= ie) goto short_input;
        a = *in++;
        if (a != 0x90) {
            *out++ = a;
        } else {
            unsigned n;
            if (in >= ie) goto short_input;
            b = *in++;
            if (b == 0) {
                *out++ = a;
            } else {
                if (in >= ie) goto short_input;
                a = *in++;
                if (b == 0xff) {
                    if (ie - in < 2) goto short_input;
                    n = ((unsigned)in[0] << 8) | in[1];
                    in += 2;
                } else {
                    n = b;
                }
                if ((size_t)(outend - out) < n) n = (unsigned)(outend - out);
                memset(out, a, n);
                out += n;
            }
        }
    }
    *done = (size_t)(out - out0);
    return 0;
short_input:
    *done = (size_t)(out - out0);
    memset(out, 0, (size_t)(outend - out));
    return 2;
}

/* ---------- QUICK ---------- */
static int dms_unpack_quick(dms_ctx_t *x, const uint8_t *in, size_t inlen, uint8_t *out,
                            uint16_t origsize, size_t *produced)
{
    dms_bits_t b;
    uint8_t *out0 = out, *outend = out + origsize;
    uint16_t i, j;

    dms_bits_init(&b, in, inlen);
    while (out < outend) {
        if (dms_bits_fail(&b)) return 1;
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
    x->quick_loc = (uint16_t)((x->quick_loc + 5) & 0xff);
    *produced = (size_t)(out - out0);
    return dms_bits_fail(&b) ? 1 : 0;
}

/* ---------- MEDIUM ---------- */
static int dms_unpack_medium(dms_ctx_t *x, const uint8_t *in, size_t inlen, uint8_t *out,
                             uint16_t origsize, size_t *produced)
{
    dms_bits_t b;
    uint8_t *out0 = out, *outend = out + origsize;
    uint16_t i, j, c;
    uint8_t u;

    dms_bits_init(&b, in, inlen);
    while (out < outend) {
        if (dms_bits_fail(&b)) return 1;
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
    x->medium_loc = (uint16_t)((x->medium_loc + 66) & 0x3fff);
    *produced = (size_t)(out - out0);
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
                           uint16_t origsize, size_t *produced)
{
    dms_bits_t b;
    uint8_t *out0 = out, *outend = out + origsize;
    uint16_t i, j, c;

    dms_bits_init(&b, in, inlen);
    if (x->deep_init) dms_deep_init_tabs(x);
    while (out < outend) {
        if (dms_bits_fail(&b)) return 1;
        c = dms_deep_decode_char(x, &b);
        if (c < 256) {
            *out++ = x->text[x->deep_loc++ & 0x3fff] = (uint8_t)c;
        } else {
            j = (uint16_t)(c - 255 + DMS_THRESHOLD);
            i = (uint16_t)(x->deep_loc - dms_deep_decode_pos(&b) - 1);
            while (j--) *out++ = x->text[x->deep_loc++ & 0x3fff] = x->text[i++ & 0x3fff];
        }
    }
    x->deep_loc = (uint16_t)((x->deep_loc + 60) & 0x3fff);
    *produced = (size_t)(out - out0);
    return dms_bits_fail(&b) ? 1 : 0;
}

/* ---------- HEAVY 1 / 2 (LZ + static Huffman, from UNIX LHA) ---------- */
/*
 * Decoding table of a canonical Huffman code (lengths in bitlen[0..nchar-1]): the first
 * `tablebits` bits of the stream index `table`; a longer code goes on in the nodes left[] / right[]
 * (indices from nchar up). The code of a symbol is given by its length and by its position among the symbols of
 * that length; lengths go up to tablebits + 16 (17 bits and more occur in DMS 1.11 / 1.12 archives). A code that
 * does not fill the code space is accepted: DMS 1.11 writes one for a track made of a single kind of match, and
 * a decoder is not asked to complete it. What is not assigned decodes as symbol 0. Returns 0, or an error number
 * for an over-subscribed code, a length the decoders cannot read or too many nodes.
 */
static int dms_make_table(dms_ctx_t *x, uint16_t nchar, const uint8_t *bitlen,
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

static int dms_heavy_read_tree_p(dms_ctx_t *x, dms_bits_t *b)
{
    uint16_t i, n;
    n = DMS_GETBITS(b, 5);
    dms_drop(b, 5);
    if (n > 0) {
        if (n > DMS_NPT) return 1;                 /* more lengths than pt_len holds */
        for (i = 0; i < n; i++) {
            x->pt_len[i] = (uint8_t)DMS_GETBITS(b, 4);
            dms_drop(b, 4);
        }
        for (i = n; i < x->np; i++) x->pt_len[i] = 0;
        if (dms_make_table(x, x->np, x->pt_len, 8, x->pt_table)) return 1;
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

static int dms_unpack_heavy(dms_ctx_t *x, const uint8_t *in, size_t inlen, uint8_t *out,
                            uint8_t flags, uint16_t origsize, size_t *produced)
{
    dms_bits_t b;
    uint8_t *out0 = out, *outend = out + origsize;
    uint16_t j, i, c, bitmask;

    if (flags & 8) { x->np = 15; bitmask = 0x1fff; }   /* Heavy 2: 8 KiB dictionary */
    else           { x->np = 14; bitmask = 0x0fff; }   /* Heavy 1: 4 KiB dictionary */
    dms_bits_init(&b, in, inlen);
    if (flags & 2) {
        if (dms_heavy_read_tree_c(x, &b)) return 1;
        if (dms_heavy_read_tree_p(x, &b)) return 2;
    }
    while (out < outend) {
        if (dms_bits_fail(&b)) return 3;
        c = dms_heavy_decode_c(x, &b);
        if (c == 0xFFFF) return 4;
        if (c < 256) {
            *out++ = x->text[x->heavy_loc++ & bitmask] = (uint8_t)c;
        } else {
            uint16_t p = dms_heavy_decode_p(x, &b);
            if (p == 0xFFFF) return 5;
            j = (uint16_t)(c - DMS_OFFSET);
            i = (uint16_t)(x->heavy_loc - p - 1);
            while (j--) *out++ = x->text[x->heavy_loc++ & bitmask] = x->text[i++ & bitmask];
        }
    }
    *produced = (size_t)(out - out0);
    return dms_bits_fail(&b) ? 6 : 0;
}

/* ---------- one track ---------- */
/* b1: packed track (pklen1 bytes); the unpacked track is returned in b2 (unpklen bytes).
 * Both buffers have DMS_TRACK_MAX + DMS_SLACK bytes. Returns 0 or an error number. */
/* One LZ stage (modes 2-6) producing `want` bytes (the last match may run past them: *made) */
static int dms_lz_stage(dms_ctx_t *x, uint8_t cmode, uint8_t flags, uint8_t *b1, size_t pklen1, uint8_t *b2,
                        uint16_t want, size_t *made)
{
    switch (cmode) {
    case 2: return dms_unpack_quick(x, b1, pklen1, b2, want, made);
    case 3: return dms_unpack_medium(x, b1, pklen1, b2, want, made);
    case 4: return dms_unpack_deep(x, b1, pklen1, b2, want, made);
    default: return dms_unpack_heavy(x, b1, pklen1, b2, cmode == 5 ? (uint8_t)(flags & 7) : (uint8_t)(flags | 8),
                                     want, made);
    }
}

/*
 * Unpacks one track into b2. Returns 0 (done), 1 (failed), 2 (unknown mode) or 3 (done, but the RLE data ended
 * before the track was complete: the rest is zero). The RLE stage reads all that the LZ stage produced,
 * also what a last match put past pklen2: pklen2 is the length of the data that the stage must decode, not
 * always the end of the RLE data.
 */
static int dms_unpack_track(dms_ctx_t *x, uint8_t *b1, size_t pklen1, uint8_t *b2,
                            uint16_t pklen2, uint16_t unpklen, uint8_t cmode, uint8_t flags)
{
    size_t made = 0, done = 0;
    int rr = 0;

    switch (cmode) {
    case 0:                                        /* NOCOMP */
        if (unpklen > pklen1) return 1;
        memcpy(b2, b1, unpklen);
        break;
    case 1:                                        /* SIMPLE */
        rr = dms_unpack_rle(b1, pklen1, b2, unpklen, &done);
        break;
    case 2:                                        /* QUICK */
    case 3:                                        /* MEDIUM */
    case 4:                                        /* DEEP */
        if (dms_lz_stage(x, cmode, flags, b1, pklen1, b2, pklen2, &made)) return 1;
        rr = dms_unpack_rle(b2, made, b1, unpklen, &done);
        memcpy(b2, b1, unpklen);
        break;
    case 5:                                        /* HEAVY 1 */
    case 6:                                        /* HEAVY 2 */
        if (dms_lz_stage(x, cmode, flags, b1, pklen1, b2, pklen2, &made)) return 1;
        if (flags & 4) {                           /* RLE only if this flag is set */
            rr = dms_unpack_rle(b2, made, b1, unpklen, &done);
            memcpy(b2, b1, unpklen);
        } else if (unpklen > made) {
            return 1;
        }
        break;
    default:
        return 2;                                  /* unknown mode */
    }
    if (!(flags & 1)) dms_init_decrunchers(x);
    return rr == 2 ? 3 : 0;
}

static const char *dms_mode_name(unsigned m)
{
    static const char *n[7] = { "NOCOMP", "SIMPLE", "QUICK", "MEDIUM", "DEEP", "HEAVY1", "HEAVY2" };
    return m < 7 ? n[m] : "unknown";
}

/* ---------- archive ---------- */
/* "DMS!", the 56-byte archive header and the header of a first track ("TR"): a file that is only a header
 * (56 bytes: valid header CRC, no track) has no disk in it and is not a DMS */
/* "TR" and sane sizes: the part of the test of a track header that does not need the CRC */
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

/* Heavy track lost: the encoder wrote pklen2 bytes in the dictionary for it, so the write position
 * goes on by that much; if the track was to end with a reset, the dictionary is cleared as the encoder did */
static void dms_heavy_lost(dms_ctx_t *x, unsigned flags, uint16_t loc_before, unsigned pklen2)
{
    if (!(flags & 1)) dms_init_decrunchers(x);
    else x->heavy_loc = (uint16_t)(loc_before + pklen2);
}

/* First valid track header ("TR", header CRC, sane sizes) at or after `from`: where a damaged archive goes on */
/* a valid track header at d[pos] (the test dms_resync uses at every offset) */
static bool dms_header_at(const uint8_t *d, uint64_t size, uint64_t pos)
{
    const uint8_t *t;
    if (pos + DMS_THLEN > size) return false;
    t = d + pos;
    return dms_hdr_sane(t) && dms_crc16(t, DMS_THLEN - 2) == (uint16_t)(((unsigned)t[18] << 8) | t[19]);
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
 * consistency test, only if it is the only candidate.
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
 * CRC of those data is the one in the candidate (the strongest proof: it rejects candidates that changed the
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

/*
 * Repair of a track whose packed data are damaged. CRC-16/ARC with initial value 0 is linear: the CRC of
 * damaged data differs from the stored CRC by a value that depends only on the error pattern and its position, so
 * the CRC alone points at the candidates, and the 16-bit checksum of the unpacked track (and a trial unpack) tells
 * which candidate is right. Three kinds of damage are repaired:
 *   - substitution: any error confined to a window of 16 bits (one bit, one byte, two adjacent bytes, ...). CRC-16
 *     detects every such burst and, for a given position, exactly one pattern explains the CRC difference;
 *   - one byte lost inside the track data (the data are one byte shorter than the header says): the byte is
 *     re-inserted at every position, with every value that satisfies the CRC;
 *   - one byte added (one byte longer): each byte is removed in turn.
 * A repair is accepted only if the unpacked track has the checksum of the track header. The decoder state is
 * always left as it was before the track (x == xs on return); the caller unpacks the repaired data again.
 */
#define DMS_REPAIR_MAX_TRIES 40000


static uint16_t dms_crc_tab_at(unsigned i) { return dms_crc_tab()[i & 255]; }

/* L: state transition of the CRC for a zero input byte */
static uint16_t dms_crc_L(uint16_t s) { return (uint16_t)(dms_crc_tab_at(s) ^ (s >> 8)); }

static uint16_t dms_zval(const uint16_t zb[8], unsigned e)
{
    uint16_t v = 0;
    unsigned j;
    for (j = 0; j < 8; j++) if ((e >> j) & 1u) v ^= zb[j];
    return v;
}

/* M v, where the columns of M are col[j] = M(1 << j) */
static uint16_t dms_apply(const uint16_t col[16], uint16_t v)
{
    uint16_t r = 0;
    unsigned q;
    for (q = 0; q < 16; q++) if ((v >> q) & 1u) r ^= col[q];
    return r;
}

/* trial: unpack `cand` (pklen1 bytes) from the state xs (x is left as the unpack made it); true if the track
 * checksum is the expected one */
/* The search for repairs is bounded: by a number of trial unpacks and by time, so that opening a damaged archive in
 * the panel does not take for ever. Raise DMS_REPAIR_SECONDS to search longer. */
#ifndef DMS_REPAIR_SECONDS
#  define DMS_REPAIR_SECONDS 6
#endif
static long g_dms_budget;                              /* trial unpacks left for this archive */
static clock_t g_dms_deadline;
static bool g_dms_budget_hit;

static bool dms_budget_over(void)
{
    if (g_dms_budget > 0 && ((g_dms_budget & 63) != 0 || clock() < g_dms_deadline)) return false;
    g_dms_budget_hit = true;
    g_dms_budget = 0;
    return true;
}

static bool dms_trial(dms_ctx_t *x, const dms_ctx_t *xs, const uint8_t *cand, size_t pklen1, uint8_t *b1, uint8_t *b2,
                      uint16_t pklen2, uint16_t unpklen, uint8_t cmode, uint8_t flags, uint16_t usum)
{
    int r;
    g_dms_budget--;
    *x = *xs;
    memcpy(b1, cand, pklen1);
    r = dms_unpack_track(x, b1, pklen1, b2, pklen2, unpklen, cmode, flags);
    return (r == 0 || r == 3) && dms_checksum(b2, unpklen) == usum;      /* the caller restores x from xs */
}

/* substitution (burst of at most 16 bits). `orig` has pklen1 bytes; on success the fixed data are in `fixed` */
static bool dms_fix_burst(dms_ctx_t *x, const dms_ctx_t *xs, const uint8_t *orig, size_t pklen1, uint16_t dcrc,
                          uint8_t *fixed, uint8_t *b1, uint8_t *b2, uint16_t pklen2, uint16_t unpklen,
                          uint8_t cmode, uint8_t flags, uint16_t usum, size_t *rpos)
{
    uint16_t delta = (uint16_t)(dms_crc16(orig, pklen1) ^ dcrc);
    uint16_t z0[8], z1[8], z2[8];
    unsigned tries = 0, j, t;
    size_t p;

    if (pklen1 == 0 || delta == 0) return false;
    memcpy(fixed, orig, pklen1);                                /* the candidates are applied to `fixed` and undone */
    for (j = 0; j < 8; j++) { z0[j] = dms_crc_tab_at(1u << j); z1[j] = z2[j] = 0; }
    for (p = pklen1; p-- > 0; ) {
        /* z0: effect of an error byte at p (followed by n-1-p zero bytes); z1 at p+1; z2 at p+2 */
        for (t = 0; t < 8; t++) {
            uint16_t bv[16], bm[16], c[16], v, w = 0;
            unsigned bit, m;
            uint32_t E;
            bool ok = true, tr;
            uint8_t eb[3];

            for (j = 0; j < 16; j++) {
                E = (uint32_t)1u << (j + (8 - t));
                c[j] = (uint16_t)(dms_zval(z0, (E >> 16) & 255) ^ dms_zval(z1, (E >> 8) & 255) ^
                                  dms_zval(z2, E & 255));
            }
            memset(bv, 0, sizeof(bv));
            memset(bm, 0, sizeof(bm));
            for (j = 0; j < 16; j++) {
                v = c[j];
                m = 1u << j;
                for (bit = 16; bit-- > 0; ) {
                    if (!((v >> bit) & 1u)) continue;
                    if (bv[bit]) { v ^= bv[bit]; m ^= bm[bit]; }
                    else { bv[bit] = v; bm[bit] = (uint16_t)m; break; }
                }
            }
            v = delta;
            for (bit = 16; bit-- > 0; ) {
                if (!((v >> bit) & 1u)) continue;
                if (!bv[bit]) { ok = false; break; }
                v ^= bv[bit];
                w ^= bm[bit];
            }
            if (!ok || v != 0 || !(w & 0x8000u)) continue;     /* the burst starts at its top bit: counted once */
            E = (uint32_t)w << (8 - t);
            eb[0] = (uint8_t)(E >> 16);
            eb[1] = (uint8_t)(E >> 8);
            eb[2] = (uint8_t)E;
            if (p + 1 >= pklen1 && eb[1]) continue;
            if (p + 2 >= pklen1 && eb[2]) continue;
            if (tries++ >= DMS_REPAIR_MAX_TRIES || dms_budget_over()) return false;
            /* the CRC is right by construction (the pattern was solved from it): no CRC check here */
            fixed[p] ^= eb[0];
            if (p + 1 < pklen1) fixed[p + 1] ^= eb[1];
            if (p + 2 < pklen1) fixed[p + 2] ^= eb[2];
            tr = dms_trial(x, xs, fixed, pklen1, b1, b2, pklen2, unpklen, cmode, flags, usum);
            if (tr && dms_crc16(fixed, pklen1) == dcrc) { *rpos = p; return true; }
            fixed[p] ^= eb[0];                                   /* undo */
            if (p + 1 < pklen1) fixed[p + 1] ^= eb[1];
            if (p + 2 < pklen1) fixed[p + 2] ^= eb[2];
        }
        memcpy(z2, z1, sizeof(z2));
        memcpy(z1, z0, sizeof(z1));
        for (j = 0; j < 8; j++) z0[j] = dms_crc_L(z0[j]);
    }
    return false;
}

/* substitution: TWO flipped bits anywhere in the track (CRC meet-in-the-middle: a hash of the CRC effect of every
 * bit, then for each bit the partner whose effect completes the CRC difference) */
static bool dms_fix_twobits(dms_ctx_t *x, const dms_ctx_t *xs, const uint8_t *orig, size_t pklen1, uint16_t dcrc,
                            uint8_t *fixed, uint8_t *b1, uint8_t *b2, uint16_t pklen2, uint16_t unpklen,
                            uint8_t cmode, uint8_t flags, uint16_t usum, size_t *rpos)
{
    uint16_t delta = (uint16_t)(dms_crc16(orig, pklen1) ^ dcrc);
    size_t nb = pklen1 * 8, i, p;
    uint16_t *c;
    int32_t *head, *next;
    uint16_t zb[8];
    unsigned j, tries = 0;
    bool found = false;

    if (pklen1 == 0 || delta == 0 || pklen1 > 8192) return false;
    c = (uint16_t *)xmalloc(nb * sizeof(uint16_t));
    next = (int32_t *)xmalloc(nb * sizeof(int32_t));
    head = (int32_t *)xmalloc(65536 * sizeof(int32_t));
    for (i = 0; i < 65536; i++) head[i] = -1;
    for (j = 0; j < 8; j++) zb[j] = dms_crc_tab_at(1u << j);
    for (p = pklen1; p-- > 0; ) {
        for (j = 0; j < 8; j++) {
            size_t pos = p * 8 + j;
            c[pos] = zb[j];
            next[pos] = head[zb[j]];
            head[zb[j]] = (int32_t)pos;
            zb[j] = dms_crc_L(zb[j]);
        }
    }
    memcpy(fixed, orig, pklen1);
    for (i = 0; i < nb && !found; i++) {
        int32_t q;
        for (q = head[(uint16_t)(delta ^ c[i])]; q >= 0; q = next[q]) {
            size_t pa = i / 8, pb = (size_t)q / 8;
            uint8_t ma = (uint8_t)(1u << (i % 8)), mb = (uint8_t)(1u << ((size_t)q % 8));
            if ((size_t)q <= i) continue;
            if (tries++ >= DMS_REPAIR_MAX_TRIES || dms_budget_over()) goto done;
            fixed[pa] ^= ma;                                     /* the CRC is right by construction */
            fixed[pb] ^= mb;
            if (dms_trial(x, xs, fixed, pklen1, b1, b2, pklen2, unpklen, cmode, flags, usum) &&
                dms_crc16(fixed, pklen1) == dcrc) {
                *rpos = pa;
                found = true;
                break;
            }
            fixed[pa] ^= ma;                                     /* undo */
            fixed[pb] ^= mb;
        }
    }
done:
    free(c);
    free(next);
    free(head);
    return found;
}

/* one byte lost: `reg` has reglen = pklen1 - 1 bytes. A byte e is inserted at position p (0..reglen) */
static bool dms_fix_lost(dms_ctx_t *x, const dms_ctx_t *xs, const uint8_t *reg, size_t reglen, uint16_t dcrc,
                         uint8_t *fixed, uint8_t *b1, uint8_t *b2, uint16_t pklen2, uint16_t unpklen,
                         uint8_t cmode, uint8_t flags, uint16_t usum, size_t *rpos)
{
    size_t pklen1 = reglen + 1, p, i;
    uint16_t *P = (uint16_t *)xmalloc((reglen + 1) * sizeof(uint16_t));    /* P[p]: CRC state after reg[0..p) */
    uint16_t col[16], total, s;
    unsigned tries = 0, j, e;
    bool found = false;

    s = 0;
    for (p = 0; p <= reglen; p++) {
        P[p] = s;
        if (p < reglen) s = (uint16_t)(dms_crc_L(s) ^ dms_crc_tab_at(reg[p]));
    }
    total = s;
    for (j = 0; j < 16; j++) col[j] = (uint16_t)(1u << j);        /* M = L^m with m = |B| = reglen - p; start p = reglen */
    for (p = reglen + 1; p-- > 0 && !found; ) {
        uint16_t Sx, aL, base;
        Sx = (uint16_t)(total ^ dms_apply(col, P[p]));                  /* CRC of the suffix reg[p..) */
        aL = dms_apply(col, dms_crc_L(P[p]));
        base = (uint16_t)(dcrc ^ Sx ^ aL);                       /* needed: apply(M, tab[e]) == base */
        for (e = 0; e < 256; e++) {
            uint16_t te = dms_crc_tab_at(e);
            if (dms_apply(col, te) != base) continue;
            if (tries++ >= DMS_REPAIR_MAX_TRIES || dms_budget_over()) { found = false; goto done; }
            for (i = 0; i < p; i++) fixed[i] = reg[i];
            fixed[p] = (uint8_t)e;
            for (i = p; i < reglen; i++) fixed[i + 1] = reg[i];
            if (dms_crc16(fixed, pklen1) != dcrc) continue;
            if (dms_trial(x, xs, fixed, pklen1, b1, b2, pklen2, unpklen, cmode, flags, usum)) {
                *rpos = p;
                found = true;
                break;
            }
        }
        for (j = 0; j < 16; j++) col[j] = dms_crc_L(col[j]);
    }
done:
    free(P);
    return found;
}

/* one byte added: `reg` has reglen = pklen1 + 1 bytes; each byte is removed in turn. With P[p] the CRC state after
 * reg[0..p), M = L^|B| for the part B after the byte and `total` the state after all of reg, the CRC after
 * removing the byte p is M (P[p] ^ P[p+1]) ^ total: O(1) per position, instead of a CRC of the whole track */
static bool dms_fix_extra(dms_ctx_t *x, const dms_ctx_t *xs, const uint8_t *reg, size_t reglen, uint16_t dcrc,
                          uint8_t *fixed, uint8_t *b1, uint8_t *b2, uint16_t pklen2, uint16_t unpklen,
                          uint8_t cmode, uint8_t flags, uint16_t usum, size_t *rpos)
{
    size_t pklen1 = reglen - 1, p;
    uint16_t *P = (uint16_t *)xmalloc((reglen + 1) * sizeof(uint16_t));
    uint16_t col[16], s = 0;
    unsigned j;
    bool found = false;

    for (p = 0; p <= reglen; p++) {
        P[p] = s;
        if (p < reglen) s = (uint16_t)(dms_crc_L(s) ^ dms_crc_tab_at(reg[p]));
    }
    for (j = 0; j < 16; j++) col[j] = (uint16_t)(1u << j);
    for (p = reglen; p-- > 0 && !found; ) {
        if ((uint16_t)(dms_apply(col, (uint16_t)(P[p] ^ P[p + 1])) ^ s) == dcrc) {
            memcpy(fixed, reg, p);
            memcpy(fixed + p, reg + p + 1, reglen - p - 1);
            if (dms_trial(x, xs, fixed, pklen1, b1, b2, pklen2, unpklen, cmode, flags, usum)) {
                *rpos = p;
                found = true;
            }
        }
        for (j = 0; j < 16; j++) col[j] = dms_crc_L(col[j]);
    }
    free(P);
    return found;
}

/* ---------------------------------------------------------------------------------------------------- */
/* DMS archive -> disk image, in memory                                                                     */
/* ---------------------------------------------------------------------------------------------------- */

/* what became of each track (cylinder) of the disk */
enum {
    DMT_ABSENT = 0,      /* not in the archive: zeros */
    DMT_OK,              /* CRC of the packed data and checksum of the unpacked track are right */
    DMT_REPAIRED,        /* the header or the packed data were damaged and repaired: CRC and checksum are right */
    DMT_UNVERIFIED,      /* the packed data are intact (CRC) but the checksum of the track is not the expected one */
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
    REFUSE_ENCRYPTED                       /* an encrypted DMS archive */
};

typedef struct {
    uint8_t *image;                        /* malloc'ed disk image (zeros where there is no data) */
    size_t   image_size;
    unsigned tsize;                        /* bytes per track */
    unsigned maxnum;                       /* highest track found */
    uint8_t  st[DMS_MAX_TRACKS];           /* DT_* per track */
    unsigned n_ok, n_repaired, n_unverified, n_best, n_lost, n_absent;
    unsigned hdr_repaired;                 /* track headers repaired */
    bool     hdr_ok;                       /* the archive header was right */
    bool     sig_ok;                       /* the signature "DMS!" was right */
    bool     budget_hit;                   /* the search for repairs was stopped by the effort limit */
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

/*
 * Unpacks a DMS archive held in memory. Returns 0 if a disk image was produced (o->image, also when tracks are
 * missing or damaged: see o->st), -1 if the archive cannot be used (the report says why). Track headers are
 * repaired (one or two damaged bytes, the CRC field), packed data are repaired (burst of up to 16 bits, two
 * bits, a lost or an added byte) and accepted only if the track checksum is right, tracks end at the next
 * valid header, a truncated last track is unpacked as far as it goes, damaged tracks are kept as unpacked
 * (best effort) so that the tracks that copy from them are not lost too.
 */
static int dms_unpack_image(const uint8_t *data, uint64_t size, DmsOut *o)
{
    uint8_t *b1 = NULL, *b2 = NULL, *b0 = NULL, *fx = NULL, *img = NULL;
    uint64_t pos;
    dms_ctx_t *x = NULL, *xs = NULL;
    unsigned geninfo, from, to, disktype, cmode_hdr, n_ok = 0, n_err = 0;
    unsigned tsize = 0, maxnum = 0, last_num = 0, exp_num, tmax = DMS_STD_TRACKS;
    uint8_t hfix[DMS_THLEN];
    unsigned n_shown = 0;
    bool any_error = false, hdr_ok = true, got[DMS_MAX_TRACKS];
    bool state_bad = false;                    /* QUICK..DEEP: the state left by a damaged track is missing */
    bool degraded = false;                     /* HEAVY: a track was lost or damaged: the next ones are kept as unpacked, reported as not verified */
    bool trees_ok = true;                      /* HEAVY: the Huffman trees in use are known */
    int rc = 0;

    memset(o, 0, sizeof(*o));
    memset(got, 0, sizeof(got));
    g_dms_budget = DMS_REPAIR_BUDGET;
    g_dms_deadline = clock() + (clock_t)DMS_REPAIR_SECONDS * CLOCKS_PER_SEC;
    g_dms_budget_hit = false;
    if (!dms_is_signature(data, size < 512 ? (size_t)size : 512, size)) {
        log_error("DMS: invalid signature");
        o->refuse = REFUSE_NODISK;
        return -1;
    }

    /* archive header (56 bytes): CRC over bytes 4..53 */
    if (dms_crc16(data + 4, DMS_HEADLEN - 6) != (uint16_t)(((unsigned)data[DMS_HEADLEN - 2] << 8) | data[DMS_HEADLEN - 1])) {
        dms_log_error(&n_shown, "DMS: INVALID HEADER: archive header CRC mismatch (disk information ignored)");
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
    if (!hdr_ok) { geninfo = 0; disktype = 0; to = 0xFFFF; from = 0; }
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
    if (geninfo & 2) {
        log_error("DMS: UNSUPPORTED METHOD (encrypted)");
        o->refuse = REFUSE_ENCRYPTED;
        return -1;
    }

    x = (dms_ctx_t *)xcalloc(1, sizeof(*x));
    b1 = (uint8_t *)xcalloc(1, DMS_TRACK_MAX + DMS_SLACK);
    b2 = (uint8_t *)xcalloc(1, DMS_TRACK_MAX + DMS_SLACK);
    b0 = (uint8_t *)xcalloc(1, DMS_TRACK_MAX + DMS_SLACK);
    fx = (uint8_t *)xcalloc(1, DMS_TRACK_MAX + DMS_SLACK);
    xs = (dms_ctx_t *)xcalloc(1, sizeof(*x));
    dms_init_decrunchers(x);

    pos = DMS_HEADLEN;
    for (;;) {
        unsigned hcrc, dcrc, usum, number, pklen1, pklen2, unpklen, cmode, flags;
        const uint8_t *th;
        int r = 1;                                 /* 1: no unpack attempted (DMS_LOSE_TRACK reads it) */
        bool crc_ok, sum_ok, indel, repaired = false, hdr_fixed = false;
        unsigned stat_new = DMT_OK;
        uint64_t npos = 0, after;
        size_t reglen;
        uint16_t loc_before;

        if (pos >= size) break;                    /* clean end of the data */
        th = data + pos;
        if (size - pos >= DMS_THLEN && !dms_header_at(data, size, pos) && !(n_ok > 0 && last_num >= to) &&
            dms_fix_header(data, size, pos, exp_num, hfix)) {
            /* one damaged byte in the header: the track is not lost any more */
            if (n_shown++ < DMS_MAX_LOGGED)
                log_note("DMS: track header at offset %llu damaged and repaired (track %u)",
                         (unsigned long long)pos, ((unsigned)hfix[2] << 8) | hfix[3]);
            th = hfix;
            hdr_fixed = true;
            o->hdr_repaired++;
        }
        if (size - pos < DMS_THLEN || th[0] != 'T' || th[1] != 'R') {
            /* whatever follows the last track (text, zeros) is not an error: the highest track
             * number of the archive header decides, if it was reached the rest is ignored */
            if (n_ok > 0 && last_num >= to) break;
            if (dms_resync(data, size, pos + 1, &npos)) goto resync;
            dms_log_error(&n_shown, "DMS: INVALID HEADER: track header expected at offset %llu (%llu bytes left)",
                      (unsigned long long)pos, (unsigned long long)(size - pos));
            any_error = true;
            n_err++;
            break;
        }
        hcrc = ((unsigned)th[18] << 8) | th[19];
        if (dms_crc16(th, DMS_THLEN - 2) != hcrc) {
            if (dms_resync(data, size, pos + 1, &npos)) goto resync;
            dms_log_error(&n_shown, "DMS: INVALID HEADER: track header CRC mismatch at offset %llu, cannot continue",
                      (unsigned long long)pos);
            any_error = true;
            n_err++;
            break;
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

        if (pklen1 > DMS_TRACK_MAX || pklen2 > DMS_TRACK_MAX || unpklen > DMS_TRACK_MAX) {
            if (dms_resync(data, size, pos + 1, &npos)) goto resync;
            dms_log_error(&n_shown, "DMS: INVALID HEADER: track %u too big, cannot continue", number);
            any_error = true;
            n_err++;
            break;
        }
        /* a byte lost or added inside the data of this track (or a damaged length) shows as a next valid
         * header that is not where pklen1 says: the region up to it is the data of this track */
        reglen = pklen1;
        after = pos + DMS_THLEN + pklen1;
        indel = false;
        if (after < size ? !dms_header_at(data, size, after) : after > size) {
            uint64_t nh = 0;
            bool have = dms_resync(data, size, pos + DMS_THLEN, &nh);
            uint64_t gap = have ? nh - (pos + DMS_THLEN) : size - (pos + DMS_THLEN);
            if (gap + 1 == pklen1 || gap == (uint64_t)pklen1 + 1) {
                indel = true;
                reglen = (size_t)gap;
                after = have ? nh : size;
            }
        }
        if (!indel && size - pos - DMS_THLEN < pklen1) {
            /* the archive ends inside this track: what is there is unpacked (the rest of the packed data is
             * zero) and kept as a best effort: the beginning of the track is right */
            if (size - pos - DMS_THLEN >= 16 && number < DMS_MAX_TRACKS && unpklen > 2048) {
                indel = true;
                reglen = (size_t)(size - pos - DMS_THLEN);
                after = size;
                dms_log_error(&n_shown, "DMS: TRUNCATED, track %u (%llu of %u packed bytes present)", number, (unsigned long long)reglen,
                              pklen1);
                any_error = true;
                n_err++;
            } else {
                dms_log_error(&n_shown, "DMS: TRUNCATED, track %u", number);
                any_error = true;
                n_err++;
                break;
            }
        }
        if (indel) {
            memcpy(b0, data + pos + DMS_THLEN, reglen);
            memcpy(b1, b0, reglen < pklen1 ? reglen : pklen1);
            if (reglen < pklen1) memset(b1 + reglen, 0, pklen1 - reglen);
        } else {
            memcpy(b1, data + pos + DMS_THLEN, pklen1);
        }
        pos = after;

        /* tracks 80 (FILEID.DIZ), 0xFFFF (banner) and the 1 KiB fake boot block are not disk data; the 160 tracks of
         * 5632 bytes are */
        if (unpklen <= 2048 || number >= DMS_MAX_TRACKS || (number >= DMS_STD_TRACKS && unpklen != DMS_TRACK_HALF))
            continue;

        loc_before = x->heavy_loc;
        /* A lost track (discarded): the stateful modes depend on what it left. QUICK..DEEP: the state is unknown up
         * to the first track that ends with a reset. HEAVY: the write position of the dictionary is kept right
         * (pklen2), the trees are unknown only if the lost track carried them, and the next tracks are decoded and
         * kept only if their checksum is right (what they copy from the lost track is wrong). */
#define DMS_LOSE_TRACK() do {                                                                       \
            any_error = true;                                                                       \
            n_err++;                                                                                \
            o->st[number] = DMT_LOST;                                                                \
            if (cmode >= 5) {                                                                       \
                dms_heavy_lost(x, flags, loc_before, pklen2);                                       \
                degraded = true;                                                                    \
                if ((flags & 2) && r != 0 && r != 3) trees_ok = false;   /* trees not read */       \
            } else {                                                                                \
                dms_init_decrunchers(x);                                                            \
                if (cmode >= 2 && (flags & 1)) state_bad = true;                                    \
            }                                                                                       \
        } while (0)

        if (state_bad && cmode >= 2 && cmode <= 4) {
            /* the state is unknown, but a track that does not need it still unpacks right: if its packed
             * data are intact (CRC) and the track checksum is right, it is kept */
            if (!indel && dms_crc16(b1, pklen1) == dcrc) {
                r = dms_unpack_track(x, b1, pklen1, b2, (uint16_t)pklen2, (uint16_t)unpklen, (uint8_t)cmode,
                                     (uint8_t)flags);
                if (r == 0 && dms_checksum(b2, unpklen) == usum) {
                    if (n_shown++ < DMS_MAX_LOGGED)
                        log_note("DMS: track %u: it did not need the state of the damaged track: track kept (verified)",
                                 number);
                    crc_ok = true;
                    sum_ok = true;
                    if (!(flags & 1)) state_bad = false;
                    goto store_track;
                }
                dms_init_decrunchers(x);
            }
            dms_log_error(&n_shown, "DMS: track %u discarded: it depends on the state of a damaged track", number);
            any_error = true;
            n_err++;
            o->st[number] = DMT_LOST;
            if (!(flags & 1)) { state_bad = false; dms_init_decrunchers(x); }
            continue;
        }
        r = 1;
        if (cmode >= 5 && !trees_ok && !(flags & 2)) {
            dms_log_error(&n_shown, "DMS: track %u discarded: its Huffman trees are in a damaged track", number);
            DMS_LOSE_TRACK();
            continue;
        }
        crc_ok = dms_crc16(b1, pklen1) == dcrc && !indel;
        if (!crc_ok) {
            *xs = *x;                              /* the state before the track, for the repair */
            if (!indel) memcpy(b0, b1, pklen1);
        }
        /* a track whose packed-data CRC is wrong is unpacked all the same: if the track checksum then is right the
         * data are good (damaged padding bits, or a wrong CRC field) */
        r = dms_unpack_track(x, b1, pklen1, b2, (uint16_t)pklen2, (uint16_t)unpklen, (uint8_t)cmode,
                             (uint8_t)flags);
        if (!crc_ok && r != 2) {
            /* wrong CRC and no verified result: look for what damaged the packed data. `fixed` is the
             * second work buffer of the repair (b1 and b2 are used by the trial unpacks) */
            size_t rp = 0;
            const char *what = NULL;
            *x = *xs;
            if (indel && reglen + 1 == pklen1) {
                if (dms_fix_lost(x, xs, b0, reglen, (uint16_t)dcrc, fx, b1, b2, (uint16_t)pklen2,
                                 (uint16_t)unpklen, (uint8_t)cmode, (uint8_t)flags, (uint16_t)usum, &rp))
                    what = "a lost byte was re-inserted at";
            } else if (indel && reglen == pklen1 + 1) {
                if (dms_fix_extra(x, xs, b0, reglen, (uint16_t)dcrc, fx, b1, b2, (uint16_t)pklen2,
                                  (uint16_t)unpklen, (uint8_t)cmode, (uint8_t)flags, (uint16_t)usum, &rp))
                    what = "an extra byte was removed at";
            } else if (indel) {
                /* more than one byte missing: nothing to repair */
            } else if (dms_fix_burst(x, xs, b0, pklen1, (uint16_t)dcrc, fx, b1, b2, (uint16_t)pklen2,
                                     (uint16_t)unpklen, (uint8_t)cmode, (uint8_t)flags, (uint16_t)usum, &rp)) {
                what = "damaged packed data were corrected at";
            } else if (dms_fix_twobits(x, xs, b0, pklen1, (uint16_t)dcrc, fx, b1, b2, (uint16_t)pklen2,
                                       (uint16_t)unpklen, (uint8_t)cmode, (uint8_t)flags, (uint16_t)usum, &rp)) {
                what = "two damaged bits were corrected near";
            }
            if (what) {
                memcpy(b1, fx, pklen1);
                *x = *xs;
                r = dms_unpack_track(x, b1, pklen1, b2, (uint16_t)pklen2, (uint16_t)unpklen, (uint8_t)cmode,
                                     (uint8_t)flags);
                crc_ok = true;
                repaired = true;
                if (n_shown++ < DMS_MAX_LOGGED)
                    log_note("DMS: track %u repaired: %s offset %llu of the packed data (CRC and track checksum are "
                             "right)", number, what, (unsigned long long)rp);
            } else {
                /* no repair: unpack the data as they are (the trials spoiled b1, b2 and x) */
                *x = *xs;
                memcpy(b1, b0, reglen < pklen1 ? reglen : pklen1);
                if (reglen < pklen1) memset(b1 + reglen, 0, pklen1 - reglen);
                r = dms_unpack_track(x, b1, pklen1, b2, (uint16_t)pklen2, (uint16_t)unpklen, (uint8_t)cmode,
                                     (uint8_t)flags);
                if (r != 0 && r != 3) *x = *xs;    /* a failed unpack leaves garbage: the state before the track */
            }
        }
        if (r == 2) {
            dms_log_error(&n_shown, "DMS: UNSUPPORTED METHOD %u, track %u discarded", cmode, number);
            DMS_LOSE_TRACK();
            continue;
        }
        if (r == 1) {
            if (crc_ok)
                dms_log_error(&n_shown, "DMS: CRC (decompression failed, mode %s), track %u discarded",
                              dms_mode_name(cmode), number);
            else
                dms_log_error(&n_shown, "DMS: CRC mismatch, track %u discarded", number);
            DMS_LOSE_TRACK();
            continue;
        }
        if (cmode >= 5 && (flags & 2)) trees_ok = true;            /* the trees of this track were read */
        sum_ok = (r == 0) && dms_checksum(b2, unpklen) == usum;
        if (!sum_ok && (!crc_ok || degraded)) {
            if (r == 0 || r == 3) {
                /* best effort: the stream unpacks to a whole track but the track is not verified (packed data
                 * damaged and not repairable, or copies from a damaged track). What was unpacked is kept: the part
                 * before the damage is right, and zeros would lose it too. The state it leaves is the one the next
                 * tracks have to use. */
                dms_log_error(&n_shown, "DMS: track %u: %s: track kept as unpacked (best effort, not verified)",
                              number,
                              crc_ok ? "track checksum mismatch after a damaged track"
                                     : "the packed data are damaged and cannot be repaired");
                any_error = true;
                n_err++;
                stat_new = DMT_BEST;
                if (!crc_ok) {
                    if (cmode >= 5) degraded = true;
                    else if (flags & 1) state_bad = true;
                }
            } else {
                if (crc_ok)
                    dms_log_error(&n_shown, "DMS: track %u: track checksum mismatch after a damaged track, discarded",
                                  number);
                else
                    dms_log_error(&n_shown, "DMS: CRC mismatch, track %u discarded", number);
                DMS_LOSE_TRACK();
                continue;
            }
        } else if (!crc_ok) {
            if (n_shown++ < DMS_MAX_LOGGED)
                log_note("DMS: track %u: the CRC of the packed data is wrong but the track checksum is right: "
                         "track kept", number);
        } else if (!sum_ok) {
            /* the packed data are intact (CRC) and the stream decodes: the track is kept, its content is not
             * verified. The dictionary left by the stream is the right one for the next tracks. */
            dms_log_error(&n_shown, "DMS: track %u: %s (mode %s): track kept, not verified", number,
                          r == 3 ? "the RLE data end before the track does (the rest is zero)" : "track checksum mismatch",
                          dms_mode_name(cmode));
            any_error = true;
            n_err++;
            stat_new = DMT_UNVERIFIED;
        }
#undef DMS_LOSE_TRACK
        if (stat_new == DMT_OK && (repaired || hdr_fixed)) stat_new = DMT_REPAIRED;
store_track:
        if (tsize == 0) {
            tsize = unpklen;
            tmax = dms_tracks_for(tsize);
            img = (uint8_t *)xcalloc(tmax, tsize);
        } else if (unpklen != tsize) {
            dms_log_error(&n_shown, "DMS: track %u has size %u instead of %u, discarded", number, unpklen, tsize);
            any_error = true;
            n_err++;
            o->st[number] = DMT_LOST;
            continue;
        }
        if (number >= tmax) continue;              /* a track the image cannot hold (160-track numbers in a 80-track disk) */
        memcpy(img + (size_t)number * tsize, b2, tsize);
        got[number] = true;
        o->st[number] = (uint8_t)stat_new;
        if (number > maxnum) maxnum = number;
        last_num = number;
        n_ok++;
        continue;
resync:                                            /* damaged header: go on at the next valid one */
        dms_log_error(&n_shown, "DMS: INVALID HEADER at offset %llu: %llu bytes skipped, the archive goes on at offset %llu",
                      (unsigned long long)pos, (unsigned long long)(npos - pos), (unsigned long long)npos);
        any_error = true;
        n_err++;
        degraded = true;                           /* the tracks in the gap are lost, and with them their state */
        state_bad = true;
        pos = npos;
    }

    if (n_ok == 0) {
        log_error("DMS: no disk track found in the archive");
        any_error = true;
        o->refuse = REFUSE_NODISK;
        rc = -1;
    } else {
        /* a standard DD or HD disk is always written at its full size (80 cylinders), missing tracks are
         * zero; any other geometry is written up to its highest track */
        size_t imgsize = (tsize == DMS_TRACK_DD || tsize == DMS_TRACK_HD || tsize == DMS_TRACK_HALF)
                             ? (size_t)tmax * tsize
                             : (size_t)(maxnum + 1) * tsize;
        unsigned t, missing = 0;

        if (tsize == DMS_TRACK_DD || tsize == DMS_TRACK_HD) imgsize = (size_t)DMS_STD_TRACKS * tsize;
        for (t = 0; t <= maxnum; t++) if (!got[t]) missing++;
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
        log_msg("  DMS: %u further errors not shown (only the first %d are listed)", n_shown - DMS_MAX_LOGGED,
                DMS_MAX_LOGGED);
    if (g_dms_budget_hit) {
        o->budget_hit = true;
        log_msg("  DMS: the search for repairs was stopped by the effort limit (%d trial unpacks or %d seconds)", DMS_REPAIR_BUDGET, DMS_REPAIR_SECONDS);
    }
    o->any_error = any_error;
    log_msg("  DMS: %u tracks extracted, %u in error", n_ok, n_err);
    free(img);
    free(b1);
    free(b2);
    free(b0);
    free(fx);
    free(xs);
    free(x);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------- */
/* ADF file system reader (OFS/FFS) with the recovery of damaged disks                                    */
/* ---------------------------------------------------------------------------------------------------- */

#define ADF_BLOCK_SIZE  512
#define T_HEADER   2
#define T_DATA     8
#define T_LIST     16
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
    if (img->noidx > 1) qsort(img->oidx, img->noidx, sizeof(*img->oidx), adf_oent_cmp);
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

/*
 * One data block of an OFS file: `db` is what the block table says (0 if the table is lost), `seq` the
 * position (from 1) in the file. A block of another file (its header_key or sequence number differ) or a
 * missing/damaged one is replaced by the block that names this file with this sequence number, if there is one.
 * A block that is not a data block but has no other structure (its header is damaged) gives its 488 bytes.
 * Returns 0 (right), 1 (found through the index: right), 2 (damaged header, body used as it is), -1 (unreadable).
 */
static int adf_ofs_pick(adf_img_t *img, uint32_t hdr_blk, uint32_t seq, uint32_t db, size_t expect,
                        const uint8_t **src, size_t *len)
{
    const uint8_t *d = adf_block(img, db);
    uint32_t ib;
    size_t i;

    if (d && read_be32(d) == T_DATA) {
        uint32_t dsz = read_be32(d + 12);
        bool mine = read_be32(d + 4) == hdr_blk && read_be32(d + 8) == seq;
        if (!mine && (ib = adf_oidx_find(img, hdr_blk, seq)) != 0 && ib != db) {
            d = adf_block(img, ib);
            *src = d + 24;
            *len = read_be32(d + 12) < expect ? read_be32(d + 12) : expect;
            return 1;
        }
        if (dsz <= ADF_BLOCK_SIZE - 24) {
            *src = d + 24;
            *len = dsz < expect ? dsz : expect;
            return 0;
        }
        if ((ib = adf_oidx_find(img, hdr_blk, seq)) != 0) {
            d = adf_block(img, ib);
            *src = d + 24;
            *len = read_be32(d + 12) < expect ? read_be32(d + 12) : expect;
            return 1;
        }
        *src = d + 24;
        *len = expect;
        return 2;
    }
    if ((ib = adf_oidx_find(img, hdr_blk, seq)) != 0) {
        d = adf_block(img, ib);
        *src = d + 24;
        *len = read_be32(d + 12) < expect ? read_be32(d + 12) : expect;
        return 1;
    }
    if (d) {
        uint32_t t = read_be32(d);
        if (t != T_HEADER && t != T_LIST && t != 33 /* directory cache */) {
            for (i = 24; i < ADF_BLOCK_SIZE; i++)
                if (d[i]) {
                    *src = d + 24;
                    *len = expect;
                    return 2;
                }
        }
    }
    return -1;
}

/*
 * Contents of a file. The data block table is stored backwards: the i-th
 * block (from 0) of the header/extension block sits in slot (71 - i), i.e.
 * at offset 24 + (71 - i) * 4; extension blocks (T_LIST) are chained from
 * offset 504. Every block is placed at its position (i * block size, as the
 * file system does). A block that cannot be read is replaced by zeros of the
 * length it should have had, and the file is reported as incomplete; the file
 * is deleted only if no block is readable. In an OFS file the blocks
 * that the table cannot give (broken chain, damaged block header, block of
 * another file) are looked up by (header, sequence number), and a size field
 * that makes no sense is taken from the data blocks.
 * Returns 0 (with *out malloc'ed, also for an empty file), 1 (incomplete) or -1. out == NULL: a dry run that only
 * reports the state of the file.
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
        return 0;
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
        int q;

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
        q = adf_ofs_pick(img, hdr_blk, i + 1, db, expect, &src, &len);
        if (q < 0) {
            if (i < got) {
                if (!d) ADF_NOTE("file '%s': data block pointer %u outside the image", nm, db);
                else { ADF_NOTE("file '%s': block %u is not a data block", nm, db); adf_note_bad(img, db); }
            }
            bad++;
            continue;
        }
        if (buf) memcpy(buf + off, src, len);
        good++;
        if (q == 1) found++;
        else if (q == 2) {
            ADF_NOTE("file '%s': block %u has a damaged header: its data are used as they are", nm, db);
            adf_note_bad(img, db);
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
 * and, if it had errors or there is no root, the recovery. Returns 0 if the image can be shown, -1 if it is not a
 * DOS disk at all (img is then freed).
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
    if (rootless && img->nhf == 0) {
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
    if (rootless)
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
    X(MDescIncomplete,       L"incomplete (unreadable blocks)") \
    X(MDescSizeFixed,        L"size taken from the data blocks") \
    X(MDescRecFile,          L"recovered from the header") \
    X(MDescRecData,          L"data blocks without a header") \
    X(MDescRecDir,           L"files and data not reached by the directory tree") \
    X(MDescImage,            L"unpacked disk image") \
    X(MDescReport,           L"what was found and repaired") \
    /* the box shown when a file that the plug-in recognizes cannot be opened: the name of the file, then one of these */ \
    X(MErrDiskType,          L"This archive does not hold a disk image (disk type 7)") \
    X(MErrEncrypted,         L"This DMS archive is encrypted") \
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
    X(MCopyFailed,           L"Items that could not be copied: %u")

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
                                         !pd->Dms.sig_ok || pd->Dms.budget_hit || pd->TotalBadTracksCount));
            pd->HasReport = pd->Damaged && pd->Log.lines > 0;
            if (pd->HasReport) build_report(pd);
        }
    OOM_GUARD_END(failed)
    g_clog = NULL;
    if (failed || !usable) {
        /* (memory of an aborted call is lost; what was set up is released) */
        *why = failed ? REFUSE_MEMORY : (pd->IsDMS && pd->Dms.refuse != REFUSE_NONE ? pd->Dms.refuse : REFUSE_NODISK);
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
    volatile int ok = 0, failed = 0;
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
            }
            if (owned) free((void *)buf);
        }
    OOM_GUARD_END(failed)
    pd->Adf.quiet = 0;
    if (ok && !failed) x->copied++; else x->failed++;
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
    Info->Version       = MAKEFARVERSION(2, 3, 0, 0, VS_RELEASE);     /* 2.3.0: Far wants four numbers, the build is 0 */
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

    n = info_line(pd, n, MInfoContainer, L"%ls", GetMsg(pd->IsDMS ? MValContainerDms : MValContainerAdf));
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
            n = info_line(pd, n, MInfoRootBlock, L"%ls", GetMsg(MValRootMissing));
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
    x.mode = XM_OVERWRITE;
    x.silent = ((unsigned long long)Info->OpMode & ((unsigned long long)OPM_SILENT | (unsigned long long)OPM_VIEW |
                (unsigned long long)OPM_EDIT | (unsigned long long)OPM_QUICKVIEW | (unsigned long long)OPM_FIND |
                (unsigned long long)OPM_DESCR)) != 0;
    wcscpy(dest, Info->DestPath);
    if (!x.silent) {                           /* F5: the dialog */
        int mode = g_copy_mode;
        int r = copy_dialog(Info, typed, MAX_PATH * 2, &mode);
        if (r == 0) return -1;                 /* cancelled: nothing is done, Far keeps the selection */
        if (r > 0) {
            g_copy_mode = mode;
            x.mode = mode;
            if (!resolve_dest(Info->DestPath, typed, dest, MAX_PATH) || !make_dirs(dest)) {
                box_text(MCopyTitle, GetMsg(MErrDest), typed);
                return 0;
            }
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
    if (x.failed && !x.silent) {
        wchar_t msg[96];
        wfmt(msg, 96, GetMsgFmt(MCopyFailed), x.failed);
        box_text(MCopyTitle, msg, NULL);
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
