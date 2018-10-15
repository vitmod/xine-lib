/*
 * Copyright (C) 2000-2018 the xine project
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
 * along with self program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 */

/**
 * @file
 * @brief xine-lib audio output implementation
 *
 * @date 2001-08-20 First implementation of Audio sync and Audio driver separation.
 *       (c) 2001 James Courtier-Dutton <james@superbug.demon.co.uk>
 * @date 2001-08-22 James imported some useful AC3 sections from the previous
 *       ALSA driver. (c) 2001 Andy Lo A Foe <andy@alsaplayer.org>
 *
 *
 * General Programming Guidelines: -
 * New concept of an "audio_frame".
 * An audio_frame consists of all the samples required to fill every
 * audio channel to a full amount of bits.
 * So, it does not mater how many bits per sample, or how many audio channels
 * are being used, the number of audio_frames is the same.
 * E.g.  16 bit stereo is 4 bytes, but one frame.
 *       16 bit 5.1 surround is 12 bytes, but one frame.
 * The purpose of this is to make the audio_sync code a lot more readable,
 * rather than having to multiply by the amount of channels all the time
 * when dealing with audio_bytes instead of audio_frames.
 *
 * The number of samples passed to/from the audio driver is also sent
 * in units of audio_frames.
 *
 * Currently, James has tested with OSS: Standard stereo out, SPDIF PCM, SPDIF AC3
 *                                 ALSA: Standard stereo out
 * No testing has been done of ALSA SPDIF AC3 or any 4,5,5.1 channel output.
 * Currently, I don't think resampling functions, as I cannot test it.
 *
 * equalizer based on
 *
 *   PCM time-domain equalizer
 *
 *   Copyright (C) 2002  Felipe Rivera <liebremx at users sourceforge net>
 *
 * heavily modified by guenter bartsch 2003 for use in libxine
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <inttypes.h>
#include <math.h>
#include <sys/time.h>

#define XINE_ENABLE_EXPERIMENTAL_FEATURES

#define LOG_MODULE "audio_out"
#define LOG_VERBOSE
/*
#define LOG
*/

#define LOG_RESAMPLE_SYNC 0

#include <xine/xine_internal.h>
#include <xine/xineutils.h>
#include <xine/audio_out.h>
#include <xine/resample.h>
#include <xine/metronom.h>

#include "xine_private.h"


#define NUM_AUDIO_BUFFERS       32
#define AUDIO_BUF_SIZE       32768

/* By adding gap errors (difference between reported and expected
 * sound card clock) into metronom's vpts_offset we can use its
 * smoothing algorithms to correct sound card clock drifts.
 * obs: previously this error was added to xine scr.
 *
 * audio buf ---> metronom --> audio fifo --> (buf->vpts - hw_vpts)
 *           (vpts_offset + error)                     gap
 *                    <---------- control --------------|
 *
 * Unfortunately audio fifo adds a large delay to our closed loop.
 *
 * The defines below are designed to avoid updating the metronom too fast.
 * - it will only be updated 1 time per second (so it has a chance of
 *   distributing the error for several frames).
 * - it will only be updated 2 times for the whole audio fifo size
 *   length (so the control will wait to see the feedback effect)
 * - each update will be of gap/SYNC_GAP_RATE.
 *
 * Sound card clock correction can only provide smooth playback for
 * errors < 1% nominal rate. For bigger errors (bad streams) audio
 * buffers may be dropped or gaps filled with silence.
 */
#define SYNC_TIME_INTERVAL  (1 * 90000)
#define SYNC_BUF_INTERVAL   NUM_AUDIO_BUFFERS / 2
#define SYNC_GAP_RATE_LOG2  2

/* Alternative for metronom feedback: fix sound card clock drift
 * by resampling all audio data, so that the sound card keeps in
 * sync with the system clock. This may help, if one uses a DXR3/H+
 * decoder board. Those have their own clock (which serves as xine's
 * master clock) and can only operate at fixed frame rates (if you
 * want smooth playback). Resampling then avoids A/V sync problems,
 * gaps filled with 0-frames and jerky video playback due to different
 * clock speeds of the sound card and DXR3/H+.
 */
#define RESAMPLE_SYNC_WINDOW 50
#define RESAMPLE_MAX_GAP_DIFF 150
#define RESAMPLE_REDUCE_GAP_THRESHOLD 200



typedef struct {
  double   last_factor;
  int      window;
  int      reduce_gap;
  uint64_t window_duration, last_vpts;
  int64_t  recent_gap[8], last_avg_gap;
  int      valid;
} resample_sync_t;

/*
 * equalizer stuff
 */

#define EQ_BANDS    10
#define EQ_CHANNELS  8

#define FP_FRBITS 28

#define EQ_REAL(x) ((int)((x) * (1 << FP_FRBITS)))

typedef struct  {
  int beta;
  int alpha;
  int gamma;
} sIIRCoefficients;

static const sIIRCoefficients iir_cf[] = {
  /* 31 Hz*/
  { EQ_REAL(9.9691562441e-01), EQ_REAL(1.5421877947e-03), EQ_REAL(1.9968961468e+00) },
  /* 62 Hz*/
  { EQ_REAL(9.9384077546e-01), EQ_REAL(3.0796122698e-03), EQ_REAL(1.9937629855e+00) },
  /* 125 Hz*/
  { EQ_REAL(9.8774277725e-01), EQ_REAL(6.1286113769e-03), EQ_REAL(1.9874275518e+00) },
  /* 250 Hz*/
  { EQ_REAL(9.7522112569e-01), EQ_REAL(1.2389437156e-02), EQ_REAL(1.9739682661e+00) },
  /* 500 Hz*/
  { EQ_REAL(9.5105628526e-01), EQ_REAL(2.4471857368e-02), EQ_REAL(1.9461077269e+00) },
  /* 1k Hz*/
  { EQ_REAL(9.0450844499e-01), EQ_REAL(4.7745777504e-02), EQ_REAL(1.8852109613e+00) },
  /* 2k Hz*/
  { EQ_REAL(8.1778971701e-01), EQ_REAL(9.1105141497e-02), EQ_REAL(1.7444877599e+00) },
  /* 4k Hz*/
  { EQ_REAL(6.6857185264e-01), EQ_REAL(1.6571407368e-01), EQ_REAL(1.4048592171e+00) },
  /* 8k Hz*/
  { EQ_REAL(4.4861333678e-01), EQ_REAL(2.7569333161e-01), EQ_REAL(6.0518718075e-01) },
  /* 16k Hz*/
  { EQ_REAL(2.4201241845e-01), EQ_REAL(3.7899379077e-01), EQ_REAL(-8.0847117831e-01) },
};

struct audio_fifo_s {
  audio_buffer_t    *first;
  audio_buffer_t   **add;

  pthread_mutex_t    mutex;
  pthread_cond_t     not_empty;
  pthread_cond_t     empty;

  int                num_buffers;
  int                num_buffers_max;
  int                num_waiters;
};

typedef struct {

  xine_audio_port_t    ao; /* public part */

  /* private stuff */
  ao_driver_t         *driver;
  int                  dreqs_all;          /* statistics */
  int                  dreqs_wait;
  pthread_mutex_t      driver_lock;

  uint32_t             driver_open:1;
  uint32_t             audio_loop_running:1;
  uint32_t             grab_only:1; /* => do not start thread, frontend will consume samples */
  uint32_t             do_resample:1;
  uint32_t             do_compress:1;
  uint32_t             do_amp:1;
  uint32_t             amp_mute:1;
  uint32_t             do_equ:1;

  int                  num_driver_actions; /* number of threads, that wish to call
                                            * functions needing driver_lock */
  pthread_mutex_t      driver_action_lock; /* protects num_driver_actions */
  pthread_cond_t       driver_action_cond; /* informs about num_driver_actions-- */

  metronom_clock_t    *clock;
  xine_t              *xine;

#define STREAMS_DEFAULT_SIZE 32
  int                  num_null_streams;
  int                  num_anon_streams;
  int                  num_streams;
  int                  streams_size;
  xine_stream_t      **streams, *streams_default[STREAMS_DEFAULT_SIZE];
  xine_rwlock_t        streams_lock;

  pthread_t       audio_thread;

  uint32_t        audio_step;           /* pts per 32 768 samples (sample = #bytes/2) */
  uint32_t        frames_per_kpts;      /* frames per 1024/90000 sec                  */
  uint32_t        pts_per_kframe;       /* pts per 1024 frames                        */

  int             av_sync_method_conf;
  resample_sync_t resample_sync_info;
  double          resample_sync_factor; /* correct buffer length by this factor
                                         * to sync audio hardware to (dxr3) clock */
  int             resample_sync_method; /* fix sound card clock drift by resampling */

  int             gap_tolerance;

  ao_format_t     input, output;        /* format conversion done at audio_out.c */
  double          frame_rate_factor;
  double          output_frame_excess;  /* used to keep track of 'half' frames */

  int             resample_conf;
  uint32_t        force_rate;           /* force audio output rate to this value if non-zero */

  audio_fifo_t    free_fifo;
  audio_fifo_t    out_fifo;

  int64_t         last_audio_vpts;
  pthread_mutex_t current_speed_lock;
  uint32_t        current_speed;        /* the current playback speed */
  int             slow_fast_audio;      /* play audio even on slow/fast speeds */

  int16_t	  last_sample[RESAMPLE_MAX_CHANNELS];
  audio_buffer_t *frame_buf[2];         /* two buffers for "stackable" conversions */
  int16_t        *zero_space;

  int             passthrough_offset, ptoffs;
  int             flush_audio_driver;
  int             discard_buffers;

  int             dropped;
  int             step;
  pthread_mutex_t step_mutex;
  pthread_cond_t  done_stepping;

  /* some built-in audio filters */

  double          compression_factor;   /* current compression */
  double          compression_factor_max; /* user limit on compression */
  double          amp_factor;

  /* 10-band equalizer */

  int             eq_settings[EQ_BANDS];
  int             eq_gain[EQ_BANDS];
  /* Coefficient history for the IIR filter */
  int             eq_data_history[EQ_CHANNELS][EQ_BANDS][4];

  int             last_gap;
  int             last_sgap;

  xine_stream_t  *buf_streams[NUM_AUDIO_BUFFERS];
  uint8_t        *base_samp;

  /* extra info ring buffer */
#define EI_RING_SIZE 32 /* 2^n please */
  int             ei_write;
  int             ei_read;

  audio_buffer_t  base_buf[NUM_AUDIO_BUFFERS + 2];
  extra_info_t    base_ei[EI_RING_SIZE + NUM_AUDIO_BUFFERS + 2];
} aos_t;

static int ao_set_property (xine_audio_port_t *this_gen, int property, int value);

/********************************************************************
 * streams register.                                                *
 * Reading is way more speed relevant here.                         *
 *******************************************************************/

static void ao_streams_open (aos_t *this) {
#ifndef HAVE_ZERO_SAFE_MEM
  this->num_null_streams   = 0;
  this->num_anon_streams   = 0;
  this->num_streams        = 0;
  this->streams_default[0] = NULL;
#endif
  this->streams_size = STREAMS_DEFAULT_SIZE;
  this->streams      = &this->streams_default[0];
  xine_rwlock_init_default (&this->streams_lock);
}

static void ao_streams_close (aos_t *this) {
  xine_rwlock_destroy (&this->streams_lock);
  if (this->streams != &this->streams_default[0])
    _x_freep (&this->streams);
#if 0 /* not yet needed */
  this->num_null_streams = 0;
  this->num_anon_streams = 0;
  this->num_streams      = 0;
  this->streams_size     = 0;
#endif
}

