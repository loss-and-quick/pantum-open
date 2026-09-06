/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CUPS raster filter for Pantum M6500/M6507 series printers.
 *
 * Free replacement for the proprietary ptm6500Filter.  It turns a CUPS
 * raster stream (8 bit/pixel grayscale, CUPS_CSPACE_W) into the ZjStream
 * the M6500 firmware expects: a big-endian Zenographics stream tagged
 * "JZJZ", wrapped in UEL, carrying one standard JBIG1 entity per page.
 *
 * The format is documented in notes/print-protocol.md; proto/rastertopantum.py
 * is the Python prototype this file was grown from.  Both produce byte for
 * byte the same stream as the vendor filter.
 *
 * Everything specific to the vendor's "platform 3" (PLATFORM_M) branch is
 * hard-wired here: no PJL, no ACL packets, no hardware duplex.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <cups/cups.h>
#include <cups/raster.h>
#include <jbig.h>

/* The PPD API is the only way a CUPS 1.x/2.x filter can see the *Default*
 * entries of its own PPD, and CUPS 2.x offers no replacement for it on the
 * filter side, so the deprecation warning is silenced deliberately.  The
 * region reaches from this include down to the matching "pop", and holds
 * every function that touches a ppd_file_t. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <cups/ppd.h>

/* ---------------------------------------------------------------- stream */

#define UEL "\033%-12345X"
#define UEL_LEN 9
#define ZJ_MAGIC "JZJZ"
#define ZJ_SIGNATURE 0x5a5a

/* Chunk types; the numbering is the one foo2zjs uses, except for 0x11 which
 * is a Pantum addition. */
#define ZJT_START_DOC 0x00
#define ZJT_END_DOC 0x01
#define ZJT_START_PAGE 0x02
#define ZJT_END_PAGE 0x03
#define ZJT_JBIG_BIH 0x04
#define ZJT_JBIG_BID 0x05
#define ZJT_END_JBIG 0x06
#define ZJT_2600N_PAUSE 0x0b
#define ZJT_PANTUM_PARAM 0x11

/* Item types inside a chunk payload. */
#define ZJI_PAGECOUNT 0x0000
#define ZJI_DMCOLLATE 0x0001
#define ZJI_DMDUPLEX 0x0002
#define ZJI_DMPAPER 0x0003
#define ZJI_DMCOPIES 0x0004
#define ZJI_DMDEFAULTSOURCE 0x0005
#define ZJI_DMMEDIATYPE 0x0006
#define ZJI_NBIE 0x0007
#define ZJI_RESOLUTION_X 0x0008
#define ZJI_RESOLUTION_Y 0x0009
#define ZJI_RASTER_X 0x000c
#define ZJI_RASTER_Y 0x000d
#define ZJI_VIDEO_BPP 0x0010
#define ZJI_VIDEO_X 0x0011
#define ZJI_VIDEO_Y 0x0012
#define ZJI_RET 0x0016
#define ZJI_CUSTOM_WIDTH 0x0018
#define ZJI_CUSTOM_HEIGHT 0x0019
#define ZJI_CUSTOM_UNIT 0x001a
#define ZJI_PANTUM_DENSITY 0xff00

/* DMDUP_SIMPLEX: the M6500 branch never announces hardware duplex. */
#define ZJ_DMDUPLEX_SIMPLEX 1

/* The firmware wants at most 64 KiB of JBIG data per ZJT_JBIG_BID chunk. */
#define ZJ_BID_MAX 0x10000u

/* A JBIG BIH is a fixed 20-byte prefix of the BIE. */
#define JBIG_BIH_LEN 20

/* Upper bound on one page, in 600 dpi device dots.  The raster header is job
 * data: a filter must not turn a nonsense width or height into a gigabyte
 * allocation, and must not let the arithmetic wrap.  Rounding cupsWidth up to
 * a multiple of 32 overflows to zero just under UINT_MAX, and a zero-width
 * page would then walk off the row buffer while rotating.  32768 dots is 54
 * inches at 600 dpi, close to four times the longest sheet the PPD offers,
 * and it keeps the page buffer under 256 MiB even at two bits per pixel. */
#define PT_MAX_DOTS 32768u

/* JBIG parameters, taken from the vendor's private copy of jbig-kit:
 * stripes of 256 lines, no adaptive template, LRLTWO context. */
#define JBIG_L0 256
#define JBIG_ORDER (JBG_ILEAVE | JBG_SMID)
#define JBIG_OPTIONS (JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON)

/* Payload of chunk 0x11, produced by GetTonerParamCmdForPlatformM().  It is
 * 36 big-endian uint16 fuser/toner parameters and depends on nothing. */
static const uint8_t pantum_param_blob[72] = {
  0x00, 0x00, 0x00, 0x51, 0x00, 0x73, 0x00, 0x6e, 0x00, 0x96, 0x00, 0x7a,
  0x00, 0x64, 0x00, 0x96, 0x00, 0xc8, 0x00, 0x64, 0x00, 0x51, 0x00, 0x64,
  0x00, 0x64, 0x00, 0x64, 0x00, 0x64, 0x00, 0x64, 0x00, 0x64, 0x00, 0x64,
  0x00, 0x64, 0x00, 0x64, 0x00, 0x64, 0x00, 0x64, 0x00, 0x64, 0x00, 0x64,
  0x00, 0x78, 0x00, 0x87, 0x00, 0x96, 0x00, 0xb4, 0x00, 0xc8, 0x00, 0x78,
  0x00, 0x96, 0x00, 0xbe, 0x01, 0x5e, 0x01, 0x90, 0x00, 0x64, 0x00, 0x64
};

