/* 
 * Copyright (C) 2000-2003 the xine project
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 *
 * input plugin for Digital TV (Digital Video Broadcast - DVB) devices
 * e.g. Hauppauge WinTV Nova supported by DVB drivers from Convergence
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#ifdef __sun
#include <sys/ioccom.h>
#endif
#include <sys/poll.h>

/* These will eventually be #include <linux/dvb/...> */
#include "dvb/dmx.h"
#include "dvb/frontend.h"

#define LOG_MODULE "input_dvb"
#define LOG_VERBOSE
/*
#define LOG
*/

#include "xine_internal.h"
#include "xineutils.h"
#include "input_plugin.h"
#include "net_buf_ctrl.h"

/* comment this out to have audio-only streams in the menu as well */
/* workaround for xine's unability to handle audio-only ts streams */
#define FILTER_RADIO_STREAMS 

#define FRONTEND_DEVICE "/dev/dvb/adapter0/frontend0"
#define DEMUX_DEVICE    "/dev/dvb/adapter0/demux0"
#define DVR_DEVICE      "/dev/dvb/adapter0/dvr0"

#define BUFSIZE 4096

#define NOPID 0xffff

typedef struct {
  int                            fd_frontend;
  int                            fd_demuxa, fd_demuxv;

  struct dvb_frontend_info       feinfo;

  struct dmx_pes_filter_params   pesFilterParamsV;
  struct dmx_pes_filter_params   pesFilterParamsA;

  xine_t                        *xine;
} tuner_t;

typedef struct {

  char                            *name;
  struct dvb_frontend_parameters   front_param;
  int                              vpid;
  int                              apid;
  int                              sat_no;
  int                              tone;
  int                              pol;
} channel_t;

typedef struct {

  input_class_t     input_class;

  xine_t           *xine;

  char             *mrls[5];

} dvb_input_class_t;

typedef struct {
  input_plugin_t      input_plugin;

  dvb_input_class_t  *class;

  xine_stream_t      *stream;

  char               *mrl;

  off_t               curpos;

  nbc_t              *nbc;

  tuner_t            *tuner;
  channel_t          *channels;
  int                 fd;
  int                 num_channels;
  int                 channel;
  pthread_mutex_t     mutex;

  osd_object_t       *osd;
  osd_object_t       *rec_osd;
  osd_object_t	     *name_osd;
  
  xine_event_queue_t *event_queue;

  /* scratch buffer for forward seeking */
  char                seek_buf[BUFSIZE];

  /* simple vcr-like functionality */
  int                 record_fd;

  /* centre cutout zoom */
  int 		      zoom_ok;
  /* display channel name */
  int                 displaying;
  int                 displaybydefault;
} dvb_input_plugin_t;

typedef struct {
	char *name;
	int value;
} Param;

static const Param inversion_list [] = {
	{ "INVERSION_OFF", INVERSION_OFF },
	{ "INVERSION_ON", INVERSION_ON },
	{ "INVERSION_AUTO", INVERSION_AUTO },
        { NULL, 0 }
};

static const Param bw_list [] = {
	{ "BANDWIDTH_6_MHZ", BANDWIDTH_6_MHZ },
	{ "BANDWIDTH_7_MHZ", BANDWIDTH_7_MHZ },
	{ "BANDWIDTH_8_MHZ", BANDWIDTH_8_MHZ },
        { NULL, 0 }
};

static const Param fec_list [] = {
	{ "FEC_1_2", FEC_1_2 },
	{ "FEC_2_3", FEC_2_3 },
	{ "FEC_3_4", FEC_3_4 },
	{ "FEC_4_5", FEC_4_5 },
	{ "FEC_5_6", FEC_5_6 },
	{ "FEC_6_7", FEC_6_7 },
	{ "FEC_7_8", FEC_7_8 },
	{ "FEC_8_9", FEC_8_9 },
	{ "FEC_AUTO", FEC_AUTO },
	{ "FEC_NONE", FEC_NONE },
        { NULL, 0 }
};

static const Param guard_list [] = {
	{"GUARD_INTERVAL_1_16", GUARD_INTERVAL_1_16},
	{"GUARD_INTERVAL_1_32", GUARD_INTERVAL_1_32},
	{"GUARD_INTERVAL_1_4", GUARD_INTERVAL_1_4},
	{"GUARD_INTERVAL_1_8", GUARD_INTERVAL_1_8},
        { NULL, 0 }
};

static const Param hierarchy_list [] = {
	{ "HIERARCHY_1", HIERARCHY_1 },
	{ "HIERARCHY_2", HIERARCHY_2 },
	{ "HIERARCHY_4", HIERARCHY_4 },
	{ "HIERARCHY_NONE", HIERARCHY_NONE },
        { NULL, 0 }
};

static const Param qam_list [] = {
	{ "QPSK", QPSK },
	{ "QAM_128", QAM_128 },
	{ "QAM_16", QAM_16 },
	{ "QAM_256", QAM_256 },
	{ "QAM_32", QAM_32 },
	{ "QAM_64", QAM_64 },
        { NULL, 0 }
};

