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
 * $Id: xine_decoder.c,v 1.14 2001/09/18 11:36:03 jkeil Exp $
 *
 * stuff needed to turn libmpeg2 into a xine decoder plugin
 */


#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "video_out.h"
#include "mpeg2.h"
#include "mpeg2_internal.h"
#include "buffer.h"
#include "xine_internal.h"


typedef struct mpeg2dec_decoder_s {
  video_decoder_t  video_decoder;
  mpeg2dec_t       mpeg2;
  vo_instance_t   *video_out;
  /*int              mpeg_file;*/ /* debugging purposes only */
} mpeg2dec_decoder_t;

static int mpeg2dec_can_handle (video_decoder_t *this_gen, int buf_type) {
  return ((buf_type & 0xFFFF0000) == BUF_VIDEO_MPEG) ;
}


static void mpeg2dec_init (video_decoder_t *this_gen, vo_instance_t *video_out) {

  mpeg2dec_decoder_t *this = (mpeg2dec_decoder_t *) this_gen;

  mpeg2_init (&this->mpeg2, video_out);
  video_out->open(video_out);
  this->video_out = video_out;

  /* debugging purposes */
  /*this->mpeg_file = open ("/tmp/video.mpv",O_CREAT | O_WRONLY | O_TRUNC, 0644);*/
}

static void mpeg2dec_decode_data (video_decoder_t *this_gen, buf_element_t *buf) {
  mpeg2dec_decoder_t *this = (mpeg2dec_decoder_t *) this_gen;

  if (buf->decoder_info[0] == 0) {
    mpeg2_find_sequence_header (&this->mpeg2, buf->content, buf->content + buf->size);
  } else {
    
    /* debugging purposes */
    /* write (this->mpeg_file, buf->content, buf->size); */

    mpeg2_decode_data (&this->mpeg2, buf->content, buf->content + buf->size,
		       buf->PTS);

  }

}

static void mpeg2dec_close (video_decoder_t *this_gen) {

  mpeg2dec_decoder_t *this = (mpeg2dec_decoder_t *) this_gen;

  mpeg2_close (&this->mpeg2);

  this->video_out->close(this->video_out);

  /* debugging purposes */
  /*
  close (this->mpeg_file);
  this->mpeg_file = -1;
  */
}

static char *mpeg2dec_get_id(void) {
  return "mpeg2dec";
}

video_decoder_t *init_video_decoder_plugin (int iface_version, config_values_t *cfg) {

  mpeg2dec_decoder_t *this ;

  if (iface_version != 2) {
    printf( "libmpeg2: plugin doesn't support plugin API version %d.\n"
	    "libmpeg2: this means there's a version mismatch between xine and this "
	    "libmpeg2: decoder plugin.\nInstalling current plugins should help.\n",
	    iface_version);
    return NULL;
  }

  this = (mpeg2dec_decoder_t *) malloc (sizeof (mpeg2dec_decoder_t));
  memset(this, 0, sizeof (mpeg2dec_decoder_t));

  this->video_decoder.interface_version   = 2;
  this->video_decoder.can_handle          = mpeg2dec_can_handle;
  this->video_decoder.init                = mpeg2dec_init;
  this->video_decoder.decode_data         = mpeg2dec_decode_data;
  this->video_decoder.close               = mpeg2dec_close;
  this->video_decoder.get_identifier      = mpeg2dec_get_id;
  this->video_decoder.priority            = 1;

  return (video_decoder_t *) this;
}

