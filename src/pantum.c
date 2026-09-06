/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SANE backend for Pantum M6500/M6507 series scanners (USB 232b:0e20).
 *
 * Free reimplementation of the "ASP" bulk protocol; talks to libusb-1.0
 * directly, so it does not need sanei_usb or anything else from the
 * sane-backends source tree.
 *
 * The device is driven synchronously: sane_start sets up the job and waits
 * for the first block of pixels, sane_read pumps further frames whenever its
 * output queue runs dry.  No reader thread is needed because every bulk read
 * is bounded by a timeout and the frontend calls us often enough.
 */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <sane/sane.h>
#include <sane/saneopts.h>

#include "pantum.h"

#define PANTUM_VERSION_BUILD 1
#define PANTUM_CONFIG_FILE "pantum.conf"
#ifndef PANTUM_CONFIG_DIR
#define PANTUM_CONFIG_DIR "/etc/sane.d"
#endif

/* Bulk timeouts in milliseconds.  Scan frames may arrive only after the lamp
 * has warmed up and the carriage has homed, which takes tens of seconds. */
#define TIMEOUT_WRITE 10000
#define TIMEOUT_READ 180000
/* Tearing a job down must not block the frontend for minutes. */
#define TIMEOUT_TEARDOWN 5000
/* Non-blocking mode only ever glances at the endpoint. */
#define TIMEOUT_POLL 20

/* Bulk endpoints of the scanner interface. */
#define EP_OUT 0x02
#define EP_IN 0x82

#define MM_PER_UNIT 0.254 /* one hundredth of an inch */
#define MM_PER_INCH 25.4

/* ------------------------------------------------------------------ debug */

static int dbg_level;

#define DBG(level, ...)                                                       \
  do                                                                          \
    {                                                                         \
      if (dbg_level >= (level))                                               \
        pantum_debug (__VA_ARGS__);                                           \
    }                                                                         \
  while (0)

static void
pantum_debug (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  fputs ("[pantum] ", stderr);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
}

/* ----------------------------------------------------------- global state */

typedef struct Pantum_Id
{
  struct Pantum_Id *next;
  uint16_t vid;
  uint16_t pid;
  char *model;
} Pantum_Id;

static SANE_Bool initialised;
static libusb_context *usb_ctx;
static Pantum_Id *id_list;             /* usb ids from pantum.conf */
static Pantum_Device *device_list;     /* probed devices */
static const SANE_Device **device_ptrs; /* NULL-terminated view for SANE */
static Pantum_Handle *handle_list;

/* Halftone is a mode the vendor backend has but never offers to a SANE
 * frontend; see the dither section below for what it does here. */
static const SANE_String_Const mode_list[]
    = { SANE_VALUE_SCAN_MODE_LINEART, SANE_VALUE_SCAN_MODE_HALFTONE,
        SANE_VALUE_SCAN_MODE_GRAY, SANE_VALUE_SCAN_MODE_COLOR, NULL };

/* The vendor backend offers the feeder entries unconditionally and lets the
 * job fail when there is none; we only advertise them once the device has
 * answered CMD_ADF_STATUS with "feeder present". */
#define SOURCE_NAME_FLATBED "Flatbed"
#define SOURCE_NAME_ADF "ADF"
#define SOURCE_NAME_ADF_DUPLEX "ADF Duplex"

static const SANE_String_Const source_list_flatbed[]
    = { SOURCE_NAME_FLATBED, NULL };
static const SANE_String_Const source_list_adf[]
    = { SOURCE_NAME_FLATBED, SOURCE_NAME_ADF, SOURCE_NAME_ADF_DUPLEX, NULL };

/* The firmware rounds a requested resolution down to one of these.  1200 dpi
 * is advertised by the vendor driver for flatbed, and the device does accept
 * it, but a full page at that size is ~140 MB, so it is offered as-is. */
static const SANE_Word resolution_list[] = { 5, 75, 150, 300, 600, 1200 };
/* The feeder tops out at 600 dpi; the vendor's fix_window folds 1200 down. */
static const SANE_Word resolution_list_adf[] = { 4, 75, 150, 300, 600 };

static const SANE_Range threshold_range
    = { SANE_FIX (0.0), SANE_FIX (100.0), SANE_FIX (1.0) };
static const SANE_Range x_range
    = { SANE_FIX (0.0), SANE_FIX (PANTUM_MAX_X * MM_PER_UNIT), SANE_FIX (0.0) };
static const SANE_Range y_range
    = { SANE_FIX (0.0), SANE_FIX (PANTUM_MAX_Y * MM_PER_UNIT), SANE_FIX (0.0) };
static const SANE_Range y_range_adf
    = { SANE_FIX (0.0), SANE_FIX (PANTUM_MAX_Y_ADF * MM_PER_UNIT),
        SANE_FIX (0.0) };

/* ------------------------------------------------------------- byte order */

