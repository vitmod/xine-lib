/* 
 * Copyright (C) 2000-2001 the xine project
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
 * $Id: video_out_aa.c,v 1.38 2003/10/22 20:38:10 komadori Exp $
 *
 * video_out_aa.c, ascii-art output plugin for xine
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>

#include <aalib.h>

#include "xine.h"
#include "video_out.h"
#include "xine_internal.h"
#include "xineutils.h"

/*
 * global variables
 */

typedef struct aa_frame_s {

  vo_frame_t    vo_frame;

  int           width, height;
  uint8_t      *mem[3];

  double        ratio;

  int           format;

} aa_frame_t;

typedef struct {
  vo_driver_t        vo_driver;

  config_values_t   *config;
  int                user_ratio;
  aa_context        *context;

} aa_driver_t;

typedef struct {

  video_driver_class_t driver_class;
  config_values_t     *config;

} aa_class_t;

/*
 * our video driver
 */
static uint32_t aa_get_capabilities (vo_driver_t *this) {
  return VO_CAP_YV12 | VO_CAP_YUY2;
}

static void aa_dispose_frame (vo_frame_t *vo_img) {
  aa_frame_t *frame = (aa_frame_t *)vo_img;
  
  if (frame->mem[0])
    free (frame->mem[0]);
  if (frame->mem[1])
    free (frame->mem[1]);
  if (frame->mem[2])
    free (frame->mem[2]);

  free (frame);
}

static void aa_frame_field (vo_frame_t *vo_img, int which_field) {
  /* nothing to be done here */
}


static vo_frame_t *aa_alloc_frame(vo_driver_t *this) {
  aa_frame_t *frame;

  frame = (aa_frame_t *) malloc (sizeof (aa_frame_t));
  memset (frame, 0, sizeof (aa_frame_t));

  frame->vo_frame.proc_slice = NULL;
  frame->vo_frame.proc_frame = NULL;
  frame->vo_frame.field = aa_frame_field;
  frame->vo_frame.dispose = aa_dispose_frame;
  frame->vo_frame.driver = this;
  
  return (vo_frame_t*) frame;
}

static void aa_update_frame_format (vo_driver_t *this, vo_frame_t *img,
				    uint32_t width, uint32_t height, 
				    double ratio, int format, int flags) {

  aa_frame_t *frame = (aa_frame_t *) img;

  /* printf ("aa_update_format...\n"); */

  if ((frame->width != width) || (frame->height != height) 
      || (frame->format != format)) {

    if (frame->mem[0]) {
      free (frame->mem[0]);
      frame->mem[0] = NULL;
    }
    if (frame->mem[1]) {
      free (frame->mem[1]);
      frame->mem[1] = NULL;
    }
      
    if (frame->mem[2]) {
      free (frame->mem[2]);
      frame->mem[2] = NULL;
    }

    frame->width  = width;
    frame->height = height;
    frame->format = format;


    if (format == XINE_IMGFMT_YV12) {
      frame->vo_frame.pitches[0] = 8*((width + 7) / 8);
      frame->vo_frame.pitches[1] = 8*((width + 15) / 16);
      frame->vo_frame.pitches[2] = 8*((width + 15) / 16);
      frame->vo_frame.base[0] = xine_xmalloc_aligned(16, frame->vo_frame.pitches[0] * height, (void**) &frame->mem[0]);
      frame->vo_frame.base[1] = xine_xmalloc_aligned(16, frame->vo_frame.pitches[1] * ((height+1)/2), (void**) &frame->mem[1]);
      frame->vo_frame.base[2] = xine_xmalloc_aligned(16, frame->vo_frame.pitches[2] * ((height+1)/2), (void**) &frame->mem[2]);

      /* printf ("allocated yuv memory for %d x %d image\n", width, height); */

    } else if (format == XINE_IMGFMT_YUY2) {
      frame->vo_frame.pitches[0] = 8*((width + 3) / 4);
      frame->vo_frame.base[0] = xine_xmalloc_aligned(16, frame->vo_frame.pitches[0] * height, (void**) &frame->mem[0]);
    } else {
      printf ("alert! unsupported image format %04x\n", format);
      abort();
    }

    frame->ratio = ratio;

  }

  /* printf ("aa_update_format done\n"); */
}

