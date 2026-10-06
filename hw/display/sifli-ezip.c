/*
 * SiFli EZIP decompression accelerator
 *
 * Register map from the SiFli SDK's drivers/cmsis/sf32lb52x/ezip.h.
 *
 * EZIP decompresses into a destination buffer in memory: firmware writes the
 * source, destination, mode and geometry, then sets EZIP_CTRL, and reads the
 * result out of the destination when the END bit comes up. Three formats are
 * involved, and they are not equally accessible:
 *
 *   - GZIP is a bare DEFLATE stream, inflated with zlib's inflate(). The
 *     wrapper is not part of what the hardware sees: the SDK builds these
 *     assets with "ezip -gzip <file> -length -noheader".
 *   - LZ4 is a raw LZ4 block, about fifty lines.
 *   - The proprietary EZIP bitstream has no decoder in the SDK at all --
 *     external/ffmpeg/libavcodec/ezipdec.c only copies the packet, and
 *     ezipenc.c shells out to the closed-source eZIP.exe. This model does
 *     the same thing the SDK's own encoder does: it runs the vendor's
 *     tools/png2ezip binary, which is the only implementation there is.
 *
 * Thin semantics, as with the other models here:
 *
 *   - The job finishes inside the write that starts it. Firmware never sees
 *     EZIP_CTRL set with work outstanding, which is what the HAL's
 *     "if (0 != EZIP_CTRL) return HAL_ERROR" guard at the top of
 *     HAL_EZIP_Decode assumes.
 *   - INT_STA and INT_MASK both latch the result and both clear on a write
 *     of ones. They are the same value; the part has two registers that
 *     firmware polls or acknowledges through, and HAL_EZIP_Decode polls
 *     INT_MASK having never set INT_EN, so neither is gated by INT_EN.
 *     INT_EN only gates the interrupt line.
 *   - The interrupt is a level: INT_EN & INT_MASK & the status bits.
 *
 * Two host-side consequences worth knowing before running this:
 *
 *   - The proprietary format decodes by running a host binary, synchronously,
 *     inside the MMIO write. A guest that asks for it stalls the vCPU for as
 *     long as the tool takes, and the tool is parsing guest-controlled
 *     bytes. That is acceptable for a development model and is the same
 *     trust the rest of QEMU places in "-drive" images; it is not something
 *     to expose to a hostile guest.
 *   - The tool is found through the machine's "ezip-tool" property or the
 *     SIFLI_EZIP_TOOL environment variable, because QEMU has no way to know
 *     where the SDK was unpacked. Without it, only the proprietary format
 *     fails; GZIP and LZ4 still decode.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/sifli-ezip.h"
#include "hw/irq.h"
#include "hw/misc/sifli-sbus.h"
#include "hw/qdev-properties.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"
/* g_mkdir, g_remove and g_rmdir live here, not in glib.h. Include it
 * explicitly: GCC 11 only warns about the implicit declarations, but GCC 14
 * and later make them an error, and MSYS2 ships one of those. */
#include <glib/gstdio.h>
#include <zlib.h>

/*
 * A cap on what one decode may produce. The real accelerator writes whatever
 * the stream expands to, so this only ever bites on a corrupt or hostile
 * stream -- without it, a deflate bomb would be a deflate bomb.
 */
#define EZIP_MAX_OUTPUT         (16 * 1024 * 1024)

/* The tool's four-byte container header, which precedes the pixels. */
#define EZIP_TOOL_HEADER_SIZE   4

/* Room to start from when a decode does not know how big it will get. */
#define EZIP_OUTPUT_CHUNK       (64 * 1024)

/* ------------------------------------------------------------------ */
/* A growable host buffer for one decode's output.                     */
/* ------------------------------------------------------------------ */

typedef struct EzipBuffer {
    uint8_t *data;
    size_t len;
    size_t cap;
} EzipBuffer;

static bool ezip_buffer_reserve(EzipBuffer *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t cap;

    if (need <= b->cap) {
        return true;
    }
    if (need > EZIP_MAX_OUTPUT) {
        return false;
    }

    cap = MAX(b->cap, (size_t)EZIP_OUTPUT_CHUNK);
    while (cap < need) {
        cap *= 2;
    }
    if (cap > EZIP_MAX_OUTPUT) {
        cap = EZIP_MAX_OUTPUT;
    }

    b->data = g_realloc(b->data, cap);
    b->cap = cap;
    return true;
}