static void ao_streams_register (aos_t *this, xine_stream_t *s) {
  xine_rwlock_wrlock (&this->streams_lock);
  if (!s) {
    this->num_null_streams++;
  } else if (s == XINE_ANON_STREAM) {
    this->num_anon_streams++;
  } else do {
    xine_stream_t **a = this->streams;
    if (this->num_streams + 2 > this->streams_size) {
      xine_stream_t **n = malloc ((this->streams_size + 32) * sizeof (void *));
      if (!n)
        break;
      memcpy (n, a, this->streams_size * sizeof (void *));
      this->streams = n;
      if (a != &this->streams_default[0])
        free (a);
      a = n;
      this->streams_size += 32;
    }
    a[this->num_streams++] = s;
    a[this->num_streams] = NULL;
  } while (0);
  xine_rwlock_unlock (&this->streams_lock);
}

static int ao_streams_unregister (aos_t *this, xine_stream_t *s) {
  int n;
  xine_rwlock_wrlock (&this->streams_lock);
  if (!s) {
    this->num_null_streams--;
  } else if (s == XINE_ANON_STREAM) {
    this->num_anon_streams--;
  } else {
    xine_stream_t **a = this->streams;
    while (*a && (*a != s))
      a++;
    if (*a) {
      do {
        a[0] = a[1];
        a++;
      } while (*a);
      this->num_streams--;
    }
  }
  n = this->num_null_streams + this->num_anon_streams + this->num_streams;
  xine_rwlock_unlock (&this->streams_lock);
  return n;
}


/********************************************************************
 * reuse buffer stream refs.                                        *
 * be the current owner of buf when calling this.                   *
 *******************************************************************/

static int ao_reref (aos_t *this, audio_buffer_t *buf) {
  /* Paranoia? */
  if (PTR_IN_RANGE (buf, this->base_buf, NUM_AUDIO_BUFFERS * sizeof (*buf))) {
    xine_stream_t **s = this->buf_streams + (buf - this->base_buf);
    if (buf->stream != *s) {
      if (*s)
        _x_refcounter_dec ((*s)->refcounter);
      if (buf->stream)
        _x_refcounter_inc (buf->stream->refcounter);
      *s = buf->stream;
      return 1;
    }
  }
  return 0;
}

static int ao_unref_buf (aos_t *this, audio_buffer_t *buf) {
  /* Paranoia? */
  if (PTR_IN_RANGE (buf, this->base_buf, NUM_AUDIO_BUFFERS * sizeof (*buf))) {
    xine_stream_t **s = this->buf_streams + (buf - this->base_buf);
    buf->stream = NULL;
    if (*s) {
      _x_refcounter_dec ((*s)->refcounter);
      *s = NULL;
      return 1;
    }
  }
  return 0;
}

static void ao_unref_all (aos_t *this) {
  audio_buffer_t *buf;
  int n = 0;
  pthread_mutex_lock (&this->free_fifo.mutex);
  for (buf = this->free_fifo.first; buf; buf = buf->next)
    n += ao_unref_buf (this, buf);
  if (n && (this->free_fifo.num_buffers == NUM_AUDIO_BUFFERS))
    xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: unreferenced stream.\n");
  pthread_mutex_unlock (&this->free_fifo.mutex);
}

static void ao_force_unref_all (aos_t *this) {
  audio_buffer_t *buf;
  int a = 0, n = 0;
  pthread_mutex_lock (&this->out_fifo.mutex);
  for (buf = this->out_fifo.first; buf; buf = buf->next) {
    n += ao_unref_buf (this, buf);
    a++;
  }
  pthread_mutex_unlock (&this->out_fifo.mutex);
  pthread_mutex_lock (&this->free_fifo.mutex);
  for (buf = this->free_fifo.first; buf; buf = buf->next) {
    n += ao_unref_buf (this, buf);
    a++;
  }
  pthread_mutex_unlock (&this->free_fifo.mutex);
  if (n && (a == NUM_AUDIO_BUFFERS))
    xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: unreferenced stream.\n");
}

/********************************************************************
 * frame queue (fifo)                                               *
 *******************************************************************/

static void ao_fifo_open (audio_fifo_t *fifo) {
#ifndef HAVE_ZERO_SAFE_MEM
  fifo->first           = NULL;
  fifo->num_buffers     = 0;
  fifo->num_buffers_max = 0;
  fifo->num_waiters     = 0;
#endif
  fifo->add             = &fifo->first;
  pthread_mutex_init (&fifo->mutex, NULL);
  pthread_cond_init  (&fifo->not_empty, NULL);
  pthread_cond_init  (&fifo->empty, NULL);
}

static void ao_fifo_close (audio_fifo_t *fifo) {
#if 0 /* not yet needed */
  fifo->first           = NULL;
  fifo->add             = &fifo->first;
  fifo->num_buffers     = 0;
  fifo->num_buffers_max = 0;
  fifo->num_waiters     = 0;
#endif
  pthread_mutex_destroy (&fifo->mutex);
  pthread_cond_destroy  (&fifo->not_empty);
  pthread_cond_destroy  (&fifo->empty);
}

static void ao_fifo_append_int (audio_fifo_t *fifo, audio_buffer_t *buf) {

  _x_assert (!buf->next);

  fifo->num_buffers = (fifo->first ? fifo->num_buffers : 0) + 1;
  *(fifo->add)      = buf;
  fifo->add         = &buf->next;
  
  if (fifo->num_buffers_max < fifo->num_buffers)
    fifo->num_buffers_max = fifo->num_buffers;
}

static void ao_fifo_append (audio_fifo_t *fifo, audio_buffer_t *buf) {
  pthread_mutex_lock (&fifo->mutex);
  ao_fifo_append_int (fifo, buf);
  if (fifo->num_waiters)
    pthread_cond_signal (&fifo->not_empty);
  pthread_mutex_unlock (&fifo->mutex);
}

static void ao_free_fifo_append (aos_t *this, audio_buffer_t *buf) {
  pthread_mutex_lock (&this->free_fifo.mutex);
  ao_fifo_append_int (&this->free_fifo, buf);
  if (this->free_fifo.num_waiters)
    pthread_cond_signal (&this->free_fifo.not_empty);
  if (!this->num_streams) {
    if (ao_unref_buf (this, buf) && (this->free_fifo.num_buffers == NUM_AUDIO_BUFFERS))
      xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: unreferenced stream.\n");
  }
  pthread_mutex_unlock (&this->free_fifo.mutex);
}

static audio_buffer_t *ao_fifo_pop_int (audio_fifo_t *fifo) {
  audio_buffer_t *buf;
  buf         = fifo->first;
  fifo->first = buf->next;
  buf->next   = NULL;
  fifo->num_buffers--;
  if (!fifo->first) {
    fifo->add         = &fifo->first;
    fifo->num_buffers = 0;
  }
  return buf;
}

static audio_buffer_t *ao_out_fifo_get (aos_t *this, audio_buffer_t *buf) {

  pthread_mutex_lock (&this->out_fifo.mutex);
  while (1) {

    if (this->flush_audio_driver) {
      this->ao.control (&this->ao, AO_CTRL_FLUSH_BUFFERS, NULL);
      this->flush_audio_driver--;
      xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: flushed driver.\n");
    }

    if (this->discard_buffers) {
      audio_buffer_t *list = NULL, **add = &list;
      int n = 0;
      if (buf) {
        *add = buf;
        add = &buf->next;
        n++;
      } else if (this->out_fifo.first) {
        n += this->out_fifo.num_buffers;
        *add = this->out_fifo.first;
        add = this->out_fifo.add;
        this->out_fifo.first = NULL;
        this->out_fifo.add = &this->out_fifo.first;
        this->out_fifo.num_buffers = 0;
      }
      if (n) {
        pthread_mutex_lock (&this->free_fifo.mutex);
        this->free_fifo.num_buffers = n + (this->free_fifo.first ? this->free_fifo.num_buffers : 0);
        *(this->free_fifo.add) = list;
        this->free_fifo.add = add;
        if (this->free_fifo.num_waiters)
          pthread_cond_broadcast (&this->free_fifo.not_empty);
        pthread_mutex_unlock (&this->free_fifo.mutex);
      }
      pthread_cond_broadcast (&this->out_fifo.empty);
      buf = NULL;
      xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: flushed out %d buffers.\n", n);
    }

    if (buf || !this->audio_loop_running) {
      pthread_mutex_unlock (&this->out_fifo.mutex);
      return buf;
    }

    buf = this->out_fifo.first;
    if (buf) {
      this->out_fifo.first = buf->next;
      buf->next            = NULL;
      if (this->out_fifo.first) {
        this->out_fifo.num_buffers--;
      } else {
        this->out_fifo.add = &this->out_fifo.first;
        this->out_fifo.num_buffers = 0;
      }
      pthread_mutex_unlock (&this->out_fifo.mutex);
      return buf;
    }

    this->out_fifo.num_waiters++;
    pthread_cond_wait (&this->out_fifo.not_empty, &this->out_fifo.mutex);
    this->out_fifo.num_waiters--;
  }
}

static void ao_ticket_revoked (void *user_data, int flags) {
  aos_t *this = (aos_t *)user_data;
  const char *s1 = (flags & XINE_TICKET_FLAG_ATOMIC) ? " atomic" : "";
  const char *s2 = (flags & XINE_TICKET_FLAG_REWIRE) ? " port_rewire" : "";
  pthread_cond_signal (&this->free_fifo.not_empty);
  xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: port ticket revoked%s%s.\n", s1, s2);
}

static audio_buffer_t *ao_free_fifo_get (aos_t *this) {
  audio_buffer_t *buf;

  pthread_mutex_lock (&this->free_fifo.mutex);
  while (!(buf = this->free_fifo.first)) {
    if (this->xine->port_ticket->ticket_revoked) {
      pthread_mutex_unlock (&this->free_fifo.mutex);
      this->xine->port_ticket->renew (this->xine->port_ticket, 1);
      if (!(this->xine->port_ticket->ticket_revoked & XINE_TICKET_FLAG_REWIRE)) {
        pthread_mutex_lock (&this->free_fifo.mutex);
        continue;
      }
      /* O dear. Port rewiring ahead. Try unblock. */
      if (this->clock->speed == XINE_SPEED_PAUSE) {
        pthread_mutex_lock (&this->out_fifo.mutex);
        if (this->out_fifo.first) {
          buf = ao_fifo_pop_int (&this->out_fifo);
          pthread_mutex_unlock (&this->out_fifo.mutex);
          xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: try unblocking decoder.\n");
          return buf;
        }
        pthread_mutex_unlock (&this->out_fifo.mutex);
      }
      pthread_mutex_lock (&this->free_fifo.mutex);
    }
    {
      struct timespec ts = {0, 0};
      xine_gettime (&ts);
      ts.tv_sec += 1;
      this->free_fifo.num_waiters++;
      pthread_cond_timedwait (&this->free_fifo.not_empty, &this->free_fifo.mutex, &ts);
      this->free_fifo.num_waiters--;
    }
  }

  this->free_fifo.first = buf->next;
  buf->next = NULL;
  this->free_fifo.num_buffers--;
  if (!this->free_fifo.first) {
    this->free_fifo.add = &this->free_fifo.first;
    this->free_fifo.num_buffers = 0;
  }
  pthread_mutex_unlock (&this->free_fifo.mutex);
  return buf;
}

/* This function is currently not needed */
#if 0
static int ao_fifo_num_buffers (audio_fifo_t *fifo) {

  int ret;

  pthread_mutex_lock (&fifo->mutex);
  ret = fifo->num_buffers;
  pthread_mutex_unlock (&fifo->mutex);

  return ret;
}
#endif

