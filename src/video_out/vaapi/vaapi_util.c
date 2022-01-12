/*
 * Copyright (C) 2012 Edgar Hucek <gimli|@dark-green.com>
 * Copyright (C) 2012-2022 xine developers
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
 *
 * vaapi_util.c, VAAPI video extension interface for xine
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vaapi_util.h"

#include <stdlib.h>

#include <xine/xine_internal.h>
#include <xine/xineutils.h>

#include <va/va.h>
#include <va/va_x11.h>

#if defined(HAVE_VA_VA_GLX_H)
# include <va/va_glx.h>
#endif

#if defined(LOG) || defined(DEBUG)
static const char *_x_va_string_of_VAImageFormat(VAImageFormat *imgfmt)
{
  static char str[5];
  str[0] = imgfmt->fourcc;
  str[1] = imgfmt->fourcc >> 8;
  str[2] = imgfmt->fourcc >> 16;
  str[3] = imgfmt->fourcc >> 24;
  str[4] = '\0';
  return str;
}
#endif

const char *_x_va_profile_to_string(VAProfile profile)
{
  switch(profile) {
#define PROFILE(profile) \
    case VAProfile##profile: return "VAProfile" #profile
      PROFILE(MPEG2Simple);
      PROFILE(MPEG2Main);
      PROFILE(MPEG4Simple);
      PROFILE(MPEG4AdvancedSimple);
      PROFILE(MPEG4Main);
      PROFILE(H264Main);
      PROFILE(H264High);
      PROFILE(VC1Simple);
      PROFILE(VC1Main);
      PROFILE(VC1Advanced);
#if VA_CHECK_VERSION(0, 37, 0)
      PROFILE(HEVCMain);
      PROFILE(HEVCMain10);
#endif
#undef PROFILE
    default: break;
  }
  return "<unknown>";
}

const char *_x_va_entrypoint_to_string(VAEntrypoint entrypoint)
{
  switch(entrypoint)
  {
#define ENTRYPOINT(entrypoint) \
    case VAEntrypoint##entrypoint: return "VAEntrypoint" #entrypoint
      ENTRYPOINT(VLD);
      ENTRYPOINT(IZZ);
      ENTRYPOINT(IDCT);
      ENTRYPOINT(MoComp);
      ENTRYPOINT(Deblocking);
#undef ENTRYPOINT
    default: break;
  }
  return "<unknown>";
}

int _x_va_check_status(vaapi_context_impl_t *this, VAStatus vaStatus, const char *msg)
{
  if (vaStatus != VA_STATUS_SUCCESS) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " Error : %s: %s\n", msg, vaErrorStr(vaStatus));
    return 0;
  }
  return 1;
}

void _x_va_reset_va_context(ff_vaapi_context_t *va_context)
{
  int i;

  va_context->va_config_id              = VA_INVALID_ID;
  va_context->va_context_id             = VA_INVALID_ID;
  va_context->valid_context             = 0;
  va_context->va_head                   = 0;

  for(i = 0; i < RENDER_SURFACES; i++) {
    ff_vaapi_surface_t *va_surface      = &va_context->va_render_surfaces[i];

    va_surface->index                   = i;
    va_surface->status                  = SURFACE_FREE;
    va_surface->va_surface_id           = VA_INVALID_SURFACE;

    va_context->va_surface_ids[i]       = VA_INVALID_SURFACE;
  }
}

static VADisplay _get_display(void *native_display, int opengl_render)
{
  VADisplay ret;

  if (opengl_render) {
#if defined(HAVE_VA_VA_GLX_H)
    ret = vaGetDisplayGLX(native_display);
#else
    return NULL;
#endif
  } else {
    ret = vaGetDisplay(native_display);
  }

  if (vaDisplayIsValid(ret))
    return ret;

  return NULL;
}

VAStatus _x_va_terminate(ff_vaapi_context_t *va_context)
{
  VAStatus vaStatus = VA_STATUS_SUCCESS;

  _x_freep(&va_context->va_image_formats);
  va_context->va_num_image_formats  = 0;

  if (va_context->va_display) {
    vaStatus = vaTerminate(va_context->va_display);
    va_context->va_display = NULL;
  }

  return vaStatus;
}

VAStatus _x_va_initialize(ff_vaapi_context_t *va_context, void *display, int opengl_render)
{
  VAStatus vaStatus;
  int      maj, min;
  int      fmt_count = 0;

  va_context->va_display = _get_display(display, opengl_render);
  if (!va_context->va_display) {
    return VA_STATUS_ERROR_UNKNOWN;
  }

  vaStatus = vaInitialize(va_context->va_display, &maj, &min);
  if (vaStatus != VA_STATUS_SUCCESS) {
    goto fail;
  }

  lprintf("libva: %d.%d\n", maj, min);

  fmt_count = vaMaxNumImageFormats(va_context->va_display);
  va_context->va_image_formats = calloc(fmt_count, sizeof(*va_context->va_image_formats));
  if (!va_context->va_image_formats) {
    goto fail;
  }

  vaStatus = vaQueryImageFormats(va_context->va_display, va_context->va_image_formats, &va_context->va_num_image_formats);
  if (vaStatus != VA_STATUS_SUCCESS) {
    goto fail;
  }

  return vaStatus;

fail:
  _x_va_terminate(va_context);
  return vaStatus;
}

void _x_va_destroy_image(vaapi_context_impl_t *va_context, VAImage *va_image)
{
  VAStatus              vaStatus;

  if (va_image->image_id != VA_INVALID_ID) {
    lprintf("vaapi_destroy_image 0x%08x\n", va_image->image_id);
    vaStatus = vaDestroyImage(va_context->c.va_display, va_image->image_id);
    _x_va_check_status(va_context, vaStatus, "vaDestroyImage()");
  }
  va_image->image_id      = VA_INVALID_ID;
  va_image->width         = 0;
  va_image->height        = 0;
}

VAStatus _x_va_create_image(vaapi_context_impl_t *va_context, VASurfaceID va_surface_id, VAImage *va_image, int width, int height, int clear, int *is_bound)
{
  int i = 0;
  VAStatus vaStatus;

  if (!va_context->c.valid_context || va_context->c.va_image_formats == NULL || va_context->c.va_num_image_formats == 0)
    return VA_STATUS_ERROR_UNKNOWN;

  *is_bound = 0;

  vaStatus = vaDeriveImage(va_context->c.va_display, va_surface_id, va_image);
  if (vaStatus == VA_STATUS_SUCCESS) {
    if (va_image->image_id != VA_INVALID_ID && va_image->buf != VA_INVALID_ID) {
      *is_bound = 1;
    }
  }

  if (!*is_bound) {
    for (i = 0; i < va_context->c.va_num_image_formats; i++) {
      if (va_context->c.va_image_formats[i].fourcc == VA_FOURCC( 'Y', 'V', '1', '2' ) ||
          va_context->c.va_image_formats[i].fourcc == VA_FOURCC( 'I', '4', '2', '0' ) /*||
          va_context->c.va_image_formats[i].fourcc == VA_FOURCC( 'N', 'V', '1', '2' ) */) {
        vaStatus = vaCreateImage( va_context->c.va_display, &va_context->c.va_image_formats[i], width, height, va_image );
        if (!_x_va_check_status(va_context, vaStatus, "vaCreateImage()"))
          goto error;
        break;
      }
    }
  }

  void *p_base = NULL;

  vaStatus = vaMapBuffer( va_context->c.va_display, va_image->buf, &p_base );
  if (!_x_va_check_status(va_context, vaStatus, "vaMapBuffer()"))
    goto error;

  if (clear) {
    if (va_image->format.fourcc == VA_FOURCC( 'Y', 'V', '1', '2' ) ||
        va_image->format.fourcc == VA_FOURCC( 'I', '4', '2', '0' )) {
      memset((uint8_t*)p_base + va_image->offsets[0],   0, va_image->pitches[0] * va_image->height);
      memset((uint8_t*)p_base + va_image->offsets[1], 128, va_image->pitches[1] * (va_image->height/2));
      memset((uint8_t*)p_base + va_image->offsets[2], 128, va_image->pitches[2] * (va_image->height/2));
    } else if (va_image->format.fourcc == VA_FOURCC( 'N', 'V', '1', '2' ) ) {
      memset((uint8_t*)p_base + va_image->offsets[0],   0, va_image->pitches[0] * va_image->height);
      memset((uint8_t*)p_base + va_image->offsets[1], 128, va_image->pitches[1] * (va_image->height/2));
    }
  }

  vaStatus = vaUnmapBuffer( va_context->c.va_display, va_image->buf );
  _x_va_check_status(va_context, vaStatus, "vaUnmapBuffer()");

  lprintf("_x_va_create_image 0x%08x width %d height %d format %s\n", va_image->image_id, va_image->width, va_image->height,
      _x_va_string_of_VAImageFormat(&va_image->format));

  return VA_STATUS_SUCCESS;

