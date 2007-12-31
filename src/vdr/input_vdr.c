/*
 * Copyright (C) 2003-2004 the xine project
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <errno.h>
#include <pthread.h>

#include <sys/socket.h>
#include <resolv.h>
#include <netdb.h>

#define LOG_MODULE "input_vdr"
#define LOG_VERBOSE
/*
#define LOG
*/
#include <xine/xine_internal.h>
#include <xine/xineutils.h>
#include <xine/input_plugin.h>

#include <xine/input_vdr.h>
#include "post_vdr.h"



#define VDR_MAX_NUM_WINDOWS 16
#define VDR_ABS_FIFO_DIR "/tmp/vdr-xine"

#define BUF_SIZE 1024

#define LOG_OSD(x)
/*
#define LOG_OSD(x) x
*/


typedef struct
{
  input_plugin_t      input_plugin;
   
  xine_stream_t      *stream;
  xine_stream_t      *stream_external;

  int                 fh;
  int                 fh_control;
  int                 fh_result;
  int                 fh_event;
  
  char               *mrl;

  off_t               curpos;
  char                seek_buf[ BUF_SIZE ];
   
  char               *preview;
  off_t               preview_size;
   
  enum funcs          cur_func;
  off_t               cur_size;
  off_t               cur_done;

  xine_osd_t         *osd_window[ VDR_MAX_NUM_WINDOWS ];
  uint8_t            *osd_buffer;
  uint32_t            osd_buffer_size;
  uint8_t             osd_unscaled_blending;

  uint8_t             audio_channels;
  uint8_t             trick_speed_mode;
  uint8_t             mute_mode;
  uint8_t             volume_mode;
  int                 last_volume;
  vdr_frame_size_changed_data_t frame_size;
  
  pthread_t           rpc_thread;
  int                 rpc_thread_shutdown;
  pthread_mutex_t     rpc_thread_shutdown_lock;
  pthread_cond_t      rpc_thread_shutdown_cond;

  xine_event_queue_t *event_queue;
  xine_event_queue_t *event_queue_external;

  pthread_mutex_t     adjust_zoom_lock;
  uint16_t            image4_3_zoom_x;
  uint16_t            image4_3_zoom_y;
  uint16_t            image16_9_zoom_x;
  uint16_t            image16_9_zoom_y;

}
vdr_input_plugin_t;



typedef struct
{
  input_class_t       input_class;
  xine_t             *xine;
  char               *mrls[ 2 ];
}
vdr_input_class_t;



static int vdr_write(int f, void *b, int n)
{
  int t = 0, r;
  
  while (t < n)
  {
    /*
     * System calls are not a thread cancellation point in Linux
     * pthreads.  However, the RT signal sent to cancel the thread
     * will cause recv() to return with EINTR, and we can manually
     * check cancellation.
     */
    pthread_testcancel();
    r = write(f, ((char *)b) + t, n - t);
    pthread_testcancel();

    if (r < 0
        && (errno == EINTR
          || errno == EAGAIN))
    {
      continue;
    }
    
    if (r < 0)
      return r;
    
    t += r;
  }
  
  return t;
}



static int internal_write_event_play_external(vdr_input_plugin_t *this, uint32_t key);

static void event_handler_external(void *user_data, const xine_event_t *event)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)user_data;
  uint32_t key = key_none;
/*  
  printf("event_handler_external(): event->type: %d\n", event->type);
*/
  switch (event->type)
  {
  case XINE_EVENT_UI_PLAYBACK_FINISHED:
    break;

  default:
    return;
  }	
  
  if (0 != internal_write_event_play_external(this, key))
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG, 
            _("%s: input event write: %s.\n"), LOG_MODULE, strerror(errno));
}

static void external_stream_stop(vdr_input_plugin_t *this)
{
  if (this->stream_external)
  {    
    xine_stop(this->stream_external);
    xine_close(this->stream_external);

    if (this->event_queue_external)
    {
      xine_event_dispose_queue(this->event_queue_external);
      this->event_queue_external = 0;
    }

    _x_demux_flush_engine(this->stream_external);
    
    xine_dispose(this->stream_external);    
    this->stream_external = 0;
  }
}

static void external_stream_play(vdr_input_plugin_t *this, char *file_name)
{
  external_stream_stop(this);

  this->stream_external = xine_stream_new(this->stream->xine, this->stream->audio_out, this->stream->video_out);
  
  this->event_queue_external = xine_event_new_queue(this->stream_external);

  xine_event_create_listener_thread(this->event_queue_external, event_handler_external, this);

  if (!xine_open(this->stream_external, file_name)
      || !xine_play(this->stream_external, 0, 0))
  {
    uint32_t key = key_none;
    
    if ( 0 != internal_write_event_play_external(this, key))
      xprintf(this->stream->xine, XINE_VERBOSITY_LOG, 
              _("%s: input event write: %s.\n"), LOG_MODULE, strerror(errno));
  }
}

static off_t vdr_read_abort(xine_stream_t *stream, int fd, char *buf, off_t todo)
{  
  off_t ret;
  
  while (1)
  {
    /*
     * System calls are not a thread cancellation point in Linux
     * pthreads.  However, the RT signal sent to cancel the thread
     * will cause recv() to return with EINTR, and we can manually
     * check cancellation.
     */
    pthread_testcancel();
    ret = _x_read_abort(stream, fd, buf, todo);
    pthread_testcancel();
    
    if (ret < 0
        && (errno == EINTR
          || errno == EAGAIN))
    {
      continue;
    }
    
    break;
  }

  return ret;
}

#define READ_DATA_OR_FAIL(kind, log) \
  data_##kind##_t *data = &data_union.kind; \
  { \
    log; \
    n = vdr_read_abort(this->stream, this->fh_control, (char *)data + sizeof (data->header), sizeof (*data) - sizeof (data->header)); \
    if (n != sizeof (*data) - sizeof (data->header)) \
      return -1; \
    \
    this->cur_size -= n; \
  } 

static double _now()
{
  struct timeval tv;
  
  gettimeofday(&tv, 0);

  return (tv.tv_sec * 1000000.0 + tv.tv_usec) / 1000.0;
}

static void adjust_zoom(vdr_input_plugin_t *this)
{
  pthread_mutex_lock(&this->adjust_zoom_lock); 

  if (this->image4_3_zoom_x && this->image4_3_zoom_y
    && this->image16_9_zoom_x && this->image16_9_zoom_y)
  {
    int ratio = (int)(10000 * this->frame_size.r + 0.5);
fprintf(stderr, "ratio: %d\n", ratio);
    if (13332 <= ratio && ratio <= 13334)
    {
      xine_set_param(this->stream, XINE_PARAM_VO_ZOOM_X, this->image4_3_zoom_x);
      xine_set_param(this->stream, XINE_PARAM_VO_ZOOM_Y, this->image4_3_zoom_y);
    }
    else /* if (17777 <= ratio && ratio <= 17779) */
    {
      xine_set_param(this->stream, XINE_PARAM_VO_ZOOM_X, this->image16_9_zoom_x);
      xine_set_param(this->stream, XINE_PARAM_VO_ZOOM_Y, this->image16_9_zoom_y);
    }
  }

  pthread_mutex_unlock(&this->adjust_zoom_lock); 
}