static bool ezip_buffer_append(EzipBuffer *b, const uint8_t *src, size_t n)
{
    if (!ezip_buffer_reserve(b, n)) {
        return false;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

/* ------------------------------------------------------------------ */
/* Guest memory.                                                       */
/* ------------------------------------------------------------------ */

/*
 * How much of the source can be read. The formats do not all say how long
 * they are, so the model reads a window and lets the decoder stop where it
 * stops: a deflate stream ends itself, and the LZ4 block and the proprietary
 * bitstream both carry their length in their first bytes. Truncating to the
 * end of the memory region keeps a wild address from turning into a read
 * past the end of the guest.
 */
static size_t ezip_source_window(SifliEzipState *s, uint32_t addr)
{
    /*
     * len is an in/out parameter: it goes in as the most the caller wants
     * and comes back clamped to what the region actually holds. Left
     * uninitialised it clamps against whatever was on the stack.
     */
    hwaddr xlat, len = s->window_bytes;

    address_space_translate(&address_space_memory, addr, &xlat, &len, false,
                            MEMTXATTRS_UNSPECIFIED);

    return len;
}

static bool ezip_decode_gzip(const uint8_t *in, size_t in_len, EzipBuffer *out,
                             size_t *used)
{
    z_stream z = { 0 };
    bool ok = false;
    int rc;

    /*
     * Negative window bits: raw DEFLATE, no gzip or zlib wrapper.
     *
     * The SDK's own tool writes these streams with
     * "ezip -gzip <file> -length -noheader"
     * (docs/source/zh_CN/app_note/ezip_tool_usage.md), and the note says the
     * bytes after the four-byte length go to the hardware as they are: the
     * gzip header has been stripped. The example assets bear that out. What
     * follows the length in assets/gzip_input.dat is an anonymous deflate
     * stream -- the 1f 8b 08 08 that looks like a gzip header is inside
     * commented-out lines -- and it still ends with the gzip trailer's
     * CRC32/ISIZE, which a deflate decoder simply never reaches.
     *
     * Window size is not a constraint: 15 accepts any deflate stream, and
     * the 57x assets are documented as using a window of 2048 or less.
     */
    if (inflateInit2(&z, -15) != Z_OK) {
        return false;
    }

    z.next_in = (Bytef *)in;
    z.avail_in = in_len;

    for (;;) {
        size_t produced;

        if (!ezip_buffer_reserve(out, EZIP_OUTPUT_CHUNK)) {
            break;
        }
        z.next_out = out->data + out->len;
        z.avail_out = out->cap - out->len;
        produced = out->len;

        rc = inflate(&z, Z_NO_FLUSH);
        out->len = out->cap - z.avail_out;

        if (rc == Z_STREAM_END) {
            ok = true;
            break;
        }
        if (rc != Z_OK) {
            break;
        }
        if (z.avail_in == 0 && out->len == produced) {
            /* Out of input with nothing coming out: the stream is cut. */
            break;
        }
    }

    /*
     * How much of the window was the stream itself. That is the number the
     * hardware leaves in DB_DATA2, and it is knowable here because a deflate
     * stream marks its own end -- the trailing gzip CRC32/ISIZE after it are
     * left unconsumed, which is what the hardware does with them too.
     */
    *used = z.total_in;
    inflateEnd(&z);
    return ok;
}

/*
 * A raw LZ4 block, as produced by lz4_compress_default(). Sequences of
 * literals and back-references; the last sequence is literals only, which is
 * what ends the loop (and why the block needs no explicit terminator).
 *
 * The match copy has to be byte at a time rather than a memcpy: LZ4 allows
 * the match to overlap the output cursor, and repeating a pattern is the
 * whole point -- a run-length encoded image would come out wrong otherwise.
 */
static bool ezip_decode_lz4(const uint8_t *in, size_t in_len, EzipBuffer *out)
{
    size_t ip = 0;

    while (ip < in_len) {
        unsigned token = in[ip++];
        size_t lit_len = token >> 4;
        size_t match_len;
        unsigned offset;

        if (lit_len == 15) {
            unsigned b;

            do {
                if (ip >= in_len) {
                    return false;
                }
                b = in[ip++];
                lit_len += b;
            } while (b == 255);
        }

        if (lit_len > in_len - ip) {
            return false;
        }
        if (!ezip_buffer_append(out, in + ip, lit_len)) {
            return false;
        }
        ip += lit_len;

        if (ip == in_len) {
            /* Last sequence: the block ends on its literals. */
            break;
        }

        if (in_len - ip < 2) {
            return false;
        }
        offset = in[ip] | (in[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > out->len) {
            return false;
        }

        match_len = token & 0xf;
        if (match_len == 15) {
            unsigned b;

            do {
                if (ip >= in_len) {
                    return false;
                }
                b = in[ip++];
                match_len += b;
            } while (b == 255);
        }
        match_len += 4;

        if (!ezip_buffer_reserve(out, match_len)) {
            return false;
        }
        while (match_len--) {
            out->data[out->len] = out->data[out->len - offset];
            out->len++;
        }
    }

    return out->len > 0;
}

/* ------------------------------------------------------------------ */
/* The vendor's ezip binary.                                           */
/* ------------------------------------------------------------------ */

/*
 * Decode the proprietary bitstream by running the SDK's converter.
 *
 * The command line is the one the tool's own Help.txt documents for going
 * from a bare ezip binary to a bare pixel binary, and it was checked against
 * the example firmware's own test vector: 2980 bytes of bitstream in,
 * 4 + 68*37*3 bytes out, byte-identical to the pixels the firmware expects.
 *
 *   -convert <in> -spt 1 -dpt 1 -binfile 1 -dec_off_no_header 0 -outdir <out>
 *
 * Input goes in one directory and output in another because the tool names
 * its output after its input's basename: sharing a directory would have it
 * overwrite the file it was reading. Trailing bytes past the end of the
 * bitstream are harmless -- the bitstream says how long it is, and feeding
 * it 4K of erased flash still produced identical pixels.
 */
static bool ezip_decode_proprietary(SifliEzipState *s, const uint8_t *in,
                                    size_t in_len, EzipBuffer *out,
                                    unsigned *db_width, unsigned *db_height,
                                    unsigned *db_format)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *in_dir = NULL;
    g_autofree char *out_dir = NULL;
    g_autofree char *in_path = NULL;
    g_autofree char *out_path = NULL;
    g_autofree char *tool_abs = NULL;
    g_autofree char *stdout_text = NULL;
    g_autoptr(GError) err = NULL;
    g_autofree gchar *contents = NULL;
    gsize contents_len = 0;
    /* Thirteen arguments and the NULL g_spawn_sync wants at the end. */
    const char *argv[14];
    int argc = 0;
    int status = 0;
    gboolean spawned;
    bool ok = false;
    uint32_t header;

    if (s->tool == NULL || s->tool[0] == '\0') {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: the EZIP format needs the vendor decoder "
                      "and none is configured; set -machine ezip-tool=<path> "
                      "or SIFLI_EZIP_TOOL\n");
        return false;
    }

    /*
     * The child runs with the temporary directory as its working directory,
     * so the tool has to be named in a way that survives that.
     */
    tool_abs = g_canonicalize_filename(s->tool, NULL);
    if (!g_file_test(tool_abs, G_FILE_TEST_IS_EXECUTABLE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: %s is not executable\n", tool_abs);
        return false;
    }

    tmpdir = g_dir_make_tmp("sifli-ezip-XXXXXX", &err);
    if (tmpdir == NULL) {
        qemu_log_mask(LOG_GUEST_ERROR, "sifli-ezip: %s\n", err->message);
        return false;
    }
    in_dir = g_build_filename(tmpdir, "in", NULL);
    out_dir = g_build_filename(tmpdir, "out", NULL);
    if (g_mkdir(in_dir, 0700) != 0 || g_mkdir(out_dir, 0700) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "sifli-ezip: cannot make %s\n", tmpdir);
        goto out;
    }

    /*
     * The tool names its output after its input and replaces the extension
     * with ".bin" when asked for a bare binary, so the two names differ.
     */
    in_path = g_build_filename(in_dir, "image.ezip", NULL);
    out_path = g_build_filename(out_dir, "image.bin", NULL);
    if (!g_file_set_contents(in_path, (const char *)in, in_len, &err)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sifli-ezip: %s\n", err->message);
        goto out;
    }

    argv[argc++] = tool_abs;
    argv[argc++] = "-convert";
    argv[argc++] = "in/image.ezip";
    argv[argc++] = "-spt";
    argv[argc++] = "1";
    argv[argc++] = "-dpt";
    argv[argc++] = "1";
    argv[argc++] = "-binfile";
    argv[argc++] = "1";
    argv[argc++] = "-dec_off_no_header";
    argv[argc++] = "0";
    argv[argc++] = "-outdir";
    argv[argc++] = "out";
    argv[argc] = NULL;

    spawned = g_spawn_sync(tmpdir, (char **)argv, NULL,
                           G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                           &stdout_text, NULL, &status, &err);
    if (!spawned) {
        qemu_log_mask(LOG_GUEST_ERROR, "sifli-ezip: cannot run %s: %s\n",
                      tool_abs, err->message);
        goto out;
    }
    if (!g_spawn_check_exit_status(status, &err)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sifli-ezip: %s failed: %s%s%s\n",
                      tool_abs, err->message,
                      (stdout_text && *stdout_text) ? ": " : "",
                      (stdout_text && *stdout_text) ? stdout_text : "");
        goto out;
    }

    if (!g_file_get_contents(out_path, &contents, &contents_len, &err)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: %s produced no output (%s)\n",
                      tool_abs, err->message);
        goto out;
    }
    if (contents_len <= EZIP_TOOL_HEADER_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: %s produced %zu bytes, too few to be an "
                      "image\n", tool_abs, contents_len);
        goto out;
    }

    /*
     * The pixels sit behind a four-byte header describing what they are,
     * little-endian:
     *
     *     [31:21] height  [20:10] width  [9:5] reserved  [4:0] format
     *
     * which is also what the hardware leaves in DB_DATA1, so it is worth
     * reading on the way past rather than discarding. The format is the
     * vendor tool's colour format, which is LVGL's: 4 is TRUE_COLOR and 5 is
     * TRUE_COLOR_ALPHA, and for the 16-bit tool that built these assets that
     * is two and three bytes per pixel.
     */
    header = ldl_le_p((const uint8_t *)contents);
    *db_width = (header >> 10) & 0x7ff;
    *db_height = (header >> 21) & 0x7ff;
    *db_format = header & 0x1f;

    ok = ezip_buffer_append(out, (const uint8_t *)contents +
                            EZIP_TOOL_HEADER_SIZE,
                            contents_len - EZIP_TOOL_HEADER_SIZE);