error:
  /* house keeping */
  _x_va_destroy_image(va_context, va_image);
  return VA_STATUS_ERROR_UNKNOWN;
}

static VAStatus _x_va_destroy_render_surfaces(vaapi_context_impl_t *va_context)
{
  int                 i;
  VAStatus            vaStatus;

  for (i = 0; i < RENDER_SURFACES; i++) {
    if (va_context->c.va_surface_ids[i] != VA_INVALID_SURFACE) {
      vaStatus = vaSyncSurface(va_context->c.va_display, va_context->c.va_surface_ids[i]);
      _x_va_check_status(va_context, vaStatus, "vaSyncSurface()");
      vaStatus = vaDestroySurfaces(va_context->c.va_display, &va_context->c.va_surface_ids[i], 1);
      _x_va_check_status(va_context, vaStatus, "vaDestroySurfaces()");
      va_context->c.va_surface_ids[i] = VA_INVALID_SURFACE;

      ff_vaapi_surface_t *va_surface  = &va_context->c.va_render_surfaces[i];
      va_surface->index               = i;
      va_surface->status              = SURFACE_FREE;
      va_surface->va_surface_id       = va_context->c.va_surface_ids[i];
    }
  }

  return VA_STATUS_SUCCESS;
}

void _x_va_close(vaapi_context_impl_t *va_context)
{
  VAStatus vaStatus;

  if (va_context->c.va_context_id != VA_INVALID_ID) {
    vaStatus = vaDestroyContext(va_context->c.va_display, va_context->c.va_context_id);
    _x_va_check_status(va_context, vaStatus, "vaDestroyContext()");
    va_context->c.va_context_id = VA_INVALID_ID;
  }

  _x_va_destroy_render_surfaces(va_context);

  if (va_context->c.va_config_id != VA_INVALID_ID) {
    vaStatus = vaDestroyConfig(va_context->c.va_display, va_context->c.va_config_id);
    _x_va_check_status(va_context, vaStatus, "vaDestroyConfig()");
    va_context->c.va_config_id = VA_INVALID_ID;
  }

  va_context->c.valid_context = 0;

  _x_va_reset_va_context(&va_context->c);
}