static uint32_t
get_be32 (const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

static void
put_be32 (uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* --------------------------------------------------------------- helpers */

static char *
pantum_strdup (const char *s)
{
  size_t n;
  char *p;

  if (!s)
    return NULL;
  n = strlen (s) + 1;
  p = malloc (n);
  if (p)
    memcpy (p, s, n);
  return p;
}

static void
sleep_ms (unsigned ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep (&ts, NULL);
}

static int
mm_to_units (SANE_Word fixed_mm)
{
  double mm = SANE_UNFIX (fixed_mm);
  int units = (int)(mm / MM_PER_UNIT + 0.5);
  return units < 0 ? 0 : units;
}

/* --------------------------------------------------------- tone correction */

static uint8_t
clamp_byte (double v)
{
  int i = (int)v; /* the vendor truncates here, it does not round */

  if (i < 0)
    return 0;
  if (i > 255)
    return 255;
  return (uint8_t)i;
}

/* The sensor is linear and dark: a blank sheet reads about 251 out of 255.
 * The vendor backend runs every scanned byte through a fixed tone curve
 * before handing it to the frontend, which is why its output is brighter than
 * a raw frame.  The curve is built in two steps, both taken from the vendor
 * blob (the table builder at 0xbbe0 and the gamma pass at 0xbb40):
 *
 *   1. a hand-fitted piecewise curve, different for 8-bit and 24-bit data,
 *      evaluated in double precision and truncated towards zero;
 *   2. base[i] -> round(255 * (base[i] / 255) ^ (1 / gamma)) with gamma 1.8.
 *
 * Only the coefficients are reproduced here, and the result is checked
 * against the vendor's output rather than against its table: see
 * dumps/scan/tone-curve-check.txt.gz.  Models the vendor special-cases to
 * gamma 1.0 (CM1100, BM23xx) are not in the M6500 family and are not
 * handled. */
static void
build_gamma (uint8_t table[256], SANE_Bool colour)
{
  double scale = pow (255.0, -1.0 / PANTUM_GAMMA);
  int i;

  for (i = 0; i < 256; i++)
    {
      double x = (double)i;
      double x2 = x * x;
      double x3 = x2 * x;
      double v;

      if (colour)
        {
          if (i < 50)
            v = (x * 326.56441) / (x + 223.04027);
          else if (i < 160)
            v = x * 1.3679907 + 1.3433091 - x2 * 0.0028273626
                + x3 * 9.1069919e-06;
          else
            v = 382.17813 - x * 0.2533356 - 3965432.2 / x2;
        }
      else
        {
          if (i < 80)
            v = x * 1.8279807 + 0.56917985 - x2 * 0.015226293
                + x3 * 0.00013340606;
          else
            v = x * 1.4804465 + 2.4442346 - x2 * 0.0024925889
                + x3 * 5.6432708e-06;
        }
      table[i] = clamp_byte (v);
    }

  for (i = 0; i < 256; i++)
    {
      double v = pow ((double)table[i], 1.0 / PANTUM_GAMMA) * scale * 255.0;
      table[i] = v > 255.0 ? 255 : (uint8_t)floor (v + 0.5);
    }
}

/* ------------------------------------------------------ halftone dithering */

/* The vendor backend has a fourth scan mode that its SANE list never offers:
 * mode 4 dithers the same 8-bit grey the device sends through a 128x128
 * blue-noise threshold mask, and only Pantum's own application is shown the
 * string that selects it.  This backend offers it, with a mask of its own.
 *
 * The vendor's mask is 16 KiB of data in a proprietary blob and is not
 * reproduced here.  What is reproduced is its class: the mask below is built
 * with Ulichney's void-and-cluster method, the standard way of constructing
 * a blue-noise threshold matrix, and comes out with the properties measured
 * on the vendor's one (notes/disasm-protocol.md section 5) - every threshold
 * from 1 to 255 used 64 or 65 times, a mean of 128, no period dividing 128 in
 * either axis, and Fourier energy that rises with radius (mean |F| 59 in the
 * innermost eighth against 10380 in the outermost).
 *
 * It is generated at run time instead of being committed as a generated
 * table because the generator is the part worth keeping and a table in the
 * tree would only be a cache of it.  That costs no predictability: the
 * initial pattern comes from a fixed seed and every step after it is a
 * deterministic scan, so every run builds the same 16384 bytes.  It costs
 * about a fifth of a second, once per process, in front of a scan that takes
 * tens of seconds.
 *
 * One deliberate difference from the vendor: the dither sits behind the same
 * tone curve as every other mode, where the vendor's halftone path skips the
 * curve and swaps in a separate contrast LUT below 300 dpi.  Halftone here is
 * Gray screened down to one bit, so it has to see the tones Gray shows. */

#define DITHER_SIZE 128
#define DITHER_CELLS (DITHER_SIZE * DITHER_SIZE)
#define DITHER_SIGMA 1.5
/* exp(-r^2 / 2 sigma^2) has fallen below 1e-6 past this radius. */
#define DITHER_RADIUS 8
#define DITHER_KERNEL (2 * DITHER_RADIUS + 1)
#define DITHER_SEED 0x9e3779b9u

/* One mask per process: it depends on nothing a handle can change. */
static uint8_t dither_mask[DITHER_CELLS]; /* thresholds, 1..255 */
static SANE_Bool dither_ready;

typedef struct
{
  uint8_t *pattern; /* the binary pattern being grown or thinned */
  float *energy;    /* pattern convolved with the gaussian, on the torus */
  float kernel[DITHER_KERNEL * DITHER_KERNEL];
} Dither_State;

/* Set or clear one cell and fold the same change into the energy field, so
 * the field never has to be recomputed from scratch. */
static void
dither_toggle (Dither_State *s, int cell, SANE_Bool set)
{
  int cx = cell % DITHER_SIZE;
  int cy = cell / DITHER_SIZE;
  int dy, dx;

  s->pattern[cell] = set ? 1 : 0;
  for (dy = -DITHER_RADIUS; dy <= DITHER_RADIUS; dy++)
    {
      int row = ((cy + dy) & (DITHER_SIZE - 1)) * DITHER_SIZE;
      const float *k
          = s->kernel + (dy + DITHER_RADIUS) * DITHER_KERNEL + DITHER_RADIUS;

      for (dx = -DITHER_RADIUS; dx <= DITHER_RADIUS; dx++)
        {
          int col = (cx + dx) & (DITHER_SIZE - 1);

          if (set)
            s->energy[row + col] += k[dx];
          else
            s->energy[row + col] -= k[dx];
        }
    }
}

/* The tightest cluster is the highest-energy cell that is set; the largest
 * void is the lowest-energy cell that is clear.  Ties go to the first cell in
 * scan order, which is what keeps the result reproducible.  Returns -1 when
 * the pattern holds no cell of the wanted colour. */
static int
dither_extreme (const Dither_State *s, uint8_t want, SANE_Bool biggest)
{
  int best = -1;
  float best_e = 0.0f;
  int i;

  for (i = 0; i < DITHER_CELLS; i++)
    {
      if (s->pattern[i] != want)
        continue;
      if (best < 0
          || (biggest ? s->energy[i] > best_e : s->energy[i] < best_e))
        {
          best = i;
          best_e = s->energy[i];
        }
    }
  return best;
}

static SANE_Status
build_dither_mask (void)
{
  Dither_State s;
  uint8_t *proto = NULL;
  uint16_t *rank = NULL;
  uint32_t rnd = DITHER_SEED;
  int ones = DITHER_CELLS / 10; /* Ulichney's starting density */
  int dy, dx, i, placed;

  if (dither_ready)
    return SANE_STATUS_GOOD;

  s.pattern = calloc (DITHER_CELLS, 1);
  s.energy = calloc (DITHER_CELLS, sizeof (float));
  proto = malloc (DITHER_CELLS);
  rank = calloc (DITHER_CELLS, sizeof (uint16_t));
  if (!s.pattern || !s.energy || !proto || !rank)
    {
      free (s.pattern);
      free (s.energy);
      free (proto);
      free (rank);
      return SANE_STATUS_NO_MEM;
    }

  for (dy = -DITHER_RADIUS; dy <= DITHER_RADIUS; dy++)
    for (dx = -DITHER_RADIUS; dx <= DITHER_RADIUS; dx++)
      s.kernel[(dy + DITHER_RADIUS) * DITHER_KERNEL + dx + DITHER_RADIUS]
          = (float)exp (-(double)(dx * dx + dy * dy)
                        / (2.0 * DITHER_SIGMA * DITHER_SIGMA));

  /* Seed: a fixed-seed xorshift white-noise pattern.  Only its statistics
   * matter - the next loop rearranges every point of it. */
  for (placed = 0; placed < ones;)
    {
      int cell;

      rnd ^= rnd << 13;
      rnd ^= rnd >> 17;
      rnd ^= rnd << 5;
      cell = (int)(rnd % (uint32_t)DITHER_CELLS);
      if (s.pattern[cell])
        continue;
      dither_toggle (&s, cell, SANE_TRUE);
      placed++;
    }

  /* Prototype: move the point from the tightest cluster into the largest
   * void until the two are the same cell, which is where the pattern has
   * stopped clustering.
   *
   * This loop and the two ranking loops after it keep the pattern between 1
   * and DITHER_CELLS - 1 points, so dither_extreme always has a cell to
   * return; the -1 tests are there so that a future change to those bounds
   * fails visibly instead of indexing pattern[] and rank[] at -1. */
  for (;;)
    {
      int cluster = dither_extreme (&s, 1, SANE_TRUE);
      int hole;

      if (cluster < 0)
        break;
      dither_toggle (&s, cluster, SANE_FALSE);
      hole = dither_extreme (&s, 0, SANE_FALSE);
      if (hole < 0)
        {
          dither_toggle (&s, cluster, SANE_TRUE);
          break;
        }
      dither_toggle (&s, hole, SANE_TRUE);
      if (hole == cluster)
        break;
    }
  memcpy (proto, s.pattern, DITHER_CELLS);

  /* Ranks below the prototype's density: thin it out, tightest cluster
   * first, and record the order in reverse. */
  for (i = ones - 1; i >= 0; i--)
    {
      int cluster = dither_extreme (&s, 1, SANE_TRUE);

      if (cluster < 0)
        break;
      dither_toggle (&s, cluster, SANE_FALSE);
      rank[cluster] = (uint16_t)i;
    }

  /* Ranks above it: back to the prototype, then fill the largest void over
   * and over until every cell has a rank.  The gaussian is linear on the
   * torus, so filling the largest void of the pattern is the same operation
   * as thinning the tightest cluster of its complement; the second half of
   * the range needs no separate loop. */
  memset (s.pattern, 0, DITHER_CELLS);
  memset (s.energy, 0, DITHER_CELLS * sizeof (float));
  for (i = 0; i < DITHER_CELLS; i++)
    if (proto[i])
      dither_toggle (&s, i, SANE_TRUE);
  for (i = ones; i < DITHER_CELLS; i++)
    {
      int hole = dither_extreme (&s, 0, SANE_FALSE);

      if (hole < 0)
        break;
      dither_toggle (&s, hole, SANE_TRUE);
      rank[hole] = (uint16_t)i;
    }

  /* Spread the 16384 ranks over the thresholds 1..255, so that a pixel of 0
   * is always black and one of 255 is always white. */
  for (i = 0; i < DITHER_CELLS; i++)
    dither_mask[i]
        = (uint8_t)(1 + (int)((uint32_t)rank[i] * 255u / DITHER_CELLS));

  free (s.pattern);
  free (s.energy);
  free (proto);
  free (rank);
  dither_ready = SANE_TRUE;
  DBG (2, "halftone: built the %dx%d blue-noise dither mask\n", DITHER_SIZE,
       DITHER_SIZE);
  return SANE_STATUS_GOOD;
}

/* Map a device status word onto SANE.  Codes come from the vendor backend's
 * reader loop; anything unknown is a plain error. */
static SANE_Status
map_status (uint32_t status)
{
  switch (status)
    {
    case 0:
      return SANE_STATUS_GOOD;
    case 2:
      return SANE_STATUS_DEVICE_BUSY;
    case 5:
      return SANE_STATUS_NO_DOCS;
    case 6:
    case 7:
      return SANE_STATUS_JAMMED;
    case 8:
      return SANE_STATUS_COVER_OPEN;
    default:
      return SANE_STATUS_IO_ERROR;
    }
}

/* ------------------------------------------------------------ usb plumbing */

static SANE_Status
usb_write (Pantum_Handle *h, const void *buf, size_t len)
{
  const uint8_t *p = buf;
  size_t done = 0;
  int tries;

  while (done < len)
    {
      int transferred = 0;
      int rc = LIBUSB_ERROR_OTHER;

      for (tries = 0; tries < 3; tries++)
        {
          /* libusb takes one buffer type for both directions and does not
           * write to it on an OUT transfer, hence the cast away from const. */
          rc = libusb_bulk_transfer (h->usb, EP_OUT, (unsigned char *)p + done,
                                     (int)(len - done), &transferred,
                                     TIMEOUT_WRITE);
          if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_TIMEOUT)
            break;
          DBG (1, "bulk out failed: %s, retrying\n", libusb_error_name (rc));
          libusb_clear_halt (h->usb, EP_OUT);
          sleep_ms (10);
        }
      if (rc != LIBUSB_SUCCESS || transferred <= 0)
        {
          DBG (1, "bulk out error: %s (%d/%zu bytes)\n", libusb_error_name (rc),
               transferred, len);
          return SANE_STATUS_IO_ERROR;
        }
      done += (size_t)transferred;
    }
  return SANE_STATUS_GOOD;
}

/* Bulk reads come back in transfer-sized chunks, never as whole payloads. */
static SANE_Status
usb_read (Pantum_Handle *h, void *buf, size_t len, unsigned timeout)
{
  uint8_t *p = buf;
  size_t done = 0;
  int tries;

  while (done < len)
    {
      int transferred = 0;
      int rc = LIBUSB_ERROR_OTHER;

      for (tries = 0; tries < 3; tries++)
        {
          rc = libusb_bulk_transfer (h->usb, EP_IN, p + done,
                                     (int)(len - done), &transferred,
                                     timeout);
          if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_TIMEOUT)
            break;
          DBG (1, "bulk in failed: %s, retrying\n", libusb_error_name (rc));
          libusb_clear_halt (h->usb, EP_IN);
          sleep_ms (10);
        }
      if (rc != LIBUSB_SUCCESS || transferred <= 0)
        {
          DBG (1, "bulk in error: %s (%zu/%zu bytes)\n", libusb_error_name (rc),
               done, len);
          return SANE_STATUS_IO_ERROR;
        }
      done += (size_t)transferred;
    }
  return SANE_STATUS_GOOD;
}

/* Throw away whatever the device still has queued.  A frontend that died
 * mid-job leaves the tail of a scan in the endpoint, and reading that as a
 * frame header would desynchronise the whole session. */
static void
flush_input (Pantum_Handle *h)
{
  uint8_t scratch[4096];
  int i;

  for (i = 0; i < 4096; i++)
    {
      int transferred = 0;
      int rc = libusb_bulk_transfer (h->usb, EP_IN, scratch, sizeof (scratch),
                                     &transferred, 200);
      if (rc != LIBUSB_SUCCESS || transferred <= 0)
        break;
      DBG (2, "flushed %d stale bytes\n", transferred);
    }
}

static SANE_Status
send_frame (Pantum_Handle *h, uint32_t cmd, const void *payload, size_t len)
{
  uint8_t buf[PANTUM_HEADER_LEN + PANTUM_SETTINGS_LEN];

  if (len > PANTUM_SETTINGS_LEN)
    return SANE_STATUS_INVAL;

  memset (buf, 0, sizeof (buf));
  put_be32 (buf + 0, PANTUM_MAGIC);
  put_be32 (buf + 4, cmd);
  put_be32 (buf + 20, (uint32_t)len);
  if (len)
    memcpy (buf + PANTUM_HEADER_LEN, payload, len);

  DBG (4, "-> cmd 0x%02x, %zu bytes payload\n", cmd, len);
  return usb_write (h, buf, PANTUM_HEADER_LEN + len);
}

/* Read one frame header.  With `probe` the first transfer only glances at the
 * endpoint and an empty one is reported through *idle instead of stalling the
 * caller; the rest of the header is then read normally, so the stream can
 * never end up desynchronised in the middle of a frame. */