out:
    if (in_path) {
        g_remove(in_path);
    }
    if (out_path) {
        g_remove(out_path);
    }
    if (in_dir) {
        g_rmdir(in_dir);
    }
    if (out_dir) {
        g_rmdir(out_dir);
    }
    if (tmpdir) {
        g_rmdir(tmpdir);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* The EPIC co-engine.                                                 */
/* ------------------------------------------------------------------ */

static void ezip_coeng_clear(SifliEzipState *s)
{
    g_free(s->coeng_pixels);
    s->coeng_pixels = NULL;
    s->coeng_width = 0;
    s->coeng_height = 0;
    s->coeng_start_col = 0;
    s->coeng_start_row = 0;
}

bool sifli_ezip_coeng_frame(SifliEzipState *s, SifliEzipFrame *frame)
{
    if (s->coeng_pixels == NULL) {
        return false;
    }

    frame->pixels = s->coeng_pixels;
    frame->width = s->coeng_width;
    frame->height = s->coeng_height;
    frame->start_col = s->coeng_start_col;
    frame->start_row = s->coeng_start_row;
    return true;
}

/* 5-bit / 6-bit channel to the 8-bit one bit replication gives. */
static uint8_t ezip_expand5(unsigned v)
{
    return (v << 3) | (v >> 2);
}

static uint8_t ezip_expand6(unsigned v)
{
    return (v << 2) | (v >> 4);
}

/*
 * Hand a decoded frame to EPIC.
 *
 * The window registers say which part of the source image the layer is
 * showing: START_POINT and END_POINT carry (row, column) pairs, and hardware
 * decodes only that rectangle, because a layer clipped by the canvas only
 * needs its visible part. The host decoder has already produced the whole
 * image, so the model crops to the window, converts to the ARGB8888 the
 * EPIC co-engine speaks, and parks it in the device state for epic_run().
 *
 * The format is deduced from the image size the container header gave and
 * the number of bytes the decoder produced; only the three-byte case needs
 * the header's colour format, to tell RGB888 from ARGB8565.
 *
 * Everything that can go wrong here is a limit of the model rather than of
 * the hardware -- the real decoder knows the format from the bitstream. So a
 * frame the model cannot interpret is logged and dropped, and the job still
 * reports END: firmware is never left waiting on a job that will not finish,
 * and EPIC then reports the missing frame in turn.
 */
static void ezip_coeng_store(SifliEzipState *s, const EzipBuffer *out,
                             unsigned img_w, unsigned img_h, unsigned format)
{
    unsigned col_start = (s->reg[SIFLI_EZIP_START_POINT / 4] >> 16) & 0xffff;
    unsigned col_end = (s->reg[SIFLI_EZIP_END_POINT / 4] >> 16) & 0xffff;
    unsigned row_start = s->reg[SIFLI_EZIP_START_POINT / 4] & 0xffff;
    unsigned row_end = s->reg[SIFLI_EZIP_END_POINT / 4] & 0xffff;
    unsigned bpp, width, height, x, y;

    if (img_w == 0 || img_h == 0 || out->len == 0 ||
        out->len % ((size_t)img_w * img_h) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: cannot tell the image size of %zu decoded "
                      "bytes for the EPIC co-engine\n", out->len);
        return;
    }
    bpp = out->len / ((size_t)img_w * img_h);
    /*
     * Three bytes per pixel is RGB888 or ARGB8565, and only the container
     * header's colour format separates them. Refuse an unknown one rather
     * than decode it as something it is not.
     */
    if (bpp == 3 && format != EZIP_PIXEL_FMT_TRUE_COLOR &&
        format != EZIP_PIXEL_FMT_TRUE_COLOR_ALPHA) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: colour format %u with three bytes per "
                      "pixel is not one the EPIC co-engine can be fed\n",
                      format);
        return;
    }
    if (bpp != 2 && bpp != 3 && bpp != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: %u bytes per pixel is not a format the "
                      "EPIC co-engine can be fed\n", bpp);
        return;
    }

    if (col_end < col_start || row_end < row_start ||
        col_end >= img_w || row_end >= img_h) {
        /*
         * The HAL always programs a window that fits (bf0_hal_epic.c:3875
         * takes it from the layer/canvas intersection), so a bad one is a
         * register set no HAL call produces. Drawing the whole image is not
         * what hardware would do, but it is visible in the log.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: window (%u,%u)-(%u,%u) does not fit the "
                      "%ux%u image; using the whole image\n",
                      col_start, row_start, col_end, row_end, img_w, img_h);
        col_start = 0;
        row_start = 0;
        col_end = img_w - 1;
        row_end = img_h - 1;
    }

    width = col_end - col_start + 1;
    height = row_end - row_start + 1;
    s->coeng_pixels = g_malloc((size_t)width * height * 4);

    {
        const uint8_t *row = out->data +
                             (size_t)row_start * img_w * bpp +
                             (size_t)col_start * bpp;

        for (y = 0; y < height; y++, row += (size_t)img_w * bpp) {
            for (x = 0; x < width; x++) {
                const uint8_t *raw = row + (size_t)x * bpp;
                uint8_t *px = s->coeng_pixels +
                              ((size_t)y * width + x) * 4;

                if (bpp == 4) {
                    /* ARGB8888: B, G, R, A, the co-engine's own order. */
                    memcpy(px, raw, 4);
                    continue;
                }

                if (bpp == 3 && format == EZIP_PIXEL_FMT_TRUE_COLOR) {
                    /* RGB888: B, G, R with no alpha. */
                    px[0] = raw[0];
                    px[1] = raw[1];
                    px[2] = raw[2];
                    px[3] = 0xff;
                    continue;
                }

                /*
                 * RGB565 and ARGB8565 expand to 8888 by bit replication,
                 * which is lossless when EPIC packs it back into a 565
                 * destination.
                 */
                {
                    uint16_t v = lduw_le_p(raw);
                    unsigned r = (v >> 11) & 0x1f;
                    unsigned g = (v >> 5) & 0x3f;
                    unsigned b = v & 0x1f;

                    px[0] = ezip_expand5(b);
                    px[1] = ezip_expand6(g);
                    px[2] = ezip_expand5(r);
                    px[3] = (bpp == 3) ? raw[2] : 0xff;
                }
            }
        }
    }

    s->coeng_width = width;
    s->coeng_height = height;
    s->coeng_start_col = col_start;
    s->coeng_start_row = row_start;
}

