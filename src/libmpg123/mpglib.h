/* 
 * Copyright (C) 2000 the xine project
 * 
 * This file is part of xine, a unix video player.
 * xine version of libmpg123 interface
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * $Id: mpglib.h,v 1.6 2001/08/21 19:39:50 jcdutton Exp $
 */

#ifndef HAVE_MPGLIB_H
#define HAVE_MPGLIB_H

#include <inttypes.h>

#include "audio_out.h"

typedef struct mpstr {
  unsigned char   bsspace[2][MAXFRAMESIZE+512]; /* MAXFRAMESIZE */
  int             bsnum;
  int             bsize;
  int             framesize, framesize_old;

  unsigned long   header;
  struct frame    fr;

  real            hybrid_block[2][2][SBLIMIT*SSLIMIT];
  int             hybrid_blc[2];
  real            synth_buffs[2][2][0x110];
  int             synth_bo;

  real            hybridIn[2][SBLIMIT][SSLIMIT];
  real            hybridOut[2][SSLIMIT][SBLIMIT];


  int             is_output_initialized;
  int             sample_rate_device;
  ao_instance_t *ao_output;
  unsigned char   osspace[8192];

  uint32_t        pts;
} mpgaudio_t;

#ifndef BOOL
#define BOOL int
#endif

#ifdef __cplusplus
extern "C" {
#endif
  
mpgaudio_t  *mpg_audio_init (ao_instance_t *ao_output);

void mpg_audio_reset (mpgaudio_t *mp);

void mpg_audio_decode_data (mpgaudio_t *mp, uint8_t *data, uint8_t *data_end,
			    uint32_t pts);

void mpg_audio_close (mpgaudio_t *mp);

#ifdef __cplusplus
}
#endif

#endif

