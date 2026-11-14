/*
 * zz9k.library media-session example: play an MPEG-1 or MPEG-2 Program
 * Stream on a native (OCS/ECS/AGA) screen through the shared-buffer output.
 *
 * The card decodes, scales the picture to the screen and converts it to
 * bitplanes with its own palette; the 68k copies each picture into chip RAM.
 * Video only: the sound, if any, is not played.
 *
 *   zz9k-shared-video-demo FILE [DEPTH] [ORDERED|DIFFUSION|NONE] [NEAREST]
 *
 * DEPTH is the number of bitplanes, 1-8 (default 8, which needs AGA; use 5
 * on OCS/ECS). Stop with Ctrl-C or the left mouse button.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proto/zz9k.h"
#include "zz9k/reply.h"
#include "zz9k/request.h"
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_STAGING_BYTES (32UL * 1024UL)
#define DEMO_SCREEN_WIDTH 320U
#define DEMO_SCREEN_HEIGHT 256U
#define DEMO_PALETTE_BYTES 1024U
/* A picture this late is dropped instead of shown. */
#define DEMO_LATE_TICKS 10L

struct Library *ZZ9KBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

typedef struct DemoVideo {
  uint32_t codec;
  uint32_t width;
  uint32_t height;
} DemoVideo;

typedef struct DemoStats {
  uint32_t presented;
  uint32_t discarded;
  uint32_t busy;
  uint32_t pending_waits;
} DemoStats;

static const char *status_name(int status)
{
  switch (status) {
  case ZZ9K_STATUS_OK: return "OK";
  case ZZ9K_STATUS_BUSY: return "BUSY";
  case ZZ9K_STATUS_UNSUPPORTED: return "UNSUPPORTED";
  case ZZ9K_STATUS_BAD_REQUEST: return "BAD_REQUEST";
  case ZZ9K_STATUS_BAD_HANDLE: return "BAD_HANDLE";
  case ZZ9K_STATUS_NO_MEMORY: return "NO_MEMORY";
  case ZZ9K_STATUS_TIMEOUT: return "TIMEOUT";
  case ZZ9K_STATUS_IO_ERROR: return "IO_ERROR";
  default: return "error";
  }
}

/* The sequence header gives the picture size; a sequence extension right
 * after it marks MPEG-2. */
static int probe_mpeg(const char *path, DemoVideo *video)
{
  static uint8_t head[65536];
  FILE *file = fopen(path, "rb");
  size_t length;
  size_t i;

  if (!file)
    return 0;
  length = fread(head, 1U, sizeof(head), file);
  fclose(file);
  for (i = 0; i + 12U < length; i++) {
    if (head[i] == 0U && head[i + 1U] == 0U && head[i + 2U] == 1U &&
        head[i + 3U] == 0xb3U) {
      size_t j;
      video->width = ((uint32_t)head[i + 4U] << 4) | (head[i + 5U] >> 4);
      video->height = (((uint32_t)head[i + 5U] & 15U) << 8) | head[i + 6U];
      video->codec = ZZ9K_VIDEO_CODEC_MPEG1;
      for (j = i + 12U; j + 4U < length && j < i + 256U; j++) {
        if (head[j] == 0U && head[j + 1U] == 0U && head[j + 2U] == 1U) {
          if (head[j + 3U] == 0xb5U && (head[j + 4U] >> 4) == 1U)
            video->codec = ZZ9K_VIDEO_CODEC_MPEG2;
          break;
        }
      }
      return video->width != 0U && video->height != 0U;
    }
  }
  return 0;
}

static int stop_requested(void)
{
  /* CIA-A PRA bit 6 is the left mouse button, low when pressed. */
  volatile uint8_t *ciaa_pra = (volatile uint8_t *)0xbfe001;

  if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
    return 1;
  return (*ciaa_pra & 0x40U) == 0U;
}