static SANE_Status
recv_frame (Pantum_Handle *h, Pantum_Frame *f, unsigned timeout,
            SANE_Bool probe, SANE_Bool *idle)
{
  uint8_t buf[PANTUM_HEADER_LEN];
  size_t got = 0;
  SANE_Status st;

  if (idle)
    *idle = SANE_FALSE;

  if (probe)
    {
      int transferred = 0;
      int rc = libusb_bulk_transfer (h->usb, EP_IN, buf, sizeof (buf),
                                     &transferred, TIMEOUT_POLL);

      if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_TIMEOUT)
        {
          DBG (1, "bulk in poll failed: %s\n", libusb_error_name (rc));
          return SANE_STATUS_IO_ERROR;
        }
      if (transferred <= 0)
        {
          if (idle)
            *idle = SANE_TRUE;
          return SANE_STATUS_GOOD;
        }
      got = (size_t)transferred;
    }

  if (got < sizeof (buf))
    {
      st = usb_read (h, buf + got, sizeof (buf) - got, timeout);
      if (st != SANE_STATUS_GOOD)
        return st;
    }
  if (get_be32 (buf) != PANTUM_MAGIC)
    {
      DBG (1, "bad frame magic %08x\n", get_be32 (buf));
      return SANE_STATUS_IO_ERROR;
    }
  f->cmd = get_be32 (buf + 4);
  f->arg0 = get_be32 (buf + 8);
  f->status = get_be32 (buf + 16);
  f->plen = get_be32 (buf + 20);
  DBG (4, "<- cmd 0x%02x, arg0 %u, status %u, %u bytes payload\n", f->cmd,
       f->arg0, f->status, f->plen);
  /* Everything downstream sizes a buffer or a bulk transfer from plen, so a
   * nonsense length has to die here: past this point it would be a huge
   * allocation or a negative libusb transfer length.  A frame this large is a
   * lost frame boundary, and there is no way back from that except tearing
   * the session down, which the caller does on an error. */
  if (f->plen > PANTUM_MAX_PAYLOAD)
    {
      DBG (1, "frame 0x%02x announces %u bytes of payload, giving up\n",
           f->cmd, f->plen);
      return SANE_STATUS_IO_ERROR;
    }
  return SANE_STATUS_GOOD;
}

/* Discard a payload we do not care about. */
static SANE_Status
drain_payload (Pantum_Handle *h, uint32_t plen, unsigned timeout)
{
  uint8_t scratch[512];

  while (plen)
    {
      size_t chunk = plen > sizeof (scratch) ? sizeof (scratch) : plen;
      SANE_Status st = usb_read (h, scratch, chunk, timeout);
      if (st != SANE_STATUS_GOOD)
        return st;
      plen -= (uint32_t)chunk;
    }
  return SANE_STATUS_GOOD;
}

/* How many stale frames a command will step over before giving up. */
#define COMMAND_MAX_SKIP 4

/* Send a command and collect its acknowledgement.  reply/reply_len describe
 * an optional payload the device is expected to append.
 *
 * A session torn down while the device still owed a reply leaves that reply
 * sitting in the endpoint - tearing down a running job is exactly the case,
 * because the device can take longer to answer CMD_UNLOCK than the teardown
 * timeout allows, and the open-time flush is a race against it.  Every ack
 * names the command it answers, so a leftover is stepped over here rather
 * than left to shift every following reply by one frame, which is what used
 * to make the first scan after a cancelled one fail. */
static SANE_Status
command (Pantum_Handle *h, uint32_t cmd, const void *payload, size_t len,
         void *reply, size_t reply_len, Pantum_Frame *ack)
{
  Pantum_Frame f;
  unsigned timeout = TIMEOUT_READ;
  int skipped;
  SANE_Status st = send_frame (h, cmd, payload, len);

  if (st != SANE_STATUS_GOOD)
    return st;
  for (skipped = 0;; skipped++)
    {
      st = recv_frame (h, &f, timeout, SANE_FALSE, NULL);
      if (st != SANE_STATUS_GOOD)
        return st;
      if (f.cmd == cmd)
        break;
      DBG (1, "reply 0x%02x does not answer command 0x%02x, skipping\n",
           f.cmd, cmd);
      st = drain_payload (h, f.plen, TIMEOUT_TEARDOWN);
      if (st != SANE_STATUS_GOOD)
        return st;
      if (skipped + 1 >= COMMAND_MAX_SKIP)
        {
          DBG (1, "no answer to command 0x%02x in %d frames\n", cmd,
               COMMAND_MAX_SKIP);
          return SANE_STATUS_IO_ERROR;
        }
      /* Past the first frame the real ack is already on its way, so do not
       * sit on the endpoint for the full scan timeout waiting for it. */
      timeout = TIMEOUT_TEARDOWN;
    }
  if (f.plen)
    {
      if (reply && f.plen != reply_len)
        {
          DBG (1, "command 0x%02x returned %u bytes, expected %zu\n", cmd,
               f.plen, reply_len);
          drain_payload (h, f.plen, TIMEOUT_READ);
          return SANE_STATUS_IO_ERROR;
        }
      if (reply)
        st = usb_read (h, reply, reply_len, TIMEOUT_READ);
      else
        st = drain_payload (h, f.plen, TIMEOUT_READ);
      if (st != SANE_STATUS_GOOD)
        return st;
    }
  else if (reply)
    {
      DBG (1, "command 0x%02x returned no payload\n", cmd);
      return SANE_STATUS_IO_ERROR;
    }

  if (ack)
    *ack = f;
  return f.status ? map_status (f.status) : SANE_STATUS_GOOD;
}

/* Close the session.  The device shows "communication error, status 25" on its
 * panel when a host walks away from a locked scanner, so this runs on every
 * exit path - clean end, cancel, I/O error - and ignores its own errors. */
static void
unlock_device (Pantum_Handle *h)
{
  Pantum_Frame f;

  h->feeding = SANE_FALSE;
  if (!h->locked)
    return;
  h->locked = SANE_FALSE;

  if (send_frame (h, CMD_UNLOCK, NULL, 0) != SANE_STATUS_GOOD)
    return;
  /* Short timeout: by now the device may be wedged, and the frontend is
   * usually waiting to exit. */
  if (recv_frame (h, &f, TIMEOUT_TEARDOWN, SANE_FALSE, NULL)
      == SANE_STATUS_GOOD)
    drain_payload (h, f.plen, TIMEOUT_TEARDOWN);
}

/* ------------------------------------------------------- config and probe */

static void
free_ids (void)
{
  while (id_list)
    {
      Pantum_Id *next = id_list->next;
      free (id_list->model);
      free (id_list);
      id_list = next;
    }
}

static void
add_id (uint16_t vid, uint16_t pid, const char *model)
{
  Pantum_Id *id = calloc (1, sizeof (*id));

  if (!id)
    return;
  id->vid = vid;
  id->pid = pid;
  id->model = pantum_strdup (model && *model ? model : "M6500 series");
  id->next = id_list;
  id_list = id;
}

/* Accepts the usual sane config syntax: "usb 0x232b 0x0e20 M6500". */
static void
parse_config_line (char *line)
{
  char *p = line;
  char *model;
  /* %i is the only conversion that takes the 0x form the sane config files
   * are written in, and it wants a signed int; the range is checked below. */
  int vid, pid;

  while (*p && isspace ((unsigned char)*p))
    p++;
  if (*p == '#' || *p == '\0')
    return;
  if (strncmp (p, "usb", 3) != 0)
    return;
  p += 3;
  if (sscanf (p, " %i %i", &vid, &pid) != 2)
    return;
  if (vid < 0 || vid > 0xffff || pid < 0 || pid > 0xffff)
    {
      DBG (1, "config: usb id %d:%d out of range, ignored\n", vid, pid);
      return;
    }
  /* skip past both numbers to find an optional model name */
  while (*p && isspace ((unsigned char)*p))
    p++;
  while (*p && !isspace ((unsigned char)*p))
    p++;
  while (*p && isspace ((unsigned char)*p))
    p++;
  while (*p && !isspace ((unsigned char)*p))
    p++;
  while (*p && isspace ((unsigned char)*p))
    p++;
  model = p;
  p = model + strlen (model);
  while (p > model && isspace ((unsigned char)p[-1]))
    *--p = '\0';

  DBG (3, "config: usb %04x:%04x %s\n", (unsigned)vid, (unsigned)pid, model);
  add_id ((uint16_t)vid, (uint16_t)pid, model);
}

static SANE_Bool
read_config_file (const char *dir)
{
  char path[1024];
  char line[512];
  FILE *fp;

  if (snprintf (path, sizeof (path), "%s/%s", dir, PANTUM_CONFIG_FILE)
      >= (int)sizeof (path))
    return SANE_FALSE;
  fp = fopen (path, "r");
  if (!fp)
    return SANE_FALSE;
  DBG (2, "reading %s\n", path);
  while (fgets (line, sizeof (line), fp))
    parse_config_line (line);
  fclose (fp);
  return SANE_TRUE;
}

static void
read_config (void)
{
  const char *env = getenv ("SANE_CONFIG_DIR");
  SANE_Bool found = SANE_FALSE;

  if (env && *env)
    {
      char *copy = pantum_strdup (env);
      char *save = NULL;
      char *dir;

      if (copy)
        {
          for (dir = strtok_r (copy, ":", &save); dir;
               dir = strtok_r (NULL, ":", &save))
            if (read_config_file (dir))
              found = SANE_TRUE;
          free (copy);
        }
    }
  if (!found)
    found = read_config_file (PANTUM_CONFIG_DIR);
  if (!found)
    {
      DBG (2, "no %s found, using built-in device id\n", PANTUM_CONFIG_FILE);
      add_id (0x232b, 0x0e20, "M6500 series");
    }
}

static void
free_devices (void)
{
  while (device_list)
    {
      Pantum_Device *next = device_list->next;
      free (device_list->name);
      free (device_list->model);
      free (device_list);
      device_list = next;
    }
  free (device_ptrs);
  device_ptrs = NULL;
}