static off_t vdr_execute_rpc_command(vdr_input_plugin_t *this)
{
  data_union_t data_union;
  off_t n;
   
  n = vdr_read_abort(this->stream, this->fh_control, (char *)&data_union, sizeof (data_union.header));
  if (n != sizeof (data_union.header))
    return -1;
  
  this->cur_func = data_union.header.func;
  this->cur_size = data_union.header.len - sizeof (data_union.header);
  this->cur_done = 0;

  switch (this->cur_func)
  {
  case func_nop:
    {
      READ_DATA_OR_FAIL(nop, lprintf("got NOP\n"));
    }
    break;

  case func_osd_new:
    {
      READ_DATA_OR_FAIL(osd_new, LOG_OSD(lprintf("got OSDNEW\n")));
/*
      LOG_OSD(lprintf("... (%d,%d)-(%d,%d)\n", data->x, data->y, data->width, data->height));

      fprintf(stderr, "vdr: osdnew %d\n", data->window);
*/    
      if (data->window >= VDR_MAX_NUM_WINDOWS)
        return -1;
      
      if (0 != this->osd_window[ data->window ])
        return -1;
      
      this->osd_window[ data->window ] = xine_osd_new(this->stream
                                                     , data->x
                                                     , data->y
                                                     , data->width
                                                     , data->height);
      
      if (0 == this->osd_window[ data->window ])
        return -1;
    }
    break;

  case func_osd_free:
    {
      READ_DATA_OR_FAIL(osd_free, LOG_OSD(lprintf("got OSDFREE\n")));
/*      
      fprintf(stderr, "vdr: osdfree %d\n", data->window);
*/      
      if (data->window >= VDR_MAX_NUM_WINDOWS)
        return -1;
      
      if (0 != this->osd_window[ data->window ])
        xine_osd_free(this->osd_window[ data->window ]);
      
      this->osd_window[ data->window ] = 0;
    }
    break;
    
  case func_osd_show:
    {
      READ_DATA_OR_FAIL(osd_show, LOG_OSD(lprintf("got OSDSHOW\n")));
/*      
      fprintf(stderr, "vdr: osdshow %d\n", data->window);
*/      
      if (data->window >= VDR_MAX_NUM_WINDOWS)
        return -1;
      
      if (0 != this->osd_window[ data->window ])
      {
        if (this->osd_unscaled_blending)
          xine_osd_show_unscaled(this->osd_window[ data->window ], 0);
        else
          xine_osd_show(this->osd_window[ data->window ], 0);
      }
    }
    break;
    
  case func_osd_hide:
    {
      READ_DATA_OR_FAIL(osd_hide, LOG_OSD(lprintf("got OSDHIDE\n")));
/*      
      fprintf(stderr, "vdr: osdhide %d\n", data->window);
*/      
      if (data->window >= VDR_MAX_NUM_WINDOWS)
        return -1;
      
      if (0 != this->osd_window[ data->window ])
      {
        if (this->osd_unscaled_blending)
          xine_osd_show_unscaled(this->osd_window[ data->window ], 0);
        else
          xine_osd_show(this->osd_window[ data->window ], 0);
      }
    }
    break;
    
  case func_osd_flush:
    {
      double _t1, _t2;
      int _n = 0;
      int _to = 0;
      int r = 0;
      
      READ_DATA_OR_FAIL(osd_flush, LOG_OSD(lprintf("got OSDFLUSH\n")));
/*      
      fprintf(stderr, "vdr: osdflush +\n");
*/
      _t1 = _now();
      
      while ((r = _x_query_unprocessed_osd_events(this->stream)))
      {
        if ((_now() - _t1) > 200)
        {
          _to = 1;
          break;
        }
/*        
        fprintf(stderr, "redraw_needed: 1\n");
*/        
/*        sched_yield(); */
        xine_usec_sleep(5000);
        _n++;
      }

      _t2 = _now();
      fprintf(stderr, "vdr: osdflush: n: %d, %.1lf, timeout: %d, result: %d\n", _n, _t2 - _t1, _to, r);
/*      
      fprintf(stderr, "redraw_needed: 0\n");        
      
      fprintf(stderr, "vdr: osdflush -\n");
*/
    }
    break;
    
  case func_osd_set_position:
    {
      READ_DATA_OR_FAIL(osd_set_position, LOG_OSD(lprintf("got OSDSETPOSITION\n")));
/*
      fprintf(stderr, "vdr: osdsetposition %d\n", data->window);    
*/      
      if (data->window >= VDR_MAX_NUM_WINDOWS)
        return -1;
      
      if (0 != this->osd_window[ data->window ])
        xine_osd_set_position(this->osd_window[ data->window ], data->x, data->y);
    }
    break;
  
  case func_osd_draw_bitmap:
    {
      READ_DATA_OR_FAIL(osd_draw_bitmap, LOG_OSD(lprintf("got OSDDRAWBITMAP\n")));
/*
      fprintf(stderr, "vdr: osddrawbitmap %d\n", data->window);    
*/      
      if (this->osd_buffer_size < this->cur_size)
      {
        if (this->osd_buffer)
          free(this->osd_buffer);
        
        this->osd_buffer_size = 0;
        
        this->osd_buffer = xine_xmalloc(this->cur_size);
        if (!this->osd_buffer)
          return -1;
        
        this->osd_buffer_size = this->cur_size;
      }
      
      n = vdr_read_abort (this->stream, this->fh_control, (char *)this->osd_buffer, this->cur_size);
      if (n != this->cur_size)
        return -1;
      
      this->cur_size -= n;
      
      if (data->window >= VDR_MAX_NUM_WINDOWS)
        return -1;
      
      if (0 != this->osd_window[ data->window ])
        xine_osd_draw_bitmap(this->osd_window[ data->window ], this->osd_buffer, data->x, data->y, data->width, data->height, 0);
    }
    break;
    
  case func_set_color:
    {
      uint32_t vdr_color[ 256 ];
      
      READ_DATA_OR_FAIL(set_color, lprintf("got SETCOLOR\n"));
      
      if (((data->num + 1) * sizeof (uint32_t)) != this->cur_size)
        return -1;
      
      n = vdr_read_abort (this->stream, this->fh_control, (char *)&vdr_color[ data->index ], this->cur_size);
      if (n != this->cur_size)
        return -1;
      
      this->cur_size -= n;
      
      if (data->window >= VDR_MAX_NUM_WINDOWS)
        return -1;
      
      if (0 != this->osd_window[ data->window ])
      {
        uint32_t color[ 256 ];
        uint8_t trans[ 256 ];
        
        xine_osd_get_palette(this->osd_window[ data->window ], color, trans);
        
        {
          int i;
          
          for (i = data->index; i <= (data->index + data->num); i++)
          {
            int a = (vdr_color[ i ] & 0xff000000) >> 0x18;
            int r = (vdr_color[ i ] & 0x00ff0000) >> 0x10;
            int g = (vdr_color[ i ] & 0x0000ff00) >> 0x08;
            int b = (vdr_color[ i ] & 0x000000ff) >> 0x00;
            
            int y  = (( 66 * r + 129 * g +  25 * b + 128) >> 8) +  16;
            int cr = ((112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
            int cb = ((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128;

            uint8_t *dst = (uint8_t *)&color[ i ];
            *dst++ = cb;
            *dst++ = cr;
            *dst++ = y;
            *dst++ = 0;

            trans[ i ] = a >> 4;
          }
        }
        
        xine_osd_set_palette(this->osd_window[ data->window ], color, trans);
      }
    }
    break;

  case func_play_external:
    {
      char file_name[ 1024 ];
      int file_name_len = 0;
      
      READ_DATA_OR_FAIL(play_external, lprintf("got PLAYEXTERNAL\n"));

      file_name_len = this->cur_size;

      if (0 != file_name_len)
      {
        if (file_name_len <= 1
            || file_name_len > sizeof (file_name))
        {
          return -1;
        }
      
        n = vdr_read_abort (this->stream, this->fh_control, file_name, file_name_len);
        if (n != file_name_len)
          return -1;
      
        if (file_name[ file_name_len - 1 ] != '\0')
          return -1;
      
        this->cur_size -= n;
      }

      lprintf((file_name_len > 0) ? "----------- play external: %s\n" : "---------- stop external\n", file_name);
      
      if (file_name_len > 0)
        external_stream_play(this, file_name);
      else
        external_stream_stop(this);
    }
    break;

  case func_clear:
    {
      READ_DATA_OR_FAIL(clear, lprintf("got CLEAR\n"));

      {
        int orig_speed = xine_get_param(this->stream, XINE_PARAM_FINE_SPEED);
        if (orig_speed <= 0)
          xine_set_param(this->stream, XINE_PARAM_FINE_SPEED, XINE_FINE_SPEED_NORMAL);
fprintf(stderr, "+++ CLEAR(%d%c)\n", data->n, data->s ? 'b' : 'a');
/*        
        if (!this->dont_change_xine_volume)
          xine_set_param(this->stream, XINE_PARAM_AUDIO_VOLUME, 0);
*/      
        _x_demux_flush_engine(this->stream);
fprintf(stderr, "=== CLEAR(%d.1)\n", data->n);
        _x_demux_control_start(this->stream);
fprintf(stderr, "=== CLEAR(%d.2)\n", data->n);
        _x_demux_seek(this->stream, 0, 0, 0);
fprintf(stderr, "=== CLEAR(%d.3)\n", data->n);

        _x_stream_info_reset(this->stream, XINE_STREAM_INFO_AUDIO_BITRATE);
fprintf(stderr, "=== CLEAR(%d.4)\n", data->n);
	_x_meta_info_reset(this->stream, XINE_META_INFO_AUDIOCODEC);
fprintf(stderr, "=== CLEAR(%d.5)\n", data->n);

        _x_trigger_relaxed_frame_drop_mode(this->stream);
/*        _x_reset_relaxed_frame_drop_mode(this->stream); */
/*      
        if (!this->dont_change_xine_volume)
          xine_set_param(this->stream, XINE_PARAM_AUDIO_VOLUME, this->last_volume);
*/
fprintf(stderr, "--- CLEAR(%d%c)\n", data->n, data->s ? 'b' : 'a');
        if (orig_speed <= 0)
          xine_set_param(this->stream, XINE_PARAM_FINE_SPEED, orig_speed);
      }
    }
    break;

  case func_first_frame:
    {
      READ_DATA_OR_FAIL(first_frame, lprintf("got FIRST FRAME\n"));

      _x_trigger_relaxed_frame_drop_mode(this->stream);
/*      _x_reset_relaxed_frame_drop_mode(this->stream); */
    }
    break;

  case func_still_frame:
    {
      READ_DATA_OR_FAIL(still_frame, lprintf("got STILL FRAME\n"));

      _x_reset_relaxed_frame_drop_mode(this->stream);
    }
    break;

  case func_set_video_window:
    {
      READ_DATA_OR_FAIL(set_video_window, lprintf("got SET VIDEO WINDOW\n"));
/*
      fprintf(stderr, "svw: (%d, %d)x(%d, %d), (%d, %d)\n", data->x, data->y, data->w, data->h, data->wRef, data->hRef);
*/    
      {
        xine_event_t event;
        vdr_set_video_window_data_t event_data;
        
        event_data.x = data->x;
        event_data.y = data->y;
        event_data.w = data->w;
        event_data.h = data->h;
        event_data.w_ref = data->w_ref;
        event_data.h_ref = data->h_ref;
        
        event.type = XINE_EVENT_VDR_SETVIDEOWINDOW;
        event.data = &event_data;
        event.data_length = sizeof (event_data);
        
        xine_event_send(this->stream, &event);
      }
    }
    break;
    
  case func_select_audio:
    {
      READ_DATA_OR_FAIL(select_audio, lprintf("got SELECT AUDIO\n"));

      this->audio_channels = data->channels;
      
      {
        xine_event_t event;
        vdr_select_audio_data_t event_data;
        
        event_data.channels = this->audio_channels;
        
        event.type = XINE_EVENT_VDR_SELECTAUDIO;
        event.data = &event_data;
        event.data_length = sizeof (event_data);
        
        xine_event_send(this->stream, &event);
      }
    }
    break;
    
  case func_trick_speed_mode:
    {
      READ_DATA_OR_FAIL(trick_speed_mode, lprintf("got TRICK SPEED MODE\n"));

      if (this->trick_speed_mode != data->on)
      {
fprintf(stderr, "���������������������������������\n");
        this->trick_speed_mode = data->on;
      
        _x_demux_seek(this->stream, 0, 0, 0);

        {
          xine_event_t event;
        
          event.type = XINE_EVENT_VDR_TRICKSPEEDMODE;
          event.data = 0;
          event.data_length = this->trick_speed_mode;
/*          fprintf(stderr, "************************: %p, %d\n", event.data, event.data_length); */
          xine_event_send(this->stream, &event);
        }
      }
    }
    break;
    
  case func_flush:
    {
      READ_DATA_OR_FAIL(flush, lprintf("got FLUSH\n"));

      if (!data->just_wait)
      {
        if (this->stream->video_fifo)
        {
          buf_element_t *buf = this->stream->video_fifo->buffer_pool_alloc(this->stream->video_fifo);
          if (!buf)
          {
            xprintf(this->stream->xine, XINE_VERBOSITY_LOG, _("%s: buffer_pool_alloc() failed!\n"), LOG_MODULE);
            return -1;
          }
          
          buf->type = BUF_CONTROL_FLUSH_DECODER;
          
          this->stream->video_fifo->put(this->stream->video_fifo, buf);
        }
      }
      
      {
        double _t1, _t2;
        int _n = 0;
        
        int vb = -1, ab = -1, vf = -1, af = -1;
        
        uint8_t timed_out = 0;
        
        struct timeval now, then;
        
        if (data->ms_timeout >= 0)
        {
          gettimeofday(&now, 0);
          
          then = now;
          then.tv_usec += (data->ms_timeout % 1000) * 1000;
          then.tv_sec  += (data->ms_timeout / 1000);
          
          if (then.tv_usec >= 1000000)
          {
            then.tv_usec -= 1000000;
            then.tv_sec  += 1;
          }
        }
        else
        {
          then.tv_usec = 0;
          then.tv_sec  = 0;
        }

        _t1 = _now();
        
        while (1)
        {
          _x_query_buffer_usage(this->stream, &vb, &ab, &vf, &af);
          
          if (vb <= 0 && ab <= 0 && vf <= 0 && af <= 0)
            break;
          
          if (data->ms_timeout >= 0
              && timercmp(&now, &then, >=))
          {
            timed_out++;
            break;
          }
          
/*          sched_yield(); */
          xine_usec_sleep(5000);
          _n++;
          
          if (data->ms_timeout >= 0)
            gettimeofday(&now, 0);          
        }

        _t2 = _now();
        fprintf(stderr, "vdr: flush: n: %d, %.1lf\n", _n, _t2 - _t1);
        
        xprintf(this->stream->xine
                , XINE_VERBOSITY_LOG
                , _("%s: flush buffers (vb: %d, ab: %d, vf: %d, af: %d) %s.\n")
                , LOG_MODULE, vb, ab, vf, af
                , (timed_out ? "timed out" : "done"));
        
        {
          result_flush_t result_flush;
          result_flush.header.func = data->header.func;
          result_flush.header.len = sizeof (result_flush);
          
          result_flush.timed_out = timed_out;
          
          if (sizeof (result_flush) != vdr_write(this->fh_result, &result_flush, sizeof (result_flush)))
            return -1;
        }
      }
    }
    break;

  case func_mute:
    {
      READ_DATA_OR_FAIL(mute, lprintf("got MUTE\n"));

      { 
        int param_mute = (this->volume_mode == INPUT_VDR_VOLUME_CHANGE_SW) ? XINE_PARAM_AUDIO_AMP_MUTE : XINE_PARAM_AUDIO_MUTE;
        xine_set_param(this->stream, param_mute, data->mute);
      }
    }
    break;

  case func_set_volume:
    {
      READ_DATA_OR_FAIL(set_volume, lprintf("got SETVOLUME\n"));

      {
        int change_volume = (this->volume_mode != INPUT_VDR_VOLUME_IGNORE);
        int do_mute   = (this->last_volume != 0 && 0 == data->volume);
        int do_unmute = (this->last_volume <= 0 && 0 != data->volume);
        int report_change = 0;

        int param_mute   = (this->volume_mode == INPUT_VDR_VOLUME_CHANGE_SW) ? XINE_PARAM_AUDIO_AMP_MUTE  : XINE_PARAM_AUDIO_MUTE;
        int param_volume = (this->volume_mode == INPUT_VDR_VOLUME_CHANGE_SW) ? XINE_PARAM_AUDIO_AMP_LEVEL : XINE_PARAM_AUDIO_VOLUME;
        
        this->last_volume = data->volume;

        if (do_mute || do_unmute)
        {
          switch (this->mute_mode)
          {
          case INPUT_VDR_MUTE_EXECUTE:
            report_change = 1;
            xine_set_param(this->stream, param_mute, do_mute);
            
          case INPUT_VDR_MUTE_IGNORE:
            if (do_mute)
              change_volume = 0;
            break;

          case INPUT_VDR_MUTE_SIMULATE:
            change_volume = 1;
            break;

          default:
            return -1;
          };
        }
        
        if (change_volume)
        {
          report_change = 1;
          xine_set_param(this->stream, param_volume, this->last_volume);
        }

        if (report_change && this->volume_mode != INPUT_VDR_VOLUME_CHANGE_SW)
        {
          xine_event_t            event;
          xine_audio_level_data_t data;
          
          data.left
            = data.right
            = xine_get_param(this->stream, param_volume);
          data.mute
            = xine_get_param(this->stream, param_mute);
          
          event.type        = XINE_EVENT_AUDIO_LEVEL;
          event.data        = &data;
          event.data_length = sizeof (data);
          
          xine_event_send(this->stream, &event);
        }
      }
    }
    break;

  case func_set_speed:
    {
      READ_DATA_OR_FAIL(set_speed, lprintf("got SETSPEED\n"));
      
      lprintf("... got SETSPEED %d\n", data->speed);
      
      if (data->speed != xine_get_param(this->stream, XINE_PARAM_FINE_SPEED))
        xine_set_param(this->stream, XINE_PARAM_FINE_SPEED, data->speed);
    }
    break;

  case func_set_prebuffer:
    {
      READ_DATA_OR_FAIL(set_prebuffer, lprintf("got SETPREBUFFER\n"));

      xine_set_param(this->stream, XINE_PARAM_METRONOM_PREBUFFER, data->prebuffer);
    }
    break;

  case func_metronom:
    {
      READ_DATA_OR_FAIL(metronom, lprintf("got METRONOM\n"));

      _x_demux_control_newpts(this->stream, data->pts, data->flags);
    }
    break;
    
  case func_start:
    {
      READ_DATA_OR_FAIL(start, lprintf("got START\n"));

      _x_demux_control_start(this->stream);
      _x_demux_seek(this->stream, 0, 0, 0);
    }
    break;

  case func_wait:
    {
      READ_DATA_OR_FAIL(wait, lprintf("got WAIT\n"));

      {
        result_wait_t result_wait;
        result_wait.header.func = data->header.func;
        result_wait.header.len = sizeof (result_wait);
          
        if (sizeof (result_wait) != vdr_write(this->fh_result, &result_wait, sizeof (result_wait)))
          return -1;
      }
    }
    break;

  case func_setup:
    {
      READ_DATA_OR_FAIL(setup, lprintf("got SETUP\n"));

      this->osd_unscaled_blending = data->osd_unscaled_blending;
      this->volume_mode           = data->volume_mode;
      this->mute_mode             = data->mute_mode;
      this->image4_3_zoom_x       = data->image4_3_zoom_x;
      this->image4_3_zoom_y       = data->image4_3_zoom_y;
      this->image16_9_zoom_x      = data->image16_9_zoom_x;
      this->image16_9_zoom_y      = data->image16_9_zoom_y;

      adjust_zoom(this);
    }
    break;

  case func_grab_image:
    {
      READ_DATA_OR_FAIL(grab_image, lprintf("got GRABIMAGE\n"));

      {
        off_t ret_val = -1;
        
        uint8_t *img   = 0;
        int frame_size = 0;
        int width      = 0;
        int height     = 0;       
        int ratio_code = 0;
        int format     = 0;
        
        int orig_speed = xine_get_param(this->stream, XINE_PARAM_FINE_SPEED);
        if (XINE_SPEED_PAUSE != orig_speed)
          xine_set_param(this->stream, XINE_PARAM_FINE_SPEED, XINE_SPEED_PAUSE);
        
        if (xine_get_current_frame(this->stream, &width, &height, &ratio_code, &format, 0))
        {
          switch (format)
          {
          case XINE_IMGFMT_YV12:
            frame_size = width * height
              + ((width + 1) / 2) * ((height + 1) / 2)
              + ((width + 1) / 2) * ((height + 1) / 2);
            break;
            
          case XINE_IMGFMT_YUY2:
            frame_size = width * height
              + ((width + 1) / 2) * height
              + ((width + 1) / 2) * height;
            break;
          }
          
          img = xine_xmalloc(frame_size);
          
          if (!xine_get_current_frame(this->stream, &width, &height, &ratio_code, &format, img))
            frame_size = 0;
          
          if (ratio_code == XINE_VO_ASPECT_SQUARE)
            ratio_code = 10000;
          else if (ratio_code == XINE_VO_ASPECT_4_3)
            ratio_code = 13333;
          else if (ratio_code == XINE_VO_ASPECT_ANAMORPHIC)
            ratio_code = 17778;
          else if (ratio_code == XINE_VO_ASPECT_DVB)
            ratio_code = 21100;
          
          if (0 == frame_size)
          {
            width      = 0;
            height     = 0;
            ratio_code = 0;
          }          
        }
        
        if (XINE_SPEED_PAUSE != orig_speed)
          xine_set_param(this->stream, XINE_PARAM_FINE_SPEED, orig_speed);
        
        {
          result_grab_image_t result_grab_image;
          result_grab_image.header.func = data->header.func;
          result_grab_image.header.len = sizeof (result_grab_image) + frame_size;
          
          result_grab_image.width  = width;
          result_grab_image.height = height;
          result_grab_image.ratio  = ratio_code;
          result_grab_image.format = format;
          
          if (sizeof (result_grab_image) == vdr_write(this->fh_result, &result_grab_image, sizeof (result_grab_image)))
          {
            if (frame_size == vdr_write(this->fh_result, img, frame_size))
              ret_val = 0;
          }
        }
        
        if (img)
          free(img);
        
        if (ret_val != 0)
          return ret_val;
      }
    }
    break;
    
  case func_get_pts:
    {
      READ_DATA_OR_FAIL(get_pts, lprintf("got GETPTS\n"));
      
      {
        result_get_pts_t result_get_pts;
        result_get_pts.header.func = data->header.func;
        result_get_pts.header.len = sizeof (result_get_pts);
        
        result_get_pts.pts = xine_get_current_vpts(this->stream) - this->stream->metronom->get_option(this->stream->metronom, METRONOM_VPTS_OFFSET);
        
        if (sizeof (result_get_pts) != vdr_write(this->fh_result, &result_get_pts, sizeof (result_get_pts)))
          return -1;
      }
    }
    break;

  case func_get_version:
    {
      READ_DATA_OR_FAIL(get_version, lprintf("got GETVERSION\n"));
      
      {
        result_get_version_t result_get_version;
        result_get_version.header.func = data->header.func;
        result_get_version.header.len = sizeof (result_get_version);
        
        result_get_version.version = XINE_INPUT_VDR_VERSION;
        
        if (sizeof (result_get_version) != vdr_write(this->fh_result, &result_get_version, sizeof (result_get_version)))
          return -1;
      }
    }
    break;

  case func_video_size:
    {
      READ_DATA_OR_FAIL(video_size, lprintf("got VIDEO SIZE\n"));

      {
        int format;
        
        result_video_size_t result_video_size;
        result_video_size.header.func = data->header.func;
        result_video_size.header.len = sizeof (result_video_size);
        
        result_video_size.top    = -1;
        result_video_size.left   = -1;
        result_video_size.width  = -1;
        result_video_size.height = -1;
        result_video_size.ratio  = 0;
        
        xine_get_current_frame(this->stream, &result_video_size.width, &result_video_size.height, &result_video_size.ratio, &format, 0);
        
        if (result_video_size.ratio == XINE_VO_ASPECT_SQUARE)
          result_video_size.ratio = 10000;
        else if (result_video_size.ratio == XINE_VO_ASPECT_4_3)
          result_video_size.ratio = 13333;
        else if (result_video_size.ratio == XINE_VO_ASPECT_ANAMORPHIC)
          result_video_size.ratio = 17778;
        else if (result_video_size.ratio == XINE_VO_ASPECT_DVB)
          result_video_size.ratio = 21100;
        
        if (0 != this->frame_size.x
            || 0 != this->frame_size.y
            || 0 != this->frame_size.w
            || 0 != this->frame_size.h)
        {
          result_video_size.left   = this->frame_size.x;
          result_video_size.top    = this->frame_size.y;
          result_video_size.width  = this->frame_size.w;
          result_video_size.height = this->frame_size.h;
        }
//fprintf(stderr, "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE\n");
        result_video_size.zoom_x = xine_get_param(this->stream, XINE_PARAM_VO_ZOOM_X);
        result_video_size.zoom_y = xine_get_param(this->stream, XINE_PARAM_VO_ZOOM_Y);
//fprintf(stderr, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n");
        if (sizeof (result_video_size) != vdr_write(this->fh_result, &result_video_size, sizeof (result_video_size)))
          return -1;
//fprintf(stderr, "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG\n");
      }
    }
    break;

  case func_reset_audio:
    {
      double _t1, _t2;
      int _n = 0;
      
      READ_DATA_OR_FAIL(reset_audio, lprintf("got RESET AUDIO\n"));

      if (this->stream->audio_fifo)
      {
        xine_set_param(this->stream, XINE_PARAM_IGNORE_AUDIO, 1);
        xine_set_param(this->stream, XINE_PARAM_AUDIO_CHANNEL_LOGICAL, -2);

        _t1 = _now();
        
        while (1)
        {
          int n = xine_get_stream_info(this->stream, XINE_STREAM_INFO_MAX_AUDIO_CHANNEL);
          if (n <= 0)
            break;
          
          /* keep the decoder running */
          if (this->stream->audio_fifo)
          {
            buf_element_t *buf = this->stream->audio_fifo->buffer_pool_alloc(this->stream->audio_fifo);
            if (!buf)
            {
              xprintf(this->stream->xine, XINE_VERBOSITY_LOG, _("%s: buffer_pool_alloc() failed!\n"), LOG_MODULE);
              return -1;
            }
            
            buf->type = BUF_CONTROL_RESET_TRACK_MAP;
            
            this->stream->audio_fifo->put(this->stream->audio_fifo, buf);
          }
          
/*          sched_yield(); */
          xine_usec_sleep(5000);
          _n++;
        }

        _t2 = _now();
        fprintf(stderr, "vdr: reset_audio: n: %d, %.1lf\n", _n, _t2 - _t1);
        
        xine_set_param(this->stream, XINE_PARAM_AUDIO_CHANNEL_LOGICAL, -1);

        _x_stream_info_reset(this->stream, XINE_STREAM_INFO_AUDIO_BITRATE);
        _x_meta_info_reset(this->stream, XINE_META_INFO_AUDIOCODEC);

        xine_set_param(this->stream, XINE_PARAM_IGNORE_AUDIO, 0);
      }
    }
    break;
    
  default:
    lprintf("unknown function: %d\n", this->cur_func);
  }
    
  if (this->cur_size != this->cur_done)
  {
    off_t skip = this->cur_size - this->cur_done;

    lprintf("func: %d, skipping: %lld\n", this->cur_func, skip);
     
    while (skip > BUF_SIZE)
    {
      n = vdr_read_abort(this->stream, this->fh_control, this->seek_buf, BUF_SIZE);
      if (n != BUF_SIZE)
        return -1;

      skip -= BUF_SIZE;
    }

    n = vdr_read_abort(this->stream, this->fh_control, this->seek_buf, skip);
    if (n != skip)
      return -1;

    this->cur_done = this->cur_size;

    return -1;
  }

  return 0;
}

static void *vdr_rpc_thread_loop(void *arg)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)arg;
  int frontend_lock_failures = 0;
  int failed = 0;
  
  while (!failed
    && !this->rpc_thread_shutdown)
  {
    struct timeval timeout;
    fd_set rset;
  
    FD_ZERO(&rset);
    FD_SET(this->fh_control, &rset);

    timeout.tv_sec  = 0;
    timeout.tv_usec = 50000;

    if (select(this->fh_control + 1, &rset, NULL, NULL, &timeout) > 0)
    {
      if (!_x_lock_frontend(this->stream, 100))
      {
        if (++frontend_lock_failures > 50)
        {
          failed = 1;
          xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
                  LOG_MODULE ": locking frontend for rpc command execution failed, exiting ...\n");
        }
      }
      else
      {
        frontend_lock_failures = 0;

        if (_x_lock_port_rewiring(this->stream->xine, 100))
        {
          if (vdr_execute_rpc_command(this) < 0)
          {
            failed = 1;
            xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
                    LOG_MODULE ": execution of rpc command %d (%s) failed, exiting ...\n", this->cur_func, "");
          }

          _x_unlock_port_rewiring(this->stream->xine);
        }

        _x_unlock_frontend(this->stream);
      }
    }
  }

  /* close control and result channel here to have vdr-xine initiate a disconnect for the above error case ... */
  close(this->fh_control);
  this->fh_control = -1;
  
  close(this->fh_result);
  this->fh_result = -1;
  
  xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
          LOG_MODULE ": rpc thread done.\n");

  pthread_mutex_lock(&this->rpc_thread_shutdown_lock);
  this->rpc_thread_shutdown = -1;
  pthread_cond_broadcast(&this->rpc_thread_shutdown_cond);
  pthread_mutex_unlock(&this->rpc_thread_shutdown_lock);
  
  return 0;
}

