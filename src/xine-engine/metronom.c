/* 
 * Copyright (C) 2000-2001 the xine project
 * 
 * This file is part of xine, a unix video player.
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
 * $Id: metronom.c,v 1.48 2002/01/06 00:49:01 guenter Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/time.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "xine_internal.h"
#include "metronom.h"
#include "xineutils.h"

#define MAX_PTS_TOLERANCE      5000
#define MAX_VIDEO_DELTA        1600
#define MAX_AUDIO_DELTA        1600
#define AUDIO_SAMPLE_NUM      32768
#define WRAP_START_TIME      100000
#define WRAP_THRESHOLD       120000 
#define SCR_DISCONTINUITY     60000 
#define MAX_NUM_WRAP_DIFF       100
#define MAX_SCR_PROVIDERS        10
#define PREBUFFER_PTS_OFFSET  30000

#ifdef __GNUC__
#define LOG_MSG_STDERR(xine, message, args...) {                     \
    xine_log((xine_t*)xine, XINE_LOG_METRONOM, message, ##args);     \
    fprintf(stderr, message, ##args);                                \
  }
#define LOG_MSG(xine, message, args...) {                            \
    xine_log((xine_t*)xine, XINE_LOG_METRONOM, message, ##args);     \
    printf(message, ##args);                                         \
  }
#else
#define LOG_MSG_STDERR(xine, ...) {                                  \
    xine_log((xine_t*)xine, XINE_LOG_METRONOM, __VA_ARGS__);         \
    fprintf(stderr, __VA_ARGS__);                                    \
  }
#define LOG_MSG(xine, ...) {                                         \
    xine_log((xine_t*)xine, XINE_LOG_METRONOM, __VA_ARGS__);         \
    printf(__VA_ARGS__);                                             \
  }
#endif

/*
#define METRONOM_LOG
*/

/*
 * ****************************************
 *   primary SCR plugin: 
 *    unix System Clock Reference
 * ****************************************
 */

typedef struct unixscr_s {
  scr_plugin_t     scr;

  struct timeval   cur_time;
  uint32_t         cur_pts;
  double           speed_factor;

  pthread_mutex_t  lock;

} unixscr_t;

static int unixscr_get_priority (scr_plugin_t *scr) {
  return 5; /* low priority */
}

/* Only call this when already mutex locked */
static void unixscr_set_pivot (unixscr_t *this) {

  struct   timeval tv;
  uint32_t pts;
  double   pts_calc; 

  gettimeofday(&tv, NULL);
  pts_calc = (tv.tv_sec  - this->cur_time.tv_sec) * this->speed_factor;
  pts_calc += (tv.tv_usec - this->cur_time.tv_usec) * this->speed_factor / 1e6;
  pts = this->cur_pts + pts_calc;

/* This next part introduces a one off inaccuracy 
 * to the scr due to rounding tv to pts. 
 */
  this->cur_time.tv_sec=tv.tv_sec;
  this->cur_time.tv_usec=tv.tv_usec;
  this->cur_pts=pts; 

  return ;
}

static int unixscr_set_speed (scr_plugin_t *scr, int speed) {
  unixscr_t *this = (unixscr_t*) scr;

  pthread_mutex_lock (&this->lock);

  unixscr_set_pivot( this );
  this->speed_factor = (double) speed * 90000.0 / 4.0;

  pthread_mutex_unlock (&this->lock);

  return speed;
}

static void unixscr_adjust (scr_plugin_t *scr, uint32_t vpts) {
  unixscr_t *this = (unixscr_t*) scr;
  struct   timeval tv;

  pthread_mutex_lock (&this->lock);

  gettimeofday(&tv, NULL);
  this->cur_time.tv_sec=tv.tv_sec;
  this->cur_time.tv_usec=tv.tv_usec;
  this->cur_pts = vpts;

  pthread_mutex_unlock (&this->lock);
}