VAStatus _x_va_init(vaapi_context_impl_t *va_context, int va_profile, int width, int height)
{
  VAConfigAttrib va_attrib;
  VAStatus       vaStatus;
  size_t         i;

  _x_va_close(va_context);

  va_context->query_va_status = 1;

  const char *vendor = vaQueryVendorString(va_context->c.va_display);
  xprintf(va_context->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Vendor : %s\n", vendor);

  const char *p = vendor;
  for (i = strlen (vendor); i > 0; i--, p++) {
    if (strncmp(p, "VDPAU", strlen("VDPAU")) == 0) {
      xprintf(va_context->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Enable Splitted-Desktop Systems VDPAU-VIDEO workarounds.\n");
      va_context->query_va_status = 0;
      break;
    }
  }

  va_context->c.width = width;
  va_context->c.height = height;

  xprintf(va_context->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : Context width %d height %d\n", va_context->c.width, va_context->c.height);

  /* allocate decoding surfaces */
  unsigned rt_format = VA_RT_FORMAT_YUV420;
#if VA_CHECK_VERSION(0, 37, 0) && defined (VA_RT_FORMAT_YUV420_10BPP)
  if (va_profile == VAProfileHEVCMain10) {
    rt_format = VA_RT_FORMAT_YUV420_10BPP;
  }
#endif
  vaStatus = vaCreateSurfaces(va_context->c.va_display, rt_format, va_context->c.width, va_context->c.height, va_context->c.va_surface_ids, RENDER_SURFACES, NULL, 0);
  if (!_x_va_check_status(va_context, vaStatus, "vaCreateSurfaces()"))
    goto error;

  /* hardware decoding needs more setup */
  if (va_profile >= 0) {
    xprintf(va_context->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : Profile: %d (%s) Entrypoint %d (%s) Surfaces %d\n",
            va_profile, _x_va_profile_to_string(va_profile), VAEntrypointVLD, _x_va_entrypoint_to_string(VAEntrypointVLD), RENDER_SURFACES);

    memset (&va_attrib, 0, sizeof(va_attrib));
    va_attrib.type = VAConfigAttribRTFormat;

    vaStatus = vaGetConfigAttributes(va_context->c.va_display, va_profile, VAEntrypointVLD, &va_attrib, 1);
    if (!_x_va_check_status(va_context, vaStatus, "vaGetConfigAttributes()"))
      goto error;

    if ((va_attrib.value & VA_RT_FORMAT_YUV420) == 0)
      goto error;

    vaStatus = vaCreateConfig(va_context->c.va_display, va_profile, VAEntrypointVLD, &va_attrib, 1, &va_context->c.va_config_id);
    if (!_x_va_check_status(va_context, vaStatus, "vaCreateConfig()")) {
      va_context->c.va_config_id = VA_INVALID_ID;
      goto error;
    }

    vaStatus = vaCreateContext(va_context->c.va_display, va_context->c.va_config_id, va_context->c.width, va_context->c.height,
                               VA_PROGRESSIVE, va_context->c.va_surface_ids, RENDER_SURFACES, &va_context->c.va_context_id);
    if (!_x_va_check_status(va_context, vaStatus, "vaCreateContext()")) {
      va_context->c.va_context_id = VA_INVALID_ID;
      goto error;
    }
  }

  va_context->c.valid_context = 1;
  return VA_STATUS_SUCCESS;

 error:
  _x_va_close(va_context);
  return VA_STATUS_ERROR_UNKNOWN;
}