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
 * $Id: xine_decoder.c,v 1.37 2004/12/16 13:59:10 mroi Exp $
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "libfaad"
#define LOG_VERBOSE
/*
#define LOG
*/

#include "xine_internal.h"
#include "audio_out.h"
#include "buffer.h"
#include "xineutils.h"
#include "faad.h"

typedef struct {
  audio_decoder_class_t   decoder_class;
} faad_class_t;

typedef struct faad_decoder_s {
  audio_decoder_t  audio_decoder;

  xine_stream_t    *stream;
  
  /* faad2 stuff */
  faacDecHandle           faac_dec;
  faacDecConfigurationPtr faac_cfg;
  faacDecFrameInfo        faac_finfo;
  int                     faac_failed;
 
  int              raw_mode;
  
  unsigned char   *buf;
  int              size;
  int              rec_audio_src_size;
  int              max_audio_src_size;
  int              pts;
  
  unsigned char   *dec_config;
  int              dec_config_size;
  
  unsigned long    rate;
  int              bits_per_sample; 
  unsigned char    num_channels; 
  uint32_t         ao_cap_mode; 
   
  int              output_open;
} faad_decoder_t;


static void faad_reset (audio_decoder_t *this_gen) {

  faad_decoder_t *this = (faad_decoder_t *) this_gen;
  this->size = 0;
}

static int faad_open_dec( faad_decoder_t *this ) {
  int used;

  this->faac_dec = faacDecOpen();
  if( !this->faac_dec ) {
    xprintf( this->stream->xine, XINE_VERBOSITY_LOG,
             _("libfaad: libfaad faacDecOpen() failed.\n"));
    this->faac_failed++;
  } else {
    if( this->dec_config ) {
      used = faacDecInit2(this->faac_dec, this->dec_config, this->dec_config_size,
                          &this->rate, &this->num_channels);
      
      if( used < 0 ) {
        xprintf( this->stream->xine, XINE_VERBOSITY_LOG,
                _("libfaad: libfaad faacDecInit2 failed.\n"));
        this->faac_failed++;
      } else
        lprintf( "faacDecInit2 returned rate=%ld channels=%d\n",
                 this->rate, this->num_channels );
    } else {
      /* Set the default object type and samplerate */
      /* This is useful for RAW AAC files */
      this->faac_cfg = faacDecGetCurrentConfiguration(this->faac_dec);
      if( this->faac_cfg ) {
        this->faac_cfg->defSampleRate = this->rate;
        this->faac_cfg->outputFormat = FAAD_FMT_16BIT;
        this->bits_per_sample = 16;
        this->faac_cfg->useOldADTSFormat = 0;
        this->faac_cfg->dontUpSampleImplicitSBR = 1;
        faacDecSetConfiguration(this->faac_dec, this->faac_cfg);
      }
  

      used = faacDecInit(this->faac_dec, this->buf, this->size,
                        &this->rate, &this->num_channels);
        
      if( used < 0 ) {
        xprintf ( this->stream->xine, XINE_VERBOSITY_LOG,
                  _("libfaad: libfaad faacDecInit failed.\n"));
        this->faac_failed++;
      } else {
        lprintf( "faacDecInit() returned rate=%ld channels=%d (used=%d)\n",
                 this->rate, this->num_channels, used);
                      
        this->size -= used;
        memmove( this->buf, &this->buf[used], this->size );
      }
    }
  }
  
  if( this->faac_failed ) {
    if( this->faac_dec ) {
      faacDecClose( this->faac_dec );
      this->faac_dec = NULL;
    }
    _x_stream_info_set(this->stream, XINE_STREAM_INFO_AUDIO_HANDLED, 0);
  }
  
  return this->faac_failed;
}

static int faad_open_output( faad_decoder_t *this ) {
  this->rec_audio_src_size = this->num_channels * FAAD_MIN_STREAMSIZE;
       
  switch( this->num_channels ) {
    case 1:
      this->ao_cap_mode=AO_CAP_MODE_MONO; 
      break;
    case 6:
      if(this->stream->audio_out->get_capabilities(this->stream->audio_out) &
         AO_CAP_MODE_5_1CHANNEL) {
        this->ao_cap_mode = AO_CAP_MODE_5_1CHANNEL;
        break;
      } else {
        this->faac_cfg = faacDecGetCurrentConfiguration(this->faac_dec);
        this->faac_cfg->downMatrix = 1;
        faacDecSetConfiguration(this->faac_dec, this->faac_cfg);
        this->num_channels = 2;
      }
    case 2:
      this->ao_cap_mode=AO_CAP_MODE_STEREO;
      break; 
  }
   
  this->output_open = this->stream->audio_out->open (this->stream->audio_out,
                                             this->stream,
                                             this->bits_per_sample,
                                             this->rate,
                                             this->ao_cap_mode) ;
  return this->output_open;
}