static void unixscr_start (scr_plugin_t *scr, uint32_t start_vpts) {
  unixscr_t *this = (unixscr_t*) scr;

  pthread_mutex_lock (&this->lock);

  gettimeofday(&this->cur_time, NULL);
  this->cur_pts = start_vpts;

  pthread_mutex_unlock (&this->lock);
}

static uint32_t unixscr_get_current (scr_plugin_t *scr) {
  unixscr_t *this = (unixscr_t*) scr;

  struct   timeval tv;
  uint32_t pts;
  double   pts_calc; 
  pthread_mutex_lock (&this->lock);

  gettimeofday(&tv, NULL);
  
  pts_calc = (tv.tv_sec  - this->cur_time.tv_sec) * this->speed_factor;
  pts_calc += (tv.tv_usec - this->cur_time.tv_usec) * this->speed_factor / 1e6;

  pts = this->cur_pts + pts_calc;
  
  pthread_mutex_unlock (&this->lock);

  return pts;
}

static scr_plugin_t* unixscr_init () {
  unixscr_t *this;

  this = malloc(sizeof(*this));
  memset(this, 0, sizeof(*this));
  
  this->scr.interface_version = 2;
  this->scr.get_priority      = unixscr_get_priority;
  this->scr.set_speed         = unixscr_set_speed;
  this->scr.adjust            = unixscr_adjust;
  this->scr.start             = unixscr_start;
  this->scr.get_current       = unixscr_get_current;
  unixscr_set_speed (&this->scr, SPEED_NORMAL);

  pthread_mutex_init (&this->lock, NULL);

  return &this->scr;
}
 

/*
 * ****************************************
 *       master clock feature
 * ****************************************
 */


static void metronom_start_clock (metronom_t *this, uint32_t pts) {
  scr_plugin_t** scr;
  for (scr = this->scr_list; scr < this->scr_list+MAX_SCR_PROVIDERS; scr++)
    if (*scr) (*scr)->start(*scr, pts);
}


static uint32_t metronom_get_current_time (metronom_t *this) {
  return this->scr_master->get_current(this->scr_master);
}


static void metronom_stop_clock(metronom_t *this) {
  scr_plugin_t** scr;
  for (scr = this->scr_list; scr < this->scr_list+MAX_SCR_PROVIDERS; scr++)
    if (*scr) (*scr)->set_speed(*scr, SPEED_PAUSE);
}

static void metronom_resume_clock(metronom_t *this) {
  scr_plugin_t** scr;
  for (scr = this->scr_list; scr < this->scr_list+MAX_SCR_PROVIDERS; scr++)
    if (*scr) (*scr)->set_speed(*scr, SPEED_NORMAL);
}



static void metronom_adjust_clock(metronom_t *this, uint32_t desired_pts) {
  this->scr_master->adjust(this->scr_master, desired_pts);
}

static int metronom_set_speed (metronom_t *this, int speed) {

  scr_plugin_t **scr;
  int            true_speed;

  true_speed = this->scr_master->set_speed (this->scr_master, speed);

  for (scr = this->scr_list; scr < this->scr_list+MAX_SCR_PROVIDERS; scr++)
    if (*scr) (*scr)->set_speed(*scr, true_speed);

  return true_speed;
}

/*
 * virtual pts calculation
 */