static const Param transmissionmode_list [] = {
	{ "TRANSMISSION_MODE_2K", TRANSMISSION_MODE_2K },
	{ "TRANSMISSION_MODE_8K", TRANSMISSION_MODE_8K },
        { NULL, 0 }
};

static void tuner_dispose (tuner_t *this) {

  if (this->fd_frontend >= 0)
    close (this->fd_frontend);
  if (this->fd_demuxa >= 0)
    close (this->fd_demuxa);
  if (this->fd_demuxv >= 0)
    close (this->fd_demuxv);

  free (this);
}

static tuner_t *tuner_init (xine_t *xine) {

  tuner_t *this;

  this = (tuner_t *) xine_xmalloc(sizeof(tuner_t));

  this->fd_frontend = -1;
  this->fd_demuxa   = -1;
  this->fd_demuxv   = -1;
  this->xine        = xine;

  if ((this->fd_frontend = open(FRONTEND_DEVICE, O_RDWR)) < 0){
    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "FRONTEND DEVICE: %s\n", strerror(errno));
    tuner_dispose(this);
    return NULL;
  }

  if ((ioctl (this->fd_frontend, FE_GET_INFO, &this->feinfo)) < 0) {
    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "FE_GET_INFO: %s\n", strerror(errno));
    tuner_dispose(this);
    return NULL;
  }

  this->fd_demuxa = open (DEMUX_DEVICE, O_RDWR);
  if (this->fd_demuxa < 0) {
    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "DEMUX DEVICE audio: %s\n", strerror(errno));
    tuner_dispose(this);
    return NULL;
  }

  this->fd_demuxv=open (DEMUX_DEVICE, O_RDWR);
  if (this->fd_demuxv < 0) {
    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "DEMUX DEVICE video: %s\n", strerror(errno));
    tuner_dispose(this);
    return NULL;
  }

  return this;
}


static void tuner_set_vpid (tuner_t *this, ushort vpid) {

  if (vpid==0 || vpid==NOPID || vpid==0x1fff) {
    ioctl (this->fd_demuxv, DMX_STOP);
    return;
  }

  this->pesFilterParamsV.pid      = vpid;
  this->pesFilterParamsV.input    = DMX_IN_FRONTEND;
  this->pesFilterParamsV.output   = DMX_OUT_TS_TAP;
  this->pesFilterParamsV.pes_type = DMX_PES_VIDEO;
  this->pesFilterParamsV.flags    = DMX_IMMEDIATE_START;
  if (ioctl(this->fd_demuxv, DMX_SET_PES_FILTER,
	    &this->pesFilterParamsV) < 0)
    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "set_vpid: %s\n", strerror(errno));
}

static void tuner_set_apid (tuner_t *this, ushort apid) {
  if (apid==0 || apid==NOPID || apid==0x1fff) {
    ioctl (this->fd_demuxa, DMX_STOP);
    return;
  }

  this->pesFilterParamsA.pid      = apid;
  this->pesFilterParamsA.input    = DMX_IN_FRONTEND;
  this->pesFilterParamsA.output   = DMX_OUT_TS_TAP;
  this->pesFilterParamsA.pes_type = DMX_PES_AUDIO;
  this->pesFilterParamsA.flags    = DMX_IMMEDIATE_START;
  if (ioctl (this->fd_demuxa, DMX_SET_PES_FILTER,
	     &this->pesFilterParamsA) < 0)
    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "set_apid: %s\n", strerror(errno));
}

static int tuner_set_diseqc(tuner_t *this, channel_t *c)
{
   struct dvb_diseqc_master_cmd cmd =
      {{0xe0, 0x10, 0x38, 0xf0, 0x00, 0x00}, 4};

   cmd.msg[3] = 0xf0 | ((c->sat_no * 4) & 0x0f) |
      (c->tone ? 1 : 0) | (c->pol ? 0 : 2);

   if (ioctl(this->fd_frontend, FE_SET_TONE, SEC_TONE_OFF) < 0)
      return 0;
   if (ioctl(this->fd_frontend, FE_SET_VOLTAGE,
	     c->pol ? SEC_VOLTAGE_13 : SEC_VOLTAGE_18) < 0)
      return 0;
   usleep(15000);
   if (ioctl(this->fd_frontend, FE_DISEQC_SEND_MASTER_CMD, &cmd) < 0)
      return 0;
   usleep(15000);
   if (ioctl(this->fd_frontend, FE_DISEQC_SEND_BURST,
	     (c->sat_no / 4) % 2 ? SEC_MINI_B : SEC_MINI_A) < 0)
      return 0;
   usleep(15000);
   if (ioctl(this->fd_frontend, FE_SET_TONE,
	     c->tone ? SEC_TONE_ON : SEC_TONE_OFF) < 0)
      return 0;

   return 1;
}

static int tuner_tune_it (tuner_t *this, struct dvb_frontend_parameters
			  *front_param) {
  fe_status_t status;

  if (ioctl(this->fd_frontend, FE_SET_FRONTEND, front_param) <0) {
    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "setfront front: %s\n", strerror(errno));
  }
  
  do {
    if (ioctl(this->fd_frontend, FE_READ_STATUS, &status) < 0) {
      xprintf(this->xine, XINE_VERBOSITY_DEBUG, "fe get event: %s\n", strerror(errno));
      return 0;
    }

    xprintf(this->xine, XINE_VERBOSITY_DEBUG, "input_dvb: status: %x\n", status);

    if (status & FE_HAS_LOCK) {
      return 1;
    }
    usleep(500000);
  }
  while (!(status & FE_TIMEDOUT));

  return 0;
}

