/*
 * Copyright (C) 2000-2004 the xine project
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
 * $Id: planar.c,v 1.8 2004/04/17 19:54:32 mroi Exp $
 *
 * catalog for planar post plugins
 */

#include "xine_internal.h"
#include "post.h"
#include "xineutils.h"

extern void *invert_init_plugin(xine_t *xine, void *);
post_info_t invert_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

extern void *expand_init_plugin(xine_t *xine, void *);
post_info_t expand_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

extern void *eq_init_plugin(xine_t *xine, void *);
post_info_t eq_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

extern void *boxblur_init_plugin(xine_t *xine, void *);
post_info_t boxblur_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

extern void *denoise3d_init_plugin(xine_t *xine, void *);
post_info_t denoise3d_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

extern void *eq2_init_plugin(xine_t *xine, void *);
post_info_t eq2_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

extern void *unsharp_init_plugin(xine_t *xine, void *);
post_info_t unsharp_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

extern void *pp_init_plugin(xine_t *xine, void *);
post_info_t pp_special_info = { XINE_POST_TYPE_VIDEO_FILTER };

plugin_info_t xine_plugin_info[] = {
  /* type, API, "name", version, special_info, init_function */  
  { PLUGIN_POST, 9, "expand", XINE_VERSION_CODE, &expand_special_info, &expand_init_plugin },
  { PLUGIN_POST, 9, "invert", XINE_VERSION_CODE, &invert_special_info, &invert_init_plugin },
  { PLUGIN_POST, 9, "eq", XINE_VERSION_CODE, &eq_special_info, &eq_init_plugin },
  { PLUGIN_POST, 9, "denoise3d", XINE_VERSION_CODE, &denoise3d_special_info, &denoise3d_init_plugin },
  { PLUGIN_POST, 9, "boxblur", XINE_VERSION_CODE, &boxblur_special_info, &boxblur_init_plugin },
  { PLUGIN_POST, 9, "eq2", XINE_VERSION_CODE, &eq2_special_info, &eq2_init_plugin },
  { PLUGIN_POST, 9, "unsharp", XINE_VERSION_CODE, &unsharp_special_info, &unsharp_init_plugin },
  { PLUGIN_POST, 9, "pp", XINE_VERSION_CODE, &pp_special_info, &pp_init_plugin },
  { PLUGIN_NONE, 0, "", 0, NULL, NULL }
};