static void metronom_video_stream_start (metronom_t *this) {

  pthread_mutex_lock (&this->lock);

  LOG_MSG(this->xine, _("metronom: video stream start...\n"));

  if (this->video_stream_running) {
    LOG_MSG(this->xine, _("metronom: video stream start ignored\n"));
    pthread_mutex_unlock (&this->lock);
    return;
  }

  this->pts_per_frame             = 3000;
  this->avg_frame_duration        = 3000;

  this->video_vpts                = PREBUFFER_PTS_OFFSET;

  this->last_video_pts            = 0;
  this->last_video_scr            = 0;
  this->num_video_vpts_guessed    = 0;

  /* video_wrap_offset will be updated with the first pts */
  /* this->video_wrap_offset         = PREBUFFER_PTS_OFFSET; */
  this->wrap_diff_counter         = 0;

  this->video_stream_running      = 1;
  this->video_stream_starting     = 1;

  this->video_discontinuity       = 0;
  this->video_discontinuity_count = 0;

  if (this->have_audio) {
    /*while (!this->audio_stream_running) {*/
    if (!this->audio_stream_running) {
      LOG_MSG(this->xine, _("metronom: waiting for audio to start...\n"));
      pthread_cond_wait (&this->audio_started, &this->lock);
    }
  }
  pthread_cond_signal (&this->video_started);

  pthread_mutex_unlock (&this->lock);

  metronom_start_clock (this, 0);
}


static void metronom_video_stream_end (metronom_t *this) {
  
  pthread_mutex_lock (&this->lock);

  LOG_MSG(this->xine, _("metronom: video stream end\n"));

  if (!this->video_stream_running) {
    LOG_MSG(this->xine, _("metronom: video stream end ignored\n"));
    pthread_mutex_unlock (&this->lock);
    return;
  }

  this->video_stream_running = 0;

  if (this->have_audio) {
    /* while (this->audio_stream_running) { */
    if (this->audio_stream_running) {
      LOG_MSG(this->xine, _("metronom: waiting for audio to end...\n"));
      pthread_cond_wait (&this->audio_ended, &this->lock);
    }
  }
  pthread_cond_signal (&this->video_ended);


  pthread_mutex_unlock (&this->lock);
}

static void metronom_audio_stream_start (metronom_t *this) {

  pthread_mutex_lock (&this->lock);

  LOG_MSG(this->xine, _("metronom: audio stream start...\n"));

  if (this->audio_stream_running) {
    LOG_MSG(this->xine, _("metronom: audio stream start ignored\n"));
    pthread_mutex_unlock (&this->lock);
    return;
  }

  this->audio_vpts                = PREBUFFER_PTS_OFFSET;

  this->audio_pts_delta           = 0;

  this->num_audio_samples_guessed = 1;
  this->last_audio_pts            = 0;
  this->last_audio_scr            = 0;

  /* audio_wrap_offset will be updated with the first pts */
  /* this->audio_wrap_offset         = PREBUFFER_PTS_OFFSET; */
  this->wrap_diff_counter         = 0;

  this->audio_stream_running      = 1;
  this->audio_stream_starting     = 1;

  this->audio_discontinuity       = 0;
  this->audio_discontinuity_count = 0;

  /*while (!this->video_stream_running) { */
  if (!this->video_stream_running) {
    LOG_MSG(this->xine, _("metronom: waiting for video to start...\n"));
    pthread_cond_wait (&this->video_started, &this->lock);
  }

  pthread_cond_signal (&this->audio_started);

  pthread_mutex_unlock (&this->lock);

  LOG_MSG(this->xine, _("metronom: audio stream start...done\n"));

  metronom_start_clock (this, 0);
}

static void metronom_audio_stream_end (metronom_t *this) {
  
  pthread_mutex_lock (&this->lock);

  LOG_MSG(this->xine, _("metronom: audio stream end\n"));
  if (!this->audio_stream_running) {
    LOG_MSG(this->xine, _("metronom: audio stream end ignored\n"));
    pthread_mutex_unlock (&this->lock);
    return;
  }

  this->audio_stream_running = 0;

  /* while (this->video_stream_running) { */
  if (this->video_stream_running) {
    LOG_MSG(this->xine, _("metronom: waiting for video to end...\n"));
    pthread_cond_wait (&this->video_ended, &this->lock);
  }

  pthread_cond_signal (&this->audio_ended);
  pthread_mutex_unlock (&this->lock);
}

