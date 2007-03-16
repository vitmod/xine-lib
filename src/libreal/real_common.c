/*
 * Copyright (C) 2000-2007 the xine project
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
 * $Id: real_common.c,v 1.4 2007/03/16 21:37:58 dgp85 Exp $
 *
 * Common function for the thin layer to use Real binary-only codecs in xine
 */

#define LOG_MODULE "real_common"
#define LOG_VERBOSE
/*
#define LOG
*/

#include "config.h"

#include <sys/stat.h>
#include <string.h>
#include <dlfcn.h>

#include "real_common.h"

#ifdef __alpha__

void *__builtin_new(size_t size) {
  return malloc(size);
}

void __builtin_delete (void *foo) {
  /* printf ("libareal: __builtin_delete called\n"); */
  free (foo);
}

void *__builtin_vec_new(size_t size) {
  return malloc(size);
}

void __builtin_vec_delete(void *mem) {
  free(mem);
}

void __pure_virtual(void) {
  lprintf("libreal: FATAL: __pure_virtual() called!\n");
  /*      exit(1); */
}

#endif

#ifdef __FreeBSD__ /* TODO: alias them if at all possible */
void ___brk_addr(void) { exit(0); }
void __ctype_b(void) { exit(0); }
#endif

void _x_real_codecs_init(xine_t *const xine) {
  const char *default_real_codecs_path = "";
  const char *real_codecs_path = NULL;
  struct stat s;

#define try_real_path(path) \
  if (!stat (path "/dvr3.so.6.0", &s)) \
    default_real_codecs_path = path;
#define try_real_subpath(path) \
  try_real_path("/usr/" path) \
  else try_real_path("/usr/local" path) \
  else try_real_path("/opt" path)

  /* The priority is for the first found */
  try_real_subpath("lib/win32")
  else try_real_subpath("lib/codecs")
  else try_real_subpath("lib64/RealPlayer10/codecs")
  else try_real_subpath("lib/RealPlayer10/codecs")
  else try_real_subpath("lib64/RealPlayer9/users/Real/Codecs")
  else try_real_subpath("lib/RealPlayer9/users/Real/Codecs")
  else try_real_subpath("lib/RealPlayer8/Codecs")
  else try_real_subpath("RealPlayer8/Codecs");

#undef try_real_path
#undef try_real_subpath

  real_codecs_path = 
    xine->config->register_filename (xine->config, "decoder.external.real_codecs_path", 
				     default_real_codecs_path,
				     XINE_CONFIG_STRING_IS_DIRECTORY_NAME,
				     _("path to RealPlayer codecs"),
				     _("If you have RealPlayer installed, specify the path "
				       "to its codec directory here. You can easily find "
				       "the codec directory by looking for a file named "
				       "\"drv3.so.6.0\" in it. If xine can find the RealPlayer "
				       "codecs, it will use them to decode RealPlayer content "
				       "for you. Consult the xine FAQ for more information on "
				       "how to install the codecs."),
				     10, NULL, NULL);

  lprintf ("real codecs path : %s\n",  real_codec_path);
}

void *_x_real_codec_open(xine_stream_t *const stream, const char *const path,
			 const char *const codec_name) {
  char *codecpath = NULL;
  void *codecmodule = NULL;
  
  asprintf(&codecpath, "%s/%s.6.0", path, codec_name);
  if ( (codecmodule = dlopen(codecpath, RTLD_NOW)) ) {
    free(codecpath);
    return codecmodule;
  }

  xprintf (stream->xine, XINE_VERBOSITY_DEBUG,
	   LOG_MODULE ": error loading %s: %s\n", codecpath, dlerror());

  free(codecpath);
  asprintf(&codecpath, "%s/%s", path, codec_name);
  if ( (codecmodule = dlopen(codecpath, RTLD_NOW)) ) {
    free(codecpath);
    return codecmodule;
  }

  xprintf (stream->xine, XINE_VERBOSITY_DEBUG,
	   LOG_MODULE ": error loading %s: %s\n", codecpath, dlerror());

  free(codecpath);

  _x_message(stream, XINE_MSG_LIBRARY_LOAD_ERROR, codec_name, NULL);
  
  return NULL;
}