static void ao_out_fifo_manual_flush (aos_t *this) {
  pthread_mutex_lock (&this->out_fifo.mutex);
  if (this->out_fifo.first) {
    audio_buffer_t *list = NULL, **add = &list;
    int n = this->out_fifo.num_buffers;
    *add = this->out_fifo.first;
    add = this->out_fifo.add;
    this->out_fifo.first = NULL;
    this->out_fifo.add = &this->out_fifo.first;
    this->out_fifo.num_buffers = 0;
    pthread_mutex_lock (&this->free_fifo.mutex);
    this->free_fifo.num_buffers = n + (this->free_fifo.first ? this->free_fifo.num_buffers : 0);
    *(this->free_fifo.add) = list;
    this->free_fifo.add = add;
    pthread_mutex_unlock (&this->free_fifo.mutex);
  }
  if (this->free_fifo.first && this->free_fifo.num_waiters)
    pthread_cond_broadcast (&this->free_fifo.not_empty);
  pthread_mutex_unlock (&this->out_fifo.mutex);
}

static void ao_out_fifo_loop_flush (aos_t *this) {
  pthread_mutex_lock (&this->out_fifo.mutex);
  this->discard_buffers++;
  while (this->out_fifo.first) {
    /* i think it's strange to send not_empty signal here (beside the enqueue
     * function), but it should do no harm. [MF] */
    if (this->out_fifo.num_waiters)
      pthread_cond_signal (&this->out_fifo.not_empty);
    pthread_cond_wait (&this->out_fifo.empty, &this->out_fifo.mutex);
  }
  this->discard_buffers--;
  pthread_mutex_unlock (&this->out_fifo.mutex);
}


static void ao_fill_gap (aos_t *this, int64_t pts_len) {
  static const uint16_t a52_pause_head[4] = {
    0xf872,
    0x4e1f,
    /* Audio ES Channel empty, wait for DD Decoder or pause */
    0x0003,
    0x0020
  };
  int64_t num_frames = (pts_len * this->frames_per_kpts) >> 10;

  xprintf (this->xine, XINE_VERBOSITY_DEBUG,
           "audio_out: inserting %" PRId64 " 0-frames to fill a gap of %" PRId64 " pts\n", num_frames, pts_len);

  if ((this->output.mode == AO_CAP_MODE_A52) || (this->output.mode == AO_CAP_MODE_AC5)) {

    memcpy (this->zero_space, a52_pause_head, sizeof (a52_pause_head));
    while (num_frames > 1536) {
      pthread_mutex_lock (&this->driver_lock);
      if (this->driver_open)
        this->driver->write (this->driver, this->zero_space, 1536);
      pthread_mutex_unlock (&this->driver_lock);
      num_frames -= 1536;
    }

  } else {

    int max_frames = _x_ao_mode2channels (this->output.mode) * (this->output.bits >> 3);
    max_frames = max_frames ? AUDIO_BUF_SIZE / max_frames : 4096;
    memset (this->zero_space, 0, sizeof (a52_pause_head));
    while ((num_frames >= max_frames) && !this->discard_buffers) {
      pthread_mutex_lock (&this->driver_lock);
      if (this->driver_open)
        this->driver->write (this->driver, this->zero_space, max_frames);
      pthread_mutex_unlock (&this->driver_lock);
      num_frames -= max_frames;
    }
    if (num_frames && !this->discard_buffers) {
      pthread_mutex_lock (&this->driver_lock);
      if (this->driver_open)
        this->driver->write (this->driver, this->zero_space, num_frames);
      pthread_mutex_unlock (&this->driver_lock);
    }

  }
}

static void ensure_buffer_size (audio_buffer_t *buf, int bytes_per_frame,
                                int frames)
{
  int size = bytes_per_frame * frames;

  if (buf->mem_size < size) {
    buf->mem = realloc( buf->mem, size );
    buf->mem_size = size;
  }
  buf->num_frames = frames;
}

static audio_buffer_t * swap_frame_buffers ( aos_t *this ) {
  audio_buffer_t *tmp;

  tmp = this->frame_buf[1];
  this->frame_buf[1] = this->frame_buf[0];
  this->frame_buf[0] = tmp;
  return this->frame_buf[0];
}

int _x_ao_mode2channels( int mode ) {
  switch( mode ) {
  case AO_CAP_MODE_MONO:
    return 1;
  case AO_CAP_MODE_STEREO:
    return 2;
  case AO_CAP_MODE_4CHANNEL:
    return 4;
  case AO_CAP_MODE_4_1CHANNEL:
  case AO_CAP_MODE_5CHANNEL:
  case AO_CAP_MODE_5_1CHANNEL:
    return 6;
  }
  return 0;
}

int _x_ao_channels2mode( int channels ) {

  switch( channels ) {
    case 1:
      return AO_CAP_MODE_MONO;
    case 2:
      return AO_CAP_MODE_STEREO;
    case 3:
    case 4:
      return AO_CAP_MODE_4CHANNEL;
    case 5:
      return AO_CAP_MODE_5CHANNEL;
    case 6:
      return AO_CAP_MODE_5_1CHANNEL;
  }
  return AO_CAP_NOCAP;
}

static void audio_filter_compress (aos_t *this, int16_t *mem, int num_frames) {

  int    i, maxs;
  double f_max;
  int    num_channels;

  num_channels = _x_ao_mode2channels (this->input.mode);
  if (!num_channels)
    return;

  maxs = 0;

  /* measure */

  for (i=0; i<num_frames*num_channels; i++) {
    int16_t sample = abs(mem[i]);
    if (sample>maxs)
      maxs = sample;
  }

  /* calc maximum possible & allowed factor */

  if (maxs>0) {
    f_max = 32767.0 / maxs;
    this->compression_factor = this->compression_factor * 0.999 + f_max * 0.001;
    if (this->compression_factor > f_max)
      this->compression_factor = f_max;

    if (this->compression_factor > this->compression_factor_max)
      this->compression_factor = this->compression_factor_max;
  } else
    f_max = 1.0;

  lprintf ("max=%d f_max=%f compression_factor=%f\n", maxs, f_max, this->compression_factor);

  /* apply it */

  for (i=0; i<num_frames*num_channels; i++) {
    /* 0.98 to avoid overflow */
    mem[i] = mem[i] * 0.98 * this->compression_factor * this->amp_factor;
  }
}

static void audio_filter_amp (aos_t *this, void *buf, int num_frames) {
  double amp_factor;
  int    i;
  const int total_frames = num_frames * _x_ao_mode2channels (this->input.mode);

  if (!total_frames)
    return;

  amp_factor=this->amp_factor;
  if (this->amp_mute || amp_factor == 0) {
    memset (buf, 0, total_frames * (this->input.bits / 8));
    return;
  }

  if (this->input.bits == 8) {
    int16_t test;
    int8_t *mem = (int8_t *) buf;

    for (i=0; i<total_frames; i++) {
      test = mem[i] * amp_factor;
      /* Force limit on amp_factor to prevent clipping */
      if (test < INT8_MIN) {
        this->amp_factor = amp_factor = amp_factor * INT8_MIN / test;
	test=INT8_MIN;
      }
      if (test > INT8_MAX) {
        this->amp_factor = amp_factor = amp_factor * INT8_MIN / test;
	test=INT8_MAX;
      }
      mem[i] = test;
    }
  } else if (this->input.bits == 16) {
    int32_t test;
    int16_t *mem = (int16_t *) buf;

    for (i=0; i<total_frames; i++) {
      test = mem[i] * amp_factor;
      /* Force limit on amp_factor to prevent clipping */
      if (test < INT16_MIN) {
        this->amp_factor = amp_factor = amp_factor * INT16_MIN / test;
	test=INT16_MIN;
      }
      if (test > INT16_MAX) {
        this->amp_factor = amp_factor = amp_factor * INT16_MIN / test;
	test=INT16_MAX;
      }
      mem[i] = test;
    }
  }
}

static void ao_eq_update (aos_t *this) {
  /* TJ. gxine assumes a setting range of 0..100, with 100 being the default.
     Lets try to fix that very broken api like this:
     1. If all settings are the same, disable eq.
     2. A setting step of 1 means 0.5 dB relative.
     3. The highest setting refers to 0 dB absolute. */
  int smin, smax, i;
  smin = smax = this->eq_settings[0];
  for (i = 1; i < EQ_BANDS; i++) {
    if (this->eq_settings[i] < smin)
      smin = this->eq_settings[i];
    else if (this->eq_settings[i] > smax)
      smax = this->eq_settings[i];
  }
  if (smin == smax) {
    this->do_equ = 0;
  } else {
    for (i = 0; i < EQ_BANDS; i++) {
      uint32_t setting = smax - this->eq_settings[i];
      if (setting > 99) {
        this->eq_gain[i] = EQ_REAL (0.0);
      } else {
        static const int mant[12] = {
          EQ_REAL (1.0),        EQ_REAL (0.94387431), EQ_REAL (0.89089872),
          EQ_REAL (0.84089642), EQ_REAL (0.79370053), EQ_REAL (0.74915354),
          EQ_REAL (0.70710678), EQ_REAL (0.66741993), EQ_REAL (0.62996052),
          EQ_REAL (0.59460355), EQ_REAL (0.56123102), EQ_REAL (0.52973155)
        };
        uint32_t exp = setting / 12;
        setting = setting % 12;
        this->eq_gain[i] = mant[setting] >> exp;
      }
    }
    /* Not very precise but better than nothing... */
    if (this->input.rate < 15000) {
      for (i = EQ_BANDS - 1; i > 1; i--)
        this->eq_gain[i] = this->eq_gain[i - 2];
      this->eq_gain[1] = this->eq_gain[0] = EQ_REAL (1.0);
    } else if (this->input.rate < 30000) {
      for (i = EQ_BANDS - 1; i > 0; i--)
        this->eq_gain[i] = this->eq_gain[i - 1];
      this->eq_gain[0] = EQ_REAL (1.0);
    } else if (this->input.rate > 60000) {
      for (i = 0; i < EQ_BANDS - 1; i++)
        this->eq_gain[i] = this->eq_gain[i + 1];
      this->eq_gain[EQ_BANDS - 1] = EQ_REAL (1.0);
    }
    this->do_equ = 1;
  }
}

#define sat16(v) (((v + 0x8000) & ~0xffff) ? ((v) >> 31) ^ 0x7fff : (v))

static void audio_filter_equalize (aos_t *this,
				   int16_t *data, int num_frames) {
  int       index, band, channel;
  int       length;
  int       num_channels;

  num_channels = _x_ao_mode2channels (this->input.mode);
  if (!num_channels)
    return;

  length = num_frames * num_channels;

  for (index = 0; index < length; index += num_channels) {

    for (channel = 0; channel < num_channels; channel++) {

      /* Convert the PCM sample to a fixed fraction */
      int scaledpcm = ((int)data[index + channel]) << (FP_FRBITS - 16);
      int out = 0;
      /*  For each band */
      for (band = 0; band < EQ_BANDS; band++) {
        int64_t l;
        int v;
        int *p = &this->eq_data_history[channel][band][0];
        l = (int64_t)iir_cf[band].alpha * (scaledpcm - p[1])
          + (int64_t)iir_cf[band].gamma * p[2]
          - (int64_t)iir_cf[band].beta  * p[3];
        p[1] = p[0]; p[0] = scaledpcm;
        p[3] = p[2]; p[2] = v = (int)(l >> FP_FRBITS);
        l = (int64_t)v * this->eq_gain[band];
        out += (int)(l >> FP_FRBITS);
      }
      /* Adjust the fixed point fraction value to a PCM sample */
      /* Scale back to a 16bit signed int */
      out >>= (FP_FRBITS - 16);
      /* Limit the output */
      data[index+channel] = sat16 (out);
    }
  }

}