static SANE_Status
probe_devices (void)
{
  libusb_device **devs;
  ssize_t count, i;
  int n = 0;
  Pantum_Device *d;

  /* Open handles keep a pointer into this list, so never rebuild it while a
   * frontend still holds one. */
  if (handle_list && device_ptrs)
    return SANE_STATUS_GOOD;

  free_devices ();

  count = libusb_get_device_list (usb_ctx, &devs);
  if (count < 0)
    {
      DBG (1, "libusb_get_device_list: %s\n", libusb_error_name ((int)count));
      return SANE_STATUS_IO_ERROR;
    }

  for (i = 0; i < count; i++)
    {
      struct libusb_device_descriptor desc;
      Pantum_Id *id;
      char name[64];

      if (libusb_get_device_descriptor (devs[i], &desc) != LIBUSB_SUCCESS)
        continue;
      for (id = id_list; id; id = id->next)
        if (id->vid == desc.idVendor && id->pid == desc.idProduct)
          break;
      if (!id)
        continue;

      d = calloc (1, sizeof (*d));
      if (!d)
        break;
      d->bus = libusb_get_bus_number (devs[i]);
      d->addr = libusb_get_device_address (devs[i]);
      d->vid = desc.idVendor;
      d->pid = desc.idProduct;
      snprintf (name, sizeof (name), "usb:%03u:%03u", d->bus, d->addr);
      d->name = pantum_strdup (name);
      d->model = pantum_strdup (id->model);
      d->sane.name = d->name;
      d->sane.vendor = "Pantum";
      d->sane.model = d->model;
      d->sane.type = "multi-function peripheral";
      if (!d->name || !d->model)
        {
          free (d->name);
          free (d->model);
          free (d);
          break;
        }
      d->next = device_list;
      device_list = d;
      n++;
      DBG (2, "found %s (%04x:%04x %s)\n", d->name, d->vid, d->pid, d->model);
    }
  libusb_free_device_list (devs, 1);

  device_ptrs = calloc ((size_t)n + 1, sizeof (*device_ptrs));
  if (!device_ptrs)
    return SANE_STATUS_NO_MEM;
  n = 0;
  for (d = device_list; d; d = d->next)
    device_ptrs[n++] = &d->sane;
  return SANE_STATUS_GOOD;
}

/* ----------------------------------------------------------- option table */

static void
init_options (Pantum_Handle *h)
{
  SANE_Option_Descriptor *o;
  int i;

  memset (h->opt, 0, sizeof (h->opt));
  memset (h->val, 0, sizeof (h->val));

  for (i = 0; i < NUM_OPTIONS; i++)
    {
      h->opt[i].name = "";
      h->opt[i].title = "";
      h->opt[i].desc = "";
      h->opt[i].type = SANE_TYPE_INT;
      h->opt[i].size = sizeof (SANE_Word);
      h->opt[i].cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    }

  o = &h->opt[OPT_NUM_OPTS];
  o->name = SANE_NAME_NUM_OPTIONS;
  o->title = SANE_TITLE_NUM_OPTIONS;
  o->desc = SANE_DESC_NUM_OPTIONS;
  o->cap = SANE_CAP_SOFT_DETECT;
  h->val[OPT_NUM_OPTS].w = NUM_OPTIONS;

  o = &h->opt[OPT_MODE_GROUP];
  o->title = "Scan Mode";
  o->type = SANE_TYPE_GROUP;
  o->size = 0;
  o->cap = 0;

  o = &h->opt[OPT_MODE];
  o->name = SANE_NAME_SCAN_MODE;
  o->title = SANE_TITLE_SCAN_MODE;
  o->desc = SANE_DESC_SCAN_MODE;
  o->type = SANE_TYPE_STRING;
  o->size = 16;
  o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
  o->constraint.string_list = mode_list;
  h->val[OPT_MODE].s = pantum_strdup (SANE_VALUE_SCAN_MODE_GRAY);

  o = &h->opt[OPT_RESOLUTION];
  o->name = SANE_NAME_SCAN_RESOLUTION;
  o->title = SANE_TITLE_SCAN_RESOLUTION;
  o->desc = SANE_DESC_SCAN_RESOLUTION;
  o->type = SANE_TYPE_INT;
  o->unit = SANE_UNIT_DPI;
  o->constraint_type = SANE_CONSTRAINT_WORD_LIST;
  o->constraint.word_list = resolution_list;
  h->val[OPT_RESOLUTION].w = 300;

  o = &h->opt[OPT_SOURCE];
  o->name = SANE_NAME_SCAN_SOURCE;
  o->title = SANE_TITLE_SCAN_SOURCE;
  o->desc = SANE_DESC_SCAN_SOURCE;
  o->type = SANE_TYPE_STRING;
  o->size = 16;
  o->constraint_type = SANE_CONSTRAINT_STRING_LIST;
  o->constraint.string_list
      = h->has_adf ? source_list_adf : source_list_flatbed;
  h->val[OPT_SOURCE].s = pantum_strdup (SOURCE_NAME_FLATBED);

  o = &h->opt[OPT_PREVIEW];
  o->name = SANE_NAME_PREVIEW;
  o->title = SANE_TITLE_PREVIEW;
  o->desc = SANE_DESC_PREVIEW;
  o->type = SANE_TYPE_BOOL;
  h->val[OPT_PREVIEW].w = SANE_FALSE;

  o = &h->opt[OPT_THRESHOLD];
  o->name = SANE_NAME_THRESHOLD;
  o->title = SANE_TITLE_THRESHOLD;
  o->desc = SANE_DESC_THRESHOLD;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_PERCENT;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &threshold_range;
  o->cap |= SANE_CAP_INACTIVE; /* lineart only */
  h->val[OPT_THRESHOLD].w = SANE_FIX (50.0);

  o = &h->opt[OPT_GEOMETRY_GROUP];
  o->title = "Geometry";
  o->type = SANE_TYPE_GROUP;
  o->size = 0;
  o->cap = 0;

  o = &h->opt[OPT_TL_X];
  o->name = SANE_NAME_SCAN_TL_X;
  o->title = SANE_TITLE_SCAN_TL_X;
  o->desc = SANE_DESC_SCAN_TL_X;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &x_range;
  h->val[OPT_TL_X].w = SANE_FIX (0.0);

  o = &h->opt[OPT_TL_Y];
  o->name = SANE_NAME_SCAN_TL_Y;
  o->title = SANE_TITLE_SCAN_TL_Y;
  o->desc = SANE_DESC_SCAN_TL_Y;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &y_range;
  h->val[OPT_TL_Y].w = SANE_FIX (0.0);

  o = &h->opt[OPT_BR_X];
  o->name = SANE_NAME_SCAN_BR_X;
  o->title = SANE_TITLE_SCAN_BR_X;
  o->desc = SANE_DESC_SCAN_BR_X;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &x_range;
  h->val[OPT_BR_X].w = x_range.max;

  o = &h->opt[OPT_BR_Y];
  o->name = SANE_NAME_SCAN_BR_Y;
  o->title = SANE_TITLE_SCAN_BR_Y;
  o->desc = SANE_DESC_SCAN_BR_Y;
  o->type = SANE_TYPE_FIXED;
  o->unit = SANE_UNIT_MM;
  o->constraint_type = SANE_CONSTRAINT_RANGE;
  o->constraint.range = &y_range;
  h->val[OPT_BR_Y].w = y_range.max;
}

static SANE_Bool
mode_is (const Pantum_Handle *h, const char *name)
{
  return h->val[OPT_MODE].s && strcmp (h->val[OPT_MODE].s, name) == 0;
}

/* UNVERIFIED: every feeder path in this backend is written from the vendor
 * disassembly alone.  The test unit is an M6507 without an ADF, so the only
 * thing hardware has ever confirmed is the "no feeder" answer to
 * CMD_ADF_STATUS.  Do not treat ADF scanning as working code. */
static uint32_t
source_code (const Pantum_Handle *h)
{
  const char *s = h->val[OPT_SOURCE].s;

  if (s && strcmp (s, SOURCE_NAME_ADF) == 0)
    return SOURCE_ADF;
  if (s && strcmp (s, SOURCE_NAME_ADF_DUPLEX) == 0)
    return SOURCE_ADF_DUPLEX;
  return SOURCE_FLATBED;
}

static SANE_Bool
source_is_adf (const Pantum_Handle *h)
{
  return source_code (h) != SOURCE_FLATBED;
}

static SANE_Int
effective_resolution (const Pantum_Handle *h)
{
  SANE_Int dpi;

  /* A preview only needs enough detail to place the selection box. */
  if (h->val[OPT_PREVIEW].w)
    return 75;
  dpi = h->val[OPT_RESOLUTION].w;
  /* fix_window in the vendor backend folds 1200 dpi down for the feeder. */
  if (dpi > 600 && source_is_adf (h))
    dpi = 600;
  return dpi;
}

/* Window in hundredths of an inch, clamped the way the firmware expects. */
static void
compute_window (const Pantum_Handle *h, int *left, int *top, int *right,
                int *bottom)
{
  int max_y = source_is_adf (h) ? PANTUM_MAX_Y_ADF : PANTUM_MAX_Y;
  int l = mm_to_units (h->val[OPT_TL_X].w);
  int t = mm_to_units (h->val[OPT_TL_Y].w);
  int r = mm_to_units (h->val[OPT_BR_X].w);
  int b = mm_to_units (h->val[OPT_BR_Y].w);

  if (r < l)
    {
      int tmp = l;
      l = r;
      r = tmp;
    }
  if (b < t)
    {
      int tmp = t;
      t = b;
      b = tmp;
    }
  if (r > PANTUM_MAX_X)
    r = PANTUM_MAX_X;
  if (b > max_y)
    b = max_y;
  if (r - l < PANTUM_MIN_WINDOW)
    l = r - PANTUM_MIN_WINDOW;
  if (b - t < PANTUM_MIN_WINDOW)
    t = b - PANTUM_MIN_WINDOW;
  if (l < 0)
    l = 0;
  if (t < 0)
    t = 0;

  *left = l;
  *top = t;
  *right = r;
  *bottom = b;
}

/* Same arithmetic the vendor backend uses, so the frontend's estimate before
 * sane_start matches the data that actually arrives. */
static void
compute_parameters (Pantum_Handle *h, SANE_Parameters *p)
{
  int left, top, right, bottom;
  int dpi = effective_resolution (h);
  int pixels, lines;

  compute_window (h, &left, &top, &right, &bottom);
  pixels = (right - left) * dpi / 100;
  lines = (bottom - top) * dpi / 100;
  if (pixels < 1)
    pixels = 1;
  if (lines < 1)
    lines = 1;

  memset (p, 0, sizeof (*p));
  p->last_frame = SANE_TRUE;
  p->pixels_per_line = pixels;
  p->lines = lines;

  if (mode_is (h, SANE_VALUE_SCAN_MODE_COLOR))
    {
      p->format = SANE_FRAME_RGB;
      p->depth = 8;
      p->bytes_per_line = pixels * 3;
    }
  else if (mode_is (h, SANE_VALUE_SCAN_MODE_LINEART)
           || mode_is (h, SANE_VALUE_SCAN_MODE_HALFTONE))
    {
      /* Both bilevel modes carry the same frame: the device only ever sends
       * 8-bit grey and the packing happens here. */
      p->format = SANE_FRAME_GRAY;
      p->depth = 1;
      p->bytes_per_line = (pixels + 7) / 8;
    }
  else
    {
      p->format = SANE_FRAME_GRAY;
      p->depth = 8;
      p->bytes_per_line = pixels;
    }
}