static void metronom_set_video_rate (metronom_t *this, uint32_t pts_per_frame) {
  pthread_mutex_lock (&this->lock);

  this->pts_per_frame = pts_per_frame;

  this->avg_frame_duration = this->pts_per_frame;

  pthread_mutex_unlock (&this->lock);
}

static uint32_t metronom_get_video_rate (metronom_t *this) {
  int ret;
  
  pthread_mutex_lock (&this->lock);
  ret = this->avg_frame_duration;
  pthread_mutex_unlock (&this->lock);

  /* this is due bad streams, but returning 0 will
     cause problems to video out timer setting. */
  if( ret < 100 )
    ret = 100;             

  return ret;
}

static void metronom_set_audio_rate (metronom_t *this, uint32_t pts_per_smpls) {
  pthread_mutex_lock (&this->lock);

  this->pts_per_smpls = pts_per_smpls;

  pthread_mutex_unlock (&this->lock);

#ifdef METRONOM_LOG
  printf ("metronom: %d pts per %d samples\n", pts_per_smpls, AUDIO_SAMPLE_NUM);
#endif

}

static uint32_t metronom_got_spu_packet (metronom_t *this, uint32_t pts,
					 uint32_t duration, uint32_t scr ) {
  uint32_t vpts;
  
  pthread_mutex_lock (&this->lock);
  
  if (pts) {
    this->spu_vpts=pts;
  } else {
    pts=this->spu_vpts;
    this->spu_vpts=this->spu_vpts;
  }

  /* 
     It happens with the dxr3 that got_spu_packet is called before  
     got_video_frame. Since video_wrap_offset is zero until then,   
     the return value would be wrong. In this case zero is returned.
     
     Also this->video_discontinuity means that scr discontinuity was
     detected but this->video_wrap_offset not updated (would give
     wrong values too).
  */
  if ( this->video_stream_starting ) {
    vpts = 0;
  } else if ( this->video_discontinuity ) {
    /* we can safely use audio_wrap_offset if already updated */
    if( !this->audio_discontinuity ) {
      vpts = pts + this->audio_wrap_offset;
    } else {
      vpts = 0;      
    }
  } else {  
    vpts = pts + this->video_wrap_offset;
  }
  
  pthread_mutex_unlock (&this->lock);
  return vpts;
}

static void metronom_expect_video_discontinuity (metronom_t *this) {

  pthread_mutex_lock (&this->lock);

  this->video_discontinuity  = 10;
  
  this->video_discontinuity_count++;
  pthread_cond_signal (&this->video_discontinuity_reached);
  
  LOG_MSG(this->xine, _("metronom: video discontinuity #%d\n"),
	  this->video_discontinuity_count);
  
  if( this->have_audio ) {
    while ( this->audio_discontinuity_count <
            this->video_discontinuity_count ) {

      LOG_MSG(this->xine, _("metronom: waiting for audio discontinuity #%d\n"),
	      this->video_discontinuity_count);

      pthread_cond_wait (&this->audio_discontinuity_reached, &this->lock);
    }
  
    if ( this->video_vpts < this->audio_vpts ) {
      this->video_vpts = this->audio_vpts;
      LOG_MSG(this->xine, _("metronom: video vpts adjusted to %d\n"), this->video_vpts);
    }
  }
  
  /* this->num_video_vpts_guessed = 0; */
  /* this->last_video_pts = this->video_vpts - this->video_wrap_offset; */
  /*
  this->avg_frame_duration = this->pts_per_frame;
  */
  this->frames_since_start = 0;
  
  pthread_mutex_unlock (&this->lock);
}