static void aa_display_frame (vo_driver_t *this_gen, vo_frame_t *frame_gen) {
  int x,y;
  double x_fact, y_fact; /* ratio between aa's and frame's width/height */

  uint8_t *img;
  uint8_t *src_image;

  aa_driver_t *this = (aa_driver_t*) this_gen;
  aa_frame_t *frame = (aa_frame_t *) frame_gen;

  x_fact = (double) frame->width / (double) aa_imgwidth (this->context);
  y_fact = (double) frame->height / (double) aa_imgheight (this->context);

  src_image = frame->vo_frame.base[0];
  img = aa_image(this->context); /* pointer to the beginning of the output */

  /*
  fprintf(stderr,
	  "aalib sez: width: %d, height: %d\n",
	  aa_imgwidth (this->context),
	  aa_imgheight (this->context));
  */

  if (frame->format == XINE_IMGFMT_YV12) {
    for (y = 0; y<aa_imgheight (this->context); y++) {
      for (x = 0; x<aa_imgwidth (this->context); x++) {
      
	*img++ = src_image[((int)((double) x * x_fact) +
			    frame->width * (int)((double) y * y_fact))];
      
      }
    }
  } else {
    for (y = 0; y<aa_imgheight (this->context); y++) {
      for (x = 0; x<aa_imgwidth (this->context); x++) {
      
	*img++ = src_image[((int)((double) x * x_fact) * 2 +
			    frame->width * 2 * (int)((double) y * y_fact))];
      
      }
    }
  }

  frame->vo_frame.free (&frame->vo_frame);

  aa_fastrender(this->context, 0, 0, 
		aa_imgwidth (this->context), 
		aa_imgheight (this->context));

  aa_flush (this->context);

}

static int aa_get_property (vo_driver_t *this_gen, int property) {
  aa_driver_t *this = (aa_driver_t*) this_gen;
  
  if ( property == VO_PROP_ASPECT_RATIO) {
    return this->user_ratio ;
  } else {
    printf ("video_out_aa: tried to get unsupported property %d\n", property);
  }

  return 0;
}

static int aa_set_property (vo_driver_t *this_gen, 
			    int property, int value) {
  aa_driver_t *this = (aa_driver_t*) this_gen;

  if ( property == VO_PROP_ASPECT_RATIO) {
    if (value>=XINE_VO_ASPECT_NUM_RATIOS)
      value = XINE_VO_ASPECT_AUTO;
    this->user_ratio = value;

  } else {
    printf ("video_out_aa: tried to set unsupported property %d\n", property);
  }

  return value;
}

static void aa_get_property_min_max (vo_driver_t *this_gen, 
				     int property, int *min, int *max) {
  *min = 0;
  *max = 0;
}

static void aa_dispose (vo_driver_t *this_gen) {
}

static int aa_redraw_needed (vo_driver_t *this_gen) {
  return 0;
}

static vo_driver_t *open_plugin (video_driver_class_t *class_gen, const void *visual_gen) {
  aa_class_t           *class = (aa_class_t *) class_gen;
  aa_driver_t          *this;

  this = (aa_driver_t*) malloc (sizeof (aa_driver_t));
  
  this->context = (aa_context*) visual_gen;
  
  this->config = class->config;

  this->vo_driver.get_capabilities     = aa_get_capabilities;
  this->vo_driver.alloc_frame          = aa_alloc_frame ;
  this->vo_driver.update_frame_format  = aa_update_frame_format;
  this->vo_driver.display_frame        = aa_display_frame;
  this->vo_driver.overlay_begin        = NULL;
  this->vo_driver.overlay_blend        = NULL;
  this->vo_driver.overlay_end          = NULL;
  this->vo_driver.get_property         = aa_get_property;
  this->vo_driver.set_property         = aa_set_property;
  this->vo_driver.get_property_min_max = aa_get_property_min_max;
  this->vo_driver.gui_data_exchange    = NULL;
  this->vo_driver.redraw_needed        = aa_redraw_needed;
  this->vo_driver.dispose              = aa_dispose;

  return &this->vo_driver;
}    

static char* get_identifier (video_driver_class_t *this_gen) {
  return "AA";
}

static char* get_description (video_driver_class_t *this_gen) {
  return _("xine video output plugin using the ascii-art library");
}

static void dispose_class (video_driver_class_t *this_gen) {
  aa_class_t   *this = (aa_class_t *) this_gen;
  free(this);
}
static void *init_class (xine_t *xine, void *visual_gen) {
  /* aa_context    *context = (aa_context*) visual_gen; */
  aa_class_t    *this;
  
  this = (aa_class_t *) malloc(sizeof(aa_class_t));
  
  this->driver_class.open_plugin     = open_plugin;
  this->driver_class.get_identifier  = get_identifier;
  this->driver_class.get_description = get_description;
  this->driver_class.dispose         = dispose_class;
  
  this->config            = xine->config;

  return this;
}

static vo_info_t vo_info_aa = {
  6,
  XINE_VISUAL_TYPE_AA
};

plugin_info_t xine_plugin_info[] = {
  /* type, API, "name", version, special_info, init_function */  
  { PLUGIN_VIDEO_OUT, 18, "aa", XINE_VERSION_CODE, &vo_info_aa, init_class },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