static audio_buffer_t* prepare_samples( aos_t *this, audio_buffer_t *buf) {
  double          acc_output_frames;
  int             num_output_frames ;

  /*
   * volume / compressor / equalizer filter
   */

  if (this->amp_factor == 0) {
    if (this->do_amp)
      audio_filter_amp (this, buf->mem, buf->num_frames);
  } else if (this->input.bits == 16) {
    if (this->do_equ)
      audio_filter_equalize (this, buf->mem, buf->num_frames);
    if (this->do_compress)
      audio_filter_compress (this, buf->mem, buf->num_frames);
    if (this->do_amp)
      audio_filter_amp (this, buf->mem, buf->num_frames);
  } else if (this->input.bits == 8) {
    if (this->do_amp)
      audio_filter_amp (this, buf->mem, buf->num_frames);
  }


  /*
   * resample and output audio data
   */

  /* calculate number of output frames (after resampling) */
  acc_output_frames = (double) buf->num_frames * this->frame_rate_factor
    * this->resample_sync_factor + this->output_frame_excess;

  /* Truncate to an integer */
  num_output_frames = acc_output_frames;

  /* Keep track of the amount truncated */
  this->output_frame_excess = acc_output_frames - (double) num_output_frames;
  if ( this->output_frame_excess != 0 &&
       !this->do_resample && !this->resample_sync_method)
    this->output_frame_excess = 0;

  lprintf ("outputting %d frames\n", num_output_frames);

  /* convert 8 bit samples as needed */
  if ( this->input.bits == 8 &&
       (this->resample_sync_method || this->do_resample ||
        this->output.bits != 8 || this->input.mode != this->output.mode) ) {
    int channels = _x_ao_mode2channels(this->input.mode);
    ensure_buffer_size(this->frame_buf[1], 2*channels, buf->num_frames );
    _x_audio_out_resample_8to16((int8_t *)buf->mem, this->frame_buf[1]->mem,
                                channels * buf->num_frames );
    buf = swap_frame_buffers(this);
  }

  /* check if resampling may be skipped */
  if ( (this->resample_sync_method || this->do_resample) &&
       buf->num_frames != num_output_frames ) {
    switch (this->input.mode) {
    case AO_CAP_MODE_MONO:
      ensure_buffer_size(this->frame_buf[1], (this->output.bits>>3), num_output_frames);
      _x_audio_out_resample_mono (this->last_sample, buf->mem, buf->num_frames,
			       this->frame_buf[1]->mem, num_output_frames);
      buf = swap_frame_buffers(this);
      break;
    case AO_CAP_MODE_STEREO:
      ensure_buffer_size(this->frame_buf[1], (this->output.bits>>3)*2, num_output_frames);
      _x_audio_out_resample_stereo (this->last_sample, buf->mem, buf->num_frames,
				 this->frame_buf[1]->mem, num_output_frames);
      buf = swap_frame_buffers(this);
      break;
    case AO_CAP_MODE_4CHANNEL:
      ensure_buffer_size(this->frame_buf[1], (this->output.bits>>3)*4, num_output_frames);
      _x_audio_out_resample_4channel (this->last_sample, buf->mem, buf->num_frames,
				   this->frame_buf[1]->mem, num_output_frames);
      buf = swap_frame_buffers(this);
      break;
    case AO_CAP_MODE_4_1CHANNEL:
    case AO_CAP_MODE_5CHANNEL:
    case AO_CAP_MODE_5_1CHANNEL:
      ensure_buffer_size(this->frame_buf[1], (this->output.bits>>3)*6, num_output_frames);
      _x_audio_out_resample_6channel (this->last_sample, buf->mem, buf->num_frames,
				   this->frame_buf[1]->mem, num_output_frames);
      buf = swap_frame_buffers(this);
      break;
    case AO_CAP_MODE_A52:
    case AO_CAP_MODE_AC5:
      /* pass-through modes: no resampling */
      break;
    }
  } else {
    /* maintain last_sample in case we need it */
    switch (this->input.mode) {
    case AO_CAP_MODE_MONO:
      memcpy (this->last_sample, &buf->mem[buf->num_frames - 1], sizeof (this->last_sample[0]));
      break;
    case AO_CAP_MODE_STEREO:
      memcpy (this->last_sample, &buf->mem[(buf->num_frames - 1) * 2], 2 * sizeof (this->last_sample[0]));
      break;
    case AO_CAP_MODE_4CHANNEL:
      memcpy (this->last_sample, &buf->mem[(buf->num_frames - 1) * 4], 4 * sizeof (this->last_sample[0]));
      break;
    case AO_CAP_MODE_4_1CHANNEL:
    case AO_CAP_MODE_5CHANNEL:
    case AO_CAP_MODE_5_1CHANNEL:
      memcpy (this->last_sample, &buf->mem[(buf->num_frames - 1) * 6], 6 * sizeof (this->last_sample[0]));
      break;
    default:;
    }
  }

  /* mode conversion */
  if ( this->input.mode != this->output.mode ) {
    switch (this->input.mode) {
    case AO_CAP_MODE_MONO:
      if( this->output.mode == AO_CAP_MODE_STEREO ) {
	ensure_buffer_size(this->frame_buf[1], (this->output.bits>>3)*2, buf->num_frames );
	_x_audio_out_resample_monotostereo(buf->mem, this->frame_buf[1]->mem,
					   buf->num_frames );
	buf = swap_frame_buffers(this);
      }
      break;
    case AO_CAP_MODE_STEREO:
      if( this->output.mode == AO_CAP_MODE_MONO ) {
	ensure_buffer_size(this->frame_buf[1], (this->output.bits>>3), buf->num_frames );
	_x_audio_out_resample_stereotomono(buf->mem, this->frame_buf[1]->mem,
					   buf->num_frames );
	buf = swap_frame_buffers(this);
      }
      break;
    case AO_CAP_MODE_4CHANNEL:
      break;
    case AO_CAP_MODE_5CHANNEL:
      break;
    case AO_CAP_MODE_5_1CHANNEL:
      break;
    case AO_CAP_MODE_A52:
    case AO_CAP_MODE_AC5:
      break;
    }
  }

  /* convert back to 8 bits after resampling */
  if( this->output.bits == 8 &&
        (this->resample_sync_method || this->do_resample ||
         this->input.mode != this->output.mode) ) {
    int channels = _x_ao_mode2channels(this->output.mode);
    ensure_buffer_size(this->frame_buf[1], channels, buf->num_frames );
    _x_audio_out_resample_16to8(buf->mem, (int8_t *)this->frame_buf[1]->mem,
                                channels * buf->num_frames );
    buf = swap_frame_buffers(this);
  }
  return buf;
}


static int resample_rate_adjust(aos_t *this, int64_t gap, audio_buffer_t *buf) {

  /* Calculates the drift factor used to resample the audio data to
   * keep in sync with system (or dxr3) clock.
   *
   * To compensate the sound card drift it is necessary to know, how many audio
   * frames need to be added (or removed) via resampling. This function waits for
   * RESAMPLE_SYNC_WINDOW audio buffers to be sent to the card and keeps track
   * of their total duration in vpts. With the measured gap difference between
   * the reported gap values at the beginning and at the end of this window the
   * required resampling factor is calculated:
   *
   * resample_factor = (duration + gap_difference) / duration
   *
   * This factor is then used in prepare_samples() to resample the audio
   * buffers as needed so we keep in sync with the system (or dxr3) clock.
   */

  resample_sync_t *info = &this->resample_sync_info;
  int64_t avg_gap = 0;
  double factor;
  double diff;
  double duration;
  int i;

  if (llabs(gap) > AO_MAX_GAP) {
    /* drop buffers or insert 0-frames in audio out loop */
    info->valid = 0;
    return -1;
  }

  if ( ! info->valid) {
    this->resample_sync_factor = 1.0;
    info->window = 0;
    info->reduce_gap = 0;
    info->last_avg_gap = gap;
    info->last_factor = 0;
    info->window_duration = info->last_vpts = 0;
    info->valid = 1;
  }

  /* calc average gap (to compensate small errors during measurement) */
  for (i = 0; i < 7; i++) info->recent_gap[i] = info->recent_gap[i + 1];
  info->recent_gap[i] = gap;
  for (i = 0; i < 8; i++) avg_gap += info->recent_gap[i];
  avg_gap /= 8;


  /* gap too big? Change sample rate so that gap converges towards 0. */

  if (llabs(avg_gap) > RESAMPLE_REDUCE_GAP_THRESHOLD && !info->reduce_gap) {
    info->reduce_gap = 1;
    this->resample_sync_factor = (avg_gap < 0) ? 0.995 : 1.005;

    llprintf (LOG_RESAMPLE_SYNC,
              "sample rate adjusted to reduce gap: gap=%" PRId64 "\n", avg_gap);
    return 0;

  } else if (info->reduce_gap && llabs(avg_gap) < 50) {
    info->reduce_gap = 0;
    info->valid = 0;
    llprintf (LOG_RESAMPLE_SYNC, "gap successfully reduced\n");
    return 0;

  } else if (info->reduce_gap) {
    /* re-check, because the gap might suddenly change its sign,
     * also slow down, when getting close to zero (-300<gap<300) */
    if (llabs(avg_gap) > 300)
      this->resample_sync_factor = (avg_gap < 0) ? 0.995 : 1.005;
    else
      this->resample_sync_factor = (avg_gap < 0) ? 0.998 : 1.002;
    return 0;
  }


  if (info->window > RESAMPLE_SYNC_WINDOW) {

    /* adjust drift correction */

    int64_t gap_diff = avg_gap - info->last_avg_gap;

    if (gap_diff < RESAMPLE_MAX_GAP_DIFF) {
#if LOG_RESAMPLE_SYNC
      int num_frames;

      /* if we are already resampling to a different output rate, consider
       * this during calculation */
      num_frames = (this->do_resample) ? (buf->num_frames * this->frame_rate_factor)
        : buf->num_frames;
      printf("audio_out: gap=%5" PRId64 ";  gap_diff=%5" PRId64 ";  frame_diff=%3.0f;  drift_factor=%f\n",
             avg_gap, gap_diff, num_frames * info->window * info->last_factor,
             this->resample_sync_factor);
#endif
      /* we want to add factor * num_frames to each buffer */
      diff = gap_diff;
#if _MSCVER <= 1200
      /* ugly hack needed by old Visual C++ 6.0 */
      duration = (int64_t)info->window_duration;
#else
      duration = info->window_duration;
#endif
      factor = diff / duration + info->last_factor;

      info->last_factor = factor;
      this->resample_sync_factor = 1.0 + factor;

      info->last_avg_gap = avg_gap;
      info->window_duration = 0;
      info->window = 0;
    } else
      info->valid = 0;

  } else {

    /* collect data for next adjustment */
    if (info->window > 0)
      info->window_duration += buf->vpts - info->last_vpts;
    info->last_vpts = buf->vpts;
    info->window++;
  }

  return 0;
}

static int ao_change_settings(aos_t *this, uint32_t bits, uint32_t rate, int mode);

/* Audio output loop: -
 * 1) Check for pause.
 * 2) Make sure audio hardware is in RUNNING state.
 * 3) Get delay
 * 4) Do drop, 0-fill or output samples.
 * 5) Go round loop again.
 */