static int internal_write_event_key(vdr_input_plugin_t *this, uint32_t key)
{
  event_key_t event;
  event.header.func = func_key;
  event.header.len = sizeof (event);

  event.key = key;

  if (sizeof (event) != vdr_write(this->fh_event, &event, sizeof (event)))
    return -1;
  
  return 0;
}

static int internal_write_event_frame_size(vdr_input_plugin_t *this)
{
  event_frame_size_t event;
  event.header.func = func_frame_size;
  event.header.len = sizeof (event);

  event.left   = this->frame_size.x;
  event.top    = this->frame_size.y;
  event.width  = this->frame_size.w,
  event.height = this->frame_size.h;
  event.zoom_x = xine_get_param(this->stream, XINE_PARAM_VO_ZOOM_X);
  event.zoom_y = xine_get_param(this->stream, XINE_PARAM_VO_ZOOM_Y);

  if (sizeof (event) != vdr_write(this->fh_event, &event, sizeof (event)))
    return -1;
  
  return 0;
}

static int internal_write_event_play_external(vdr_input_plugin_t *this, uint32_t key)
{
  event_play_external_t event;
  event.header.func = func_play_external;
  event.header.len = sizeof (event);

  event.key = key;

  if (sizeof (event) != vdr_write(this->fh_event, &event, sizeof (event)))
    return -1;
  
  return 0;
}