static void faad_decode_audio ( faad_decoder_t *this, int end_frame ) {
  int used, decoded, outsize;
  uint8_t *sample_buffer;
  uint8_t *inbuf;
  audio_buffer_t *audio_buffer;
  int sample_size = this->size;
    
  if( !this->faac_dec )
    return;

  inbuf = this->buf;
  while( (!this->raw_mode && end_frame && this->size >= 10) ||
         (this->raw_mode && this->size >= this->rec_audio_src_size) ) {
      
    sample_buffer = faacDecDecode(this->faac_dec, 
                                  &this->faac_finfo, inbuf, sample_size);
 
    if( !sample_buffer ) {
      xprintf(this->stream->xine, XINE_VERBOSITY_DEBUG,
              "libfaad: %s\n", faacDecGetErrorMessage(this->faac_finfo.error));
      used = 1;    
    } else {
      used = this->faac_finfo.bytesconsumed;
          
      /* raw AAC parameters is only known after decoding the first frame */
      if( !this->dec_config &&
          (this->num_channels != this->faac_finfo.channels ||
           this->rate != this->faac_finfo.samplerate) ) {
    
        this->num_channels = this->faac_finfo.channels;
        this->rate = this->faac_finfo.samplerate;
	
        lprintf("faacDecDecode() returned rate=%ld channels=%d used=%d\n",
                this->rate, this->num_channels, used);
      
        this->stream->audio_out->close (this->stream->audio_out, this->stream);
        this->output_open = 0;
        faad_open_output( this );
      }
    
      decoded = this->faac_finfo.samples * 2; /* 1 sample = 2 bytes */

      lprintf("decoded %d/%d output %ld\n",
              used, this->size, this->faac_finfo.samples );
      
      while( decoded ) {
        audio_buffer = this->stream->audio_out->get_buffer (this->stream->audio_out);
        
        if( decoded < audio_buffer->mem_size )
          outsize = decoded; 
        else
          outsize = audio_buffer->mem_size;
      
        xine_fast_memcpy( audio_buffer->mem, sample_buffer, outsize );

        audio_buffer->num_frames = outsize / (this->num_channels*2);
        audio_buffer->vpts = this->pts;

        this->stream->audio_out->put_buffer (this->stream->audio_out, audio_buffer, this->stream);
        
        this->pts = 0;
        decoded -= outsize;
        sample_buffer += outsize;
      }
    }
      
    if(used >= this->size){
      this->size = 0;
    } else {
      this->size -= used;
      inbuf += used;
    }

    if( !this->raw_mode )
      this->size = 0;   
  }

  if( this->size )
    memmove( this->buf, inbuf, this->size);

}