static uint32_t metronom_got_video_frame (metronom_t *this, uint32_t pts, uint32_t scr) {

  uint32_t vpts;
  int pts_discontinuity = 0;
  
  pthread_mutex_lock (&this->lock);
  
  if( (this->audio_discontinuity || this->audio_stream_starting) &&
      (this->video_discontinuity || this->video_stream_starting) ) {
      
      /* this is needed to take care of still frame with no audio
         were vpts are not updated.
         we can only do it here because audio and video decoder threads
         have just been synced */
      if ( this->video_vpts < metronom_get_current_time(this) ) {
        this->video_vpts = metronom_get_current_time(this) + PREBUFFER_PTS_OFFSET;
        this->audio_vpts = this->video_vpts;
        LOG_MSG(this->xine, _("metronom: audio/video vpts too old, adjusted to %d\n"), 
                this->video_vpts);
      }
  }

  /* check for pts discontinuities against the predicted pts value */
  if (pts && this->last_video_pts) {
    vpts = this->last_video_pts + 
          (this->num_video_vpts_guessed+1) * this->avg_frame_duration;
    if( ( pts > vpts && (pts - vpts) > WRAP_THRESHOLD ) ||
        ( pts < vpts && (vpts - pts) > WRAP_THRESHOLD ) ) {
      pts_discontinuity = 1;
      
      /* 
         ignore discontinuities created by frame reordering around
         the REAL discontinuity. :)
      */
      if( !this->video_discontinuity && !this->video_stream_starting ) {
        pts = 0;
      }
    }
  }
  
  if (pts) {
    /*
     * check if there was any pending SCR discontinuity (video_discontinuity
     * is set from the decoder loop) together with pts discont.
     */
    if ( (this->video_discontinuity && pts_discontinuity) ||
          this->video_stream_starting ) {
      this->video_discontinuity = 0;
      this->video_stream_starting = 0;
      this->wrap_diff_counter = 0;

      this->video_wrap_offset = this->video_vpts + this->avg_frame_duration - pts;
      /* + this->num_video_vpts_guessed * this->avg_frame_duration; */
      
      vpts = pts + this->video_wrap_offset;

      LOG_MSG(this->xine, _("metronom: video pts discontinuity/start, pts is %d, wrap_offset is %d, vpts is %d\n"),
	      pts, this->video_wrap_offset, vpts);

    } else {

      /*
       * audio and video wrap are not allowed to differ for too long
       */
      if ( !this->audio_stream_starting && this->have_audio
	   && (this->video_wrap_offset != this->audio_wrap_offset) 
	   && !this->video_discontinuity && !this->audio_discontinuity ) {
	this->wrap_diff_counter++;
	
	if (this->wrap_diff_counter > MAX_NUM_WRAP_DIFF) {
	  
	  LOG_MSG(this->xine, _("metronom: forcing video_wrap (%d) and audio wrap (%d)"),
		  this->video_wrap_offset, this->audio_wrap_offset);
	  
	  if (this->video_wrap_offset > this->audio_wrap_offset)
	    this->audio_wrap_offset = this->video_wrap_offset;
	  else
	    this->video_wrap_offset = this->audio_wrap_offset;
	  
	  LOG_MSG(this->xine, _(" to %d\n"), this->video_wrap_offset);
	  
	  this->wrap_diff_counter = 0;
	}
      }

      /*
       * calc overall average frame duration (according to pts values)
       */
      if (this->frames_since_start && this->last_video_pts) {
	int current_avg_delta;
	
	int weight_old = 9;
	int weight_new = 1;
	
	/*
	  printf("foo: pts %d, last pts %d\n", pts, this->last_video_pts);
	*/
	
	if (pts > this->last_video_pts) {
	  current_avg_delta = (pts - this->last_video_pts) / (this->num_video_vpts_guessed + 1);
	  
	  /*
	    printf("foo: current_avg_delta %d\n", current_avg_delta);
	  */
	  
	  this->avg_frame_duration =
	    (((this->avg_frame_duration * weight_old) + (current_avg_delta * weight_new)) /
	   (weight_old + weight_new));
	} else { 
	  current_avg_delta = (this->last_video_pts - pts) / (this->num_video_vpts_guessed + 1);
	  
	  /*
	    printf("foo: current_avg_delta - %d\n", current_avg_delta);
	  */

	  this->avg_frame_duration =
	    (((this->avg_frame_duration * weight_old) - (current_avg_delta * weight_new)) /
	     (weight_old + weight_new));
	}
      }
    }

    this->last_video_pts  = pts;
  }

  this->video_vpts += this->avg_frame_duration;

  /* 
   * smoothen possibly wrong pts as long as delta is small 
   */

  if (pts) {
    int drift;
    int delta = this->video_vpts - this->video_wrap_offset - pts;

#ifdef METRONOM_LOG
    printf("metronom: delta (vpts <-> pts+wrap_offset): %d\n", delta);
#endif

    if (abs (delta) > 45000) {

      this->video_vpts = pts + this->video_wrap_offset; 

      LOG_MSG(this->xine, _("metronom: delta too big, setting vpts to %d\n"),
	      this->video_vpts);

  
    } else {
    
      if (this->num_video_vpts_guessed > 10)
	this->num_video_vpts_guessed = 10;
    
      drift = delta / 20 * (this->num_video_vpts_guessed + 1);
#ifdef METRONOM_LOG
      printf("metronom: compensation drift: %d\n", drift);
#endif

      this->video_vpts -= drift;
    }

    this->num_video_vpts_guessed = 0;
  } else
    this->num_video_vpts_guessed++;

  this->frames_since_start++;

#ifdef METRONOM_LOG
  printf ("metronom: video vpts for %10d : %10d (avg_frame_duration %d)\n", 
	  pts, this->video_vpts, this->avg_frame_duration);
#endif

  vpts = this->video_vpts + this->av_offset;
  
  pthread_mutex_unlock (&this->lock);

  return vpts;
}