static void *ao_loop (void *this_gen) {

  aos_t *this = (aos_t *) this_gen;
  audio_buffer_t *in_buf = NULL;
  int64_t         cur_time = -1;
  int64_t         next_sync_time = SYNC_TIME_INTERVAL;
  int             bufs_since_sync = 0;

  while (this->audio_loop_running || this->out_fifo.first) {

    xine_stream_t  *stream;
    int64_t         gap;
    int             delay;
    int             drop = 0;

    /* handle buf */
    do {
      /* get buffer to process for this loop iteration */
      {
        audio_buffer_t *last = in_buf;
        lprintf ("loop: get buf from fifo\n");
        in_buf = ao_out_fifo_get (this, in_buf);
        if (!in_buf)
          break;
        if (in_buf->num_frames <= 0) {
          /* drop empty buf */
          drop = 1;
          break;
        }
        stream = in_buf->stream;
        if (!last) {
          bufs_since_sync++;
          lprintf ("got a buffer\n");
          /* If there is no video stream to update extra info, queue this */
          if (stream) {
            if (!stream->video_decoder_plugin && !in_buf->extra_info->invalid) {
              int i = this->ei_write;
              this->base_ei[i] = in_buf->extra_info[0];
              this->ei_write = (i + 1) & (EI_RING_SIZE - 1);
            }
          }
        }
      }

      /* Paranoia? */
      {
        int new_speed = this->clock->speed;
        if (new_speed != (int)(this->current_speed))
        ao_set_property (&this->ao, AO_PROP_CLOCK_SPEED, new_speed);
      }

      /*
       * wait until user unpauses stream
       * if we are playing at a different speed (without slow_fast_audio flag)
       * we must process/free buffers otherwise the entire engine will stop.
       */
      pthread_mutex_lock (&this->current_speed_lock);
      if (this->audio_loop_running &&
          ((this->current_speed == XINE_SPEED_PAUSE) ||
          ((this->current_speed != XINE_FINE_SPEED_NORMAL) && !this->slow_fast_audio)))  {

        if ((this->current_speed != XINE_SPEED_PAUSE) || this->step) {
          cur_time = this->clock->get_current_time (this->clock);
          if (in_buf->vpts < cur_time) {
            pthread_mutex_unlock (&this->current_speed_lock);
            this->dropped++;
            drop = 1;
            break;
          }
          if (this->step) {
            pthread_mutex_lock (&this->step_mutex);
            this->step = 0;
            pthread_cond_broadcast (&this->done_stepping);
            pthread_mutex_unlock (&this->step_mutex);
            if (this->dropped)
              xprintf (this->xine, XINE_VERBOSITY_DEBUG,
                "audio_out: SINGLE_STEP: dropped %d buffers.\n", this->dropped);
          }
          this->dropped = 0;
          if ((in_buf->vpts - cur_time) > 2 * 90000)
            xprintf (this->xine, XINE_VERBOSITY_DEBUG,
              "audio_out: vpts/clock error, in_buf->vpts=%" PRId64 " cur_time=%" PRId64 "\n", in_buf->vpts, cur_time);
        }

        {
          extra_info_t *found = NULL;
          while (this->ei_read != this->ei_write) {
            extra_info_t *ei = &this->base_ei[this->ei_read];
            if (ei->vpts > cur_time)
              break;
            found = ei;
            this->ei_read = (this->ei_read + 1) & (EI_RING_SIZE - 1);
          }
          if (found && stream) {
            pthread_mutex_lock (&stream->current_extra_info_lock);
            _x_extra_info_merge (stream->current_extra_info, found);
            pthread_mutex_unlock (&stream->current_extra_info_lock);
          }
        }

        lprintf ("loop:pause: I feel sleepy (%d buffers).\n", this->out_fifo.num_buffers);
        pthread_mutex_unlock (&this->current_speed_lock);
        xine_usec_sleep (10000);
        lprintf ("loop:pause: I wake up.\n");
        continue;
      }
      /* end of pause mode */

      /* change driver's settings as needed */
      {
        int changed = in_buf->format.bits != this->input.bits
                   || in_buf->format.rate != this->input.rate
                   || in_buf->format.mode != this->input.mode;
        pthread_mutex_lock (&this->driver_lock);
        if (!this->driver_open || changed) {
          lprintf ("audio format has changed\n");
          if (stream && !stream->emergency_brake)
            ao_change_settings (this, in_buf->format.bits, in_buf->format.rate, in_buf->format.mode);
        }
        if (!this->driver_open) {
          xine_stream_t **s;
          pthread_mutex_unlock (&this->driver_lock);
          xprintf (this->xine, XINE_VERBOSITY_LOG,
            _("audio_out: delay calculation impossible with an unavailable audio device\n"));
          xine_rwlock_rdlock (&this->streams_lock);
          for (s = this->streams; *s; s++) {
            if (!(*s)->emergency_brake) {
              (*s)->emergency_brake = 1;
              _x_message (*s, XINE_MSG_AUDIO_OUT_UNAVAILABLE, NULL);
            }
          }
          xine_rwlock_unlock (&this->streams_lock);
          pthread_mutex_unlock (&this->current_speed_lock);
          drop = 1;
          break;
        }
      }

      /* buf timing pt 1 */
      delay = 0;
      while (this->audio_loop_running) {
        delay = this->driver->delay (this->driver);
        if (delay >= 0)
          break;
        /* Get the audio card into RUNNING state. */
        ao_fill_gap (this, 10000); /* FIXME, this PTS of 1000 should == period size */
      }
      cur_time = this->clock->get_current_time (this->clock);
      pthread_mutex_unlock (&this->driver_lock);
      if (!this->audio_loop_running)
        break;

      /* current_extra_info not set by video stream or getting too much out of date */
      {
        extra_info_t *found = NULL;
        while (this->ei_read != this->ei_write) {
          extra_info_t *ei = &this->base_ei[this->ei_read];
          if (ei->vpts > cur_time)
            break;
          found = ei;
          this->ei_read = (this->ei_read + 1) & (EI_RING_SIZE - 1);
        }
        if (stream) {
          if (!found && (cur_time - stream->current_extra_info->vpts) > 30000)
            found = in_buf->extra_info;
          if (found) {
            pthread_mutex_lock (&stream->current_extra_info_lock);
            _x_extra_info_merge (stream->current_extra_info, found);
            pthread_mutex_unlock (&stream->current_extra_info_lock);
          }
        }
      }

      /* buf timing pt 2: where, in the timeline is the "end" of the hardware audio buffer at the moment? */
      lprintf ("current delay is %d, current time is %" PRId64 "\n", delay, cur_time);
      /* no sound card should delay more than 23.301s ;-) */
      delay = ((uint32_t)delay * this->pts_per_kframe) >> 10;
      /* External A52 decoder delay correction (in pts) */
      delay += this->ptoffs;
      /* calculate gap: */
      gap = in_buf->vpts - cur_time - delay;
      this->last_gap = gap;
      lprintf ("now=%" PRId64 ", buffer_vpts=%" PRId64 ", gap=%" PRId64 "\n", cur_time, in_buf->vpts, gap);

      if (this->resample_sync_method) {
        /* Correct sound card drift via resampling. If gap is too big to
         * be corrected this way, we use the fallback: drop/insert frames.
         * This function only calculates the drift correction factor. The
         * actual resampling is done by prepare_samples().
         */
        resample_rate_adjust (this, gap, in_buf);
      } else {
        this->resample_sync_factor = 1.0;
      }

      /* output audio data synced to master clock */
      if (gap < (-1 * AO_MAX_GAP)) {

        /* drop late buf */
        this->last_sgap = 0;
        this->dropped++;
        drop = 1;

      } else if (gap > AO_MAX_GAP) {

        /* for big gaps output silence */
        this->last_sgap = 0;
        ao_fill_gap (this, gap);

      } else if ((abs ((int)gap) > this->gap_tolerance) &&
                 (cur_time > next_sync_time) &&
                 (bufs_since_sync >= SYNC_BUF_INTERVAL) &&
                 !this->resample_sync_method) {

        /* for small gaps ( tolerance < abs(gap) < AO_MAX_GAP )
         * feedback them into metronom's vpts_offset (when using
         * metronom feedback for A/V sync)
         */
        xine_stream_t **s;
        int sgap = (int)gap >> SYNC_GAP_RATE_LOG2;
        /* avoid asymptote trap of bringing down step with remaining gap */
        if (sgap < 0) {
          sgap = sgap <= this->last_sgap ? sgap
               : this->last_sgap < (int)gap ? (int)gap : this->last_sgap;
        } else {
          sgap = sgap >= this->last_sgap ? sgap
               : this->last_sgap > (int)gap ? (int)gap : this->last_sgap;
        }
        this->last_sgap = sgap != (int)gap ? sgap : 0;
        sgap = -sgap;
        lprintf ("audio_loop: ADJ_VPTS\n");
        xine_rwlock_rdlock (&this->streams_lock);
        for (s = this->streams; *s; s++)
          (*s)->metronom->set_option ((*s)->metronom, METRONOM_ADJ_VPTS_OFFSET, sgap);
        xine_rwlock_unlock (&this->streams_lock);
        next_sync_time = cur_time + SYNC_TIME_INTERVAL;
        bufs_since_sync = 0;

      } else {

        audio_buffer_t *out_buf;
        int result;

        if (this->dropped) {
          xprintf (this->xine, XINE_VERBOSITY_DEBUG,
            "audio_out: dropped %d late buffers.\n", this->dropped);
          this->dropped = 0;
        }
#if 0
        {
          int count;
          printf ("Audio data\n");
          for (count = 0; count < 10; count++)
            printf ("%x ", buf->mem[count]);
          printf ("\n");
        }
#endif
        out_buf = prepare_samples (this, in_buf);
#if 0
        {
          int count;
          printf ("Audio data2\n");
          for (count = 0; count < 10; count++)
            printf ("%x ", out_buf->mem[count]);
          printf ("\n");
        }
#endif
        lprintf ("loop: writing %d samples to sound device\n", out_buf->num_frames);
        if (this->driver_open) {
          pthread_mutex_lock (&this->driver_lock);
          result = this->driver_open ? this->driver->write (this->driver, out_buf->mem, out_buf->num_frames ) : 0;
          pthread_mutex_unlock (&this->driver_lock);
        } else {
          result = 0;
        }

        if (result < 0) {
          /* device unplugged. */
          xprintf (this->xine, XINE_VERBOSITY_LOG, _("write to sound card failed. Assuming the device was unplugged.\n"));
          if (stream)
            _x_message (in_buf->stream, XINE_MSG_AUDIO_OUT_UNAVAILABLE, NULL);
          pthread_mutex_lock (&this->driver_lock);
          if (this->driver_open)
            this->driver->close (this->driver);
          this->driver_open = 0;
          _x_free_audio_driver (this->xine, &this->driver);
          this->driver = _x_load_audio_output_plugin (this->xine, "none");
          if (this->driver && !in_buf->stream->emergency_brake &&
              ao_change_settings(this,
                in_buf->format.bits,
                in_buf->format.rate,
                in_buf->format.mode) == 0) {
            in_buf->stream->emergency_brake = 1;
            _x_message (in_buf->stream, XINE_MSG_AUDIO_OUT_UNAVAILABLE, NULL);
          }
          pthread_mutex_unlock (&this->driver_lock);
          /* closing the driver will result in XINE_MSG_AUDIO_OUT_UNAVAILABLE to be emitted */
        }
        drop = 1;
      }
      pthread_mutex_unlock (&this->current_speed_lock);
    } while (0);

    if (drop) {
      lprintf ("loop: next buf from fifo\n");
      ao_free_fifo_append (this, in_buf);
      in_buf = NULL;
    }

    /* Give other threads a chance to use functions which require this->driver_lock to
     * be available. This is needed when using NPTL on Linux (and probably PThreads
     * on Solaris as well). */
    if (this->num_driver_actions > 0) {
      /* calling sched_yield() is not sufficient on multicore systems */
      /* sched_yield(); */
      /* instead wait for the other thread to acquire this->driver_lock */
      pthread_mutex_lock(&this->driver_action_lock);
      if (this->num_driver_actions > 0)
        pthread_cond_wait(&this->driver_action_cond, &this->driver_action_lock);
      pthread_mutex_unlock(&this->driver_action_lock);
    }
  }

  if (in_buf)
    ao_free_fifo_append (this, in_buf);

  if (this->step) {
    pthread_mutex_lock (&this->step_mutex);
    this->step = 0;
    pthread_cond_broadcast (&this->done_stepping);
    pthread_mutex_unlock (&this->step_mutex);
  }

  return NULL;
}