/* ------------------------------------------------------------------ */
/* Interrupts.                                                         */
/* ------------------------------------------------------------------ */

static void ezip_update_irq(SifliEzipState *s)
{
    uint32_t armed = s->reg[SIFLI_EZIP_INT_EN / 4] &
                     s->reg[SIFLI_EZIP_INT_MASK / 4] & 0x3f;

    qemu_set_irq(s->irq, armed != 0);
}

static void ezip_complete(SifliEzipState *s, uint32_t status)
{
    /*
     * Both registers, unconditionally. Firmware polls INT_MASK in the
     * non-interrupt path and acknowledges through INT_STA in the interrupt
     * one, and HAL_EZIP_Decode polls INT_MASK having never touched INT_EN.
     */
    s->reg[SIFLI_EZIP_INT_STA / 4] |= status;
    s->reg[SIFLI_EZIP_INT_MASK / 4] |= status;
    ezip_update_irq(s);
}

/*
 * Finish a job, choosing the status the caller can actually consume.
 *
 * An EPIC-output job always ends with END, never with an error bit. EPIC
 * only clears its "EZIP running" flag from the completion callback
 * (EPIC_EzipCpltCallback, bf0_hal_epic.c:6138), and HAL_EZIP_IRQHandler runs
 * that callback only for END (bf0_hal_ezip.c:530) -- an error bit therefore
 * leaves the HAL spinning in "while (epic->coeng_state)" (bf0_hal_epic.c:
 * 4688, :4853, :5019) for good. So a decode the model cannot do is reported
 * through the log, and through the frame EPIC then finds missing, rather
 * than through a status that hangs the firmware. The AHB path is polled by
 * its caller, so it keeps the error bits.
 */