/* ------------------------------------------------------------ paper table */

/* gPaperSizeArray_PT2500_PTM6500: the vendor looks a page up by its size in
 * PostScript points, not by name, and takes both the DMPAPER code and the
 * padded video size from here.  Sizes that are absent become custom pages. */
typedef struct
{
  unsigned width_pt;
  unsigned height_pt;
  unsigned video_x;
  unsigned video_y;
  unsigned dmpaper;
  const char *name;
} paper_entry_t;

static const paper_entry_t paper_table[] = {
  { 420, 595, 3264, 4736, 11, "A5" },
  { 595, 842, 4736, 6784, 9, "A4" },
  { 612, 792, 4864, 6368, 1, "Letter" },
  { 612, 1008, 4864, 8192, 5, "Legal" },
  { 516, 729, 4064, 5856, 13, "B5" },
  { 279, 540, 2112, 4288, 37, "EnvMonarch" },
  { 312, 624, 2368, 4992, 27, "EnvDL" },
  { 459, 649, 3616, 5184, 28, "EnvC5" },
  { 297, 684, 2240, 5472, 20, "Com10Envelope" },
  { 283, 420, 2144, 3264, 43, "Postcard" },
  { 524, 737, 4160, 5920, 263, "16K" },
  { 553, 765, 4384, 6144, 264, "Big16K" },
  { 369, 524, 2848, 4160, 266, "32K" },
  { 383, 553, 2976, 4384, 260, "Big32K" },
  { 298, 420, 2272, 3264, 70, "A6" },
  { 499, 709, 3936, 5696, 34, "ISOB5" },
  { 522, 756, 4128, 6080, 7, "Executive" },
  { 612, 936, 4864, 7584, 14, "Folio" },
  { 612, 972, 4864, 7872, 269, "Oficio" },
  { 396, 612, 3072, 4864, 6, "Statement" },
  { 323, 459, 2464, 3616, 31, "EnvC6" },
  { 340, 652, 2624, 5216, 270, "ZL" },
  { 354, 499, 2720, 3936, 271, "B6" }
};

/* The only entry in the vendor's lookup that is not a plain table search: a
 * 421x595 pt sheet is A5 turned on its side.  GetPaperArrayIndex() answers
 * with the A5 row and raises a flag, and on this platform the flag makes the
 * filter announce DMPAPER 61 (A5 rotated) instead of 11, swap the video size,
 * and turn the gray page a quarter turn clockwise before halftoning.  No PPD
 * in either driver asks for 421x595, so this is reachable only from a raster
 * some other producer wrote; it is implemented because the size is in the
 * lookup, not because CUPS has ever been seen to emit it.  See
 * notes/print-protocol.md section 4.1. */
#define PT_A5_ROT_WIDTH_PT 421u
#define PT_A5_ROT_HEIGHT_PT 595u
#define PT_A5_WIDTH_PT 420u
#define PT_A5_HEIGHT_PT 595u
#define ZJ_DMPAPER_A5_ROTATED 61u

static const paper_entry_t *
paper_find (unsigned width_pt, unsigned height_pt)
{
  size_t i;

  for (i = 0; i < sizeof (paper_table) / sizeof (paper_table[0]); i++)
    if (paper_table[i].width_pt == width_pt
        && paper_table[i].height_pt == height_pt)
      return &paper_table[i];
  return NULL;
}

/* --------------------------------------------------------- halftone tables */

/* HT_TABLE_600: 16x16 clustered-dot threshold matrix. */
static const uint8_t ht_table_600[256] = {
  0x06, 0x52, 0xe9, 0xfe, 0xfb, 0xe2, 0x46, 0x05, 0x07, 0x55, 0xeb, 0xff,
  0xfc, 0xe4, 0x49, 0x05, 0x37, 0x79, 0xa5, 0xda, 0xd3, 0x98, 0x72, 0x31,
  0x39, 0x7a, 0xa7, 0xdc, 0xd5, 0x9c, 0x73, 0x2f, 0xc7, 0x86, 0x69, 0x29,
  0x1b, 0x5f, 0x7e, 0xc0, 0xca, 0x88, 0x6b, 0x2d, 0x1f, 0x61, 0x7b, 0xbe,
  0xf3, 0xb4, 0x12, 0x03, 0x01, 0x0d, 0xac, 0xee, 0xf4, 0xb6, 0x13, 0x03,
  0x01, 0x0c, 0xaa, 0xed, 0xf8, 0xde, 0x41, 0x04, 0x0a, 0x4f, 0xe8, 0xfd,
  0xfa, 0xe0, 0x43, 0x04, 0x09, 0x4c, 0xe6, 0xfc, 0xd0, 0x91, 0x6d, 0x35,
  0x3e, 0x77, 0xa2, 0xd8, 0xd2, 0x95, 0x70, 0x33, 0x3b, 0x75, 0x9f, 0xd7,
  0x16, 0x58, 0x83, 0xc5, 0xce, 0x8e, 0x66, 0x26, 0x18, 0x5b, 0x80, 0xc3,
  0xcc, 0x8b, 0x64, 0x22, 0x02, 0x10, 0xb1, 0xf1, 0xf7, 0xbb, 0x15, 0x02,
  0x01, 0x0f, 0xaf, 0xf0, 0xf5, 0xb9, 0x14, 0x02, 0x07, 0x55, 0xeb, 0xff,
  0xfc, 0xe4, 0x49, 0x05, 0x06, 0x52, 0xe9, 0xfe, 0xfb, 0xe2, 0x46, 0x05,
  0x39, 0x7a, 0xa7, 0xdc, 0xd5, 0x9c, 0x73, 0x2f, 0x37, 0x79, 0xa5, 0xda,
  0xd3, 0x98, 0x72, 0x31, 0xca, 0x88, 0x6b, 0x2d, 0x1f, 0x61, 0x7b, 0xbe,
  0xc7, 0x86, 0x69, 0x29, 0x1b, 0x5f, 0x7e, 0xc0, 0xf4, 0xb6, 0x13, 0x03,
  0x01, 0x0c, 0xaa, 0xed, 0xf3, 0xb4, 0x12, 0x03, 0x01, 0x0d, 0xac, 0xee,
  0xfa, 0xe0, 0x43, 0x04, 0x09, 0x4c, 0xe6, 0xfc, 0xf8, 0xde, 0x41, 0x04,
  0x0a, 0x4f, 0xe8, 0xfd, 0xd2, 0x95, 0x70, 0x33, 0x3b, 0x75, 0x9f, 0xd7,
  0xd0, 0x91, 0x6d, 0x35, 0x3e, 0x77, 0xa2, 0xd8, 0x18, 0x5b, 0x80, 0xc3,
  0xcc, 0x8b, 0x64, 0x22, 0x16, 0x58, 0x83, 0xc5, 0xce, 0x8e, 0x66, 0x26,
  0x01, 0x0f, 0xaf, 0xf0, 0xf5, 0xb9, 0x14, 0x02, 0x02, 0x10, 0xb1, 0xf1,
  0xf7, 0xbb, 0x15, 0x02
};

