/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SANE backend for Pantum M6500/M6507 series scanners.
 *
 * The device speaks a simple framed protocol ("ASP\x01") over the bulk
 * endpoints of its vendor-specific USB interface.  Everything here was
 * written from the protocol notes in notes/disasm-protocol.md and verified
 * against the device with proto/pantum_scan.py.
 */
#ifndef PANTUM_H
#define PANTUM_H

#include <stdint.h>
#include <stddef.h>

#include <libusb.h>
#include <sane/sane.h>

/* The scanner lives on interface 1; interface 0 is the printer and belongs
 * to usblp, so it must never be claimed here. */
#define PANTUM_INTERFACE 1

#define PANTUM_MAGIC 0x41535001u /* "ASP\x01" */
#define PANTUM_HEADER_LEN 32
#define PANTUM_SETTINGS_LEN 100   /* 25 big-endian words */
#define PANTUM_SETTINGS_WORDS (PANTUM_SETTINGS_LEN / 4)
#define PANTUM_DATA_HEADER_LEN 24 /* 6 big-endian words in front of pixels */

/* Largest payload a frame is allowed to announce.  The device picks that
 * number and every buffer and bulk transfer below is sized from it, so it
 * needs a ceiling: one well under INT_MAX keeps the length arithmetic exact,
 * and one this far above the real traffic - a data block is 16 rows of a
 * padded scan line, tens of kilobytes - only ever rejects a stream that has
 * lost frame sync. */
#define PANTUM_MAX_PAYLOAD (64u * 1024u * 1024u)

/* Host -> device */
#define CMD_LOCK 0x00
#define CMD_UNLOCK 0x01
#define CMD_START 0x02
#define CMD_ABORT 0x03
#define CMD_GET_SETTINGS 0x06
#define CMD_SET_SETTINGS 0x07
#define CMD_SET_DEFAULTS 0x08
#define CMD_ADF_STATUS 0x0f

/* Device -> host */
#define EVT_ABORTED 0x04
#define EVT_DATA 0x05
#define EVT_SETTINGS 0x09
#define EVT_SCAN_BEGIN 0x0a
#define EVT_PAGE_BEGIN 0x0b
#define EVT_JOB_END 0x0c
#define EVT_SCAN_END 0x0d
#define EVT_PAGE_END 0x0e

/* Word offsets into the 100-byte job settings block. */
#define SET_RESOLUTION 3
#define SET_ZERO 13
#define SET_SOURCE 14
#define SET_DATA_TYPE 15 /* read-only: how the pixel data is laid out */
#define SET_TOP 16
#define SET_LEFT 17
#define SET_BOTTOM 18
#define SET_RIGHT 19
#define SET_COLOUR 24

#define SOURCE_FLATBED 0x100
#define SOURCE_ADF 0x200
#define SOURCE_ADF_DUPLEX 0x400

/* Flatbed travel the firmware accepts, in hundredths of an inch.  The device
 * reports a slightly larger maximum window than it will actually take. */
#define PANTUM_MAX_X 850  /* 8.50" = 215.9 mm */
#define PANTUM_MAX_Y 1169 /* 11.69" = 296.9 mm */
/* The feeder takes a longer sheet: the vendor clamps a fed page at 14". */
#define PANTUM_MAX_Y_ADF 1400 /* 14.00" = 355.6 mm */
#define PANTUM_MIN_WINDOW 15 /* the firmware rejects windows <= 14 */

/* Pixel layout codes seen in the data block sub-header. */
#define DATA_GREY 0x06
#define DATA_COLOUR 0x0e
#define DATA_JPEG 0x0f

/* The tone curve the vendor backend folds into every scanned byte. */
#define PANTUM_GAMMA 1.8

typedef enum
{
  OPT_NUM_OPTS = 0,
  OPT_MODE_GROUP,
  OPT_MODE,
  OPT_RESOLUTION,
  OPT_SOURCE,
  OPT_PREVIEW,
  OPT_THRESHOLD,
  OPT_GEOMETRY_GROUP,
  OPT_TL_X,
  OPT_TL_Y,
  OPT_BR_X,
  OPT_BR_Y,
  NUM_OPTIONS
} Pantum_Option;

typedef union
{
  SANE_Word w;
  SANE_Word *wa;
  SANE_String s;
} Option_Value;

typedef struct Pantum_Device
{
  struct Pantum_Device *next;
  SANE_Device sane; /* strings below are owned by this struct */
  char *name;
  char *model;
  uint8_t bus;
  uint8_t addr;
  uint16_t vid;
  uint16_t pid;
  SANE_Bool busy; /* already handed out to a handle */
} Pantum_Device;

/* The 32-byte frame header, unpacked. */
typedef struct Pantum_Frame
{
  uint32_t cmd;
  uint32_t arg0;   /* page parity in data frames, paper flag in ADF status */
  uint32_t status; /* device status code, 0 on success */
  uint32_t plen;   /* bytes of payload following the header */
} Pantum_Frame;

typedef struct Pantum_Handle
{
  struct Pantum_Handle *next;
  Pantum_Device *dev;
  libusb_device_handle *usb;

  SANE_Option_Descriptor opt[NUM_OPTIONS];
  Option_Value val[NUM_OPTIONS];
  SANE_Parameters params;

  SANE_Bool scanning;  /* a job is running on the device */
  SANE_Bool locked;    /* CMD_LOCK went out, CMD_UNLOCK still owed */
  /* Written by sane_cancel, which frontends call from a signal handler. */
  volatile SANE_Bool cancelled;
  SANE_Bool eof;       /* the whole page has been handed to the frontend */
  SANE_Bool non_blocking; /* sane_read may come back empty-handed */
  SANE_Bool has_adf;   /* the device answered CMD_ADF_STATUS with "feeder" */
  SANE_Bool feeding;   /* an ADF job is between pages, still locked */

  SANE_Int next_row;   /* next image row to hand out */
  SANE_Int threshold;  /* 0..255, lineart only */
  SANE_Bool halftone;  /* the running job screens grey into 1 bit */
  SANE_Int white_top;    /* rows forced white at the top of the page */
  SANE_Int white_bottom; /* rows forced white at the bottom (feeder only) */
  SANE_Int white_left;   /* pixels forced white at the start of every row */
  SANE_Int white_right;  /* pixels forced white at the end (feeder only) */

  /* Tone curve for the mode of the running job, rebuilt by sane_start. */
  uint8_t gamma[256];

  uint8_t *out;        /* converted rows waiting for sane_read */
  size_t out_len;
  size_t out_pos;
  size_t out_cap;

  uint8_t *raw;        /* scratch for one data block payload */
  size_t raw_cap;
} Pantum_Handle;

#endif /* PANTUM_H */