static void metronom_expect_audio_discontinuity (metronom_t *this) {

  pthread_mutex_lock (&this->lock);
    
  this->audio_discontinuity  = 10;
  this->audio_discontinuity_count++;
  pthread_cond_signal (&this->audio_discontinuity_reached);
  
  LOG_MSG(this->xine, _("metronom: audio discontinuity #%d\n"),
	  this->audio_discontinuity_count);
  
  while ( this->audio_discontinuity_count >
	  this->video_discontinuity_count ) {

    LOG_MSG(this->xine, _("metronom: waiting for video_discontinuity #%d\n"), 
	    this->audio_discontinuity_count);

    pthread_cond_wait (&this->video_discontinuity_reached, &this->lock);
  }
  
  if ( this->audio_vpts < this->video_vpts ) {
    this->audio_vpts         = this->video_vpts;
    LOG_MSG(this->xine, _("metronom: audio vpts adjusted to %d\n"), this->audio_vpts);
  }
  
  /* this->num_audio_samples_guessed = 1; */
  /* this->last_audio_pts = this->audio_vpts - this->audio_wrap_offset; */
  
  pthread_mutex_unlock (&this->lock);
}


static uint32_t metronom_got_audio_samples (metronom_t *this, uint32_t pts, 
					    uint32_t nsamples, uint32_t scr) {

  uint32_t vpts;

#ifdef METRONOM_LOG  
  printf ("metronom: got %d samples, pts is %u, last_pts is %u, diff = %d\n",
	  nsamples, pts, this->last_audio_pts, pts - this->last_audio_pts);
#endif

  pthread_mutex_lock (&this->lock);

  if( (this->audio_discontinuity || this->audio_stream_starting) &&
      (this->video_discontinuity || this->video_stream_starting) ) {
      
      /* this is needed to take care of still frame with no audio
         were vpts are not updated.
         we can only do it here because audio and video decoder threads
         have just been synced */
      if ( this->audio_vpts < metronom_get_current_time(this) ) {
        this->audio_vpts = metronom_get_current_time(this) + PREBUFFER_PTS_OFFSET;
        this->video_vpts = this->audio_vpts;
        LOG_MSG(this->xine, _("metronom: audio/video vpts too old, adjusted to %d\n"), 
                this->audio_vpts);
      }
  }

  this->last_audio_scr = scr;

  if (pts) {

    /*
     * discontinuity ?
     */
    if ( this->audio_discontinuity || this->audio_stream_starting ) {
      this->audio_discontinuity = 0;
      this->audio_stream_starting = 0;
      this->wrap_diff_counter = 0;

      this->audio_wrap_offset = this->audio_vpts - pts ;
      /*
	+ this->num_audio_samples_guessed
	* (this->audio_pts_delta + this->pts_per_smpls) / AUDIO_SAMPLE_NUM ;
	*/
      
      vpts = pts + this->audio_wrap_offset;

      LOG_MSG(this->xine, _("metronom: audio pts discontinuity/start, pts is %d, wrap_offset is %d, vpts is %d\n"),
	      pts, this->audio_wrap_offset, vpts);


    } else {

      /*
       * audio and video wrap are not allowed to differ
       * for too long
       */
      
      if ( this->video_wrap_offset != this->audio_wrap_offset       
	   && !this->video_discontinuity && !this->audio_discontinuity ) {
	this->wrap_diff_counter++;
	
	if (this->wrap_diff_counter > MAX_NUM_WRAP_DIFF) {
	  
	  LOG_MSG(this->xine, _("metronom: forcing video_wrap (%d) and audio wrap (%d)"),
		  this->video_wrap_offset, this->audio_wrap_offset);
	  
	  if (this->video_wrap_offset > this->audio_wrap_offset)
	    this->audio_wrap_offset = this->video_wrap_offset;
	  else
	    this->video_wrap_offset = this->audio_wrap_offset;
	  
	  LOG_MSG(this->xine, _("to %d\n"), this->video_wrap_offset);
	  
	  this->wrap_diff_counter = 0;
	}
      }
      
      vpts = pts + this->audio_wrap_offset;

      /*
       * calc delta to compensate wrong samplerates 
       */
      
      if (this->last_audio_pts && (pts>this->last_audio_pts)) {
	int32_t  vpts_diff;
	
	vpts_diff   = vpts - this->audio_vpts;
	
	this->audio_pts_delta += vpts_diff*AUDIO_SAMPLE_NUM / (this->num_audio_samples_guessed);
	
	if (abs(this->audio_pts_delta) >= MAX_AUDIO_DELTA) 
	  this->audio_pts_delta = 0;
      }      
    }

    this->num_audio_samples_guessed = 0;
    this->last_audio_pts = pts;
    this->audio_vpts     = vpts;
  } else
    vpts = this->audio_vpts;

  this->audio_vpts += nsamples * (this->audio_pts_delta + this->pts_per_smpls) / AUDIO_SAMPLE_NUM;
  this->num_audio_samples_guessed += nsamples;

#ifdef METRONOM_LOG
  printf ("metronom: audio vpts for %10d : %10d\n", pts, vpts);
#endif

  pthread_mutex_unlock (&this->lock);

  return vpts;
}