static void print_channel (xine_t *xine, channel_t *channel) {
  xprintf (xine, XINE_VERBOSITY_DEBUG, 
	   "input_dvb: channel '%s' freq %d vpid %d apid %d\n",
	   channel->name,
	   channel->front_param.frequency,
	   channel->vpid,
	   channel->apid);
}


static int tuner_set_channel (tuner_t *this,
			      channel_t *c) {

  print_channel (this->xine, c);

  tuner_set_vpid (this, 0);
  tuner_set_apid (this, 0);

  if (this->feinfo.type==FE_QPSK) {
    if (!tuner_set_diseqc(this, c))
      return 0;
  }

  if (!tuner_tune_it (this, &c->front_param))
    return 0;

  tuner_set_vpid  (this, c->vpid);
  tuner_set_apid  (this, c->apid);

  return 1; /* fixme: error handling */
}

static void show_channelname_osd (dvb_input_plugin_t *this)
{
 
	  if(this->displaying!=1){
	  /* Display Channel Name on OSD */
	      this->stream->osd_renderer->clear(this->name_osd);
	      this->stream->osd_renderer->render_text (this->name_osd, 10, 10,
					       this->channels[this->channel].name, OSD_TEXT3); 
	      this->stream->osd_renderer->show_unscaled (this->name_osd, 0);
	      this->displaying=1;
	   }
	   else{
	      this->stream->osd_renderer->hide(this->name_osd,0);
	      this->displaying=0;
	   }
}


static void osd_show_channel (dvb_input_plugin_t *this) {

  int i, channel ;

  this->stream->osd_renderer->filled_rect (this->osd, 0, 0, 395, 400, 2);

  channel = this->channel - 5;

  for (i=0; i<11; i++) {

    if ( (channel >= 0) && (channel < this->num_channels) )
      this->stream->osd_renderer->render_text (this->osd, 110, 10+i*35,
					     this->channels[channel].name,
					     OSD_TEXT3);
    channel ++;
  }

  this->stream->osd_renderer->line (this->osd, 105, 183, 390, 183, 10);
  this->stream->osd_renderer->line (this->osd, 105, 183, 105, 219, 10);
  this->stream->osd_renderer->line (this->osd, 105, 219, 390, 219, 10);
  this->stream->osd_renderer->line (this->osd, 390, 183, 390, 219, 10);

  this->stream->osd_renderer->show (this->osd, 0);

}

static void switch_channel (dvb_input_plugin_t *this) {

  xine_event_t     event;
  xine_pids_data_t data;
  xine_ui_data_t   ui_data;

  _x_demux_flush_engine(this->stream); 

  pthread_mutex_lock (&this->mutex);
  
  close (this->fd);

  if (!tuner_set_channel (this->tuner, &this->channels[this->channel])) {
    xprintf (this->class->xine, XINE_VERBOSITY_LOG, _("input_dvb: tuner_set_channel failed\n"));
    pthread_mutex_unlock (&this->mutex);
    return;
  }

  event.type = XINE_EVENT_PIDS_CHANGE;
  data.vpid = this->channels[this->channel].vpid;
  data.apid = this->channels[this->channel].apid;
  event.data = &data;
  event.data_length = sizeof (xine_pids_data_t);

  xprintf (this->class->xine, XINE_VERBOSITY_DEBUG, "input_dvb: sending event\n");

  xine_event_send (this->stream, &event);

  snprintf (ui_data.str, 256, "%04d - %s", this->channel, 
      	    this->channels[this->channel].name);
  ui_data.str_len = strlen (ui_data.str);

  _x_meta_info_set(this->stream, XINE_META_INFO_TITLE, ui_data.str);

  event.type        = XINE_EVENT_UI_SET_TITLE;
  event.stream      = this->stream;
  event.data        = &ui_data;
  event.data_length = sizeof(ui_data);
  xine_event_send(this->stream, &event);

  lprintf ("ui title event sent\n");
  
  this->fd = open (DVR_DEVICE, O_RDONLY);

  pthread_mutex_unlock (&this->mutex);

  this->stream->osd_renderer->hide (this->osd, 0);

  if (this->displaying){
	  show_channelname_osd (this); /* toggle off */
	  show_channelname_osd (this); /* and back on again.. */
  }
}

static void do_record (dvb_input_plugin_t *this) {

  if (this->record_fd > -1) {

    /* stop recording */
    close (this->record_fd);
    this->record_fd = -1;

    this->stream->osd_renderer->hide (this->rec_osd, 0);

  } else {

    char filename [256];

    snprintf (filename, 256, "dvb_rec_%d.ts", (int) time (NULL));
    
    /* start recording */
    this->record_fd = open (filename, O_CREAT | O_APPEND | O_WRONLY, 0644);

    this->stream->osd_renderer->filled_rect (this->rec_osd, 0, 0, 300, 40, 0);

    this->stream->osd_renderer->render_text (this->rec_osd, 10, 10, filename,
					     OSD_TEXT3);

    this->stream->osd_renderer->show (this->rec_osd, 0);

  }
}