static void ezip_complete_run(SifliEzipState *s, bool out_epic,
                              uint32_t status)
{
    ezip_complete(s, out_epic ? EZIP_INT_END : status);
}

/* ------------------------------------------------------------------ */
/* The job itself.                                                     */
/* ------------------------------------------------------------------ */

static void ezip_run(SifliEzipState *s)
{
    uint32_t para = s->reg[SIFLI_EZIP_PARA / 4];
    uint32_t src = sifli_sbus_to_cpu_addr(s->reg[SIFLI_EZIP_SRC_ADDR / 4]);
    uint32_t dst = s->reg[SIFLI_EZIP_DST_ADDR / 4];
    unsigned mode = (para & EZIP_PARA_MOD_SEL) >> 1;
    /* Output to EPIC feeds the 2D pipeline's input instead of memory. */
    bool out_epic = !(para & EZIP_PARA_OUT_SEL);
    EzipBuffer out = { 0 };
    g_autofree uint8_t *in = NULL;
    size_t in_len;
    /* How much of the window was stream; the window itself when unknowable. */
    size_t used;
    bool ok;
    unsigned db_width = 0, db_height = 0, db_format = 0;

    /*
     * Clearing here means a decode that fails leaves no frame behind for
     * EPIC to draw by mistake.
     */
    if (out_epic) {
        ezip_coeng_clear(s);
    }

    in_len = ezip_source_window(s, src);
    if (in_len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: source address 0x%08x is not readable\n",
                      s->reg[SIFLI_EZIP_SRC_ADDR / 4]);
        ezip_complete_run(s, out_epic, EZIP_INT_BTYPE_ERR);
        return;
    }

    used = in_len;
    in = g_malloc(in_len);
    if (address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                           in, in_len) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: cannot read %zu bytes from 0x%08x\n",
                      in_len, src);
        ezip_complete_run(s, out_epic, EZIP_INT_BTYPE_ERR);
        return;
    }

    switch (mode) {
    case EZIP_PARA_MOD_EZIP:
        ok = ezip_decode_proprietary(s, in, in_len, &out,
                                     &db_width, &db_height, &db_format);
        break;
    case EZIP_PARA_MOD_GZIP:
        ok = ezip_decode_gzip(in, in_len, &out, &used);
        break;
    case EZIP_PARA_MOD_LZ4:
        /*
         * SRC_ADDR points at a four-byte little-endian length, and the
         * compressed block follows it. That is the shape the SDK's own
         * assets are cut to, and what HAL_EZIP_ConfigDecode expects the
         * hardware to find.
         */
        if (in_len < 4) {
            ok = false;
            break;
        }
        {
            uint32_t block_len = ldl_le_p(in);

            if (block_len > in_len - 4) {
                ok = false;
                break;
            }
            used = 4 + block_len;
            ok = ezip_decode_lz4(in + 4, block_len, &out);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: unknown mode %u\n", mode);
        ok = false;
        break;
    }

    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sifli-ezip: mode %u failed to decode %zu bytes from "
                      "0x%08x\n", mode, in_len, src);
        ezip_complete_run(s, out_epic, EZIP_INT_BTYPE_ERR);
        return;
    }

    if (!(para & EZIP_PARA_OUT_SEL)) {
        ezip_coeng_store(s, &out, db_width, db_height, db_format);
    } else if (out.len > 0) {
        /*
         * An empty stream is a legitimate image of nothing; there is just
         * nothing to put in the destination.
         *
         * A finite check before the write: a decode that produced more than
         * the address space holds would otherwise walk off the end of the
         * guest. As above, avail goes in as what we want and comes back
         * clamped, so it starts at the length being asked for.
         */
        hwaddr xlat, avail = out.len;

        address_space_translate(&address_space_memory, dst, &xlat, &avail,
                                true, MEMTXATTRS_UNSPECIFIED);
        if (out.len > (size_t)avail) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sifli-ezip: decoded %zu bytes but only %" HWADDR_PRIu
                          " fit at 0x%08x\n", out.len, avail, dst);
            ezip_complete(s, EZIP_INT_ETYPE_ERR);
            return;
        }
        if (address_space_write(&address_space_memory, dst,
                                MEMTXATTRS_UNSPECIFIED, out.data,
                                out.len) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sifli-ezip: cannot write %zu bytes to 0x%08x\n",
                          out.len, dst);
            ezip_complete(s, EZIP_INT_ETYPE_ERR);
            return;
        }
    }

    /*
     * The debug block. Only the geometry is derivable here: it is either
     * what the caller asked for in START_POINT/END_POINT, or -- for the
     * proprietary format -- what the bitstream said about itself.
     */
    {
        /*
         * Both point registers carry the row in the low half and the column
         * in the high half (EZIP_START_POINT_START_ROW_Pos is 0, and
         * EZIP_START_POINT_START_COL_Pos is 16), in that order.
         */
        unsigned row_start = s->reg[SIFLI_EZIP_START_POINT / 4] & 0xffff;
        unsigned col_start =
            (s->reg[SIFLI_EZIP_START_POINT / 4] >> 16) & 0xffff;
        unsigned row_end = s->reg[SIFLI_EZIP_END_POINT / 4] & 0xffff;
        unsigned col_end = (s->reg[SIFLI_EZIP_END_POINT / 4] >> 16) & 0xffff;
        unsigned width = (col_end >= col_start) ? col_end - col_start + 1 : 0;
        unsigned height = (row_end >= row_start) ? row_end - row_start + 1 : 0;

        if (width == 0 || height == 0) {
            width = db_width;
            height = db_height;
        }
        s->reg[SIFLI_EZIP_DB_DATA1 / 4] = (width << 16) | height;
        s->reg[SIFLI_EZIP_DB_DATA2 / 4] = used;
    }

    ezip_complete(s, EZIP_INT_END);
}