/* -------------------------------------------------------------- image path */

static SANE_Status
out_reserve (Pantum_Handle *h, size_t extra)
{
  size_t need;

  if (h->out_pos && h->out_pos == h->out_len)
    h->out_pos = h->out_len = 0;

  need = h->out_len + extra;
  if (need <= h->out_cap)
    return SANE_STATUS_GOOD;

  /* Compact first: a half-drained buffer usually has room already. */
  if (h->out_pos)
    {
      memmove (h->out, h->out + h->out_pos, h->out_len - h->out_pos);
      h->out_len -= h->out_pos;
      h->out_pos = 0;
      need = h->out_len + extra;
      if (need <= h->out_cap)
        return SANE_STATUS_GOOD;
    }
  while (h->out_cap < need)
    h->out_cap = h->out_cap ? h->out_cap * 2 : 65536;
  {
    uint8_t *p = realloc (h->out, h->out_cap);
    if (!p)
      return SANE_STATUS_NO_MEM;
    h->out = p;
  }
  return SANE_STATUS_GOOD;
}

/* Convert one scanner row into the frame format the frontend expects.
 * src holds `avail` pixels; missing pixels are padded white.  `row` is the
 * absolute row index, needed for the white margin at the top of the page.
 *
 * The tone curve is applied here rather than to the whole block because the
 * padding the device appends to every row is white already and the curve maps
 * white to white, so the two orders agree byte for byte. */