static void dvb_event_handler (dvb_input_plugin_t *this) {

  xine_event_t *event;

  while ((event = xine_event_get (this->event_queue))) {

    lprintf ("got event %08x\n", event->type);

    if (this->fd<0) {
      xine_event_free (event);
      return;
    }

    switch (event->type) {

    case XINE_EVENT_INPUT_DOWN:
      if (this->channel < (this->num_channels-1))
	this->channel++;
      osd_show_channel (this);
      break;

    case XINE_EVENT_INPUT_UP:
      if (this->channel>0)
	this->channel--;
      osd_show_channel (this);
      break;

    case XINE_EVENT_INPUT_NEXT:
      if (this->channel < (this->num_channels-1)) {
	this->channel++;
	switch_channel (this);
      }
      break;

    case XINE_EVENT_INPUT_PREVIOUS:
      if (this->channel>0) {
	this->channel--;
	switch_channel (this);
      }
      break;

    case XINE_EVENT_INPUT_SELECT:
      switch_channel (this);
      break;

    case XINE_EVENT_INPUT_MENU1:
      this->stream->osd_renderer->hide (this->osd, 0);
      break;

    case XINE_EVENT_INPUT_MENU2:
      do_record (this);
      break;
    case XINE_EVENT_INPUT_MENU3:
      /* zoom for cropped 4:3 in a 16:9 window */
      if (!this->zoom_ok) {
       this->zoom_ok = 1;
       this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_X, 133);
       this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_Y, 133);
      } else {
       this->zoom_ok=0;
       this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_X, 100);
       this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_Y, 100);
      }
      break;

    case XINE_EVENT_INPUT_MENU7:
      show_channelname_osd (this);
      break;



#if 0
   default:
      printf ("input_dvb: got an event, type 0x%08x\n", event->type);
#endif
    }

    xine_event_free (event);
  }
}



static off_t dvb_plugin_read (input_plugin_t *this_gen,
			      char *buf, off_t len) {
  dvb_input_plugin_t *this = (dvb_input_plugin_t *) this_gen;
  off_t n, total;

  dvb_event_handler (this);

  lprintf ("reading %lld bytes...\n", len);

  nbc_check_buffers (this->nbc);

  pthread_mutex_lock( &this->mutex ); /* protect agains channel changes */
  total=0;
  while (total<len){ 
    n = read (this->fd, &buf[total], len-total);

    lprintf ("got %lld bytes (%lld/%lld bytes read)\n", n,total,len);
  
    if (n > 0){
      this->curpos += n;
      total += n;
    } else if (n<0 && errno!=EAGAIN) {
      pthread_mutex_unlock( &this->mutex );
      return total;
    }
  }

  if (this->record_fd)
    write (this->record_fd, buf, total);

  pthread_mutex_unlock( &this->mutex );
  return total;
}

static buf_element_t *dvb_plugin_read_block (input_plugin_t *this_gen,
					     fifo_buffer_t *fifo, off_t todo) {
  /* dvb_input_plugin_t   *this = (dvb_input_plugin_t *) this_gen;  */
  buf_element_t        *buf = fifo->buffer_pool_alloc (fifo);
  int                   total_bytes;


  buf->content = buf->mem;
  buf->type    = BUF_DEMUX_BLOCK;

  total_bytes = dvb_plugin_read (this_gen, buf->content, todo);

  if (total_bytes != todo) {
    buf->free_buffer (buf);
    return NULL;
  }

  buf->size = total_bytes;

  return buf;
}

static off_t dvb_plugin_seek (input_plugin_t *this_gen, off_t offset,
			      int origin) {

  dvb_input_plugin_t *this = (dvb_input_plugin_t *) this_gen;

  lprintf ("seek %lld bytes, origin %d\n", offset, origin);

  /* only relative forward-seeking is implemented */

  if ((origin == SEEK_CUR) && (offset >= 0)) {

    for (;((int)offset) - BUFSIZE > 0; offset -= BUFSIZE) {
      this->curpos += dvb_plugin_read (this_gen, this->seek_buf, BUFSIZE);
    }

    this->curpos += dvb_plugin_read (this_gen, this->seek_buf, offset);
  }

  return this->curpos;
}

static off_t dvb_plugin_get_length (input_plugin_t *this_gen) {
  return 0;
}

static uint32_t dvb_plugin_get_capabilities (input_plugin_t *this_gen) {
  return INPUT_CAP_CHAPTERS; /* where did INPUT_CAP_AUTOPLAY go ?!? */
}

static uint32_t dvb_plugin_get_blocksize (input_plugin_t *this_gen) {
  return 0;
}

static off_t dvb_plugin_get_current_pos (input_plugin_t *this_gen){
  dvb_input_plugin_t *this = (dvb_input_plugin_t *) this_gen;

  return this->curpos;
}