/*
 * public a/v processing interface
 */

int xine_get_next_audio_frame (xine_audio_port_t *this_gen,
			       xine_audio_frame_t *frame) {

  aos_t          *this = (aos_t *) this_gen;
  audio_buffer_t *in_buf = NULL, *out_buf;
  struct timespec now = {0, 0};

  now.tv_nsec = 990000000;

  pthread_mutex_lock (&this->out_fifo.mutex);

  lprintf ("get_next_audio_frame\n");

  while (!this->out_fifo.first) {
    {
      xine_stream_t *stream = this->streams[0];
      if (stream && (stream->audio_fifo->fifo_size == 0)
        && (stream->demux_plugin->get_status(stream->demux_plugin) != DEMUX_OK)) {
        /* no further data can be expected here */
        pthread_mutex_unlock (&this->out_fifo.mutex);
        return 0;
      }
    }

    now.tv_nsec += 20000000;
    if (now.tv_nsec >= 1000000000) {
      xine_gettime (&now);
      now.tv_nsec += 20000000;
      if (now.tv_nsec >= 1000000000) {
        now.tv_sec++;
        now.tv_nsec -= 1000000000;
      }
    }
    {
      struct timespec ts = now;
      this->out_fifo.num_waiters++;
      pthread_cond_timedwait (&this->out_fifo.not_empty, &this->out_fifo.mutex, &ts);
      this->out_fifo.num_waiters--;
    }

  }

  in_buf = ao_fifo_pop_int (&this->out_fifo);
  pthread_mutex_unlock(&this->out_fifo.mutex);

  out_buf = prepare_samples (this, in_buf);

  if (out_buf != in_buf) {
    ao_free_fifo_append (this, in_buf);
    frame->xine_frame = NULL;
  } else
    frame->xine_frame    = out_buf;

  frame->vpts            = out_buf->vpts;
  frame->num_samples     = out_buf->num_frames;
  frame->sample_rate     = this->input.rate;
  frame->num_channels    = _x_ao_mode2channels (this->input.mode);
  frame->bits_per_sample = this->input.bits;
  frame->pos_stream      = out_buf->extra_info->input_normpos;
  frame->pos_time        = out_buf->extra_info->input_time;
  frame->data            = (uint8_t *) out_buf->mem;

  return 1;
}

void xine_free_audio_frame (xine_audio_port_t *this_gen, xine_audio_frame_t *frame) {

  aos_t          *this = (aos_t *) this_gen;
  audio_buffer_t *buf;

  buf = (audio_buffer_t *) frame->xine_frame;

  if (buf)
    ao_free_fifo_append (this, buf);
}

static int ao_update_resample_factor(aos_t *this) {
  unsigned int eff_input_rate;

  if( !this->driver_open )
    return 0;

  eff_input_rate = this->input.rate;
  switch (this->resample_conf) {
  case 1: /* force off */
    this->do_resample = 0;
    break;
  case 2: /* force on */
    this->do_resample = 1;
    break;
  default: /* AUTO */
    if ((this->current_speed != XINE_FINE_SPEED_NORMAL)
      && (this->current_speed != XINE_SPEED_PAUSE)
      && this->slow_fast_audio)
      eff_input_rate = (uint64_t)eff_input_rate * this->current_speed / XINE_FINE_SPEED_NORMAL;
    this->do_resample = eff_input_rate != this->output.rate;
  }

  if (this->do_resample)
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      "audio_out: will resample audio from %u to %d.\n", eff_input_rate, this->output.rate);

  if( !this->slow_fast_audio || this->current_speed == XINE_SPEED_PAUSE )
    this->frame_rate_factor = ((double)(this->output.rate)) / ((double)(this->input.rate));
  else
    this->frame_rate_factor = ( XINE_FINE_SPEED_NORMAL / (double)this->current_speed ) * ((double)(this->output.rate)) / ((double)(this->input.rate));
  this->frames_per_kpts = (this->output.rate * 1024 + 45000) / 90000;
  this->pts_per_kframe  = (90000 * 1024 + (this->output.rate >> 1)) / this->output.rate;
  this->audio_step = ((uint32_t)90000 * (uint32_t)32768) / this->input.rate;

  ao_eq_update (this);

  lprintf ("audio_step %" PRIu32 " pts per 32768 frames\n", this->audio_step);
  return this->output.rate;
}

static int ao_change_settings (aos_t *this, uint32_t bits, uint32_t rate, int mode) {
  int output_sample_rate;

  if (this->driver_open && !this->grab_only)
    this->driver->close (this->driver);
  this->driver_open = 0;

  this->input.mode = mode;
  this->input.rate = rate;
  this->input.bits = bits;

  if (!this->grab_only) {
    int caps = this->driver->get_capabilities (this->driver);
    /* not all drivers/cards support 8 bits */
    if ((this->input.bits == 8) && !(caps & AO_CAP_8BITS)) {
      bits = 16;
      xprintf (this->xine, XINE_VERBOSITY_LOG,
               _("8 bits not supported by driver, converting to 16 bits.\n"));
    }
    /* provide mono->stereo and stereo->mono conversions */
    if ((this->input.mode == AO_CAP_MODE_MONO) && !(caps & AO_CAP_MODE_MONO)) {
      mode = AO_CAP_MODE_STEREO;
      xprintf (this->xine, XINE_VERBOSITY_LOG,
               _("mono not supported by driver, converting to stereo.\n"));
    }
    if ((this->input.mode == AO_CAP_MODE_STEREO) && !(caps & AO_CAP_MODE_STEREO)) {
      mode = AO_CAP_MODE_MONO;
      xprintf (this->xine, XINE_VERBOSITY_LOG,
               _("stereo not supported by driver, converting to mono.\n"));
    }
    output_sample_rate = (this->driver->open)(this->driver, bits, this->force_rate ? this->force_rate : rate, mode);
  } else
    output_sample_rate = this->input.rate;

  if (output_sample_rate == 0) {
    xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: open failed!\n");
    return 0;
  }

  this->driver_open = 1;
  xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: output sample rate %d\n", output_sample_rate);

  this->last_audio_vpts = 0;
  this->output.mode     = mode;
  this->output.rate     = output_sample_rate;
  this->output.bits     = bits;

  this->ptoffs = (mode == AO_CAP_MODE_A52) || (mode == AO_CAP_MODE_AC5) ? this->passthrough_offset : 0;

  return ao_update_resample_factor (this);
}


static inline void ao_driver_lock (aos_t *this) {
  if (pthread_mutex_trylock (&this->driver_lock)) {
    this->dreqs_wait++;

    pthread_mutex_lock (&this->driver_action_lock);
    this->num_driver_actions++;
    pthread_mutex_unlock (&this->driver_action_lock);

    pthread_mutex_lock (&this->driver_lock);

    pthread_mutex_lock (&this->driver_action_lock);
    this->num_driver_actions--;
    /* indicate the change to ao_loop() */
    pthread_cond_broadcast (&this->driver_action_cond);
    pthread_mutex_unlock (&this->driver_action_lock);
  }
  this->dreqs_all++;
}


static inline void ao_driver_unlock (aos_t *this) {
  pthread_mutex_unlock (&this->driver_lock);
}


/*
 * open the audio device for writing to
 */

static int ao_open(xine_audio_port_t *this_gen, xine_stream_t *stream,
		   uint32_t bits, uint32_t rate, int mode) {

  aos_t *this = (aos_t *) this_gen;
  int channels;

  xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: ao_open (%p)\n", (void*)stream);

  if( !this->driver_open || bits != this->input.bits || rate != this->input.rate || mode != this->input.mode ) {
    int ret;

    if (this->audio_loop_running) {
      /* make sure there are no more buffers on queue */
      ao_out_fifo_loop_flush (this);
    }

    if( !stream->emergency_brake ) {
      pthread_mutex_lock( &this->driver_lock );
      ret = ao_change_settings(this, bits, rate, mode);
      pthread_mutex_unlock( &this->driver_lock );

      if( !ret ) {
        stream->emergency_brake = 1;
        _x_message (stream, XINE_MSG_AUDIO_OUT_UNAVAILABLE, NULL);
        return 0;
      }
    } else {
      return 0;
    }
  }

  /*
   * set metainfo
   */
  if (stream) {
    channels = _x_ao_mode2channels( mode );
    if( channels == 0 )
      channels = 255; /* unknown */

    /* faster than 4x _x_stream_info_set () */
    pthread_mutex_lock (&stream->info_mutex);
    stream->stream_info[XINE_STREAM_INFO_AUDIO_MODE]       = mode;
    stream->stream_info[XINE_STREAM_INFO_AUDIO_CHANNELS]   = channels;
    stream->stream_info[XINE_STREAM_INFO_AUDIO_BITS]       = bits;
    stream->stream_info[XINE_STREAM_INFO_AUDIO_SAMPLERATE] = rate;
    pthread_mutex_unlock (&stream->info_mutex);

    stream->metronom->set_audio_rate(stream->metronom, this->audio_step);
  }

  ao_streams_register (this, stream);

  return this->output.rate;
}

static audio_buffer_t *ao_get_buffer (xine_audio_port_t *this_gen) {

  aos_t *this = (aos_t *) this_gen;
  audio_buffer_t *buf;

  buf = ao_free_fifo_get (this);

  _x_extra_info_reset( buf->extra_info );
  buf->stream = NULL;

  return buf;
}

static void ao_put_buffer (xine_audio_port_t *this_gen,
                           audio_buffer_t *buf, xine_stream_t *stream) {

  aos_t *this = (aos_t *) this_gen;
  int64_t pts;

  if (this->discard_buffers || (buf->num_frames <= 0)) {
    ao_free_fifo_append (this, buf);
    return;
  }

  this->last_audio_vpts = pts = buf->vpts;

  /* handle anonymous streams like NULL for easy checking */
  if (stream == XINE_ANON_STREAM)
    stream = NULL;
  if (stream) {
    /* faster than 3x _x_stream_info_get () */
    pthread_mutex_lock (&stream->info_mutex);
    buf->format.bits = stream->stream_info[XINE_STREAM_INFO_AUDIO_BITS];
    buf->format.rate = stream->stream_info[XINE_STREAM_INFO_AUDIO_SAMPLERATE];
    buf->format.mode = stream->stream_info[XINE_STREAM_INFO_AUDIO_MODE];
    pthread_mutex_unlock (&stream->info_mutex);
    _x_extra_info_merge (buf->extra_info, stream->audio_decoder_extra_info);
    buf->vpts = stream->metronom->got_audio_samples (stream->metronom, pts, buf->num_frames);
  }
  buf->extra_info->vpts = buf->vpts;

  lprintf ("ao_put_buffer, pts=%" PRId64 ", vpts=%" PRId64 "\n", pts, buf->vpts);

  buf->stream = stream;
  ao_reref (this, buf);
  ao_fifo_append (&this->out_fifo, buf);

  lprintf ("ao_put_buffer done\n");
}