static void metronom_set_av_offset (metronom_t *this, int32_t pts) {

  pthread_mutex_lock (&this->lock);

  this->av_offset = pts;

  pthread_mutex_unlock (&this->lock);

  LOG_MSG(this->xine, _("metronom: av_offset=%d pts\n"), pts);
}

static int32_t metronom_get_av_offset (metronom_t *this) {
  return this->av_offset;
}

static scr_plugin_t* get_master_scr(metronom_t *this) {
  int select = -1, maxprio = 0, i;

  /* find the SCR provider with the highest priority */
  for (i=0; i<MAX_SCR_PROVIDERS; i++) if (this->scr_list[i]) {
    scr_plugin_t *scr = this->scr_list[i];
    
    if (maxprio < scr->get_priority(scr)) {
      select = i;
      maxprio = scr->get_priority(scr);
    }
  }
  if (select < 0) {
    LOG_MSG(this->xine, _("metronom: panic - no scr provider found!\n"));
    return NULL;
  }
  return this->scr_list[select];
}

static int metronom_register_scr (metronom_t *this, scr_plugin_t *scr) {
  int i;

  if (scr->interface_version != 2) return -1;

  for (i=0; i<MAX_SCR_PROVIDERS; i++)
    if (this->scr_list[i] == NULL) break;
  if (i >= MAX_SCR_PROVIDERS)
    return -1; /* No free slot available */

  scr->metronom = this;
  this->scr_list[i] = scr;
  this->scr_master = get_master_scr(this);
  return 0;
}