static void dvb_plugin_dispose (input_plugin_t *this_gen) {
  dvb_input_plugin_t *this = (dvb_input_plugin_t *) this_gen;

  if (this->fd != -1) {
    close(this->fd);
    this->fd = -1;
  }
  
  if (this->nbc) {
    nbc_close (this->nbc);
    this->nbc = NULL;
  }

  if (this->event_queue)
    xine_event_dispose_queue (this->event_queue);

  free (this->mrl);
  
  if (this->channels)
    free (this->channels);
    
  if (this->tuner)
    tuner_dispose (this->tuner);
  
  free (this);
}

static char* dvb_plugin_get_mrl (input_plugin_t *this_gen) {
  dvb_input_plugin_t *this = (dvb_input_plugin_t *) this_gen;

  return this->mrl;
}

static int dvb_plugin_get_optional_data (input_plugin_t *this_gen,
					 void *data, int data_type) {

  return INPUT_OPTIONAL_UNSUPPORTED;
}

static int find_param(const Param *list, const char *name)
{
  while (list->name && strcmp(list->name, name))
    list++;
  return list->value;;
}

static int extract_channel_from_string(channel_t * channel,char * str,fe_type_t fe_type)
{
	/*
		try to extract channel data from a string in the following format
		(DVBS) QPSK: <channel name>:<frequency>:<polarisation>:<sat_no>:<sym_rate>:<vpid>:<apid>
		(DVBC) QAM: <channel name>:<frequency>:<inversion>:<sym_rate>:<fec>:<qam>:<vpid>:<apid>
		(DVBT) OFDM: <channel name>:<frequency>:<inversion>:
						<bw>:<fec_hp>:<fec_lp>:<qam>:
						<transmissionm>:<guardlist>:<hierarchinfo>:<vpid>:<apid>
		
		<channel name> = any string not containing ':'
		<frequency>    = unsigned long
		<polarisation> = 'v' or 'h'
		<sat_no>       = unsigned long, usually 0 :D
		<sym_rate>     = symbol rate in MSyms/sec
		
		
		<inversion>    = INVERSION_ON | INVERSION_OFF | INVERSION_AUTO
		<fec>          = FEC_1_2, FEC_2_3, FEC_3_4 .... FEC_AUTO ... FEC_NONE
		<qam>          = QPSK, QAM_128, QAM_16 ...

		<bw>           = BANDWIDTH_6_MHZ, BANDWIDTH_7_MHZ, BANDWIDTH_8_MHZ
		<fec_hp>       = <fec>
		<fec_lp>       = <fec>
		<transmissionm> = TRANSMISSION_MODE_2K, TRANSMISSION_MODE_8K
		<vpid>         = video program id
		<apid>         = audio program id

	*/
	unsigned long freq;
	char *field, *tmp;

	tmp = str;
	
	/* find the channel name */
	if(!(field = strsep(&tmp,":")))return -1;
	channel->name = strdup(field);

	/* find the frequency */
	if(!(field = strsep(&tmp, ":")))return -1;
	freq = strtoul(field,NULL,0);

	switch(fe_type)
	{
		case FE_QPSK:
			if(freq > 11700)
			{
				channel->front_param.frequency = (freq - 10600)*1000;
				channel->tone = 1;
			} else {
				channel->front_param.frequency = (freq - 9750)*1000;
				channel->tone = 0;
			}
			channel->front_param.inversion = INVERSION_OFF;
	  
			/* find out the polarisation */ 
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->pol = (field[0] == 'h' ? 0 : 1);

			/* satellite number */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->sat_no = strtoul(field, NULL, 0);

			/* symbol rate */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.qpsk.symbol_rate = strtoul(field, NULL, 0) * 1000;

			channel->front_param.u.qpsk.fec_inner = FEC_AUTO;
		break;
		case FE_QAM:
			channel->front_param.frequency = freq;
			
			/* find out the inversion */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.inversion = find_param(inversion_list, field);

			/* find out the symbol rate */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.qam.symbol_rate = strtoul(field, NULL, 0);

			/* find out the fec */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.qam.fec_inner = find_param(fec_list, field);

			/* find out the qam */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.qam.modulation = find_param(qam_list, field);
		break;
		case FE_OFDM:
			channel->front_param.frequency = freq;

			/* find out the inversion */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.inversion = find_param(inversion_list, field);

			/* find out the bandwidth */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.ofdm.bandwidth = find_param(bw_list, field);

			/* find out the fec_hp */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.ofdm.code_rate_HP = find_param(fec_list, field);

			/* find out the fec_lp */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.ofdm.code_rate_LP = find_param(fec_list, field);

			/* find out the qam */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.ofdm.constellation = find_param(qam_list, field);

			/* find out the transmission mode */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.ofdm.transmission_mode = find_param(transmissionmode_list, field);

			/* guard list */
			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.ofdm.guard_interval = find_param(guard_list, field);

			if(!(field = strsep(&tmp, ":")))return -1;
			channel->front_param.u.ofdm.hierarchy_information = find_param(hierarchy_list, field);
		break;
	}

	if(!(field = strsep(&tmp, ":")))return -1;
	channel->vpid = strtoul(field, NULL, 0);

#ifdef FILTER_RADIO_STREAMS
	if(channel->vpid == 0)return -1; /* only tv channels for now */
#endif

	if(!(field = strsep(&tmp, ":")))return -1;
	channel->apid = strtoul(field, NULL, 0);

	return 0;
}