static void ao_close(xine_audio_port_t *this_gen, xine_stream_t *stream) {

  aos_t *this = (aos_t *) this_gen;
  int n;

  xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: ao_close (%p)\n", (void*)stream);

  /* unregister stream */
  n = ao_streams_unregister (this, stream);
  ao_unref_all (this);

  /* close driver if no streams left */
  if (!n && !this->grab_only && !stream->keep_ao_driver_open) {
    xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: no streams left, closing driver\n");

    if (this->audio_loop_running) {
      /* make sure there are no more buffers on queue */
      ao_out_fifo_loop_flush (this);
    }

    pthread_mutex_lock( &this->driver_lock );
    if(this->driver_open)
      this->driver->close(this->driver);
    this->driver_open = 0;
    pthread_mutex_unlock( &this->driver_lock );
  }
}

static void ao_exit(xine_audio_port_t *this_gen) {
  aos_t *this = (aos_t *) this_gen;
  int vol;
  int prop = 0;

  this->xine->port_ticket->revoke_cb_unregister (this->xine->port_ticket, ao_ticket_revoked, this);

  if (this->audio_loop_running) {
    void *p;

    this->audio_loop_running = 0;
    pthread_mutex_lock (&this->out_fifo.mutex);
    pthread_cond_signal (&this->out_fifo.not_empty);
    pthread_mutex_unlock (&this->out_fifo.mutex);

    pthread_join (this->audio_thread, &p);
  }

  if (!this->grab_only) {
    ao_driver_t *driver;

    pthread_mutex_lock( &this->driver_lock );

    driver = this->driver;

    if ((driver->get_capabilities(driver)) & AO_CAP_MIXER_VOL)
      prop = AO_PROP_MIXER_VOL;
    else if ((driver->get_capabilities(driver)) & AO_CAP_PCM_VOL)
      prop = AO_PROP_PCM_VOL;

    vol = driver->get_property(driver, prop);
    if (this->driver_open)
      driver->close(driver);

    this->driver_open = 0;
    this->driver = NULL;
    pthread_mutex_unlock( &this->driver_lock );

    this->xine->config->update_num(this->xine->config, "audio.volume.mixer_volume", vol);

    _x_free_audio_driver(this->xine, &driver);
  }

  if (this->dreqs_wait)
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      "audio_out: waited %d of %d external driver requests.\n", this->dreqs_wait, this->dreqs_all);

  /* We are about to free "this". No callback shall refer to it anymore, even if not our own. */
  this->xine->config->unregister_callbacks (this->xine->config, NULL, NULL, this, sizeof (*this));

  pthread_mutex_destroy(&this->driver_lock);
  pthread_cond_destroy(&this->driver_action_cond);
  pthread_mutex_destroy(&this->driver_action_lock);

  ao_streams_close (this);

  pthread_mutex_destroy(&this->current_speed_lock);

  pthread_mutex_destroy (&this->step_mutex);
  pthread_cond_destroy  (&this->done_stepping);

  ao_force_unref_all (this);
  ao_fifo_close (&this->free_fifo);
  ao_fifo_close (&this->out_fifo);

  _x_freep (&this->frame_buf[0]->mem);
  _x_freep (&this->frame_buf[1]->mem);
  xine_freep_aligned (&this->base_samp);

  free (this);
}

static uint32_t ao_get_capabilities (xine_audio_port_t *this_gen) {
  aos_t *this = (aos_t *) this_gen;
  uint32_t result;

  if (this->grab_only) {

    return AO_CAP_MODE_MONO | AO_CAP_MODE_STEREO ;
    /* FIXME: make configurable
      | AO_CAP_MODE_4CHANNEL | AO_CAP_MODE_5CHANNEL
      | AO_CAP_MODE_5_1CHANNEL | AO_CAP_8BITS;
    */
  } else {
    ao_driver_lock (this);
    result=this->driver->get_capabilities(this->driver);
    ao_driver_unlock (this);
  }
  return result;
}

static int ao_get_property (xine_audio_port_t *this_gen, int property) {
  aos_t *this = (aos_t *) this_gen;
  int ret;

  switch (property) {
  case XINE_PARAM_VO_SINGLE_STEP:
    ret = 0;
    break;

  case AO_PROP_COMPRESSOR:
    ret = this->compression_factor_max*100;
    break;

  case AO_PROP_BUFS_IN_FIFO:
    ret = this->audio_loop_running ? this->out_fifo.num_buffers : -1;
    break;

  case AO_PROP_BUFS_FREE:
    ret = this->audio_loop_running ? this->free_fifo.num_buffers : -1;
    break;

  case AO_PROP_BUFS_TOTAL:
    ret = this->audio_loop_running ? this->free_fifo.num_buffers_max : -1;
    break;

  case AO_PROP_NUM_STREAMS:
    xine_rwlock_rdlock (&this->streams_lock);
    ret = this->num_anon_streams + this->num_streams;
    xine_rwlock_unlock (&this->streams_lock);
    break;

  case AO_PROP_AMP:
    ret = this->amp_factor*100;
    break;

  case AO_PROP_AMP_MUTE:
    ret = this->amp_mute;
    break;

  case AO_PROP_EQ_30HZ:
  case AO_PROP_EQ_60HZ:
  case AO_PROP_EQ_125HZ:
  case AO_PROP_EQ_250HZ:
  case AO_PROP_EQ_500HZ:
  case AO_PROP_EQ_1000HZ:
  case AO_PROP_EQ_2000HZ:
  case AO_PROP_EQ_4000HZ:
  case AO_PROP_EQ_8000HZ:
  case AO_PROP_EQ_16000HZ:
    ret = this->eq_settings[property - AO_PROP_EQ_30HZ];
    break;

  case AO_PROP_DISCARD_BUFFERS:
    ret = this->discard_buffers;
    break;

  case AO_PROP_CLOCK_SPEED:
    ret = this->current_speed;
    break;

  case AO_PROP_DRIVER_DELAY:
    ret = this->last_gap;
    break;

  default:
    ao_driver_lock (this);
    ret = this->driver->get_property(this->driver, property);
    ao_driver_unlock (this);
  }
  return ret;
}

static int ao_set_property (xine_audio_port_t *this_gen, int property, int value) {
  aos_t *this = (aos_t *) this_gen;
  int ret = 0;

  switch (property) {
  /* not a typo :-) */
  case XINE_PARAM_VO_SINGLE_STEP:
    ret = !!value;
    if (this->grab_only)
      break;
    pthread_mutex_lock (&this->step_mutex);
    this->step = ret;
    if (ret) {
      struct timespec ts = {0, 0};
      xine_gettime (&ts);
      ts.tv_nsec += 500000000;
      if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
      }
      if (pthread_cond_timedwait (&this->done_stepping, &this->step_mutex, &ts))
        ret = 0;
    }
    pthread_mutex_unlock (&this->step_mutex);
    break;

  case AO_PROP_COMPRESSOR:

    this->compression_factor_max = (double) value / 100.0;

    this->do_compress = (this->compression_factor_max >1.0);

    ret = this->compression_factor_max*100;
    break;

  case AO_PROP_AMP:

    this->amp_factor = (double) value / 100.0;

    this->do_amp = (this->amp_factor != 1.0 || this->amp_mute);

    ret = this->amp_factor*100;
    break;

  case AO_PROP_AMP_MUTE:
    ret = this->amp_mute = value;

    this->do_amp = (this->amp_factor != 1.0 || this->amp_mute);
    break;

  case AO_PROP_EQ_30HZ:
  case AO_PROP_EQ_60HZ:
  case AO_PROP_EQ_125HZ:
  case AO_PROP_EQ_250HZ:
  case AO_PROP_EQ_500HZ:
  case AO_PROP_EQ_1000HZ:
  case AO_PROP_EQ_2000HZ:
  case AO_PROP_EQ_4000HZ:
  case AO_PROP_EQ_8000HZ:
  case AO_PROP_EQ_16000HZ:
    this->eq_settings[property - AO_PROP_EQ_30HZ] = value;
    ao_eq_update (this);
    ret = value;
    break;

  case AO_PROP_DISCARD_BUFFERS:
    /* recursive discard buffers setting */
    pthread_mutex_lock (&this->out_fifo.mutex);
    if (value) {
      this->discard_buffers++;
      pthread_cond_signal (&this->out_fifo.not_empty);
    } else if (this->discard_buffers)
      this->discard_buffers--;
    else
      xprintf (this->xine, XINE_VERBOSITY_DEBUG,
               "audio_out: ao_set_property: discard_buffers is already zero\n");
    pthread_mutex_unlock (&this->out_fifo.mutex);

    ret = this->discard_buffers;

    /* discard buffers here because we have no output thread */
    if (this->grab_only && this->discard_buffers) {
      ao_out_fifo_manual_flush (this);
    }
    break;

  case AO_PROP_CLOSE_DEVICE:
    ao_driver_lock (this);
    if(this->driver_open)
      this->driver->close(this->driver);
    this->driver_open = 0;
    ao_driver_unlock (this);
    break;

  case AO_PROP_CLOCK_SPEED:
    /* something to do? */
    if (value == (int)(this->current_speed))
      break;
    /* TJ. pthread mutex implementation on my multicore AMD box is somewhat buggy.
       When fed by a fast single threaded decoder like mad, audio out loop does
       not release current speed lock long enough to wake us up here.
       So tell loop to enter unpause waiting _before_ we wait. */
    this->current_speed = value;
    /*
     * slow motion / fast forward does not play sound, drop buffered
     * samples from the sound driver (check slow_fast_audio flag)
     */
    if (value != XINE_FINE_SPEED_NORMAL && value != XINE_SPEED_PAUSE && !this->slow_fast_audio )
      this->ao.control(&this->ao, AO_CTRL_FLUSH_BUFFERS, NULL);

    if( value == XINE_SPEED_PAUSE ) {
      /* current_speed_lock is here to make sure the ao_loop will pause in a safe place.
       * that is, we cannot pause writing to device, filling gaps etc. */
      pthread_mutex_lock(&this->current_speed_lock);
      this->ao.control(&this->ao, AO_CTRL_PLAY_PAUSE, NULL);
      pthread_mutex_unlock(&this->current_speed_lock);
    } else {
      this->ao.control(&this->ao, AO_CTRL_PLAY_RESUME, NULL);
    }
    if( this->slow_fast_audio )
      ao_update_resample_factor(this);
    break;

  default:
    if (!this->grab_only) {
      /* Let the sound driver lock it's own mixer */
      ret =  this->driver->set_property(this->driver, property, value);
    }
  }

  return ret;
}

static int ao_control (xine_audio_port_t *this_gen, int cmd, ...) {

  aos_t *this = (aos_t *) this_gen;
  va_list args;
  void *arg;
  int rval = 0;

  if (this->grab_only)
    return 0;

  ao_driver_lock (this);
  if(this->driver_open) {
    va_start(args, cmd);
    arg = va_arg(args, void*);
    rval = this->driver->control(this->driver, cmd, arg);
    va_end(args);
  }
  ao_driver_unlock (this);

  return rval;
}

static void ao_flush (xine_audio_port_t *this_gen) {
  aos_t *this = (aos_t *) this_gen;

  xprintf (this->xine, XINE_VERBOSITY_DEBUG,
           "audio_out: ao_flush (loop running: %d)\n", this->audio_loop_running);

  if( this->audio_loop_running ) {
    /* do not try this in paused mode */
    if (this->current_speed != XINE_SPEED_PAUSE)
      this->flush_audio_driver++;
    ao_out_fifo_loop_flush (this);
  }
}