static off_t vdr_plugin_read(input_plugin_t *this_gen, 
                             void *buf_gen, off_t len)
{
  vdr_input_plugin_t  *this = (vdr_input_plugin_t *) this_gen;
  char *buf = (char *)buf_gen;
  off_t n, total;
#ifdef LOG_READ   
  lprintf ("reading %lld bytes...\n", len);
#endif   
  total=0;
  if (this->curpos < this->preview_size)
  {
    n = this->preview_size - this->curpos;
    if (n > (len - total))
      n = len - total;
#ifdef LOG_READ
    lprintf ("%lld bytes from preview (which has %lld bytes)\n",
            n, this->preview_size);
#endif
    memcpy (&buf[total], &this->preview[this->curpos], n);
    this->curpos += n;
    total += n;
  }
   
  if( (len-total) > 0 )
  {
    int retries = 0;
    do
    {
      n = vdr_read_abort (this->stream, this->fh, &buf[total], len-total);
      if (0 == n)
        lprintf("read 0, retries: %d\n", retries);
    }
    while (0 == n
           && !this->stream_external
           && _x_continue_stream_processing(this->stream)
           && 200 > retries++); /* 200 * 50ms */
#ifdef LOG_READ      
    lprintf ("got %lld bytes (%lld/%lld bytes read)\n",
            n,total,len);
#endif
    if (n < 0)
    {
      _x_message(this->stream, XINE_MSG_READ_ERROR, NULL);
      return 0;
    }

    this->curpos += n;
    total += n;
  }
  return total;
}

