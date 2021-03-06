/*
 * Copyright (C) 2012-2022 the xine project
 * Copyright (C) 2012 Christophe Thommeret <hftom@free.fr>
 *
 * This file is part of xine, a free video player.
 *
 * xine is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * xine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * mem_frame.h, generic memory backed frame
 *
 *
 */

#ifndef XINE_MEM_FRAME_H
#define XINE_MEM_FRAME_H

#include <xine/xineutils.h>

typedef struct mem_frame_t {
  vo_frame_t vo_frame;
  int width, height, format, flags;
  double ratio;
} mem_frame_t;

static void _mem_frame_proc_slice(vo_frame_t *vo_img, uint8_t **src)
{
  (void)src;
  vo_img->proc_called = 1;
}

static void _mem_frame_field(vo_frame_t *vo_img, int which_field)
{
  (void)vo_img;
  (void)which_field;
}

static void _mem_frame_free_framedata(vo_frame_t *vo_img)
{
  xine_freep_aligned (&vo_img->base[0]);
  vo_img->base[1] = NULL;
  vo_img->base[2] = NULL;
  vo_img->pitches[0] = vo_img->pitches[1] = vo_img->pitches[2] = 0;
}

static void _mem_frame_dispose(vo_frame_t *vo_img)
{
  _mem_frame_free_framedata(vo_img);
  pthread_mutex_destroy (&vo_img->mutex);
  free (vo_img);
}

static void _mem_frame_init(mem_frame_t *frame, vo_driver_t *driver)
{
  frame->vo_frame.base[0] = frame->vo_frame.base[1] = frame->vo_frame.base[2] = NULL;
  frame->width = frame->height = frame->format = frame->flags = 0;
  frame->ratio = 0.0;

  pthread_mutex_init (&frame->vo_frame.mutex, NULL);

  frame->vo_frame.proc_slice = _mem_frame_proc_slice;
  frame->vo_frame.proc_frame = NULL;
  frame->vo_frame.field      = _mem_frame_field;
  frame->vo_frame.dispose    = _mem_frame_dispose;
  frame->vo_frame.driver     = driver;
}

static vo_frame_t *_mem_frame_alloc_frame(vo_driver_t *this_gen, size_t frame_size)
{
  mem_frame_t *frame;

  frame = calloc(1, frame_size);

  if (!frame)
    return NULL;

  _mem_frame_init(frame, this_gen);

  return &frame->vo_frame;
}

static inline vo_frame_t *mem_frame_alloc_frame(vo_driver_t *this_gen)
{
  return _mem_frame_alloc_frame(this_gen, sizeof(mem_frame_t));
}

static inline void *_memset32(void *mem, uint32_t val, size_t n)
{
  uint32_t *m32 = mem, *ret = mem;
  size_t i;
  for (i = n; i; i--)
    *m32++ = val;
  return ret;
}

static inline void mem_frame_update_frame_format(vo_driver_t *this_gen, vo_frame_t *frame_gen,
      uint32_t width, uint32_t height, double ratio, int format, int flags)
{
  mem_frame_t *frame = xine_container_of(frame_gen, mem_frame_t, vo_frame);

  (void)this_gen;

  /* vo_none and vo_opengl2 need no buffer adjustment for these. */
  frame->flags = flags;
  frame->ratio = ratio;

  /* Check frame size and format and reallocate if necessary (rare case). */
  if (!((frame->width ^ width) | (frame->height ^ height) | (frame->format ^ format)))
    return;

  frame->width  = width;
  frame->height = height;
  frame->format = format;

  /* (re-) allocate render space */
  _mem_frame_free_framedata(frame_gen);

  if (format == XINE_IMGFMT_YV12) {
    uint32_t w = (width + 15) & ~15;
    uint32_t ysize = w * height;
    uint32_t uvsize = (w >> 1) * ((height + 1) >> 1);

    frame->vo_frame.base[0] = xine_malloc_aligned (ysize + 2 * uvsize);
    if (frame->vo_frame.base[0]) {
      frame->vo_frame.base[1] = frame->vo_frame.base[0] + ysize;
      frame->vo_frame.base[2] = frame->vo_frame.base[1] + uvsize;
      frame->vo_frame.pitches[0] = w;
      frame->vo_frame.pitches[1] = w >> 1;
      frame->vo_frame.pitches[2] = w >> 1;

      memset (frame->vo_frame.base[0], 0, ysize);
      memset (frame->vo_frame.base[1], 128, 2 * uvsize);
    }

  } else if (format == XINE_IMGFMT_YV12_DEEP) {
    unsigned w = (width + 15) & ~15;
    unsigned ysize = 2 * w * height;
    unsigned uvsize = w * ((height + 1) >> 1);

    frame->vo_frame.base[0] = xine_malloc_aligned (ysize + 2 * uvsize);
    if (frame->vo_frame.base[0]) {
      unsigned depth = VO_GET_FLAGS_DEPTH(flags);
      uint32_t black = 0x00010001U * (1U << (depth - 1));

      frame->vo_frame.base[1] = frame->vo_frame.base[0] + ysize;
      frame->vo_frame.base[2] = frame->vo_frame.base[1] + uvsize;
      frame->vo_frame.pitches[0] = w * 2;
      frame->vo_frame.pitches[1] = w;
      frame->vo_frame.pitches[2] = w;

      memset (frame->vo_frame.base[0], 0, ysize);
      _memset32 (frame->vo_frame.base[1], black, 2 * uvsize / sizeof(uint32_t));
    }

  } else if (format == XINE_IMGFMT_NV12) {
    uint32_t w = (width + 15) & ~15;
    uint32_t ysize = w * height;
    uint32_t uvsize = w * ((height + 1) >> 1);

    frame->vo_frame.base[0] = xine_malloc_aligned (ysize + uvsize);
    if (frame->vo_frame.base[0]) {
      frame->vo_frame.base[1] = frame->vo_frame.base[0] + ysize;
      frame->vo_frame.pitches[0] = w;
      frame->vo_frame.pitches[1] = w;

      memset (frame->vo_frame.base[0], 0, ysize);
      memset (frame->vo_frame.base[1], 128, uvsize);
    }

  } else if (format == XINE_IMGFMT_YUY2) {
    uint32_t w = (width + 15) & ~15;
    frame->vo_frame.base[0] = xine_malloc_aligned ((w << 1) * height);
    if (frame->vo_frame.base[0]) {
      const union {uint8_t bytes[4]; uint32_t word;} black = {{0, 128, 0, 128}};
      frame->vo_frame.pitches[0] = w << 1;
      _memset32 (frame->vo_frame.base[0], black.word, frame->vo_frame.pitches[0] * height / sizeof(uint32_t));
    }
  }

  if (!frame->vo_frame.base[0]) {
    /* tell vo_get_frame () to retry later */
    frame->width = 0;
    frame->vo_frame.width = 0;
  }
}

#endif /* XINE_MEM_FRAME_H */