static channel_t *load_channels (xine_t *xine, int *num_ch, fe_type_t fe_type) {

  FILE      *f;
  char       str[BUFSIZE];
  char       filename[BUFSIZE];
  channel_t *channels;
  int        num_channels;

  snprintf(filename, BUFSIZE, "%s/.xine/channels.conf", xine_get_homedir());

  f = fopen(filename, "rb");
  if (!f) {
    xprintf(xine, XINE_VERBOSITY_LOG, _("input_dvb: failed to open dvb channel file '%s'\n"), filename);
    return NULL;
  }

  /*
   * count and alloc channels
   */
  num_channels = 0;
  while ( fgets (str, BUFSIZE, f)) {
    num_channels++;
  }
  fclose (f);

  if(num_channels > 0) 
    xprintf (xine, XINE_VERBOSITY_DEBUG, "input_dvb: expecting %d channels...\n", num_channels);
  else {
    xprintf (xine, XINE_VERBOSITY_DEBUG, "input_dvb: no channels found in the file: giving up.\n");
    return NULL;
  }

  channels = malloc (sizeof (channel_t) * num_channels);

  /*
   * load channel list 
   */

  f = fopen (filename, "rb");
  num_channels = 0;
  while ( fgets (str, BUFSIZE, f)) {
    if(extract_channel_from_string(&(channels[num_channels]),str,fe_type) < 0)continue;

    num_channels++;
  }

  if(num_channels > 0) 
    xprintf (xine, XINE_VERBOSITY_DEBUG, "input_dvb: found %d channels...\n", num_channels);
  else {
    xprintf (xine, XINE_VERBOSITY_DEBUG, "input_dvb: no channels found in the file: giving up.\n");
    free(channels);
    return NULL;
  }

  *num_ch = num_channels;
  return channels;
}

/* allow center cutout zoom for dvb content */
static void
dvb_zoom_cb (input_plugin_t * this_gen, xine_cfg_entry_t * cfg)
{
  dvb_input_plugin_t *this = (dvb_input_plugin_t *) this_gen;

  this->zoom_ok = cfg->num_value;

  if (!this)
    return;

  if (this->zoom_ok)
    {
      this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_X, 133);
      this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_Y, 133);
    }
  else
    {
      this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_X, 100);
      this->stream->video_out->set_property (this->stream->video_out, VO_PROP_ZOOM_Y, 100);
    }
}