static buf_element_t *vdr_plugin_read_block(input_plugin_t *this_gen, fifo_buffer_t *fifo, 
                                            off_t todo)
{
  off_t          total_bytes;
  buf_element_t *buf = fifo->buffer_pool_alloc(fifo);

  buf->content = buf->mem;
  buf->type = BUF_DEMUX_BLOCK;

  total_bytes = vdr_plugin_read(this_gen, (char *)buf->content, todo);

  if (total_bytes != todo)
  {
    buf->free_buffer(buf);
    return NULL;
  }

  buf->size = total_bytes;

  return buf;
}

/* forward reference */
static off_t vdr_plugin_get_current_pos(input_plugin_t *this_gen);

static off_t vdr_plugin_seek(input_plugin_t *this_gen, off_t offset, int origin)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;

  lprintf("seek %lld offset, %d origin...\n",
          offset, origin);

  if ((origin == SEEK_CUR) && (offset >= 0))
  {
    for ( ; ((int)offset) - BUF_SIZE > 0; offset -= BUF_SIZE)
    {
      if (!this_gen->read(this_gen, this->seek_buf, BUF_SIZE))
        return this->curpos;
    }

    this_gen->read (this_gen, this->seek_buf, offset);
  }

  if (origin == SEEK_SET)
  {
    if (offset < this->curpos)
    {
      if (this->curpos <= this->preview_size) 
        this->curpos = offset;
      else
        lprintf("cannot seek back! (%lld > %lld)\n", this->curpos, offset);
    }
    else
    {
      offset -= this->curpos;

      for ( ; ((int)offset) - BUF_SIZE > 0; offset -= BUF_SIZE)
      {
        if (!this_gen->read(this_gen, this->seek_buf, BUF_SIZE))
          return this->curpos;
      }

      this_gen->read(this_gen, this->seek_buf, offset);
    }
  }

  return this->curpos;
}