/* HT_TABLE_2BIG maps a 0..255 ink level onto one of the four 1200 dpi dot
 * codes: 0 -> 0, 1..23 -> 1, 24..63 -> 2, 64..255 -> 3. */
static uint8_t
ht_level_to_code (int level)
{
  if (level <= 0)
    return 0;
  if (level < 24)
    return 1;
  if (level < 64)
    return 2;
  return 3;
}

/* ------------------------------------------------------------ diagnostics */

static volatile sig_atomic_t job_cancelled;

static void
on_cancel (int sig)
{
  (void)sig;
  job_cancelled = 1;
}

#ifdef __GNUC__
__attribute__ ((format (printf, 2, 3)))
#endif
static void
msg (const char *prefix, const char *fmt, ...)
{
  va_list ap;

  fputs (prefix, stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  fflush (stderr);
}

#define pt_error(...) msg ("ERROR: ", __VA_ARGS__)
#define pt_warn(...) msg ("WARNING: ", __VA_ARGS__)
#define pt_info(...) msg ("INFO: ", __VA_ARGS__)

/* --------------------------------------------------------- growable buffer */

typedef struct
{
  uint8_t *data;
  size_t len;
  size_t cap;
  int failed; /* sticky: a failed grow poisons the whole buffer */
} buf_t;

static void
buf_free (buf_t *b)
{
  free (b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

static void
buf_append (buf_t *b, const void *src, size_t n)
{
  if (b->failed)
    return;
  if (b->len + n > b->cap)
    {
      size_t cap = b->cap ? b->cap : 65536;
      uint8_t *p;

      while (cap < b->len + n)
        cap *= 2;
      p = realloc (b->data, cap);
      if (!p)
        {
          b->failed = 1;
          return;
        }
      b->data = p;
      b->cap = cap;
    }
  memcpy (b->data + b->len, src, n);
  b->len += n;
}

/* ------------------------------------------------------- chunk generation */

typedef struct
{
  uint16_t type;
  uint32_t value;
} zj_item_t;

static void
put_be16 (uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void
put_be32 (uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* Write one ZjStream chunk: 16-byte header, then nitems 12-byte items, then
 * raw bytes.  item_bytes counts only the items, never the raw payload. */
static int
write_chunk (FILE *fp, uint32_t type, const zj_item_t *items, unsigned nitems,
             const void *raw, size_t rawlen)
{
  uint8_t hdr[16];
  unsigned i;

  put_be32 (hdr, (uint32_t)(16 + nitems * 12 + rawlen));
  put_be32 (hdr + 4, type);
  put_be32 (hdr + 8, nitems);
  put_be16 (hdr + 12, (uint16_t)(nitems * 12));
  put_be16 (hdr + 14, ZJ_SIGNATURE);
  if (fwrite (hdr, 1, sizeof (hdr), fp) != sizeof (hdr))
    return -1;

  for (i = 0; i < nitems; i++)
    {
      uint8_t item[12];

      put_be32 (item, 12);
      put_be16 (item + 4, items[i].type);
      item[6] = 1; /* param */
      item[7] = 0; /* reserved */
      put_be32 (item + 8, items[i].value);
      if (fwrite (item, 1, sizeof (item), fp) != sizeof (item))
        return -1;
    }

  if (rawlen && fwrite (raw, 1, rawlen, fp) != rawlen)
    return -1;
  return 0;
}

/* ------------------------------------------------------------ job options */

typedef struct
{
  int density;       /* value of the Pantum DENSITY item */
  int dpi1200;       /* two bits per pixel, doubled JBIG width */
  int negative;      /* invert the halftone decision */
  int rotate180;     /* turn the rendered page upside down */
  int manual_duplex; /* split the job into fronts and backs */
  int tray;          /* ZJI_DMDEFAULTSOURCE for plain media */
} job_options_t;

/* Look an option up the way the vendor's GetSelectOption() does: the job
 * options win, the PPD default is the fallback. */
static const char *
opt_get (const char *keyword, int num_options, cups_option_t *options,
         ppd_file_t *ppd)
{
  const char *value = cupsGetOption (keyword, num_options, options);
  ppd_option_t *po;

  if (value)
    return value;
  if (!ppd)
    return NULL;
  po = ppdFindOption (ppd, keyword);
  if (po && po->defchoice[0])
    return po->defchoice;
  return NULL;
}

static ppd_file_t *
ppd_open (const char *path)
{
  return ppdOpenFile (path);
}

static void
ppd_free (ppd_file_t *ppd)
{
  ppdClose (ppd);
}

#pragma GCC diagnostic pop

static int
opt_bool (const char *keyword, int num_options, cups_option_t *options,
          ppd_file_t *ppd)
{
  const char *v = opt_get (keyword, num_options, options, ppd);

  if (!v)
    return 0;
  return !strcasecmp (v, "true") || !strcasecmp (v, "on")
         || !strcasecmp (v, "yes") || !strcasecmp (v, "1");
}

static void
options_read (job_options_t *opt, int num_options, cups_option_t *options,
              ppd_file_t *ppd)
{
  const char *v;

  memset (opt, 0, sizeof (*opt));
  opt->density = 2;

  v = opt_get ("Density", num_options, options, ppd);
  if (v && isdigit ((unsigned char)*v))
    {
      /* The value reaches the firmware untranslated, and strtol() saturates
       * at LONG_MAX, so pin it to the range the PPD offers rather than
       * forwarding whatever the job asked for. */
      long d = strtol (v, NULL, 10);

      opt->density = d > 4 ? 4 : (int)d;
    }
  /* Toner save has no bit of its own on this platform: it is the minimum
   * density, and it overrides an explicit Density. */
  if (opt_bool ("TonerMode", num_options, options, ppd))
    opt->density = 0;

  opt->dpi1200 = opt_bool ("DPI1200", num_options, options, ppd);
  opt->negative = opt_bool ("NegativePrint", num_options, options, ppd);
  opt->rotate180 = opt_bool ("ImageRotation", num_options, options, ppd);
  opt->manual_duplex = opt_bool ("ManualDuplex", num_options, options, ppd);

  /* The vendor PPD never declares InputSlot, so the vendor filter only ever
   * saw the default; ours declares it and the codes below are what the
   * firmware expects. */
  v = opt_get ("InputSlot", num_options, options, ppd);
  if (v && !strcasecmp (v, "ManualFeed"))
    opt->tray = 1;
  else if (v && !strcasecmp (v, "AutoFeed"))
    opt->tray = 2;
  else
    opt->tray = 0;
}

/* ------------------------------------------------------------- page layout */

typedef struct
{
  unsigned dmpaper; /* 0 means "custom", which adds three more items */
  unsigned video_x; /* width of the page buffer in 600 dpi dots */
  unsigned video_y; /* height of the page buffer in 600 dpi dots */
  unsigned depth;   /* bits per pixel of the JBIG image: 1 or 2 */
  int rotate90;     /* A5 fed sideways: quarter turn before halftoning */
} page_layout_t;

static void
page_layout (page_layout_t *lay, const cups_page_header2_t *hdr,
             const job_options_t *opt)
{
  const paper_entry_t *paper;

  lay->depth = opt->dpi1200 ? 2 : 1;
  lay->rotate90 = 0;

  paper = paper_find (hdr->PageSize[0], hdr->PageSize[1]);
  if (!paper && hdr->PageSize[0] == PT_A5_ROT_WIDTH_PT
      && hdr->PageSize[1] == PT_A5_ROT_HEIGHT_PT)
    {
      paper = paper_find (PT_A5_WIDTH_PT, PT_A5_HEIGHT_PT);
      lay->rotate90 = paper != NULL;
    }

  if (paper)
    {
      /* Everything downstream works in the geometry that reaches the engine,
       * so the swap happens here and the raster is turned to match. */
      lay->dmpaper = lay->rotate90 ? ZJ_DMPAPER_A5_ROTATED : paper->dmpaper;
      lay->video_x = lay->rotate90 ? paper->video_y : paper->video_x;
      lay->video_y = lay->rotate90 ? paper->video_x : paper->video_y;
      return;
    }

  /* Unknown size: announce a custom page and pad the raster ourselves.  The
   * width has to stay a multiple of 32 dots, the height a multiple of 8. */
  lay->dmpaper = 0;
  lay->video_x = (hdr->cupsWidth + 31u) & ~31u;
  lay->video_y = (hdr->cupsHeight + 7u) & ~7u;
}

/* ------------------------------------------------------------- halftoning */

/* Dither one 8bpp gray row into the packed page bitmap.  Grey 0 is black and
 * 255 is paper white; a set output bit means toner. */
static void
halftone_row (uint8_t *out, const uint8_t *gray, unsigned width, unsigned y,
              unsigned depth, int negative)
{
  const uint8_t *thr_row = ht_table_600 + (y & 15) * 16;
  unsigned x;
  unsigned acc = 0;
  unsigned shift = 8;

  for (x = 0; x < width; x++)
    {
      uint8_t thr = thr_row[x & 15];
      unsigned code;

      if (depth == 1)
        code = negative ? (gray[x] >= thr) : (gray[x] < thr);
      else
        {
          int level = gray[x] == 0 ? 0xff : 0xff - gray[x] - thr;

          code = ht_level_to_code (level);
          if (negative)
            code = 3 - code;
        }
      shift -= depth;
      acc |= code << shift;
      if (shift == 0)
        {
          *out++ = (uint8_t)acc;
          acc = 0;
          shift = 8;
        }
    }
}

/* --------------------------------------------------------- JBIG encoding */

static void
jbig_collect (unsigned char *start, size_t len, void *ctx)
{
  buf_append ((buf_t *)ctx, start, len);
}

/* Compress the packed page bitmap into a complete BIE.  jbig-kit with these
 * options is bit-identical to the vendor's private fork of it. */
static int
jbig_compress (buf_t *bie, uint8_t *bitmap, unsigned width, unsigned height)
{
  struct jbg_enc_state s;
  unsigned char *planes[1];

  planes[0] = bitmap;
  jbg_enc_init (&s, width, height, 1, planes, jbig_collect, bie);
  jbg_enc_layers (&s, 0); /* no resolution reduction, sequential coding */
  jbg_enc_options (&s, JBIG_ORDER, JBIG_OPTIONS, JBIG_L0, 0, -1);
  jbg_enc_out (&s);
  jbg_enc_free (&s);

  if (bie->failed || bie->len < JBIG_BIH_LEN)
    {
      pt_error ("Out of memory while compressing a page");
      return -1;
    }
  return 0;
}

/* ---------------------------------------------------------- page emission */

/* Write START_PAGE, the JBIG entity and END_PAGE for one rendered page. */
static int
emit_page (FILE *fp, const cups_page_header2_t *hdr, const page_layout_t *lay,
           const job_options_t *opt, const buf_t *bie)
{
  /* Worst case: DMPAPER, the three custom-size items, and the thirteen items
   * every page carries.  Keep this in step with the code below. */
  zj_item_t items[1 + 3 + 13];
  unsigned n = 0;
  unsigned media;
  unsigned tray;
  size_t off;

  /* The media type travels in the raster header, set by the PPD's
   * *MediaType entries; the filter never re-reads the option. */
  media = hdr->cupsMediaType ? hdr->cupsMediaType : 1;
  /* Anything but plain paper forces the manual tray. */
  tray = media != 1 ? 1u : (unsigned)opt->tray;

  items[n].type = ZJI_DMPAPER;
  items[n++].value = lay->dmpaper;
  if (lay->dmpaper == 0)
    {
      /* Custom page: three extra items, in this order, right after DMPAPER.
       * The size is expressed in 1/600 inch device dots. */
      items[n].type = ZJI_CUSTOM_UNIT;
      items[n++].value = 0;
      items[n].type = ZJI_CUSTOM_WIDTH;
      items[n++].value = (uint32_t)((uint64_t)hdr->PageSize[0] * 600u / 72u);
      items[n].type = ZJI_CUSTOM_HEIGHT;
      items[n++].value = (uint32_t)((uint64_t)hdr->PageSize[1] * 600u / 72u);
    }
  items[n].type = ZJI_DMCOPIES;
  items[n++].value = hdr->NumCopies ? hdr->NumCopies : 1u;
  items[n].type = ZJI_DMDEFAULTSOURCE;
  items[n++].value = tray;
  items[n].type = ZJI_DMMEDIATYPE;
  items[n++].value = media;
  items[n].type = ZJI_NBIE;
  items[n++].value = 1;
  items[n].type = ZJI_RESOLUTION_X;
  items[n++].value = hdr->HWResolution[0];
  items[n].type = ZJI_RESOLUTION_Y;
  items[n++].value = hdr->HWResolution[1];
  /* RET really does sit between the resolutions and the raster size. */
  items[n].type = ZJI_RET;
  items[n++].value = 0;
  items[n].type = ZJI_RASTER_X;
  items[n++].value = lay->video_x * lay->depth;
  items[n].type = ZJI_RASTER_Y;
  items[n++].value = lay->video_y;
  items[n].type = ZJI_VIDEO_BPP;
  items[n++].value = lay->depth;
  items[n].type = ZJI_VIDEO_X;
  items[n++].value = lay->video_x;
  items[n].type = ZJI_VIDEO_Y;
  items[n++].value = lay->video_y;
  items[n].type = ZJI_PANTUM_DENSITY;
  items[n++].value = (uint32_t)opt->density;

  if (write_chunk (fp, ZJT_START_PAGE, items, n, NULL, 0) < 0)
    return -1;
  if (write_chunk (fp, ZJT_JBIG_BIH, NULL, 0, bie->data, JBIG_BIH_LEN) < 0)
    return -1;
  for (off = JBIG_BIH_LEN; off < bie->len; off += ZJ_BID_MAX)
    {
      size_t n_bytes = bie->len - off;

      if (n_bytes > ZJ_BID_MAX)
        n_bytes = ZJ_BID_MAX;
      if (write_chunk (fp, ZJT_JBIG_BID, NULL, 0, bie->data + off, n_bytes) < 0)
        return -1;
    }
  if (write_chunk (fp, ZJT_END_JBIG, NULL, 0, NULL, 0) < 0)
    return -1;
  return write_chunk (fp, ZJT_END_PAGE, NULL, 0, NULL, 0);
}

/* Compress a finished page bitmap and write the whole page out.  The bitmap
 * size is passed separately from the layout because the vendor's blank
 * manual-duplex sheet does not use the layout's own; see
 * render_blank_page(). */
static int
finish_page (FILE *fp, const cups_page_header2_t *hdr,
             const page_layout_t *lay, const job_options_t *opt,
             uint8_t *bitmap, unsigned bmp_w, unsigned bmp_h)
{
  buf_t bie = { NULL, 0, 0, 0 };
  int rc;

  rc = jbig_compress (&bie, bitmap, bmp_w * lay->depth, bmp_h);
  if (rc == 0)
    {
      rc = emit_page (fp, hdr, lay, opt, &bie);
      /* emit_page() only ever fails on a short write, and that is the one
       * failure the operator has to hear about: without this the job would
       * die silently when the backend goes away mid-page. */
      if (rc < 0)
        pt_error ("Cannot write the page out: %s", strerror (errno));
    }
  buf_free (&bie);
  return rc;
}

/* Dither one raster page and write it out. */
static int
render_page (FILE *fp, cups_raster_t *ras, const cups_page_header2_t *hdr,
             const job_options_t *opt)
{
  page_layout_t lay;
  uint8_t *bitmap = NULL;
  uint8_t *gray = NULL; /* whole gray page, only for the quarter turn */
  uint8_t *row = NULL;
  size_t bpl, rowlen;
  unsigned src_w, src_h;
  unsigned y, copy_w;
  int rc = -1;

  page_layout (&lay, hdr, opt);
  /* Both turns happen before halftoning, so the raster is read in the
   * geometry it arrives in and the quarter turn is applied to the gray page;
   * lay.video_* is already the geometry the engine is told about. */
  src_w = lay.rotate90 ? lay.video_y : lay.video_x;
  src_h = lay.rotate90 ? lay.video_x : lay.video_y;

  bpl = (size_t)lay.video_x * lay.depth / 8;
  rowlen = lay.video_x > src_w ? lay.video_x : src_w;
  if (hdr->cupsBytesPerLine > rowlen)
    rowlen = hdr->cupsBytesPerLine;

  bitmap = malloc (bpl * lay.video_y);
  row = malloc (rowlen);
  if (lay.rotate90)
    gray = malloc ((size_t)src_w * src_h);
  if (!bitmap || !row || (lay.rotate90 && !gray))
    {
      pt_error ("Out of memory while rendering a page");
      goto done;
    }

  /* The vendor allocates the page buffer at the padded size and pre-fills it
   * with paper white before copying the raster in, which is why the margins
   * stay white normally and turn black under NegativePrint. */
  memset (row, 0xff, rowlen);
  copy_w = hdr->cupsWidth < src_w ? hdr->cupsWidth : src_w;

  for (y = 0; y < src_h; y++)
    {
      /* ImageRotation turns the gray page around before it is dithered, so
       * the halftone screen stays anchored to the sheet, not to the image. */
      unsigned dst = opt->rotate180 ? src_h - 1 - y : y;

      if (y < hdr->cupsHeight)
        {
          if (!cupsRasterReadPixels (ras, row, hdr->cupsBytesPerLine))
            {
              /* SIGTERM interrupts the read, so a cancelled job lands here
               * too; that is not something to shout about. */
              if (job_cancelled)
                pt_info ("Cancelled while reading page data");
              else
                pt_error ("Short read from the raster stream at line %u", y);
              goto done;
            }
          /* Re-whiten whatever the raster does not cover. */
          if (copy_w < rowlen)
            memset (row + copy_w, 0xff, rowlen - copy_w);
        }
      else if (y == hdr->cupsHeight)
        memset (row, 0xff, rowlen);

      if (opt->rotate180)
        {
          unsigned a, b;

          for (a = 0, b = src_w - 1; a < b; a++, b--)
            {
              uint8_t t = row[a];

              row[a] = row[b];
              row[b] = t;
            }
        }

      if (lay.rotate90)
        memcpy (gray + (size_t)dst * src_w, row, src_w);
      else
        halftone_row (bitmap + (size_t)dst * bpl, row, lay.video_x, dst,
                      lay.depth, opt->negative);
    }

  /* Quarter turn clockwise: output row y is source column y read from the
   * bottom up.  The halftone screen is indexed by the turned coordinates,
   * because the vendor rotates the gray page and dithers afterwards. */
  if (lay.rotate90)
    for (y = 0; y < lay.video_y; y++)
      {
        unsigned x;

        for (x = 0; x < lay.video_x; x++)
          row[x] = gray[(size_t)(src_h - 1 - x) * src_w + y];
        halftone_row (bitmap + (size_t)y * bpl, row, lay.video_x, y, lay.depth,
                      opt->negative);
      }

  /* Drain any lines that do not fit the page buffer, so the next header in
   * the stream still starts where the library expects it. */
  for (y = src_h; y < hdr->cupsHeight; y++)
    if (!cupsRasterReadPixels (ras, row, hdr->cupsBytesPerLine))
      break;

  rc = finish_page (fp, hdr, &lay, opt, bitmap, lay.video_x, lay.video_y);

done:
  free (bitmap);
  free (gray);
  free (row);
  return rc;
}

/* Write the sheet the manual duplex path needs when the page count is odd.
 * The vendor sends a bitmap with no toner at all, so this page stays empty
 * even under NegativePrint, where a dithered blank page would come out
 * solid black. */
static int
render_blank_page (FILE *fp, const cups_page_header2_t *hdr,
                   const job_options_t *opt)
{
  page_layout_t lay;
  unsigned bmp_w, bmp_h;
  uint8_t *bitmap;
  int rc;

  page_layout (&lay, hdr, opt);
  /* On a sheet that is turned a quarter (421x595 pt), the vendor announces
   * the turned video size here but compresses an image of the untuned one:
   * the swap that goes with the turn happens inside the raster loop, and a
   * blank page never enters it.  The two disagree; reproduced as found, see
   * notes/print-protocol.md section 4.1. */
  bmp_w = lay.rotate90 ? lay.video_y : lay.video_x;
  bmp_h = lay.rotate90 ? lay.video_x : lay.video_y;
  bitmap = calloc ((size_t)bmp_w * lay.depth / 8, bmp_h);
  if (!bitmap)
    {
      pt_error ("Out of memory while rendering the blank duplex page");
      return -1;
    }
  rc = finish_page (fp, hdr, &lay, opt, bitmap, bmp_w, bmp_h);
  free (bitmap);
  return rc;
}

/* -------------------------------------------------------- manual duplex */

/* Where one buffered page sits in the temporary file. */
typedef struct
{
  long offset;
  long length;
} page_slot_t;

typedef struct
{
  FILE *fp;
  page_slot_t *slots;
  unsigned count;
  unsigned cap;
} page_spool_t;

static int
spool_open (page_spool_t *sp)
{
  memset (sp, 0, sizeof (*sp));
  sp->fp = tmpfile ();
  if (!sp->fp)
    {
      pt_error ("Cannot create a temporary file for manual duplex: %s",
                strerror (errno));
      return -1;
    }
  return 0;
}

static void
spool_close (page_spool_t *sp)
{
  if (sp->fp)
    fclose (sp->fp);
  free (sp->slots);
  memset (sp, 0, sizeof (*sp));
}

/* Remember where the page that was just written to the spool begins. */
static int
spool_mark (page_spool_t *sp, long start)
{
  long end = ftell (sp->fp);

  if (end < 0)
    return -1;
  if (sp->count == sp->cap)
    {
      unsigned cap = sp->cap ? sp->cap * 2 : 16;
      page_slot_t *p = realloc (sp->slots, cap * sizeof (*p));

      if (!p)
        return -1;
      sp->slots = p;
      sp->cap = cap;
    }
  sp->slots[sp->count].offset = start;
  sp->slots[sp->count].length = end - start;
  sp->count++;
  return 0;
}

static int
spool_copy (page_spool_t *sp, unsigned index, FILE *out)
{
  char chunk[65536];
  long left = sp->slots[index].length;

  if (fseek (sp->fp, sp->slots[index].offset, SEEK_SET) < 0)
    return -1;
  while (left > 0)
    {
      size_t want = left > (long)sizeof (chunk) ? sizeof (chunk) : (size_t)left;
      size_t got = fread (chunk, 1, want, sp->fp);

      if (got != want || fwrite (chunk, 1, got, out) != got)
        return -1;
      left -= (long)got;
    }
  return 0;
}

/* ------------------------------------------------------------------ main */

int
main (int argc, char *argv[])
{
  const char *ppd_path;
  ppd_file_t *ppd = NULL;
  cups_option_t *options = NULL;
  int num_options = 0;
  job_options_t opt;
  cups_raster_t *ras = NULL;
  cups_page_header2_t hdr, last_hdr;
  page_spool_t fronts, backs;
  int spooling = 0;
  int fd = 0;
  unsigned page = 0;
  int status = 0;
  struct sigaction sa;

  if (argc < 6 || argc > 7)
    {
      pt_error ("Usage: %s job-id user title copies options [file]", argv[0]);
      return 1;
    }

  /* No SA_RESTART: an interrupted read is how a cancelled job gets noticed
   * in the middle of a page. */
  memset (&sa, 0, sizeof (sa));
  sa.sa_handler = on_cancel;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGTERM, &sa, NULL);

  /* cupsd starts filters with the default disposition for SIGPIPE, so a
   * backend that goes away would kill this process outright and the job would
   * fail with nothing in the log.  Ignoring it turns the same event into a
   * short write that the code below can report. */
  sa.sa_handler = SIG_IGN;
  sigaction (SIGPIPE, &sa, NULL);

  if (argc == 7)
    {
      fd = open (argv[6], O_RDONLY);
      if (fd < 0)
        {
          pt_error ("Cannot open the print file \"%s\": %s", argv[6],
                    strerror (errno));
          return 1;
        }
    }

  num_options = cupsParseOptions (argv[5], 0, &options);
  ppd_path = getenv ("PPD");
  if (ppd_path && *ppd_path)
    {
      ppd = ppd_open (ppd_path);
      if (!ppd)
        pt_warn ("Cannot read the PPD \"%s\"; falling back to built-in "
                 "defaults for options the job does not set",
                 ppd_path);
    }
  options_read (&opt, num_options, options, ppd);

  /* Leave no stale manual-duplex prompt behind from an earlier job. */
  fputs ("STATE: -MDuplex\n", stderr);

  memset (&fronts, 0, sizeof (fronts));
  memset (&backs, 0, sizeof (backs));
  spooling = opt.manual_duplex;
  if (spooling && (spool_open (&fronts) < 0 || spool_open (&backs) < 0))
    {
      status = 1;
      goto cleanup;
    }

  ras = cupsRasterOpen (fd, CUPS_RASTER_READ);
  if (!ras)
    {
      pt_error ("Cannot read the raster stream: %s", cupsRasterErrorString ());
      status = 1;
      goto cleanup;
    }

  {
    zj_item_t doc[3] = { { ZJI_PAGECOUNT, 0 },
                         { ZJI_DMCOLLATE, 0 },
                         { ZJI_DMDUPLEX, ZJ_DMDUPLEX_SIMPLEX } };

    if (fwrite (UEL ZJ_MAGIC, 1, UEL_LEN + 4, stdout) != UEL_LEN + 4
        || write_chunk (stdout, ZJT_PANTUM_PARAM, NULL, 0, pantum_param_blob,
                        sizeof (pantum_param_blob))
               < 0
        || write_chunk (stdout, ZJT_START_DOC, doc, 3, NULL, 0) < 0)
      {
        pt_error ("Cannot write to the printer: %s", strerror (errno));
        status = 1;
        goto cleanup;
      }
  }

  memset (&last_hdr, 0, sizeof (last_hdr));
  while (!job_cancelled && cupsRasterReadHeader2 (ras, &hdr))
    {
      FILE *sink = stdout;
      page_spool_t *spool = NULL;
      long start = 0;

      /* Every buffer below is sized from these fields, so they are checked
       * before anything is allocated.  At one byte per pixel a raster line
       * can be neither shorter than the page nor longer than the widest page
       * this filter accepts. */
      if (hdr.cupsBitsPerPixel != 8 || hdr.cupsWidth == 0
          || hdr.cupsHeight == 0 || hdr.cupsWidth > PT_MAX_DOTS
          || hdr.cupsHeight > PT_MAX_DOTS
          || hdr.cupsBytesPerLine < hdr.cupsWidth
          || hdr.cupsBytesPerLine > PT_MAX_DOTS)
        {
          pt_error ("Unsupported raster format: %u bits per pixel, %ux%u, "
                    "%u bytes per line",
                    hdr.cupsBitsPerPixel, hdr.cupsWidth, hdr.cupsHeight,
                    hdr.cupsBytesPerLine);
          status = 1;
          break;
        }
      /* CUPS_CSPACE_W and CUPS_CSPACE_SW are both 8-bit luminance with 0 as
       * black, which is what the halftoner expects. */
      if (hdr.cupsColorSpace != CUPS_CSPACE_W
          && hdr.cupsColorSpace != CUPS_CSPACE_SW)
        {
          pt_error ("Unsupported colour space %u, expected 8-bit grayscale",
                    (unsigned)hdr.cupsColorSpace);
          status = 1;
          break;
        }

      page++;
      /* "PAGE: <number> <copies>": the engine makes the copies itself, from
       * the DMCOPIES item, so they have to be accounted for here. */
      msg ("PAGE: ", "%u %u", page, hdr.NumCopies ? hdr.NumCopies : 1u);
      pt_info ("Rendering page %u", page);

      if (spooling)
        {
          spool = (page & 1) ? &fronts : &backs;
          sink = spool->fp;
          start = ftell (sink);
          if (start < 0)
            {
              pt_error ("Cannot position the manual duplex spool: %s",
                        strerror (errno));
              status = 1;
              break;
            }
        }

      if (render_page (sink, ras, &hdr, &opt) < 0)
        {
          /* A cancelled job has already been reported and is not a failure. */
          if (!job_cancelled)
            status = 1;
          break;
        }
      if (spool && spool_mark (spool, start) < 0)
        {
          pt_error ("Cannot record the page in the manual duplex spool: %s",
                    strerror (errno));
          status = 1;
          break;
        }
      last_hdr = hdr;
    }

  if (!status && !job_cancelled && page == 0)
    {
      pt_error ("No pages were found in the print file");
      status = 1;
    }

  /* Replaying the spool would print the whole job, so a cancelled manual
   * duplex job stops here with only the document trailer still to write. */
  if (!status && !job_cancelled && spooling)
    {
      unsigned i;

      /* Fronts first, in order.  Then the pause chunk, then the backs in
       * reverse order, because the operator flips the whole output stack. */
      for (i = 0; i < fronts.count; i++)
        if (spool_copy (&fronts, i, stdout) < 0)
          {
            pt_error ("Cannot replay the manual duplex spool: %s",
                      strerror (errno));
            status = 1;
            break;
          }
      if (!status)
        {
          fputs ("STATE: +MDuplex\n", stderr);
          pt_info ("Manual duplex: take the stack out of the output tray, "
                   "turn it over and put it back printed side down");
          if (write_chunk (stdout, ZJT_2600N_PAUSE, NULL, 0, NULL, 0) < 0)
            status = 1;

          /* The last sheet has no back side when the page count is odd; the
           * printer still expects one, so send a blank one. */
          if (backs.count < fronts.count)
            {
              /* A sheet the accounting has not seen yet: it was never in the
               * spool, so no PAGE: line was written for it. */
              msg ("PAGE: ", "%u 1", ++page);
              if (render_blank_page (stdout, &last_hdr, &opt) < 0)
                status = 1;
            }
          for (i = backs.count; !status && i-- > 0;)
            if (spool_copy (&backs, i, stdout) < 0)
              {
                pt_error ("Cannot replay the manual duplex spool: %s",
                          strerror (errno));
                status = 1;
              }
        }
    }

  if (write_chunk (stdout, ZJT_END_DOC, NULL, 0, NULL, 0) < 0
      || fwrite (UEL, 1, UEL_LEN, stdout) != UEL_LEN)
    {
      pt_error ("Cannot write to the printer: %s", strerror (errno));
      status = 1;
    }
  if (fflush (stdout) != 0)
    {
      pt_error ("Cannot write to the printer: %s", strerror (errno));
      status = 1;
    }

  if (job_cancelled)
    pt_info ("Job cancelled after %u page(s)", page);

cleanup:
  if (ras)
    cupsRasterClose (ras);
  if (spooling)
    {
      spool_close (&fronts);
      spool_close (&backs);
    }
  if (ppd)
    ppd_free (ppd);
  cupsFreeOptions (num_options, options);
  if (fd > 0)
    close (fd);
  return status;
}