static long ticks_now(void)
{
  struct DateStamp stamp;

  DateStamp(&stamp);
  return (stamp.ds_Days * 1440L + stamp.ds_Minute) * 3000L + stamp.ds_Tick;
}

static int media_call(ZZ9KRequest *request, ZZ9KMediaSessionMainResult *result)
{
  ZZ9KMailboxEntry reply;
  int status = ZZ9KCall(request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);

  if (status != ZZ9K_STATUS_OK)
    return status;
  return zz9k_reply_media_session_main(&reply, request->entry.opcode, result);
}

static int media_command(uint16_t opcode, uint32_t session,
                         ZZ9KMediaSessionMainResult *result)
{
  ZZ9KRequest request;
  int status = zz9k_request_media_session_command(&request, opcode, session, 0U);

  if (status != ZZ9K_STATUS_OK)
    return status;
  return media_call(&request, result);
}

static int output_status(uint32_t session, ZZ9KMediaSessionStatusResult *result)
{
  ZZ9KRequest request;
  ZZ9KMailboxEntry reply;
  int status = zz9k_request_media_session_status(
      &request, session, ZZ9K_MEDIA_STATUS_OUTPUT, 0U);

  if (status != ZZ9K_STATUS_OK)
    return status;
  status = ZZ9KCall(&request, &reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
  if (status != ZZ9K_STATUS_OK)
    return status;
  return zz9k_reply_media_session_status(&reply, result);
}

static int has_shared_output(void)
{
  ZZ9KServiceInfo service;
  int status = ZZ9KQueryService(ZZ9K_SERVICE_VIDEO, &service);

  if (status != ZZ9K_STATUS_OK) {
    printf("No video service: %s (%d)\n", status_name(status), status);
    return 0;
  }
  printf("Video service: %lu opcodes, flags 0x%08lx\n",
         (unsigned long)service.opcode_count, (unsigned long)service.flags);
  if (service.opcode_count < ZZ9K_VIDEO_OPCODE_COUNT_SHARED_OUTPUT) {
    printf("This firmware has no shared-buffer output (needs %u opcodes).\n",
           (unsigned)ZZ9K_VIDEO_OPCODE_COUNT_SHARED_OUTPUT);
    return 0;
  }
  return 1;
}

static void load_palette(struct Screen *screen, const volatile uint8_t *palette,
                         uint32_t depth)
{
  static ULONG table[1U + 256U * 3U + 1U];
  uint32_t pens = 1UL << depth;
  uint32_t i;

  table[0] = (pens << 16) | 0U;
  for (i = 0; i < pens; i++) {
    const volatile uint8_t *entry = palette + i * 4U;
    table[1U + i * 3U] = (ULONG)entry[1] * 0x01010101UL;
    table[2U + i * 3U] = (ULONG)entry[2] * 0x01010101UL;
    table[3U + i * 3U] = (ULONG)entry[3] * 0x01010101UL;
  }
  table[1U + pens * 3U] = 0U;
  LoadRGB32(&screen->ViewPort, table);
}

/* Copy one picture into the screen's bitplanes, at row top. */
static void copy_picture(struct BitMap *bm, int interleaved,
                         const volatile uint8_t *picture, uint32_t pitch,
                         uint32_t height, uint32_t depth, uint32_t top)
{
  uint32_t plane;

  if (interleaved) {
    /* Rows of every plane follow each other, in the card's order too. */
    CopyMem((APTR)picture, bm->Planes[0] + top * bm->BytesPerRow,
            height * bm->BytesPerRow);
    return;
  }
  for (plane = 0; plane < depth; plane++)
    CopyMem((APTR)(picture + plane * pitch * height),
            bm->Planes[plane] + top * bm->BytesPerRow, height * pitch);
}

static int play(const char *path, const DemoVideo *video, uint32_t depth,
                uint32_t dither, uint32_t nearest, DemoStats *stats)
{
  ZZ9KSharedBuffer staging, output, palette;
  ZZ9KMediaSessionBeginDesc begin;
  ZZ9KMediaSessionOutputDesc config;
  ZZ9KMediaSessionWriteDesc write;
  ZZ9KMediaSessionMainResult result;
  ZZ9KMediaSessionStatusResult page;
  ZZ9KRequest request;
  struct Screen *screen = NULL;
  struct BitMap *bm;
  FILE *file = NULL;
  uint32_t session = 0;
  uint32_t out_w, out_h, pitch, picture_bytes, top;
  uint32_t pending = 0, pending_offset = 0;
  uint64_t first_pts = ZZ9K_MEDIA_NO_PTS;
  long start_ticks = 0;
  int interleaved, eof = 0, eof_sent = 0, status, rc = 0;

  memset(&staging, 0, sizeof(staging));
  memset(&output, 0, sizeof(output));
  memset(&palette, 0, sizeof(palette));

  /* The screen's full width, letterboxed; a picture taller than 5:4 is
   * squeezed to the screen's height. */
  out_w = DEMO_SCREEN_WIDTH;
  out_h = (DEMO_SCREEN_WIDTH * video->height / video->width) & ~1UL;
  if (out_h > DEMO_SCREEN_HEIGHT)
    out_h = DEMO_SCREEN_HEIGHT;
  if (out_h < 2U)
    out_h = 2U;
  top = (DEMO_SCREEN_HEIGHT - out_h) / 2U;

  screen = OpenScreenTags(NULL,
                          SA_Width, DEMO_SCREEN_WIDTH,
                          SA_Height, DEMO_SCREEN_HEIGHT,
                          SA_Depth, depth,
                          SA_DisplayID, LORES_KEY,
                          SA_Type, CUSTOMSCREEN,
                          SA_Quiet, TRUE,
                          SA_ShowTitle, FALSE,
                          SA_Interleaved, TRUE,
                          TAG_DONE);
  if (!screen) {
    printf("Could not open a %lu-plane screen\n", (unsigned long)depth);
    return 20;
  }
  bm = screen->RastPort.BitMap;
  interleaved = (GetBitMapAttr(bm, BMA_FLAGS) & BMF_INTERLEAVED) != 0;
  /* The screen's own row length, so a picture is one copy (interleaved)
   * or one per plane. */
  pitch = interleaved ? bm->BytesPerRow / depth : bm->BytesPerRow;
  picture_bytes = pitch * out_h * depth;

  status = ZZ9KAllocShared(DEMO_STAGING_BYTES, 16U, 0U, &staging);
  if (status == ZZ9K_STATUS_OK)
    status = ZZ9KAllocShared(picture_bytes * 2U, 16U, 0U, &output);
  if (status == ZZ9K_STATUS_OK)
    status = ZZ9KAllocShared(DEMO_PALETTE_BYTES, 16U, 0U, &palette);
  if (status != ZZ9K_STATUS_OK) {
    printf("ZZ9KAllocShared failed: %s (%d)\n", status_name(status), status);
    rc = 20;
    goto out;
  }

  memset(&begin, 0, sizeof(begin));
  begin.video_codec = video->codec;
  begin.container = ZZ9K_VIDEO_CONTAINER_MPEG_PS;
  begin.width = video->width;
  begin.height = video->height;
  begin.output_format = ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER;
  begin.audio_codec = ZZ9K_MEDIA_AUDIO_NONE;
  status = zz9k_request_media_session_begin(&request, &begin);
  if (status == ZZ9K_STATUS_OK)
    status = media_call(&request, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("BEGIN (shared-buffer output) failed: %s (%d)\n",
           status_name(status), status);
    rc = 20;
    goto out;
  }
  session = result.session;

  memset(&config, 0, sizeof(config));
  config.session = session;
  config.buffer_handle = output.handle;
  config.slot_stride = picture_bytes;
  config.width = out_w;
  config.height = out_h;
  config.format = ZZ9K_SURFACE_FORMAT_PLANAR;
  config.pitch = pitch;
  config.flags = ZZ9K_MEDIA_OUTPUT_DEPTH(depth) | dither | nearest |
                 (interleaved ? ZZ9K_MEDIA_OUTPUT_PLANAR_INTERLEAVED : 0U);
  config.palette_handle = palette.handle;
  status = zz9k_request_media_session_output(&request, &config);
  if (status == ZZ9K_STATUS_OK)
    status = media_call(&request, &result);
  if (status != ZZ9K_STATUS_OK) {
    printf("OUTPUT_CONFIG failed: %s (%d)\n", status_name(status), status);
    rc = 20;
    goto out;
  }
  load_palette(screen, (const volatile uint8_t *)palette.data, depth);

  file = fopen(path, "rb");
  if (!file) {
    printf("Cannot open %s\n", path);
    rc = 20;
    goto out;
  }

  while (!stop_requested()) {
    /* Keep the card fed until it takes no more. */
    while (!eof_sent) {
      if (!pending && !eof) {
        pending = (uint32_t)fread((void *)staging.data, 1U,
                                  DEMO_STAGING_BYTES, file);
        pending_offset = 0;
        eof = pending < DEMO_STAGING_BYTES;
      }
      memset(&write, 0, sizeof(write));
      write.session = session;
      write.src_handle = staging.handle;
      write.src_offset = pending_offset;
      write.src_length = pending;
      write.flags = eof ? ZZ9K_MEDIA_SESSION_WRITE_EOF : 0U;
      status = zz9k_request_media_session_write(&request, &write);
      if (status == ZZ9K_STATUS_OK)
        status = media_call(&request, &result);
      if (status == ZZ9K_STATUS_BUSY)
        break;
      if (status != ZZ9K_STATUS_OK) {
        printf("WRITE failed: %s (%d)\n", status_name(status), status);
        rc = 20;
        goto out;
      }
      pending_offset += result.bytes_written;
      pending -= result.bytes_written;
      if (pending)
        break; /* the card's queue is full: decode first */
      if (eof)
        eof_sent = 1;
    }

    status = media_command(ZZ9K_OP_MEDIA_SESSION_DECODE, session, &result);
    if (status == ZZ9K_STATUS_BUSY) {
      stats->busy++;
      continue;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("DECODE failed: %s (%d)\n", status_name(status), status);
      rc = 20;
      break;
    }
    if (result.state == ZZ9K_MEDIA_SESSION_STATE_DONE)
      break;
    if (result.state != ZZ9K_MEDIA_SESSION_STATE_FRAME_HELD)
      continue;

    /* Pace by the picture's time stamp, 90 kHz against 50 Hz ticks. */
    if (first_pts == ZZ9K_MEDIA_NO_PTS) {
      first_pts = result.video_pts;
      start_ticks = ticks_now();
    } else if (result.video_pts != ZZ9K_MEDIA_NO_PTS) {
      long due = start_ticks +
                 (long)((result.video_pts - first_pts) * 50U / 90000U);
      long now = ticks_now();
      if (now - due > DEMO_LATE_TICKS) {
        if (media_command(ZZ9K_OP_MEDIA_SESSION_DISCARD, session, &result) ==
            ZZ9K_STATUS_OK)
          stats->discarded++;
        continue;
      }
      while (ticks_now() < due && !stop_requested())
        WaitTOF();
    }

    status = media_command(ZZ9K_OP_MEDIA_SESSION_PRESENT, session, &result);
    if (status != ZZ9K_STATUS_OK) {
      printf("PRESENT failed: %s (%d)\n", status_name(status), status);
      rc = 20;
      break;
    }
    if (result.flags & ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_PENDING) {
      /* Firmware that writes after replying: wait until the picture is
       * counted as complete. */
      stats->pending_waits++;
      do {
        status = output_status(session, &page);
      } while (status == ZZ9K_STATUS_OK &&
               page.value[0] < (uint64_t)stats->presented + 1U);
    }
    stats->presented++;
    WaitTOF();
    copy_picture(bm, interleaved,
                 (const volatile uint8_t *)output.data +
                     ((result.flags & ZZ9K_MEDIA_SESSION_RESULT_OUTPUT_SLOT1)
                          ? picture_bytes : 0U),
                 pitch, out_h, depth, top);
  }

out:
  if (session) {
    /* A held picture keeps CLOSE busy: drop it first. */
    media_command(ZZ9K_OP_MEDIA_SESSION_DISCARD, session, &result);
    media_command(ZZ9K_OP_MEDIA_SESSION_CLOSE, session, &result);
  }
  if (file)
    fclose(file);
  if (palette.handle)
    ZZ9KFreeShared(palette.handle);
  if (output.handle)
    ZZ9KFreeShared(output.handle);
  if (staging.handle)
    ZZ9KFreeShared(staging.handle);
  if (screen)
    CloseScreen(screen);
  printf("%lu x %lu %s -> %lu x %lu, %lu planes%s: %lu shown, %lu dropped\n",
         (unsigned long)video->width, (unsigned long)video->height,
         video->codec == ZZ9K_VIDEO_CODEC_MPEG2 ? "MPEG-2" : "MPEG-1",
         (unsigned long)out_w, (unsigned long)out_h, (unsigned long)depth,
         interleaved ? " (interleaved)" : "",
         (unsigned long)stats->presented, (unsigned long)stats->discarded);
  return rc;
}

int main(int argc, char **argv)
{
  DemoVideo video;
  DemoStats stats;
  uint32_t depth = 8U, dither = ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED, nearest = 0U;
  int i, rc;

  if (argc < 2) {
    printf("Usage: %s FILE [DEPTH] [ORDERED|DIFFUSION|NONE] [NEAREST]\n",
           argv[0]);
    return 5;
  }
  for (i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "ORDERED") || !strcmp(argv[i], "ordered"))
      dither = ZZ9K_MEDIA_OUTPUT_DITHER_ORDERED;
    else if (!strcmp(argv[i], "DIFFUSION") || !strcmp(argv[i], "diffusion"))
      dither = ZZ9K_MEDIA_OUTPUT_DITHER_DIFFUSION;
    else if (!strcmp(argv[i], "NONE") || !strcmp(argv[i], "none"))
      dither = 0U;
    else if (!strcmp(argv[i], "NEAREST") || !strcmp(argv[i], "nearest"))
      nearest = ZZ9K_MEDIA_OUTPUT_SCALE_NEAREST;
    else
      depth = (uint32_t)strtoul(argv[i], NULL, 10);
  }
  if (depth < 1U || depth > 8U) {
    printf("DEPTH must be 1-8\n");
    return 5;
  }
  memset(&video, 0, sizeof(video));
  if (!probe_mpeg(argv[1], &video)) {
    printf("%s: no MPEG sequence header in the first 64 KB\n", argv[1]);
    return 10;
  }

  GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
  IntuitionBase = (struct IntuitionBase *)OpenLibrary(
      (CONST_STRPTR)"intuition.library", 39);
  ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
  if (!GfxBase || !IntuitionBase || !ZZ9KBase) {
    printf("Needs graphics.library 39, intuition.library 39 and %s %u\n",
           ZZ9K_LIBRARY_NAME, (unsigned)ZZ9K_LIBRARY_VERSION);
    rc = 20;
  } else if (!has_shared_output()) {
    rc = 10;
  } else {
    memset(&stats, 0, sizeof(stats));
    rc = play(argv[1], &video, depth, dither, nearest, &stats);
  }
  if (ZZ9KBase)
    CloseLibrary(ZZ9KBase);
  if (IntuitionBase)
    CloseLibrary((struct Library *)IntuitionBase);
  if (GfxBase)
    CloseLibrary((struct Library *)GfxBase);
  return rc;
}