static off_t vdr_plugin_get_length(input_plugin_t *this_gen)
{
  return 0;
}

static uint32_t vdr_plugin_get_capabilities(input_plugin_t *this_gen)
{
  return INPUT_CAP_NOCAP; /* INPUT_CAP_PREVIEW; */
}

static uint32_t vdr_plugin_get_blocksize(input_plugin_t *this_gen)
{
  return 0;
}

static off_t vdr_plugin_get_current_pos(input_plugin_t *this_gen)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;

  return this->curpos;
}

static const char *vdr_plugin_get_mrl(input_plugin_t *this_gen)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;

  return this->mrl;
}

static void vdr_plugin_dispose(input_plugin_t *this_gen)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;
  int i;

  external_stream_stop(this);

  if (this->event_queue)
    xine_event_dispose_queue(this->event_queue);

  if (this->rpc_thread)
  {
    struct timespec abstime;
    int ms_to_time_out = 10000;

    xprintf(this->stream->xine, XINE_VERBOSITY_LOG, _("%s: shutting down rpc thread (timeout: %d ms) ...\n"), LOG_MODULE, ms_to_time_out);

    pthread_mutex_lock(&this->rpc_thread_shutdown_lock);

    if (this->rpc_thread_shutdown > -1)
    {
      this->rpc_thread_shutdown = 1;
      
      {
        struct timeval now;
        gettimeofday(&now, 0);
        
        abstime.tv_sec = now.tv_sec + ms_to_time_out / 1000;
        abstime.tv_nsec = now.tv_usec * 1000 + (ms_to_time_out % 1000) * 1e6;
        
        if (abstime.tv_nsec > 1e9)
        {
          abstime.tv_nsec -= 1e9;
          abstime.tv_sec++;
        }
      }
      
      if (0 != pthread_cond_timedwait(&this->rpc_thread_shutdown_cond, &this->rpc_thread_shutdown_lock, &abstime))
      {
        xprintf(this->stream->xine, XINE_VERBOSITY_LOG, _("%s: cancelling rpc thread in function %d...\n"), LOG_MODULE, this->cur_func);
        pthread_cancel(this->rpc_thread);
      }
    }
    
    pthread_mutex_unlock(&this->rpc_thread_shutdown_lock);
    
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG, _("%s: joining rpc thread ...\n"), LOG_MODULE);
    pthread_join(this->rpc_thread, 0);
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG, _("%s: rpc thread joined.\n"), LOG_MODULE);
  }

  pthread_cond_destroy(&this->rpc_thread_shutdown_cond);
  pthread_mutex_destroy(&this->rpc_thread_shutdown_lock);

  pthread_mutex_destroy(&this->adjust_zoom_lock);
  
  if (this->fh_result != -1)
    close(this->fh_result);
  
  if (this->fh_control != -1)
    close(this->fh_control);

  if (this->fh_event != -1)
    close(this->fh_event);
  
  for (i = 0; i < VDR_MAX_NUM_WINDOWS; i++)
  {
    if (0 == this->osd_window[ i ])
      continue;

    xine_osd_hide(this->osd_window[ i ], 0);
    xine_osd_free(this->osd_window[ i ]);
  }

  if (this->osd_buffer)
    free(this->osd_buffer);
  
  if ((this->fh != STDIN_FILENO) && (this->fh != -1))
    close(this->fh);

  free(this->mrl);
  free(this);
}

static int vdr_plugin_get_optional_data(input_plugin_t *this_gen, 
                                        void *data, int data_type)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;
  int preview_size = (this->preview_size > MAX_PREVIEW_SIZE) ? MAX_PREVIEW_SIZE : this->preview_size;
  (void)preview_size;
/*
  switch (data_type)
  {
  case INPUT_OPTIONAL_DATA_PREVIEW:
    memcpy (data, this->preview, preview_size);
    return preview_size;
  }
*/  
  return INPUT_OPTIONAL_UNSUPPORTED;
}

static inline const char *mrl_to_fifo (const char *mrl)
{
  /* vdr://foo -> /foo */
  return mrl + 3 + strspn (mrl + 4, "/");
}

static inline const char *mrl_to_host (const char *mrl)
{
  /* netvdr://host:port -> host:port */
  return strrchr (mrl, '/') + 1;
}

static int vdr_plugin_open_fifo_mrl(input_plugin_t *this_gen)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;
  char *filename = strdup (mrl_to_fifo (this->mrl));

  _x_mrl_unescape (filename);
  this->fh = open(filename, O_RDONLY | O_NONBLOCK);

  lprintf("filename '%s'\n", filename);

  if (this->fh == -1)
  {
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
            _("%s: failed to open '%s' (%s)\n"), LOG_MODULE,
            filename,
            strerror(errno));
    free (filename);
    return 0;
  }

  {
    struct pollfd poll_fh = { this->fh, POLLIN, 0 };

    int r = poll(&poll_fh, 1, 300);
    if (1 != r)
    {
      xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
              _("%s: failed to open '%s' (%s)\n"), LOG_MODULE,
              filename,
              _("timeout expired during setup phase"));
      free (filename);
      return 0;
    }
  }

  fcntl(this->fh, F_SETFL, ~O_NONBLOCK & fcntl(this->fh, F_GETFL, 0));

  {
    char *filename_control = 0;
    asprintf(&filename_control, "%s.control", filename);

    this->fh_control = open(filename_control, O_RDONLY);

    if (this->fh_control == -1) {
      xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
              _("%s: failed to open '%s' (%s)\n"), LOG_MODULE,
              filename_control,
              strerror(errno));

      free(filename_control);
      free (filename);
      return 0;
    }

    free(filename_control);
  }

  {
    char *filename_result = 0;
    asprintf(&filename_result, "%s.result", filename);

    this->fh_result = open(filename_result, O_WRONLY);

    if (this->fh_result == -1) {
      perror("failed");

      xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
              _("%s: failed to open '%s' (%s)\n"), LOG_MODULE,
              filename_result,
              strerror(errno));

      free(filename_result);
      free (filename);
      return 0;
    }

    free(filename_result);
  }

  {
    char *filename_event = 0;
    asprintf(&filename_event, "%s.event", filename);

    this->fh_event = open(filename_event, O_WRONLY);

    if (this->fh_event == -1) {
      perror("failed");

      xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
              _("%s: failed to open '%s' (%s)\n"), LOG_MODULE,
              filename_event,
              strerror(errno));

      free(filename_event);
      free (filename);
      return 0;
    }

    free(filename_event);
  }
 
  free (filename);
  return 1;
}