static void faad_decode_data (audio_decoder_t *this_gen, buf_element_t *buf) {

  faad_decoder_t *this = (faad_decoder_t *) this_gen;
  
  if (buf->decoder_flags & BUF_FLAG_PREVIEW)
    return;

  /* store config information from ESDS mp4/qt atom */
  if( !this->faac_dec && (buf->decoder_flags & BUF_FLAG_SPECIAL) &&
      buf->decoder_info[1] == BUF_SPECIAL_DECODER_CONFIG ) {
    
    this->dec_config = xine_xmalloc(buf->decoder_info[2]);
    this->dec_config_size = buf->decoder_info[2];
    memcpy(this->dec_config, buf->decoder_info_ptr[2], buf->decoder_info[2]);
    
    if( faad_open_dec(this) )
      return;

    this->raw_mode = 0;
  }

  /* get audio parameters from file header 
     (may be overwritten by libfaad returned parameters) */  
  if (buf->decoder_flags & BUF_FLAG_STDHEADER) {
    this->rate=buf->decoder_info[1];
    this->bits_per_sample=buf->decoder_info[2] ; 
    this->num_channels=buf->decoder_info[3] ; 
 
    if( !this->bits_per_sample )
      this->bits_per_sample = 16;
    
    if( buf->size > sizeof(xine_waveformatex) ) {
      xine_waveformatex *wavex = (xine_waveformatex *) buf->content;

      if( wavex->cbSize > 0 ) {
        this->dec_config = xine_xmalloc(wavex->cbSize);
        this->dec_config_size = wavex->cbSize;
        memcpy(this->dec_config, buf->content + sizeof(xine_waveformatex),
               wavex->cbSize);
               
        if( faad_open_dec(this) )
          return;
      }
    }
                                               
    /* stream/meta info */
    _x_meta_info_set_utf8(this->stream, XINE_META_INFO_AUDIOCODEC,
      "AAC (libfaad)");

  } else {

    lprintf ("decoding %d data bytes...\n", buf->size);

    if( (int)buf->size <= 0 || this->faac_failed )
      return;

    if( !this->size )
      this->pts = buf->pts;
  
    if( this->size + buf->size > this->max_audio_src_size ) {
      this->max_audio_src_size = this->size + 2 * buf->size;
      this->buf = realloc( this->buf, this->max_audio_src_size );
    }
  
    memcpy (&this->buf[this->size], buf->content, buf->size);
    this->size += buf->size;
    
    if( !this->faac_dec && faad_open_dec(this) )
      return;

    /* open audio device as needed */
    if (!this->output_open) {
      faad_open_output( this );
    }

    faad_decode_audio(this, buf->decoder_flags & BUF_FLAG_FRAME_END );
  }
}

static void faad_discontinuity (audio_decoder_t *this_gen) {
}

static void faad_dispose (audio_decoder_t *this_gen) {

  faad_decoder_t *this = (faad_decoder_t *) this_gen; 

  if (this->output_open) 
    this->stream->audio_out->close (this->stream->audio_out, this->stream);
  this->output_open = 0;
    
  if( this->buf )
    free(this->buf);
  this->buf = NULL;
  this->size = 0;
  this->max_audio_src_size = 0;
  
  if( this->dec_config )
    free(this->dec_config);
  this->dec_config = NULL;
  this->dec_config_size = 0;
  
  if( this->faac_dec )
    faacDecClose(this->faac_dec);
  this->faac_dec = NULL;
  this->faac_failed = 0;

  free (this);
}


static audio_decoder_t *open_plugin (audio_decoder_class_t *class_gen, xine_stream_t *stream) {

  faad_decoder_t *this ;

  this = (faad_decoder_t *) xine_xmalloc (sizeof (faad_decoder_t));

  this->audio_decoder.decode_data         = faad_decode_data;
  this->audio_decoder.reset               = faad_reset;
  this->audio_decoder.discontinuity       = faad_discontinuity;
  this->audio_decoder.dispose             = faad_dispose;

  this->stream             = stream;
  this->output_open        = 0;
  this->raw_mode           = 1;
  this->faac_dec           = NULL; 
  this->faac_failed        = 0;
  this->buf                = NULL;
  this->size               = 0;
  this->max_audio_src_size = 0;
  this->dec_config         = NULL;
  this->dec_config_size    = 0;

  this->rate               = 44100;

  return &this->audio_decoder;
}

static char *get_identifier (audio_decoder_class_t *this) {
  return "FAAD";
}

static char *get_description (audio_decoder_class_t *this) {
  return "Freeware Advanced Audio Decoder";
}

static void dispose_class (audio_decoder_class_t *this) {
  free (this);
}

static void *init_plugin (xine_t *xine, void *data) {

  faad_class_t *this ;

  this = (faad_class_t *) xine_xmalloc (sizeof (faad_class_t));

  this->decoder_class.open_plugin     = open_plugin;
  this->decoder_class.get_identifier  = get_identifier;
  this->decoder_class.get_description = get_description;
  this->decoder_class.dispose         = dispose_class;

  return this;
}

static uint32_t audio_types[] = { 
  BUF_AUDIO_AAC, 0
 };

static decoder_info_t dec_info_audio = {
  audio_types,         /* supported types */
  1                    /* priority        */
};

plugin_info_t xine_plugin_info[] = {
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_AUDIO_DECODER, 15, "faad", XINE_VERSION_CODE, &dec_info_audio, init_plugin },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