/* ------------------------------------------------------------------ */
/* MMIO.                                                               */
/* ------------------------------------------------------------------ */

static uint64_t ezip_read(void *opaque, hwaddr addr, unsigned size)
{
    SifliEzipState *s = opaque;
    unsigned index = addr / 4;
    unsigned shift = (addr & 3) * 8;
    uint32_t mask;

    if (size > 4 || addr + size > SIFLI_EZIP_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return 0;
    }
    if (index >= SIFLI_EZIP_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: offset 0x%" HWADDR_PRIx " is not a register\n",
                      __func__, addr);
        return 0;
    }

    mask = (size == 4) ? ~0u : ((1u << (size * 8)) - 1);
    return (s->reg[index] >> shift) & mask;
}

static void ezip_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    SifliEzipState *s = opaque;
    unsigned index = addr / 4;
    unsigned shift = (addr & 3) * 8;
    uint32_t field;
    uint32_t v;

    if (size > 4 || addr + size > SIFLI_EZIP_MMIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return;
    }
    if (index >= SIFLI_EZIP_NUM_REGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: offset 0x%" HWADDR_PRIx " is not a register\n",
                      __func__, addr);
        return;
    }

    field = (size == 4) ? ~0u : (((1u << (size * 8)) - 1) << shift);
    v = ((uint32_t)value << shift) & field;

    switch (index * 4) {
    case SIFLI_EZIP_INT_STA:
    case SIFLI_EZIP_INT_MASK:
        /* Write one to clear; the two are independent latches. */
        s->reg[index] &= ~v;
        ezip_update_irq(s);
        return;

    case SIFLI_EZIP_INT_EN:
        s->reg[index] = (s->reg[index] & ~field) | v;
        ezip_update_irq(s);
        return;

    case SIFLI_EZIP_CTRL:
        s->reg[index] = (s->reg[index] & ~field) | v;
        if (s->reg[index] & EZIP_CTRL_START) {
            ezip_run(s);
            /*
             * The job is over, so the busy bit is not: firmware checks this
             * register before starting the next one.
             */
            s->reg[index] = 0;
        }
        return;

    default:
        s->reg[index] = (s->reg[index] & ~field) | v;
        return;
    }
}