static int vdr_plugin_open_socket(vdr_input_plugin_t *this, struct hostent *host, unsigned short port)
{
  int fd;
  struct sockaddr_in sain;
  struct in_addr iaddr;

  if ((fd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
  {
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
            _("%s: failed to create socket for port %d (%s)\n"), LOG_MODULE,
            port, strerror(errno));
    return -1;
  }

  iaddr.s_addr = *((unsigned int *)host->h_addr_list[0]);

  sain.sin_port = htons(port);
  sain.sin_family = AF_INET;
  sain.sin_addr = iaddr;

  if (connect(fd, (struct sockaddr *)&sain, sizeof (sain)) < 0)
  {
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
            _("%s: failed to connect to port %d (%s)\n"), LOG_MODULE, port,
            strerror(errno));

    return -1;
  }

  xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
          _("%s: socket opening (port %d) successful, fd = %d\n"), LOG_MODULE, port, fd);

  return fd;
}    

static int vdr_plugin_open_sockets(vdr_input_plugin_t *this)
{
  struct hostent *host;
  char *mrl_host = strdup (mrl_to_host (this->mrl));
  char *mrl_port;
  int port = 18701;

  mrl_port = strchr(mrl_host, '#');
  if (mrl_port)
    *mrl_port = 0; /* strip off things like '#demux:mpeg_pes' */

  _x_mrl_unescape (mrl_host);

  mrl_port = strchr(mrl_host, ':');
  if (mrl_port)
  {
    port = atoi(mrl_port + 1);
    *mrl_port = 0;
  }

  host = gethostbyname(mrl_host);
 
  xprintf(this->stream->xine, XINE_VERBOSITY_LOG, 
          _("%s: connecting to vdr.\n"), LOG_MODULE);

  if (!host)
  {
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
            _("%s: failed to resolve hostname '%s' (%s)\n"), LOG_MODULE,
            mrl_host,
            strerror(errno));
    free (mrl_host);
    return 0;
  }
  free (mrl_host);

  if ((this->fh = vdr_plugin_open_socket(this, host, port + 0)) == -1)
    return 0;
  
  fcntl(this->fh, F_SETFL, ~O_NONBLOCK & fcntl(this->fh, F_GETFL, 0));
  
  if ((this->fh_control = vdr_plugin_open_socket(this, host, port + 1)) == -1)
    return 0;

  if ((this->fh_result = vdr_plugin_open_socket(this, host, port + 2)) == -1)
    return 0;

  if ((this->fh_event = vdr_plugin_open_socket(this, host, port + 3)) == -1)
    return 0;

  xprintf(this->stream->xine, XINE_VERBOSITY_LOG,
          _("%s: connecting to all sockets (port %d .. %d) was successful.\n"), LOG_MODULE, port, port + 3);

  return 1;
}

static int vdr_plugin_open_socket_mrl(input_plugin_t *this_gen)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;

  lprintf("input_vdr: connecting to vdr-xine-server...\n");
 
  if (!vdr_plugin_open_sockets(this))
    return 0;
   
  return 1;
}

static int vdr_plugin_open(input_plugin_t *this_gen)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)this_gen;

  lprintf("trying to open '%s'...\n", this->mrl);

  if (this->fh == -1) 
  {
    int err = 0;

    if (!strncasecmp(&this->mrl[0], "vdr:/", 5))
    {
      if (!vdr_plugin_open_fifo_mrl(this_gen))
        return 0;
    }
    else if (!strncasecmp(&this->mrl[0], "netvdr:/", 8))
    {
      if (!vdr_plugin_open_socket_mrl(this_gen))
        return 0;
    }
    else
    {
      xprintf(this->stream->xine, XINE_VERBOSITY_LOG, 
              _("%s: MRL (%s) invalid! MRL should start with vdr://path/to/fifo/stream or netvdr://host:port where ':port' is optional.\n"), LOG_MODULE,
              strerror(err));
      return 0;
    }

    this->rpc_thread_shutdown = 0;
    if ((err = pthread_create(&this->rpc_thread, NULL,
                              vdr_rpc_thread_loop, (void *)this)) != 0)
    {
      xprintf(this->stream->xine, XINE_VERBOSITY_LOG, 
              _("%s: can't create new thread (%s)\n"), LOG_MODULE,
              strerror(err));
      
      return 0;
    }
  }

  /*
   * mrl accepted and opened successfully at this point
   *
   * => create plugin instance
   */

  this->preview      = NULL;
  this->preview_size = 0;
  this->curpos       = 0;

  return 1;
}