static int ao_status (xine_audio_port_t *this_gen, xine_stream_t *stream,
	       uint32_t *bits, uint32_t *rate, int *mode) {
  aos_t *this = (aos_t *) this_gen;
  xine_stream_t **s;
  int ret = 0;

  if (!stream || (stream == XINE_ANON_STREAM)) {
    *bits = this->input.bits;
    *rate = this->input.rate;
    *mode = this->input.mode;
    return 0;
  }

  xine_rwlock_rdlock (&this->streams_lock);
  for (s = this->streams; *s; s++) {
    if (*s == stream) {
      *bits = this->input.bits;
      *rate = this->input.rate;
      *mode = this->input.mode;
      ret = 1;
      break;
    }
  }
  xine_rwlock_unlock (&this->streams_lock);

  return ret;
}

static void ao_update_av_sync_method(void *this_gen, xine_cfg_entry_t *entry) {
  aos_t *this = (aos_t *) this_gen;

  lprintf ("av_sync_method = %d\n", entry->num_value);

  this->av_sync_method_conf = entry->num_value;

  switch (this->av_sync_method_conf) {
  case 0:
    this->resample_sync_method = 0;
    break;
  case 1:
    this->resample_sync_method = 1;
    break;
  default:
    this->resample_sync_method = 0;
    break;
  }
  this->resample_sync_info.valid = 0;
}

static void ao_update_ptoffs (void *this_gen, xine_cfg_entry_t *entry) {
  aos_t *this = (aos_t *)this_gen;
  this->passthrough_offset = entry->num_value;
  this->ptoffs = (this->output.mode == AO_CAP_MODE_A52) || (this->output.mode == AO_CAP_MODE_AC5) ? this->passthrough_offset : 0;
}

static void ao_update_slow_fast (void *this_gen, xine_cfg_entry_t *entry) {
  aos_t *this = (aos_t *)this_gen;
  this->slow_fast_audio = entry->num_value;
}

xine_audio_port_t *_x_ao_new_port (xine_t *xine, ao_driver_t *driver,
				int grab_only) {

  config_values_t *config = xine->config;
  aos_t           *this;
  uint8_t         *vsbuf0, *vsbuf1;
  int              i, err;
  pthread_attr_t   pth_attrs;
  pthread_mutexattr_t attr;
  static const char *const resample_modes[] = {"auto", "off", "on", NULL};
  static const char *const av_sync_methods[] = {"metronom feedback", "resample", NULL};

  this = calloc(1, sizeof(aos_t)) ;
  if (!this)
    return NULL;
#ifndef HAVE_ZERO_SAFE_MEM
  /* Do these first, when compiler still knows "this" is all zeroed.
   * Let it optimize away this on most systems where clear mem
   * interpretes as 0, 0f or NULL safely.
   */
  this->num_driver_actions     = 0;
  this->dreqs_all              = 0;
  this->dreqs_wait             = 0;
  this->audio_loop_running     = 0;
  this->flush_audio_driver     = 0;
  this->discard_buffers        = 0;
  this->step                   = 0;
  this->last_gap               = 0;
  this->last_sgap              = 0;
  this->compression_factor_max = 0.0;
  this->do_compress            = 0;
  this->do_amp                 = 0;
  this->amp_mute               = 0;
  this->do_equ                 = 0;
  this->eq_settings[0]         = 0;
  this->eq_settings[1]         = 0;
  this->eq_settings[2]         = 0;
  this->eq_settings[3]         = 0;
  this->eq_settings[4]         = 0;
  this->eq_settings[5]         = 0;
  this->eq_settings[6]         = 0;
  this->eq_settings[7]         = 0;
  this->eq_settings[8]         = 0;
  this->eq_settings[9]         = 0;
#endif

  this->driver        = driver;
  this->xine          = xine;
  this->clock         = xine->clock;
  this->current_speed = xine->clock->speed;

  this->base_samp = xine_mallocz_aligned ((NUM_AUDIO_BUFFERS + 1) * AUDIO_BUF_SIZE);
  vsbuf0          = calloc (1, 4 * AUDIO_BUF_SIZE);
  vsbuf1          = calloc (1, 4 * AUDIO_BUF_SIZE);
  if (!this->base_samp || !vsbuf0 || !vsbuf1) {
    xine_free_aligned (this->base_samp);
    free (vsbuf0);
    free (vsbuf1);
    free (this);
    return NULL;
  }

  ao_streams_open (this);
  
  /* warning: driver_lock is a recursive mutex. it must NOT be
   * used with neither pthread_cond_wait() or pthread_cond_timedwait()
   */
  pthread_mutexattr_init    (&attr);
  pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init        (&this->driver_lock, &attr);
  pthread_mutexattr_destroy (&attr);

  pthread_mutex_init        (&this->driver_action_lock, NULL);
  pthread_cond_init         (&this->driver_action_cond, NULL);

  this->ao.open             = ao_open;
  this->ao.get_buffer       = ao_get_buffer;
  this->ao.put_buffer       = ao_put_buffer;
  this->ao.close            = ao_close;
  this->ao.exit             = ao_exit;
  this->ao.get_capabilities = ao_get_capabilities;
  this->ao.get_property     = ao_get_property;
  this->ao.set_property     = ao_set_property;
  this->ao.control          = ao_control;
  this->ao.flush            = ao_flush;
  this->ao.status           = ao_status;

  this->grab_only           = grab_only;

  pthread_mutex_init (&this->current_speed_lock, NULL);

  pthread_mutex_init (&this->step_mutex, NULL);
  pthread_cond_init  (&this->done_stepping, NULL);

  if (!grab_only)
    this->gap_tolerance = driver->get_gap_tolerance (this->driver);

  this->av_sync_method_conf = config->register_enum (
    config, "audio.synchronization.av_sync_method", 0, (char **)av_sync_methods,
    _("method to sync audio and video"),
    _("When playing audio and video, there are at least two clocks involved: "
      "The system clock, to which video frames are synchronized and the clock "
      "in your sound hardware, which determines the speed of the audio playback. "
      "These clocks are never ticking at the same speed except for some rare "
      "cases where they are physically identical. In general, the two clocks "
      "will run drift after some time, for which xine offers two ways to keep "
      "audio and video synchronized:\n\n"
      "metronom feedback\n"
      "This is the standard method, which applies a countereffecting video drift, "
      "as soon as the audio drift has accumulated over a threshold.\n\n"
      "resample\n"
      "For some video hardware, which is limited to a fixed frame rate (like the "
      "DXR3 or other decoder cards) the above does not work, because the video "
      "cannot drift. Therefore we resample the audio stream to make it longer "
      "or shorter to compensate the audio drift error. This does not work for "
      "digital passthrough, where audio data is passed to an external decoder in "
      "digital form."),
    20, ao_update_av_sync_method, this);
  this->resample_sync_method = this->av_sync_method_conf == 1 ? 1 : 0;
  this->resample_sync_info.valid = 0;

  this->resample_conf = config->register_enum (
    config, "audio.synchronization.resample_mode", 0, (char **)resample_modes,
    _("enable resampling"),
    _("When the sample rate of the decoded audio does not match the capabilities "
      "of your sound hardware, an adaptation called \"resampling\" is required. "
      "Here you can select, whether resampling is enabled, disabled or used "
      "automatically when necessary."),
    20, NULL, NULL);

  this->force_rate = config->register_num (
    config, "audio.synchronization.force_rate", 0,
    _("always resample to this rate (0 to disable)"),
    _("Some audio drivers do not correctly announce the capabilities of the audio "
      "hardware. By setting a value other than zero here, you can force the audio "
      "stream to be resampled to the given rate."),
    20, NULL, NULL);

  this->passthrough_offset = config->register_num (
    config, "audio.synchronization.passthrough_offset", 0,
    _("offset for digital passthrough"),
    _("If you use an external surround decoder and audio is ahead or behind video, "
      "you can enter a fixed offset here to compensate.\n"
      "The unit of the value is one PTS tick, which is the 90000th part of a second."),
    10, ao_update_ptoffs, this);

  this->slow_fast_audio = config->register_bool (
    config, "audio.synchronization.slow_fast_audio", 0,
    _("play audio even on slow/fast speeds"),
    _("If you enable this option, the audio will be heard even when playback speed is "
      "different than 1X. Of course, it will sound distorted (lower/higher pitch). "
      "If want to experiment preserving the pitch you may try the 'stretch' audio post plugin instead."),
    10, ao_update_slow_fast, this);

  this->compression_factor = 2.0;
  this->amp_factor         = 1.0;

  /*
   * pre-allocate memory for samples
   */

  ao_fifo_open (&this->free_fifo);
  ao_fifo_open (&this->out_fifo);

  {
    audio_buffer_t *buf = this->base_buf, *list = NULL, **add = &list;
    extra_info_t    *ei = this->base_ei + EI_RING_SIZE;
    uint8_t        *mem = this->base_samp;

    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
      buf->mem        = (int16_t *)mem;
      buf->mem_size   = AUDIO_BUF_SIZE;
      buf->extra_info = ei;
      *add            = buf;
      add             = &buf->next;
      buf++;
      ei++;
      mem += AUDIO_BUF_SIZE;
    }
    *add = NULL;
    this->free_fifo.first = list;
    this->free_fifo.add   = add;
    this->free_fifo.num_buffers     =
    this->free_fifo.num_buffers_max = i;

    this->zero_space = (int16_t *)mem;

    /* buffers used for audio conversions. need to be resizable */
    buf->mem        = (int16_t *)vsbuf0;
    buf->mem_size   = 4 *AUDIO_BUF_SIZE;
    buf->extra_info = ei;
    this->frame_buf[0] = buf;
    buf++;
    ei++;
    buf->mem        = (int16_t *)vsbuf1;
    buf->mem_size   = 4 *AUDIO_BUF_SIZE;
    buf->extra_info = ei;
    this->frame_buf[1] = buf;
  }

  this->xine->port_ticket->revoke_cb_register (this->xine->port_ticket, ao_ticket_revoked, this);

  /*
   * Set audio volume to latest used one ?
   */
  if (this->driver) {
    int vol;

    vol = config->register_range (config, "audio.volume.mixer_volume", 50, 0, 100,
      _("startup audio volume"),
      _("The overall audio volume set at xine startup."),
      10, NULL, NULL);

    if (config->register_bool (config, "audio.volume.remember_volume", 0,
      _("restore volume level at startup"),
      _("If disabled, xine will not modify any mixer settings at startup."),
      10, NULL, NULL)) {
      int caps = this->driver->get_capabilities (this->driver);
      if (caps & AO_CAP_MIXER_VOL)
        this->driver->set_property (this->driver, AO_PROP_MIXER_VOL, vol);
      else if (caps & AO_CAP_PCM_VOL)
        this->driver->set_property (this->driver, AO_PROP_PCM_VOL, vol);
    }
  }

  if (!this->grab_only) {
    /*
     * start output thread
     */

    this->audio_loop_running = 1;

    pthread_attr_init(&pth_attrs);
#if defined(_POSIX_THREAD_PRIORITY_SCHEDULING) && (_POSIX_THREAD_PRIORITY_SCHEDULING > 0)
    pthread_attr_setscope(&pth_attrs, PTHREAD_SCOPE_SYSTEM);
#endif

    err = pthread_create (&this->audio_thread, &pth_attrs, ao_loop, this);
    pthread_attr_destroy(&pth_attrs);

    if (err != 0) {
      xprintf (this->xine, XINE_VERBOSITY_NONE,
	       "audio_out: can't create thread (%s)\n", strerror(err));
      xprintf (this->xine, XINE_VERBOSITY_LOG,
	       _("audio_out: sorry, this should not happen. please restart xine.\n"));

      this->audio_loop_running = 0;
      ao_exit(&this->ao);
      return NULL;
     }

    xprintf (this->xine, XINE_VERBOSITY_DEBUG, "audio_out: thread created\n");
  }

  return &this->ao;
}