static void metronom_unregister_scr (metronom_t *this, scr_plugin_t *scr) {
  int i;

  /* Never unregister scr_list[0]! */
  for (i=1; i<MAX_SCR_PROVIDERS; i++)
    if (this->scr_list[i] == scr) break;

  if (i >= MAX_SCR_PROVIDERS)
    return; /* Not found */
  
  this->scr_list[i] = NULL;
  this->scr_master = get_master_scr(this);
}

static int metronom_sync_loop (metronom_t *this) {
  scr_plugin_t** scr;
  uint32_t pts;
  
  while (1) {
    pts = this->scr_master->get_current(this->scr_master);
    
    for (scr = this->scr_list; scr < this->scr_list+MAX_SCR_PROVIDERS; scr++)
      if (*scr && *scr != this->scr_master) (*scr)->adjust(*scr, pts);

    sleep(5); /* synchronise every 5 seconds */
  }
}


metronom_t * metronom_init (int have_audio, void *xine) {

  metronom_t *this = xine_xmalloc (sizeof (metronom_t));
  int         err;

  this->xine                 = xine;
  this->audio_stream_start   = metronom_audio_stream_start;
  this->audio_stream_end     = metronom_audio_stream_end  ;
  this->video_stream_start   = metronom_video_stream_start;
  this->video_stream_end     = metronom_video_stream_end  ;
  this->set_video_rate       = metronom_set_video_rate;
  this->get_video_rate       = metronom_get_video_rate;
  this->set_audio_rate       = metronom_set_audio_rate;
  this->got_video_frame      = metronom_got_video_frame;
  this->got_audio_samples    = metronom_got_audio_samples;
  this->got_spu_packet       = metronom_got_spu_packet;
  this->expect_audio_discontinuity = metronom_expect_audio_discontinuity;
  this->expect_video_discontinuity = metronom_expect_video_discontinuity;
  this->set_av_offset        = metronom_set_av_offset;
  this->get_av_offset        = metronom_get_av_offset;
  this->start_clock          = metronom_start_clock;
  this->stop_clock           = metronom_stop_clock;
  this->resume_clock         = metronom_resume_clock;
  this->get_current_time     = metronom_get_current_time;
  this->adjust_clock         = metronom_adjust_clock;
  this->register_scr         = metronom_register_scr;
  this->unregister_scr       = metronom_unregister_scr;
  this->set_speed            = metronom_set_speed;

  this->scr_list = calloc(MAX_SCR_PROVIDERS, sizeof(void*));
  this->register_scr(this, unixscr_init());

  if ((err = pthread_create(&this->sync_thread, NULL,
      			    (void*(*)(void*)) metronom_sync_loop, this)) != 0)
    LOG_MSG(this->xine, _("metronom: cannot create sync thread (%s)\n"),
	    strerror(err));

  pthread_mutex_init (&this->lock, NULL);
  pthread_cond_init (&this->video_started, NULL);
  pthread_cond_init (&this->audio_started, NULL);
  pthread_cond_init (&this->video_ended, NULL);
  pthread_cond_init (&this->audio_ended, NULL);
  pthread_cond_init (&this->video_discontinuity_reached, NULL);
  pthread_cond_init (&this->audio_discontinuity_reached, NULL);
    
  this->av_offset   = 0;
  this->have_audio  = have_audio;

  return this;
}