static const MemoryRegionOps sifli_ezip_ops = {
    .read = ezip_read,
    .write = ezip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sifli_ezip_reset(DeviceState *dev)
{
    SifliEzipState *s = SIFLI_EZIP(dev);

    memset(s->reg, 0, sizeof(s->reg));
    ezip_coeng_clear(s);
    ezip_update_irq(s);
}

static void sifli_ezip_init(Object *obj)
{
    SifliEzipState *s = SIFLI_EZIP(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    memory_region_init_io(&s->mmio, obj, &sifli_ezip_ops, s,
                          TYPE_SIFLI_EZIP, SIFLI_EZIP_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const Property sifli_ezip_properties[] = {
    /*
     * No default: the SDK can be unpacked anywhere, so guessing a path would
     * only produce a confusing failure at an unrelated moment. The machine's
     * "ezip-tool" property and SIFLI_EZIP_TOOL are the ways in.
     */
    DEFINE_PROP_STRING("tool", SifliEzipState, tool),
    DEFINE_PROP_UINT32("window-bytes", SifliEzipState, window_bytes,
                       64 * 1024),
};

static void sifli_ezip_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sifli_ezip_reset);
    device_class_set_props(dc, sifli_ezip_properties);
}

static const TypeInfo sifli_ezip_info = {
    .name          = TYPE_SIFLI_EZIP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SifliEzipState),
    .instance_init = sifli_ezip_init,
    .class_init    = sifli_ezip_class_init,
};

static void sifli_ezip_register_types(void)
{
    type_register_static(&sifli_ezip_info);
}

type_init(sifli_ezip_register_types)