static int dvb_plugin_open (input_plugin_t *this_gen) {
  dvb_input_plugin_t *this = (dvb_input_plugin_t *) this_gen;
  tuner_t            *tuner;
  channel_t          *channels;
  int                 num_channels;
  char                str[256];
  char               *ptr;
  xine_cfg_entry_t zoomdvb;
  xine_cfg_entry_t displaychan;

	config_values_t *config = this->stream->xine->config;

  if ( !(tuner = tuner_init(this->class->xine)) ) {
    xprintf (this->class->xine, XINE_VERBOSITY_LOG, _("input_dvb: cannot open dvb device\n"));
    return 0;
  }

	if(strncasecmp(this->mrl,"dvb://",6) == 0)
	{
		/*
		 * This is either dvb://<number>
		 * or the "magic" dvb://<channel name>
		 * We load the channels from ~/.xine/channels.conf
		 * and assume that its format is valid for our tuner type
		 */

		if(!(channels = load_channels(this->class->xine,&num_channels,tuner->feinfo.type)))
		{
			/* failed to load the channels */
			tuner_dispose(tuner);
			return 0;
		}

		if(sscanf(this->mrl,"dvb://%d",&this->channel) == 1)
		{
			/* dvb://<number> format: load channels from ~/.xine/channels.conf */
			if(this->channel >= num_channels)
			{
				xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
					_("input_dvb: channel %d out of range, defaulting to 0\n"),this->channel);
				this->channel = 0;
			}
		} else {
			/* dvb://<channel name> format ? */
			char * channame = this->mrl + 6;
			if(*channame)
			{
				/* try to find the specified channel */
				int idx = 0;

				xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
					_("input_dvb: searching for channel %s\n"),channame);

				while(idx < num_channels)
				{
					if(strcasecmp(channels[idx].name,channame) == 0)break;
					idx++;
				}

				if(idx < num_channels)
				{
					this->channel = idx;
				} else {
					/*
					 * try a partial match too
					 * be smart and compare starting from the first char, then from 
					 * the second etc..
					 * Yes, this is expensive, but it happens really often
					 * that the channels have really ugly names, sometimes prefixed
					 * by numbers...
					 */
					int chanlen = strlen(channame);
					int offset = 0;

					xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
							_("input_dvb: exact match for %s not found: trying partial matches\n"),channame);

					do {
						idx = 0;
						while(idx < num_channels)
						{
							if(strlen(channels[idx].name) > offset)
							{
								if(strncasecmp(channels[idx].name + offset,channame,chanlen) == 0)
								{
									xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
										_("input_dvb: found matching channel %s\n"),channels[idx].name);
									break;
								}
							}
							idx++;
						}
						offset++;
						printf("%d,%d,%d\n",offset,idx,num_channels);
					} while((offset < 6) && (idx == num_channels));
					
					if(idx < num_channels)
					{
						this->channel = idx;
					} else {
						xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
							_("input_dvb: channel %s not found in channels.conf, defaulting to channel 0\n"),channame);
						this->channel = 0;
					}
				}
			} else {
				/* just default to channel 0 */
				xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
					_("input_dvb: invalid channel specification, defaulting to channel 0\n"));
				this->channel = 0;
			}
		}

	} else if(strncasecmp(this->mrl,"dvbs://",7) == 0)
	{
		/*
		 * This is dvbs://<channel name>:<qpsk tuning parameters>
		 */
		if(tuner->feinfo.type != FE_QPSK)
		{
			xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
				_("input_dvb: dvbs mrl specified but the tuner doesn't appear to be QPSK (DVB-S)\n"));
			tuner_dispose(tuner);
			return 0;
		}
		ptr = this->mrl;
		ptr += 7;
		channels = malloc(sizeof(channel_t));
		if(extract_channel_from_string(channels,ptr,tuner->feinfo.type) < 0)
		{
			free(channels);
			tuner_dispose(tuner);
			return 0;
		}
		this->channel = 0;
	} else if(strncasecmp(this->mrl,"dvbt://",7) == 0)
	{
		/*
		 * This is dvbt://<channel name>:<ofdm tuning parameters>
		 */
		if(tuner->feinfo.type != FE_OFDM)
		{
			xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
				_("input_dvb: dvbt mrl specified but the tuner doesn't appear to be OFDM (DVB-T)\n"));
			tuner_dispose(tuner);
			return 0;
		}
		ptr = this->mrl;
		ptr += 7;
		channels = malloc(sizeof(channel_t));
		if(extract_channel_from_string(channels,ptr,tuner->feinfo.type) < 0)
		{
			free(channels);
			tuner_dispose(tuner);
			return 0;
		}
		this->channel = 0;
	} else if(strncasecmp(this->mrl,"dvbc://",7) == 0)
	{
		/*
		 * This is dvbc://<channel name>:<qam tuning parameters>
		 */
		if(tuner->feinfo.type != FE_QAM)
		{
			xprintf (this->class->xine, XINE_VERBOSITY_LOG, 
				_("input_dvb: dvbc mrl specified but the tuner doesn't appear to be QAM (DVB-C)\n"));
			tuner_dispose(tuner);
			return 0;
		}
		ptr = this->mrl;
		ptr += 7;
		channels = malloc(sizeof(channel_t));
		if(extract_channel_from_string(channels,ptr,tuner->feinfo.type) < 0)
		{
			free(channels);
			tuner_dispose(tuner);
			return 0;
		}
		this->channel = 0;
	} else {
		/* not our mrl */
		tuner_dispose(tuner);
		return 0;
	}

	this->tuner    = tuner;
	this->channels = channels;
	this->num_channels = num_channels;

  if (!tuner_set_channel (this->tuner, &this->channels[this->channel])) {
    xprintf (this->class->xine, XINE_VERBOSITY_LOG, _("input_dvb: tuner_set_channel failed\n"));
    tuner_dispose(this->tuner);
    free(this->channels);
    return 0;
  }

  if ((this->fd = open (DVR_DEVICE, O_RDONLY)) < 0){
    xprintf (this->class->xine, XINE_VERBOSITY_LOG, _("input_dvb: cannot open dvr device '%s'\n"), DVR_DEVICE);
    tuner_dispose(this->tuner);
    free(this->channels);
    return 0;
  }

  this->curpos       = 0;
  this->osd          = NULL;

  pthread_mutex_init (&this->mutex, NULL);

  this->event_queue = xine_event_new_queue (this->stream);

  /*
   * this osd is used for the channel selection menu
   */
  
  this->osd = this->stream->osd_renderer->new_object (this->stream->osd_renderer,
						      410, 410);
  this->stream->osd_renderer->set_position (this->osd, 20, 20);
  this->stream->osd_renderer->set_font (this->osd, "cetus", 32);
  this->stream->osd_renderer->set_encoding(this->osd, NULL);
  this->stream->osd_renderer->set_text_palette (this->osd,
						TEXTPALETTE_WHITE_NONE_TRANSLUCID,
						OSD_TEXT3);

  /*
   * this osd is used to draw the "recording" sign
   */
  
  this->rec_osd = this->stream->osd_renderer->new_object (this->stream->osd_renderer,
	  					          301, 41);
  this->stream->osd_renderer->set_position (this->rec_osd, 10, 10);
  this->stream->osd_renderer->set_font (this->rec_osd, "cetus", 16);
  this->stream->osd_renderer->set_encoding(this->rec_osd, NULL);
  this->stream->osd_renderer->set_text_palette (this->rec_osd,
						TEXTPALETTE_WHITE_NONE_TRANSLUCID,
						OSD_TEXT3);
  /* 
   * this osd is for displaying currently shown channel name 
   */
   
  this->name_osd = this->stream->osd_renderer->new_object (this->stream->osd_renderer,
  							 301, 61);
  this->stream->osd_renderer->set_position (this->name_osd, 10, 10);
  this->stream->osd_renderer->set_font (this->name_osd, "cetus", 40);
  this->stream->osd_renderer->set_encoding (this->name_osd, NULL);
  this->stream->osd_renderer->set_text_palette (this->name_osd,
  						XINE_TEXTPALETTE_YELLOW_BLACK_TRANSPARENT,
  						OSD_TEXT3);