static void event_handler(void *user_data, const xine_event_t *event)
{
  vdr_input_plugin_t *this = (vdr_input_plugin_t *)user_data;
  uint32_t key = key_none;

  lprintf("eventHandler(): event->type: %d\n", event->type);

  if (XINE_EVENT_VDR_FRAMESIZECHANGED == event->type)
  {
    memcpy(&this->frame_size, event->data, event->data_length);

    if (0 != internal_write_event_frame_size(this))
      xprintf(this->stream->xine, XINE_VERBOSITY_LOG, 
              _("%s: input event write: %s.\n"), LOG_MODULE, strerror(errno));

    adjust_zoom(this);
    return;
  }
  else if (XINE_EVENT_VDR_PLUGINSTARTED == event->type)
  {
    if (0 == event->data_length) /* vdr_video */
    {
      xine_event_t event;
        
      event.type = XINE_EVENT_VDR_TRICKSPEEDMODE;
      event.data = 0;
      event.data_length = this->trick_speed_mode;

      xine_event_send(this->stream, &event);
    }
    else if (1 == event->data_length) /* vdr_audio */
    {
      xine_event_t event;
      vdr_select_audio_data_t event_data;
        
      event_data.channels = this->audio_channels;
        
      event.type = XINE_EVENT_VDR_SELECTAUDIO;
      event.data = &event_data;
      event.data_length = sizeof (event_data);
        
      xine_event_send(this->stream, &event);
    }
    else
    {
      fprintf(stderr, "input_vdr: illegal XINE_EVENT_VDR_PLUGINSTARTED: %d\n", event->data_length);
    }
  }

  switch (event->type)
  {
  case XINE_EVENT_INPUT_UP:            key = key_up;               break;
  case XINE_EVENT_INPUT_DOWN:          key = key_down;             break;
  case XINE_EVENT_INPUT_LEFT:          key = key_left;             break;
  case XINE_EVENT_INPUT_RIGHT:         key = key_right;            break;
  case XINE_EVENT_INPUT_SELECT:        key = key_ok;               break;
  case XINE_EVENT_VDR_BACK:            key = key_back;             break;
  case XINE_EVENT_VDR_CHANNELPLUS:     key = key_channel_plus;     break;
  case XINE_EVENT_VDR_CHANNELMINUS:    key = key_channel_minus;    break;
  case XINE_EVENT_VDR_RED:             key = key_red;              break;
  case XINE_EVENT_VDR_GREEN:           key = key_green;            break;
  case XINE_EVENT_VDR_YELLOW:          key = key_yellow;           break;
  case XINE_EVENT_VDR_BLUE:            key = key_blue;             break;
  case XINE_EVENT_VDR_PLAY:            key = key_play;             break;
  case XINE_EVENT_VDR_PAUSE:           key = key_pause;            break;
  case XINE_EVENT_VDR_STOP:            key = key_stop;             break;
  case XINE_EVENT_VDR_RECORD:          key = key_record;           break;
  case XINE_EVENT_VDR_FASTFWD:         key = key_fast_fwd;         break;
  case XINE_EVENT_VDR_FASTREW:         key = key_fast_rew;         break;
  case XINE_EVENT_VDR_POWER:           key = key_power;            break;
  case XINE_EVENT_VDR_SCHEDULE:        key = key_schedule;         break;
  case XINE_EVENT_VDR_CHANNELS:        key = key_channels;         break;
  case XINE_EVENT_VDR_TIMERS:          key = key_timers;           break;
  case XINE_EVENT_VDR_RECORDINGS:      key = key_recordings;       break;
  case XINE_EVENT_INPUT_MENU1:         key = key_menu;             break;
  case XINE_EVENT_VDR_SETUP:           key = key_setup;            break;
  case XINE_EVENT_VDR_COMMANDS:        key = key_commands;         break;
  case XINE_EVENT_INPUT_NUMBER_0:      key = key_0;                break;
  case XINE_EVENT_INPUT_NUMBER_1:      key = key_1;                break;
  case XINE_EVENT_INPUT_NUMBER_2:      key = key_2;                break;
  case XINE_EVENT_INPUT_NUMBER_3:      key = key_3;                break;
  case XINE_EVENT_INPUT_NUMBER_4:      key = key_4;                break;
  case XINE_EVENT_INPUT_NUMBER_5:      key = key_5;                break;
  case XINE_EVENT_INPUT_NUMBER_6:      key = key_6;                break;
  case XINE_EVENT_INPUT_NUMBER_7:      key = key_7;                break;
  case XINE_EVENT_INPUT_NUMBER_8:      key = key_8;                break;
  case XINE_EVENT_INPUT_NUMBER_9:      key = key_9;                break;
  case XINE_EVENT_VDR_USER1:           key = key_user1;            break;
  case XINE_EVENT_VDR_USER2:           key = key_user2;            break;
  case XINE_EVENT_VDR_USER3:           key = key_user3;            break;
  case XINE_EVENT_VDR_USER4:           key = key_user4;            break;
  case XINE_EVENT_VDR_USER5:           key = key_user5;            break;
  case XINE_EVENT_VDR_USER6:           key = key_user6;            break;
  case XINE_EVENT_VDR_USER7:           key = key_user7;            break;
  case XINE_EVENT_VDR_USER8:           key = key_user8;            break;
  case XINE_EVENT_VDR_USER9:           key = key_user9;            break;
  case XINE_EVENT_VDR_VOLPLUS:         key = key_volume_plus;      break;
  case XINE_EVENT_VDR_VOLMINUS:        key = key_volume_minus;     break;
  case XINE_EVENT_VDR_MUTE:            key = key_mute;             break;
  case XINE_EVENT_VDR_AUDIO:           key = key_audio;            break;
  case XINE_EVENT_VDR_INFO:            key = key_info;             break;
  case XINE_EVENT_VDR_CHANNELPREVIOUS: key = key_channel_previous; break;
  case XINE_EVENT_INPUT_NEXT:          key = key_next;             break;
  case XINE_EVENT_INPUT_PREVIOUS:      key = key_previous;         break;
  case XINE_EVENT_VDR_SUBTITLES:       key = key_subtitles;        break;
  default:
    return;
  }

  if (0 != internal_write_event_key(this, key))
    xprintf(this->stream->xine, XINE_VERBOSITY_LOG, 
            _("%s: input event write: %s.\n"), LOG_MODULE, strerror(errno));
}

static input_plugin_t *vdr_class_get_instance(input_class_t *cls_gen, xine_stream_t *stream,
                                               const char *data)
{
  vdr_input_plugin_t *this;
  char               *mrl = strdup(data);

  if (!strncasecmp(mrl, "vdr:/", 5))
    lprintf("filename '%s'\n", mrl_to_path (mrl));
  else if (!strncasecmp(mrl, "netvdr:/", 5))
    lprintf("host '%s'\n", mrl_to_socket (mrl));
  else
  {
    free(mrl);
    return NULL;
  }

  /*
   * mrl accepted and opened successfully at this point
   *
   * => create plugin instance
   */

  this = (vdr_input_plugin_t *)xine_xmalloc(sizeof (vdr_input_plugin_t));

  this->stream     = stream;
  this->curpos     = 0;
  this->mrl        = mrl;
  this->fh         = -1;
  this->fh_control = -1;
  this->fh_result  = -1;
  this->fh_event   = -1;

  this->input_plugin.open              = vdr_plugin_open;
  this->input_plugin.get_capabilities  = vdr_plugin_get_capabilities;
  this->input_plugin.read              = vdr_plugin_read;
  this->input_plugin.read_block        = vdr_plugin_read_block;
  this->input_plugin.seek              = vdr_plugin_seek;
  this->input_plugin.get_current_pos   = vdr_plugin_get_current_pos;
  this->input_plugin.get_length        = vdr_plugin_get_length;
  this->input_plugin.get_blocksize     = vdr_plugin_get_blocksize;
  this->input_plugin.get_mrl           = vdr_plugin_get_mrl;
  this->input_plugin.dispose           = vdr_plugin_dispose;
  this->input_plugin.get_optional_data = vdr_plugin_get_optional_data;
  this->input_plugin.input_class       = cls_gen;

  this->cur_func = func_unknown;
  this->cur_size = 0;
  this->cur_done = 0;

  memset(this->osd_window, 0, sizeof (this->osd_window));

  this->osd_buffer              = 0;
  this->osd_buffer_size         = 0;
  this->osd_unscaled_blending   = 0;
  this->trick_speed_mode        = 0;
  this->audio_channels          = 0;
  this->mute_mode               = INPUT_VDR_MUTE_SIMULATE;
  this->volume_mode             = INPUT_VDR_VOLUME_CHANGE_HW;
  this->last_volume             = -1;
  this->frame_size.x            = 0;
  this->frame_size.y            = 0;
  this->frame_size.w            = 0;
  this->frame_size.h            = 0;
  this->frame_size.r            = 0;
  
  this->stream_external      = 0;
  this->event_queue_external = 0;

  pthread_mutex_init(&this->rpc_thread_shutdown_lock, 0);
  pthread_cond_init(&this->rpc_thread_shutdown_cond, 0);  
  
  pthread_mutex_init(&this->adjust_zoom_lock, 0);
  this->image4_3_zoom_x  = 0;
  this->image4_3_zoom_y  = 0;
  this->image16_9_zoom_x = 0;
  this->image16_9_zoom_y = 0;

  this->event_queue = xine_event_new_queue(this->stream);
  if (this->event_queue)
    xine_event_create_listener_thread(this->event_queue, event_handler, this);

  return &this->input_plugin;
}

/*
 * vdr input plugin class stuff
 */
static char **vdr_class_get_autoplay_list(input_class_t *this_gen,
                                          int *num_files)
{
  vdr_input_class_t *class = (vdr_input_class_t *)this_gen;

  *num_files = 1;
  return class->mrls;
}

static void *init_class(xine_t *xine, void *data)
{
  vdr_input_class_t *this;
  
  lprintf("init_class\n");
  
  this = (vdr_input_class_t *)xine_xmalloc(sizeof (vdr_input_class_t));
  
  this->xine = xine;

  this->mrls[ 0 ] = "vdr:/" VDR_ABS_FIFO_DIR "/stream#demux:mpeg_pes";
  this->mrls[ 1 ] = 0;

  this->input_class.get_instance      = vdr_class_get_instance;
  this->input_class.identifier        = "VDR";
  this->input_class.description       = N_("VDR display device plugin");
  this->input_class.get_dir           = NULL;
  this->input_class.get_autoplay_list = vdr_class_get_autoplay_list;
  this->input_class.dispose           = default_input_class_dispose;
  this->input_class.eject_media       = NULL;

  return this;
}

/*
 * exported plugin catalog entry
 */

const plugin_info_t xine_plugin_info[] EXPORTED =
{
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_INPUT, 18, "VDR", XINE_VERSION_CODE, NULL, init_class },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};