static void
convert_row (Pantum_Handle *h, const uint8_t *src, int avail, int row,
             uint8_t *dst)
{
  const uint8_t *lut = h->gamma;
  int pixels = h->params.pixels_per_line;
  int lead = h->white_left < pixels ? h->white_left : pixels;
  int tail = pixels - (h->white_right < pixels ? h->white_right : pixels);
  int i;
  int n = avail < pixels ? avail : pixels;

  /* The vendor whitens whole rows at the edges of the page before it does
   * anything else with them; the sensor sees the lip of the glass there. */
  if (row < h->white_top
      || (h->white_bottom && row >= h->params.lines - h->white_bottom))
    {
      memset (dst, h->params.depth == 1 ? 0x00 : 0xff,
              (size_t)h->params.bytes_per_line);
      return;
    }

  if (h->params.format == SANE_FRAME_RGB)
    {
      /* Samples arrive interleaved per pixel, blue first: the vendor's
       * e_RGBPackedData path simply reverses each triplet. */
      for (i = 0; i < n; i++)
        {
          dst[3 * i + 0] = lut[src[3 * i + 2]];
          dst[3 * i + 1] = lut[src[3 * i + 1]];
          dst[3 * i + 2] = lut[src[3 * i + 0]];
        }
      memset (dst + 3 * n, 0xff, (size_t)(pixels - n) * 3);
      memset (dst, 0xff, (size_t)lead * 3);
      memset (dst + 3 * tail, 0xff, (size_t)(pixels - tail) * 3);
    }
  else if (h->params.depth == 1)
    {
      /* The scanner has no bilevel mode; pack on the host, after the tone
       * curve, exactly where the vendor does it.  A set bit is black, which
       * is also what PBM expects, so a white margin is a clear bit.
       *
       * Lineart compares against one threshold for the whole page; halftone
       * compares against the cell of the dither mask under the pixel, so the
       * tones between the two extremes survive as a density of dots.  The
       * mask is indexed by the absolute row and column, which keeps it
       * continuous across the block boundaries the device sends. */
      memset (dst, 0, (size_t)h->params.bytes_per_line);
      if (n > tail)
        n = tail;
      if (h->halftone)
        {
          const uint8_t *cell
              = dither_mask + (row & (DITHER_SIZE - 1)) * DITHER_SIZE;

          for (i = lead; i < n; i++)
            if (lut[src[i]] < cell[i & (DITHER_SIZE - 1)])
              dst[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
      else
        {
          for (i = lead; i < n; i++)
            if (lut[src[i]] < h->threshold)
              dst[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
    }
  else
    {
      for (i = 0; i < n; i++)
        dst[i] = lut[src[i]];
      memset (dst + n, 0xff, (size_t)(pixels - n));
      memset (dst, 0xff, (size_t)lead);
      memset (dst + tail, 0xff, (size_t)(pixels - tail));
    }
}

static SANE_Status
emit_white_rows (Pantum_Handle *h, int upto)
{
  size_t bpl = (size_t)h->params.bytes_per_line;

  while (h->next_row < upto && h->next_row < h->params.lines)
    {
      SANE_Status st = out_reserve (h, bpl);
      if (st != SANE_STATUS_GOOD)
        return st;
      /* White is 0xff in grey/RGB and a clear bit in lineart. */
      memset (h->out + h->out_len, h->params.depth == 1 ? 0x00 : 0xff, bpl);
      h->out_len += bpl;
      h->next_row++;
    }
  return SANE_STATUS_GOOD;
}

static SANE_Status
handle_data_block (Pantum_Handle *h, uint32_t plen)
{
  uint8_t head[PANTUM_DATA_HEADER_LEN];
  uint32_t type, first, rows, planes, pixels, avail, want_planes;
  size_t stride, body, bpl;
  SANE_Status st;
  uint32_t i;

  if (plen < PANTUM_DATA_HEADER_LEN)
    {
      drain_payload (h, plen, TIMEOUT_READ);
      return SANE_STATUS_IO_ERROR;
    }
  st = usb_read (h, head, sizeof (head), TIMEOUT_READ);
  if (st != SANE_STATUS_GOOD)
    return st;

  type = get_be32 (head + 0);
  first = get_be32 (head + 4);
  rows = get_be32 (head + 8);
  planes = get_be32 (head + 12);
  pixels = get_be32 (head + 16);
  body = plen - PANTUM_DATA_HEADER_LEN;

  DBG (5, "data block: type 0x%02x rows %u..%u, %u px, %u planes, %zu bytes\n",
       type, first, first + rows, pixels, planes, body);

  /* The vendor backend can also take a JPEG stream here, spooled to a file
   * and handed to libjpeg.  The M6507 has never selected it: see the note in
   * docs/TODO.md for the settings-block survey behind that statement. */
  if (type == DATA_JPEG)
    {
      DBG (1, "device sent a JPEG block, which this backend cannot decode\n");
      drain_payload (h, (uint32_t)body, TIMEOUT_READ);
      return SANE_STATUS_UNSUPPORTED;
    }

  if (!rows || !planes || body / rows < planes)
    {
      DBG (1, "malformed data block (rows %u, planes %u, body %zu)\n", rows,
           planes, body);
      drain_payload (h, (uint32_t)body, TIMEOUT_READ);
      return SANE_STATUS_IO_ERROR;
    }

  /* Rows are padded to a hardware-friendly length that the sub-header does
   * not state - it only gives the number of useful pixels - so the real
   * stride has to come from the payload size. */
  stride = body / rows;
  avail = (uint32_t)(stride / planes);
  if (pixels < avail)
    avail = pixels;
  /* Not just "3 or not 3": convert_row reads the row back at the stride this
   * plane count implies, so any other value would slice the payload wrong. */
  want_planes = h->params.format == SANE_FRAME_RGB ? 3u : 1u;
  if (planes != want_planes)
    {
      DBG (1, "unexpected plane count %u for the requested mode\n", planes);
      drain_payload (h, (uint32_t)body, TIMEOUT_READ);
      return SANE_STATUS_IO_ERROR;
    }

  if (h->raw_cap < body)
    {
      uint8_t *p = realloc (h->raw, body);
      if (!p)
        {
          drain_payload (h, (uint32_t)body, TIMEOUT_READ);
          return SANE_STATUS_NO_MEM;
        }
      h->raw = p;
      h->raw_cap = body;
    }
  st = usb_read (h, h->raw, body, TIMEOUT_READ);
  if (st != SANE_STATUS_GOOD)
    return st;

  /* Rows carry an absolute index, so a gap can be filled with white and a
   * repeat can be dropped without disturbing the byte count we promised.
   * The index is the device's number, though, so the arithmetic stays
   * unsigned and every row is checked against the page before it is used. */
  st = emit_white_rows (h, first < (uint32_t)h->params.lines
                               ? (int)first
                               : h->params.lines);
  if (st != SANE_STATUS_GOOD)
    return st;

  bpl = (size_t)h->params.bytes_per_line;
  for (i = 0; i < rows; i++)
    {
      uint32_t row = first + i;

      if (row < first /* the device's first row index wrapped */
          || row < (uint32_t)h->next_row
          || row >= (uint32_t)h->params.lines)
        continue;
      st = out_reserve (h, bpl);
      if (st != SANE_STATUS_GOOD)
        return st;
      convert_row (h, h->raw + (size_t)i * stride, (int)avail, (int)row,
                   h->out + h->out_len);
      h->out_len += bpl;
      h->next_row = (int)row + 1;
    }
  return SANE_STATUS_GOOD;
}

/* The device repeats the settings block once the job is running.  Nothing in
 * it has to be acted on, but it is the only place that states which pixel
 * layout the firmware picked, so it is worth a debug line. */
static SANE_Status
log_settings_echo (Pantum_Handle *h, uint32_t plen)
{
  uint8_t settings[PANTUM_SETTINGS_LEN];

  if (plen != PANTUM_SETTINGS_LEN || dbg_level < 2)
    return drain_payload (h, plen, TIMEOUT_READ);

  if (usb_read (h, settings, sizeof (settings), TIMEOUT_READ)
      != SANE_STATUS_GOOD)
    return SANE_STATUS_IO_ERROR;
  DBG (2, "job settings echo: %u dpi, source 0x%x, data type 0x%x, colour %u\n",
       get_be32 (settings + 4 * SET_RESOLUTION),
       get_be32 (settings + 4 * SET_SOURCE),
       get_be32 (settings + 4 * SET_DATA_TYPE),
       get_be32 (settings + 4 * SET_COLOUR));
  return SANE_STATUS_GOOD;
}

/* Read and process exactly one frame from the device.  With `probe` set the
 * call returns immediately, and *idle tells whether anything was there. */
static SANE_Status
pump_frame (Pantum_Handle *h, SANE_Bool probe, SANE_Bool *idle,
            SANE_Bool *aborted, uint32_t *abort_status)
{
  Pantum_Frame f;
  SANE_Bool nothing = SANE_FALSE;
  SANE_Status st = recv_frame (h, &f, TIMEOUT_READ, probe, &nothing);

  if (st != SANE_STATUS_GOOD)
    return st;
  if (nothing)
    {
      if (idle)
        *idle = SANE_TRUE;
      return SANE_STATUS_GOOD;
    }

  switch (f.cmd)
    {
    case EVT_DATA:
      return handle_data_block (h, f.plen);

    case EVT_ABORTED:
      drain_payload (h, f.plen, TIMEOUT_READ);
      DBG (2, "job aborted by device, status %u\n", f.status);
      h->scanning = SANE_FALSE;
      unlock_device (h);
      if (aborted)
        {
          *aborted = SANE_TRUE;
          if (abort_status)
            *abort_status = f.status;
          return SANE_STATUS_GOOD;
        }
      return f.status ? map_status (f.status) : SANE_STATUS_IO_ERROR;

    case EVT_JOB_END:
      drain_payload (h, f.plen, TIMEOUT_READ);
      DBG (3, "job finished after %d rows\n", h->next_row);
      st = emit_white_rows (h, h->params.lines);
      h->eof = SANE_TRUE;
      h->scanning = SANE_FALSE;
      unlock_device (h);
      return st;

    case EVT_PAGE_END:
      drain_payload (h, f.plen, TIMEOUT_READ);
      /* UNVERIFIED: only the feeder produces more than one page per job, so
       * the flatbed path is deliberately left as it was - it ends on
       * EVT_JOB_END and has been that way through every hardware test. */
      if (!source_is_adf (h))
        return SANE_STATUS_GOOD;
      DBG (3, "page finished after %d rows\n", h->next_row);
      st = emit_white_rows (h, h->params.lines);
      h->eof = SANE_TRUE;
      h->feeding = SANE_TRUE;
      return st;

    case EVT_SETTINGS:
      return log_settings_echo (h, f.plen);

    default:
      /* Page and scan begin frames: nothing to do beyond consuming whatever
       * payload they carry. */
      return drain_payload (h, f.plen, TIMEOUT_READ);
    }
}

/* ------------------------------------------------------------- scan setup */

static SANE_Status
lock_device (Pantum_Handle *h)
{
  int tries;

  for (tries = 0; tries < 5; tries++)
    {
      /* command() only fills the frame once one has been read, so the
       * "device busy" test below must not see stack rubbish. */
      Pantum_Frame ack = { 0, 0, 0, 0 };
      SANE_Status st;

      /* The unlock is owed from the moment the request leaves the host, not
       * from the moment its reply arrives: if the reply is lost or garbled
       * the device may well have taken the lock, and a scanner left locked
       * puts "communication error, status 25" on the panel. */
      h->locked = SANE_TRUE;
      st = command (h, CMD_LOCK, NULL, 0, NULL, 0, &ack);
      if (st == SANE_STATUS_GOOD)
        return SANE_STATUS_GOOD;
      if (ack.status != 2)
        {
          unlock_device (h);
          return st;
        }
      DBG (2, "device busy, retrying lock\n");
      sleep_ms (2000);
    }
  unlock_device (h);
  return SANE_STATUS_DEVICE_BUSY;
}

/* Ask the device whether it has a document feeder and, if so, whether there
 * is paper in it.  The vendor backend runs this inside its scan thread right
 * after taking the lock, and reads the answer as: status 0 means a feeder
 * answered, and then arg0 == 1 means it is loaded.
 *
 * The M6507 on the bench replies with a non-zero status, so only the "no
 * feeder" branch has ever been exercised on hardware. */
static SANE_Status
check_adf (Pantum_Handle *h, SANE_Bool *present, SANE_Bool *loaded)
{
  Pantum_Frame f;
  SANE_Status st = send_frame (h, CMD_ADF_STATUS, NULL, 0);

  *present = SANE_FALSE;
  *loaded = SANE_FALSE;
  if (st != SANE_STATUS_GOOD)
    return st;
  st = recv_frame (h, &f, TIMEOUT_READ, SANE_FALSE, NULL);
  if (st != SANE_STATUS_GOOD)
    return st;
  if (f.plen)
    drain_payload (h, f.plen, TIMEOUT_READ);
  if (f.cmd != CMD_ADF_STATUS)
    {
      DBG (1, "reply 0x%02x does not match the ADF status request\n", f.cmd);
      return SANE_STATUS_IO_ERROR;
    }
  *present = f.status == 0;
  *loaded = *present && f.arg0 == 1;
  DBG (2, "ADF status: %s, %s (status %u, arg0 %u)\n",
       *present ? "feeder present" : "no feeder",
       *loaded ? "loaded" : "empty", f.status, f.arg0);
  return SANE_STATUS_GOOD;
}

/* One lock/query/unlock round trip, so that the option list can tell the
 * truth about which sources this unit really has. */
static void
probe_adf (Pantum_Handle *h)
{
  SANE_Bool present = SANE_FALSE, loaded = SANE_FALSE;

  h->has_adf = SANE_FALSE;
  /* Owe the unlock from the request, as lock_device does; a single try is
   * enough here because a busy device simply means no feeder is advertised. */
  h->locked = SANE_TRUE;
  if (command (h, CMD_LOCK, NULL, 0, NULL, 0, NULL) != SANE_STATUS_GOOD)
    {
      DBG (2, "cannot lock the device to look for a feeder\n");
      unlock_device (h);
      return;
    }
  if (check_adf (h, &present, &loaded) == SANE_STATUS_GOOD)
    h->has_adf = present;
  unlock_device (h);
}

static SANE_Status
setup_job (Pantum_Handle *h)
{
  uint8_t settings[PANTUM_SETTINGS_LEN];
  uint32_t words[PANTUM_SETTINGS_WORDS];
  int left, top, right, bottom, i;
  SANE_Status st;

  st = command (h, CMD_SET_DEFAULTS, NULL, 0, NULL, 0, NULL);
  if (st != SANE_STATUS_GOOD)
    return st;
  st = command (h, CMD_GET_SETTINGS, NULL, 0, settings, sizeof (settings),
                NULL);
  if (st != SANE_STATUS_GOOD)
    return st;

  for (i = 0; i < PANTUM_SETTINGS_WORDS; i++)
    words[i] = get_be32 (settings + 4 * i);

  compute_window (h, &left, &top, &right, &bottom);

  /* Read-modify-write: every field the firmware did not document stays as the
   * device returned it. */
  words[SET_RESOLUTION] = (uint32_t)effective_resolution (h);
  words[SET_ZERO] = 0;
  words[SET_SOURCE] = source_code (h);
  words[SET_TOP] = (uint32_t)top;
  words[SET_LEFT] = (uint32_t)left;
  words[SET_BOTTOM] = (uint32_t)bottom;
  words[SET_RIGHT] = (uint32_t)right;
  words[SET_COLOUR] = mode_is (h, SANE_VALUE_SCAN_MODE_COLOR) ? 1 : 0;

  DBG (2, "window %d,%d..%d,%d (1/100 in) at %u dpi, source 0x%x, %s\n", left,
       top, right, bottom, words[SET_RESOLUTION], words[SET_SOURCE],
       words[SET_COLOUR] ? "colour" : "grey");
  DBG (2, "device offers data type 0x%x for this job\n", words[SET_DATA_TYPE]);

  for (i = 0; i < PANTUM_SETTINGS_WORDS; i++)
    put_be32 (settings + 4 * i, words[i]);

  st = command (h, CMD_SET_SETTINGS, settings, sizeof (settings), NULL, 0,
                NULL);
  if (st != SANE_STATUS_GOOD)
    return st;

  return command (h, CMD_START, NULL, 0, NULL, 0, NULL);
}

/* Wait until the device has produced pixels or given up, so that paper and
 * cover errors surface in sane_start rather than in the middle of a read. */
static SANE_Status
await_first_rows (Pantum_Handle *h, SANE_Bool *aborted, uint32_t *abort_status)
{
  while (h->scanning && !h->out_len && !*aborted && !h->eof)
    {
      SANE_Status st
          = pump_frame (h, SANE_FALSE, NULL, aborted, abort_status);
      if (st != SANE_STATUS_GOOD)
        {
          h->scanning = SANE_FALSE;
          unlock_device (h);
          return st;
        }
    }
  return SANE_STATUS_GOOD;
}

/* Run one job attempt: lock, configure, start, and wait for the first rows. */
static SANE_Status
run_job (Pantum_Handle *h, SANE_Bool *aborted, uint32_t *abort_status)
{
  SANE_Status st;

  *aborted = SANE_FALSE;
  h->next_row = 0;
  h->out_len = h->out_pos = 0;
  h->eof = SANE_FALSE;

  st = lock_device (h);
  if (st != SANE_STATUS_GOOD)
    return st;

  if (source_is_adf (h))
    {
      /* UNVERIFIED on hardware: the bench unit has no feeder. */
      SANE_Bool present = SANE_FALSE, loaded = SANE_FALSE;

      st = check_adf (h, &present, &loaded);
      if (st != SANE_STATUS_GOOD)
        {
          unlock_device (h);
          return st;
        }
      if (!present || !loaded)
        {
          unlock_device (h);
          return present ? SANE_STATUS_NO_DOCS : SANE_STATUS_UNSUPPORTED;
        }
    }

  st = setup_job (h);
  if (st != SANE_STATUS_GOOD)
    {
      unlock_device (h);
      return st;
    }

  h->scanning = SANE_TRUE;
  return await_first_rows (h, aborted, abort_status);
}

/* Pick up the next page of a feeder job that is already running.  The device
 * keeps the lock and simply starts another page, so nothing is re-sent.
 * UNVERIFIED: written from the vendor's reader loop, never run on hardware. */
static SANE_Status
resume_job (Pantum_Handle *h)
{
  SANE_Bool aborted = SANE_FALSE;
  uint32_t abort_status = 0;
  SANE_Status st;

  h->next_row = 0;
  h->out_len = h->out_pos = 0;
  h->eof = SANE_FALSE;
  h->feeding = SANE_FALSE;

  st = await_first_rows (h, &aborted, &abort_status);
  if (st != SANE_STATUS_GOOD)
    return st;
  if (aborted)
    return abort_status ? map_status (abort_status) : SANE_STATUS_NO_DOCS;
  /* The job ended instead of starting another page: the feeder is empty. */
  if (!h->out_len)
    return SANE_STATUS_NO_DOCS;
  return SANE_STATUS_GOOD;
}

/* ------------------------------------------------------------ handle admin */

static void
end_scan (Pantum_Handle *h)
{
  if (h->scanning)
    {
      Pantum_Frame f;
      int frames;

      DBG (2, "aborting running job\n");
      send_frame (h, CMD_ABORT, NULL, 0);
      /* Swallow whatever is still in flight so the next session starts on a
       * frame boundary, but never wait forever: the unlock below matters more
       * than a tidy drain. */
      for (frames = 0; frames < 4096; frames++)
        {
          if (recv_frame (h, &f, TIMEOUT_TEARDOWN, SANE_FALSE, NULL)
              != SANE_STATUS_GOOD)
            break;
          if (drain_payload (h, f.plen, TIMEOUT_TEARDOWN) != SANE_STATUS_GOOD)
            break;
          if (f.cmd == EVT_JOB_END || f.cmd == EVT_ABORTED)
            break;
        }
      h->scanning = SANE_FALSE;
    }
  unlock_device (h);
}

static void
free_handle (Pantum_Handle *h)
{
  int i;

  for (i = 0; i < NUM_OPTIONS; i++)
    if (h->opt[i].type == SANE_TYPE_STRING)
      free (h->val[i].s);
  free (h->out);
  free (h->raw);
  if (h->usb)
    {
      libusb_release_interface (h->usb, PANTUM_INTERFACE);
      libusb_close (h->usb);
    }
  if (h->dev)
    h->dev->busy = SANE_FALSE;
  free (h);
}

static SANE_Bool
handle_is_valid (const Pantum_Handle *h)
{
  const Pantum_Handle *p;

  for (p = handle_list; p; p = p->next)
    if (p == h)
      return SANE_TRUE;
  return SANE_FALSE;
}

/* ------------------------------------------------------------- SANE API */

SANE_Status
sane_pantum_init (SANE_Int *version_code, SANE_Auth_Callback authorize)
{
  const char *env = getenv ("SANE_DEBUG_PANTUM");

  (void)authorize;

  dbg_level = env ? atoi (env) : 0;
  DBG (1, "sane_pantum_init: version %d.%d.%d\n", SANE_CURRENT_MAJOR,
       SANE_CURRENT_MINOR, PANTUM_VERSION_BUILD);

  if (version_code)
    *version_code = SANE_VERSION_CODE (SANE_CURRENT_MAJOR, SANE_CURRENT_MINOR,
                                       PANTUM_VERSION_BUILD);

  if (initialised)
    return SANE_STATUS_GOOD;

  /* Explicit reset: sane_init may run again after sane_exit. */
  id_list = NULL;
  device_list = NULL;
  device_ptrs = NULL;
  handle_list = NULL;
  usb_ctx = NULL;

  if (libusb_init (&usb_ctx) != LIBUSB_SUCCESS)
    {
      DBG (1, "libusb_init failed\n");
      return SANE_STATUS_IO_ERROR;
    }

  read_config ();
  initialised = SANE_TRUE;
  return SANE_STATUS_GOOD;
}

void
sane_pantum_exit (void)
{
  DBG (1, "sane_pantum_exit\n");
  if (!initialised)
    return;

  while (handle_list)
    {
      Pantum_Handle *next = handle_list->next;
      end_scan (handle_list);
      free_handle (handle_list);
      handle_list = next;
    }
  free_devices ();
  free_ids ();
  if (usb_ctx)
    {
      libusb_exit (usb_ctx);
      usb_ctx = NULL;
    }
  initialised = SANE_FALSE;
}

SANE_Status
sane_pantum_get_devices (const SANE_Device ***device_list_out,
                         SANE_Bool local_only)
{
  SANE_Status st;

  (void)local_only; /* every device we can see is local */

  if (!initialised)
    return SANE_STATUS_INVAL;
  if (!device_list_out)
    return SANE_STATUS_INVAL;

  st = probe_devices ();
  if (st != SANE_STATUS_GOOD)
    return st;
  *device_list_out = device_ptrs;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_pantum_open (SANE_String_Const devicename, SANE_Handle *handle)
{
  Pantum_Device *d;
  Pantum_Handle *h;
  libusb_device **devs;
  libusb_device_handle *usb = NULL;
  ssize_t count, i;
  int rc;

  if (!initialised || !handle)
    return SANE_STATUS_INVAL;
  *handle = NULL;

  if (!device_list)
    {
      SANE_Status st = probe_devices ();
      if (st != SANE_STATUS_GOOD)
        return st;
    }

  if (!devicename || !*devicename)
    d = device_list; /* frontends without -d expect the first device */
  else
    {
      for (d = device_list; d; d = d->next)
        if (strcmp (d->name, devicename) == 0)
          break;
    }
  if (!d)
    {
      DBG (1, "no such device: %s\n", devicename ? devicename : "(default)");
      return SANE_STATUS_INVAL;
    }
  if (d->busy)
    return SANE_STATUS_DEVICE_BUSY;

  count = libusb_get_device_list (usb_ctx, &devs);
  if (count < 0)
    return SANE_STATUS_IO_ERROR;
  for (i = 0; i < count; i++)
    {
      if (libusb_get_bus_number (devs[i]) != d->bus
          || libusb_get_device_address (devs[i]) != d->addr)
        continue;
      rc = libusb_open (devs[i], &usb);
      if (rc != LIBUSB_SUCCESS)
        {
          DBG (1, "libusb_open: %s\n", libusb_error_name (rc));
          libusb_free_device_list (devs, 1);
          return rc == LIBUSB_ERROR_ACCESS ? SANE_STATUS_ACCESS_DENIED
                                           : SANE_STATUS_IO_ERROR;
        }
      break;
    }
  libusb_free_device_list (devs, 1);
  if (!usb)
    return SANE_STATUS_IO_ERROR;

  /* Claim only the scanner interface: interface 0 is the printer and is in
   * use by usblp/CUPS.  The configuration is left alone for the same reason. */
  rc = libusb_claim_interface (usb, PANTUM_INTERFACE);
  if (rc != LIBUSB_SUCCESS)
    {
      DBG (1, "claim interface %d: %s\n", PANTUM_INTERFACE,
           libusb_error_name (rc));
      libusb_close (usb);
      return rc == LIBUSB_ERROR_BUSY ? SANE_STATUS_DEVICE_BUSY
                                     : SANE_STATUS_IO_ERROR;
    }

  h = calloc (1, sizeof (*h));
  if (!h)
    {
      libusb_release_interface (usb, PANTUM_INTERFACE);
      libusb_close (usb);
      return SANE_STATUS_NO_MEM;
    }
  h->dev = d;
  h->usb = usb;
  flush_input (h);
  /* Ask before building the option list: a unit without a feeder must not
   * advertise one. */
  probe_adf (h);
  init_options (h);
  compute_parameters (h, &h->params);
  d->busy = SANE_TRUE;

  h->next = handle_list;
  handle_list = h;
  *handle = h;
  DBG (1, "opened %s\n", d->name);
  return SANE_STATUS_GOOD;
}

void
sane_pantum_close (SANE_Handle handle)
{
  Pantum_Handle *h = handle;
  Pantum_Handle **pp;

  if (!h || !handle_is_valid (h))
    return;

  DBG (1, "sane_pantum_close\n");
  end_scan (h); /* frontends do not always cancel before closing */

  for (pp = &handle_list; *pp; pp = &(*pp)->next)
    if (*pp == h)
      {
        *pp = h->next;
        break;
      }
  free_handle (h);
}

const SANE_Option_Descriptor *
sane_pantum_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
  Pantum_Handle *h = handle;

  if (!h || !handle_is_valid (h) || option < 0 || option >= NUM_OPTIONS)
    return NULL;
  return &h->opt[option];
}

static void
update_option_state (Pantum_Handle *h)
{
  SANE_Bool adf = source_is_adf (h);
  const SANE_Range *yr = adf ? &y_range_adf : &y_range;

  if (mode_is (h, SANE_VALUE_SCAN_MODE_LINEART))
    h->opt[OPT_THRESHOLD].cap &= ~SANE_CAP_INACTIVE;
  else
    h->opt[OPT_THRESHOLD].cap |= SANE_CAP_INACTIVE;

  /* The feeder takes a longer sheet but stops at 600 dpi.  Swapping the
   * constraint is not enough: a value set under the old one has to be pulled
   * back inside the new one, or the option would report a value its own
   * descriptor forbids. */
  h->opt[OPT_TL_Y].constraint.range = yr;
  h->opt[OPT_BR_Y].constraint.range = yr;
  if (h->val[OPT_TL_Y].w > yr->max)
    h->val[OPT_TL_Y].w = yr->max;
  if (h->val[OPT_BR_Y].w > yr->max)
    h->val[OPT_BR_Y].w = yr->max;

  h->opt[OPT_RESOLUTION].constraint.word_list
      = adf ? resolution_list_adf : resolution_list;
  if (adf && h->val[OPT_RESOLUTION].w > 600)
    h->val[OPT_RESOLUTION].w = 600;
}

/* Snap a word to the nearest allowed value of its constraint. */
static SANE_Word
constrain_word (const SANE_Option_Descriptor *o, SANE_Word v, SANE_Bool *inexact)
{
  SANE_Word out = v;

  if (o->constraint_type == SANE_CONSTRAINT_RANGE)
    {
      const SANE_Range *r = o->constraint.range;
      if (out < r->min)
        out = r->min;
      if (out > r->max)
        out = r->max;
      if (r->quant)
        {
          SANE_Word steps = (out - r->min + r->quant / 2) / r->quant;
          out = r->min + steps * r->quant;
          if (out > r->max)
            out = r->max;
        }
    }
  else if (o->constraint_type == SANE_CONSTRAINT_WORD_LIST)
    {
      const SANE_Word *list = o->constraint.word_list;
      SANE_Word best = list[1];
      SANE_Word i;

      for (i = 1; i <= list[0]; i++)
        {
          SANE_Word d1 = list[i] > v ? list[i] - v : v - list[i];
          SANE_Word d2 = best > v ? best - v : v - best;
          if (d1 < d2)
            best = list[i];
        }
      out = best;
    }
  if (inexact && out != v)
    *inexact = SANE_TRUE;
  return out;
}

SANE_Status
sane_pantum_control_option (SANE_Handle handle, SANE_Int option,
                            SANE_Action action, void *value, SANE_Int *info)
{
  Pantum_Handle *h = handle;
  const SANE_Option_Descriptor *o;

  if (!h || !handle_is_valid (h) || option < 0 || option >= NUM_OPTIONS)
    return SANE_STATUS_INVAL;
  if (h->scanning)
    return SANE_STATUS_DEVICE_BUSY;

  o = &h->opt[option];
  if (info)
    *info = 0;
  if (!SANE_OPTION_IS_ACTIVE (o->cap))
    return SANE_STATUS_INVAL;

  switch (action)
    {
    case SANE_ACTION_GET_VALUE:
      if (!value)
        return SANE_STATUS_INVAL;
      /* A group carries no value and is declared with size 0, so writing a
       * word through the frontend's buffer would overrun it. */
      if (o->type == SANE_TYPE_GROUP)
        return SANE_STATUS_INVAL;
      if (o->type == SANE_TYPE_STRING)
        strcpy (value, h->val[option].s ? h->val[option].s : "");
      else
        *(SANE_Word *)value = h->val[option].w;
      return SANE_STATUS_GOOD;

    case SANE_ACTION_SET_VALUE:
      if (!value || !SANE_OPTION_IS_SETTABLE (o->cap))
        return SANE_STATUS_INVAL;
      if (o->type == SANE_TYPE_STRING)
        {
          const SANE_String_Const *list = o->constraint.string_list;
          const char *want = value;
          int i;

          for (i = 0; list && list[i]; i++)
            if (strcasecmp (list[i], want) == 0)
              break;
          if (!list || !list[i])
            return SANE_STATUS_INVAL;
          free (h->val[option].s);
          h->val[option].s = pantum_strdup (list[i]);
          if (!h->val[option].s)
            return SANE_STATUS_NO_MEM;
          if (option == OPT_MODE || option == OPT_SOURCE)
            {
              update_option_state (h);
              if (info)
                *info |= SANE_INFO_RELOAD_OPTIONS | SANE_INFO_RELOAD_PARAMS;
            }
          return SANE_STATUS_GOOD;
        }
      else
        {
          SANE_Bool inexact = SANE_FALSE;
          SANE_Word v = *(SANE_Word *)value;

          if (o->type == SANE_TYPE_BOOL)
            v = v ? SANE_TRUE : SANE_FALSE;
          else
            v = constrain_word (o, v, &inexact);

          h->val[option].w = v;
          if (inexact)
            {
              *(SANE_Word *)value = v;
              if (info)
                *info |= SANE_INFO_INEXACT;
            }
          /* Deliberately no tl < br check here: frontends set the corners one
           * at a time and would trip over a temporarily inverted window. */
          if (info
              && (option == OPT_RESOLUTION || option == OPT_PREVIEW
                  || (option >= OPT_TL_X && option <= OPT_BR_Y)))
            *info |= SANE_INFO_RELOAD_PARAMS;
          return SANE_STATUS_GOOD;
        }

    case SANE_ACTION_SET_AUTO:
      return SANE_STATUS_UNSUPPORTED;
    }
  return SANE_STATUS_INVAL;
}

SANE_Status
sane_pantum_get_parameters (SANE_Handle handle, SANE_Parameters *params)
{
  Pantum_Handle *h = handle;

  if (!h || !handle_is_valid (h) || !params)
    return SANE_STATUS_INVAL;

  /* While a job runs or its data is still being drained the parameters stay
   * frozen; otherwise they are an estimate computed from the current options
   * with the very same arithmetic, so the two always agree. */
  if (!h->scanning && h->out_pos == h->out_len)
    compute_parameters (h, &h->params);
  *params = h->params;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_pantum_start (SANE_Handle handle)
{
  Pantum_Handle *h = handle;
  SANE_Status st;
  int attempt;
  int left, top, right, bottom;
  int dpi;

  if (!h || !handle_is_valid (h))
    return SANE_STATUS_INVAL;
  /* A frontend may cancel and immediately start again without ever calling
   * sane_read, so finish the abandoned job here rather than reporting busy. */
  if (h->cancelled)
    end_scan (h);
  /* A feeder job stays open between pages: the next start just collects the
   * next page instead of setting up a second job.  UNVERIFIED on hardware. */
  if (h->feeding)
    return resume_job (h);
  if (h->scanning)
    return SANE_STATUS_DEVICE_BUSY;

  h->cancelled = SANE_FALSE;
  h->eof = SANE_FALSE;
  compute_parameters (h, &h->params);
  h->threshold = (int)(SANE_UNFIX (h->val[OPT_THRESHOLD].w) * 255.0 / 100.0);
  if (h->threshold < 0)
    h->threshold = 0;
  if (h->threshold > 255)
    h->threshold = 255;

  h->halftone = mode_is (h, SANE_VALUE_SCAN_MODE_HALFTONE);
  if (h->halftone)
    {
      st = build_dither_mask ();
      if (st != SANE_STATUS_GOOD)
        return st;
    }

  build_gamma (h->gamma, h->params.format == SANE_FRAME_RGB);

  /* The vendor paints out the strip the carriage sees before the paper
   * starts.  On the flatbed that is 2 mm down the page and 1.5 mm in from the
   * left edge, and only when the frontend did not ask for an offset of its
   * own; the feeder gets 2.5 mm at the top and 1.5 mm on the three other
   * sides regardless of the window.  UNVERIFIED for the feeder. */
  dpi = effective_resolution (h);
  compute_window (h, &left, &top, &right, &bottom);
  h->white_left = h->white_right = h->white_top = h->white_bottom = 0;
  if (source_is_adf (h))
    {
      int edge = (int)((double)dpi * 1.5 / MM_PER_INCH);

      h->white_top = (int)((double)dpi * 2.5 / MM_PER_INCH);
      h->white_bottom = edge;
      h->white_left = h->white_right = edge;
    }
  else
    {
      if (top == 0)
        h->white_top = (int)((double)(dpi * 2) / MM_PER_INCH);
      if (left == 0)
        h->white_left = (int)((double)dpi * 1.5 / MM_PER_INCH);
    }

  DBG (1, "sane_pantum_start: %d x %d, %d bpp, %d bytes/line\n",
       h->params.pixels_per_line, h->params.lines, h->params.depth,
       h->params.bytes_per_line);
  DBG (2, "white margin: %d rows at the top, %d pixels on the left\n",
       h->white_top, h->white_left);

  /* A 0x04 reply means the device refused the job, usually because the
   * previous one is still winding down; that is worth retrying. */
  for (attempt = 0; attempt < 4; attempt++)
    {
      SANE_Bool aborted = SANE_FALSE;
      uint32_t abort_status = 0;

      st = run_job (h, &aborted, &abort_status);
      if (st != SANE_STATUS_GOOD)
        return st;
      if (!aborted)
        return SANE_STATUS_GOOD;
      if (abort_status != 0 && abort_status != 2)
        return map_status (abort_status);
      DBG (2, "job rejected (status %u), retrying\n", abort_status);
      sleep_ms (2000);
    }
  return SANE_STATUS_DEVICE_BUSY;
}

SANE_Status
sane_pantum_read (SANE_Handle handle, SANE_Byte *data, SANE_Int max_length,
                  SANE_Int *length)
{
  Pantum_Handle *h = handle;
  size_t avail;

  if (!h || !handle_is_valid (h) || !data || !length || max_length <= 0)
    return SANE_STATUS_INVAL;
  *length = 0;

  if (h->cancelled)
    {
      /* sane_cancel only sets the flag, so the job is torn down here, back in
       * ordinary context where talking to the device is safe again. */
      end_scan (h);
      h->out_len = h->out_pos = 0;
      h->eof = SANE_TRUE;
      return SANE_STATUS_CANCELLED;
    }

  while (h->out_pos == h->out_len && !h->eof)
    {
      SANE_Bool idle = SANE_FALSE;
      SANE_Status st;

      if (!h->scanning)
        {
          /* The device stopped talking without ending the job cleanly. */
          h->eof = SANE_TRUE;
          break;
        }
      st = pump_frame (h, h->non_blocking, &idle, NULL, NULL);
      if (st != SANE_STATUS_GOOD)
        {
          h->scanning = SANE_FALSE;
          unlock_device (h);
          return st;
        }
      /* Nothing on the wire yet.  The standard asks for a zero-length read
       * rather than a wait, and *length is already 0. */
      if (idle)
        return SANE_STATUS_GOOD;
    }

  avail = h->out_len - h->out_pos;
  if (!avail)
    return SANE_STATUS_EOF;

  if (avail > (size_t)max_length)
    avail = (size_t)max_length;
  memcpy (data, h->out + h->out_pos, avail);
  h->out_pos += avail;
  *length = (SANE_Int)avail;
  return SANE_STATUS_GOOD;
}

void
sane_pantum_cancel (SANE_Handle handle)
{
  Pantum_Handle *h = handle;

  if (!h || !handle_is_valid (h))
    return;

  /* Frontends call this from a signal handler (scanimage does it on SIGINT),
   * possibly while another thread sits in libusb, so nothing here may touch
   * the device: just raise the flag and let sane_read or sane_close do the
   * actual teardown.  That also makes repeated calls harmless. */
  h->cancelled = SANE_TRUE;
}

SANE_Status
sane_pantum_set_io_mode (SANE_Handle handle, SANE_Bool non_blocking)
{
  Pantum_Handle *h = handle;

  if (!h || !handle_is_valid (h))
    return SANE_STATUS_INVAL;
  /* In non-blocking mode sane_read only glances at the bulk endpoint and
   * hands back a zero-length buffer when the device has not produced the next
   * frame yet.  The glance is taken at a frame boundary and the rest of a
   * started frame is still read to completion, so the stream cannot end up
   * desynchronised. */
  h->non_blocking = non_blocking;
  DBG (2, "io mode: %s\n", non_blocking ? "non-blocking" : "blocking");
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_pantum_get_select_fd (SANE_Handle handle, SANE_Int *fd)
{
  Pantum_Handle *h = handle;

  if (!h || !handle_is_valid (h) || !fd)
    return SANE_STATUS_INVAL;
  /* There is no descriptor to hand out.  The frontend would select() it for
   * reading, and the only descriptors this backend owns are libusb's, which
   * on Linux are usbfs handles registered for POLLOUT - they signal that a
   * submitted URB can be reaped, not that bytes are waiting, and they never
   * become readable at all.  Making one work would mean either driving the
   * transfers through libusb's asynchronous API and pumping its event loop
   * from the frontend's select(), or the usual reader thread writing into a
   * pipe; both replace the synchronous core of this backend.  Non-blocking
   * reads work without either, so they are what is offered. */
  (void)fd;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_String_Const
sane_pantum_strstatus (SANE_Status status)
{
  switch (status)
    {
    case SANE_STATUS_GOOD:
      return "everything A-OK";
    case SANE_STATUS_UNSUPPORTED:
      return "operation is not supported";
    case SANE_STATUS_CANCELLED:
      return "operation was cancelled";
    case SANE_STATUS_DEVICE_BUSY:
      return "device is busy";
    case SANE_STATUS_INVAL:
      return "invalid argument";
    case SANE_STATUS_EOF:
      return "no more data available";
    case SANE_STATUS_JAMMED:
      return "document feeder jammed";
    case SANE_STATUS_NO_DOCS:
      return "document feeder out of documents";
    case SANE_STATUS_COVER_OPEN:
      return "scanner cover is open";
    case SANE_STATUS_IO_ERROR:
      return "error during device I/O";
    case SANE_STATUS_NO_MEM:
      return "out of memory";
    case SANE_STATUS_ACCESS_DENIED:
      return "access to resource has been denied";
    }
  return "unknown status";
}