/* zoom for 4:3 in a 16:9 window */
  config->register_bool (config, "input.dvbzoom",
			 0,
			 "Enable DVB 'center cutout' (zoom)?",
			 "This "
			 "will allow fullscreen "
			 "playback of 4:3 content "
			 "transmitted in a 16:9 frame",
			 10, &dvb_zoom_cb, (void *) this);

  if (xine_config_lookup_entry (this->stream->xine,
				"input.dvbzoom", &zoomdvb))
                         dvb_zoom_cb ((input_plugin_t *) this, &zoomdvb);

/* dislay channel name in top left of display */ 
  config->register_bool (config, "input.dvbdisplaychan",
			 0,
			 "Enable DVB channel name by default?",
			 "This "
			 "will display current "
			 "channel name on OSD "
			 "MENU7 button will disable",
			 10, NULL, NULL);

  if (xine_config_lookup_entry (this->stream->xine,
				"input.dvbdisplaychan", &displaychan))
	if(displaychan.num_value)
	      show_channelname_osd (this);


  /*
   * init metadata (channel title)
   */
  snprintf (str, 256, "%04d - %s", this->channel, 
      	    this->channels[this->channel].name);

  _x_meta_info_set(this->stream, XINE_META_INFO_TITLE, str);
  
  return 1;
}

static input_plugin_t *dvb_class_get_instance (input_class_t *class_gen,
				    xine_stream_t *stream,
				    const char *data) {

  dvb_input_class_t  *class = (dvb_input_class_t *) class_gen;
  dvb_input_plugin_t *this;
  char               *mrl = (char *) data;

  if(strncasecmp (mrl, "dvb://",6))
    if(strncasecmp(mrl,"dvbs://",7))
      if(strncasecmp(mrl,"dvbt://",7))
        if(strncasecmp(mrl,"dvbc://",7))
          return NULL;

  this = (dvb_input_plugin_t *) xine_xmalloc (sizeof(dvb_input_plugin_t));

  this->stream       = stream;
  this->mrl          = strdup(mrl);
  this->class        = class;
  this->tuner        = NULL;
  this->channels     = NULL;
  this->fd           = -1;
  this->nbc          = nbc_init (this->stream);
  this->osd          = NULL;
  this->event_queue  = NULL;
  this->record_fd    = -1;
    
  this->input_plugin.open              = dvb_plugin_open;
  this->input_plugin.get_capabilities  = dvb_plugin_get_capabilities;
  this->input_plugin.read              = dvb_plugin_read;
  this->input_plugin.read_block        = dvb_plugin_read_block;
  this->input_plugin.seek              = dvb_plugin_seek;
  this->input_plugin.get_current_pos   = dvb_plugin_get_current_pos;
  this->input_plugin.get_length        = dvb_plugin_get_length;
  this->input_plugin.get_blocksize     = dvb_plugin_get_blocksize;
  this->input_plugin.get_mrl           = dvb_plugin_get_mrl;
  this->input_plugin.get_optional_data = dvb_plugin_get_optional_data;
  this->input_plugin.dispose           = dvb_plugin_dispose;
  this->input_plugin.input_class       = class_gen;

  return &this->input_plugin;
}

/*
 * dvb input plugin class stuff
 */

static char *dvb_class_get_description (input_class_t *this_gen) {
  return _("DVB (Digital TV) input plugin");
}

static char *dvb_class_get_identifier (input_class_t *this_gen) {
  return "dvb";
}

static void dvb_class_dispose (input_class_t *this_gen) {
  dvb_input_class_t *class = (dvb_input_class_t *) this_gen;
  free (class);
}

static int dvb_class_eject_media (input_class_t *this_gen) {
  return 1;
}

static char ** dvb_class_get_autoplay_list (input_class_t *this_gen,
					    int *num_files) {
  dvb_input_class_t *class = (dvb_input_class_t *) this_gen;

  *num_files = 1;
  return class->mrls;
}

static void *init_class (xine_t *xine, void *data) {

  dvb_input_class_t  *this;

  this = (dvb_input_class_t *) xine_xmalloc (sizeof (dvb_input_class_t));

  this->xine   = xine;

  this->input_class.get_instance       = dvb_class_get_instance;
  this->input_class.get_identifier     = dvb_class_get_identifier;
  this->input_class.get_description    = dvb_class_get_description;
  this->input_class.get_dir            = NULL;
  this->input_class.get_autoplay_list  = dvb_class_get_autoplay_list;
  this->input_class.dispose            = dvb_class_dispose;
  this->input_class.eject_media        = dvb_class_eject_media;

  this->mrls[0] = "dvb://";
  this->mrls[1] = "dvbs://";
  this->mrls[2] = "dvbc://";
  this->mrls[3] = "dvbt://";
  this->mrls[4] = 0;

  lprintf ("init class succeeded\n");

  return this;
}


/*
 * exported plugin catalog entry
 */

plugin_info_t xine_plugin_info[] = {
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_INPUT, 14, "DVB", XINE_VERSION_CODE, NULL, init_class },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
