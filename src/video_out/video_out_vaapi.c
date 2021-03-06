/*
 * Copyright (C) 2012 Edgar Hucek <gimli|@dark-green.com>
 * Copyright (C) 2012-2020 xine developers
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
 * video_out_vaapi.c, VAAPI video extension interface for xine
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <sys/types.h>
#if defined(__FreeBSD__)
#include <machine/param.h>
#endif
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <time.h>
#include <unistd.h>

#define LOG_MODULE "video_out_vaapi"
#define LOG_VERBOSE
/*
#define LOG
*/
/*
#define DEBUG_SURFACE
*/
#include "xine.h"
#include <xine/video_out.h>
#include <xine/xine_internal.h>
#include <xine/xineutils.h>
#include <xine/vo_scale.h>

#include <va/va_x11.h>

#if defined(HAVE_OPENGL) && defined(HAVE_VA_VA_GLX_H)
# ifdef HAVE_GLU
#   include <GL/glu.h>
# endif
# include <GL/glx.h>
# include <GL/glext.h>
# include <GL/gl.h>
# include <dlfcn.h>

# include <va/va_glx.h>
# define ENABLE_VA_GLX
#endif /* OPENGL */

#include "vaapi/vaapi_util.h"
#include "vaapi/vaapi_frame.h"
#include "vaapi/xine_va_display.h" /* interop flags */

#include <pthread.h>

#ifndef VA_SURFACE_ATTRIB_SETTABLE
#define vaCreateSurfaces(d, f, w, h, s, ns, a, na) \
    vaCreateSurfaces(d, w, h, f, ns, s)
#endif

#define  MIN_SURFACES     22
#define  SOFT_SURFACES    3
#define  SW_WIDTH         1920
#define  SW_HEIGHT        1080
#define  STABLE_FRAME_COUNTER 4
#define  SW_CONTEXT_INIT_FORMAT -1 //VAProfileH264Main

#if defined VA_SRC_BT601 && defined VA_SRC_BT709
# define USE_VAAPI_COLORSPACE 1
#else
# define USE_VAAPI_COLORSPACE 0
#endif

#define FOVY     60.0f
#define ASPECT   1.0f
#define Z_NEAR   0.1f
#define Z_FAR    100.0f
#define Z_CAMERA 0.869f

#ifndef GLAPIENTRY
#ifdef APIENTRY
#define GLAPIENTRY APIENTRY
#else
#define GLAPIENTRY
#endif
#endif

#ifndef HAVE_THREAD_SAFE_X11
#define LOCK_DISPLAY(_this) XLockDisplay (_this->display)
#define UNLOCK_DISPLAY(_this) XUnlockDisplay (_this->display)
#else
#define LOCK_DISPLAY(_this)
#define UNLOCK_DISPLAY(_this)
#endif

#define RECT_IS_EQ(a, b) ((a).x1 == (b).x1 && (a).y1 == (b).y1 && (a).x2 == (b).x2 && (a).y2 == (b).y2)

static const char *const scaling_level_enum_names[] = {
  "default",  /* VA_FILTER_SCALING_DEFAULT       */
  "fast",     /* VA_FILTER_SCALING_FAST          */
  "hq",       /* VA_FILTER_SCALING_HQ            */
  "nla",      /* VA_FILTER_SCALING_NL_ANAMORPHIC */
  NULL
};

static const int scaling_level_enum_values[] = {
  VA_FILTER_SCALING_DEFAULT,
  VA_FILTER_SCALING_FAST,
  VA_FILTER_SCALING_HQ,
  VA_FILTER_SCALING_NL_ANAMORPHIC
};

typedef struct vaapi_driver_s vaapi_driver_t;

typedef struct {
    int x1, y1, x2, y2;
} vaapi_rect_t;

typedef struct {
  VADisplayAttribType type;
  int                 value;
  int                 min;
  int                 max;
  int                 atom;

  cfg_entry_t        *entry;

  vaapi_driver_t     *this;

} va_property_t;

struct vaapi_driver_s {

  vo_driver_t        vo_driver;

  /* X11 related stuff */
  Display            *display;
  int                 screen;
  Drawable            drawable;
  XColor              black;
  Window              window;

  uint32_t            capabilities;

  int ovl_changed;
  vo_overlay_t       *overlays[XINE_VORAW_MAX_OVL];
  uint32_t           *overlay_bitmap;
  uint32_t            overlay_bitmap_size;
  uint32_t            overlay_bitmap_width;
  uint32_t            overlay_bitmap_height;
  vaapi_rect_t        overlay_bitmap_src;
  vaapi_rect_t        overlay_bitmap_dst;

  uint32_t            vdr_osd_width;
  uint32_t            vdr_osd_height;

  uint32_t            overlay_output_width;
  uint32_t            overlay_output_height;
  vaapi_rect_t        overlay_dirty_rect;
  int                 has_overlay;

  /* all scaling information goes here */
  vo_scale_t          sc;

  xine_t             *xine;

  unsigned int        deinterlace;

#ifdef ENABLE_VA_GLX
  int                 opengl_render;
  unsigned int        init_opengl_render;
  int                 opengl_use_tfp;

  GLuint              gl_texture;
  GLXContext          gl_context;
  Pixmap              gl_pixmap;
  Pixmap              gl_image_pixmap;
  /* OpenGL surface */
  void                *gl_surface;
#endif

  ff_vaapi_context_t  *va_context;
  /* soft surfaces */
  int                 sw_width;
  int                 sw_height;
  VASurfaceID         *va_soft_surface_ids;
  VAImage             *va_soft_images;
  unsigned int        va_soft_head;
  int                 soft_image_is_bound;

  /* subpicture */
  VAImageFormat       *va_subpic_formats;
  int                 va_num_subpic_formats;
  VAImage             va_subpic_image;
  VASubpictureID      va_subpic_id;
  int                 va_subpic_width;
  int                 va_subpic_height;
  unsigned int        last_sub_image_fmt;

  pthread_mutex_t     vaapi_lock;

  unsigned int        guarded_render;
  unsigned int        scaling_level_enum;
  unsigned int        scaling_level;
  va_property_t       props[VO_NUM_PROPERTIES];
  unsigned int        swap_uv_planes;

  /* color matrix and fullrange emulation */
  uint8_t             cm_lut[32];
  int                 cm_state;
  int                 color_matrix;
  int                 vaapi_cm_flags;
#define CSC_MODE_USER_MATRIX      0
#define CSC_MODE_FLAGS            1
#define CSC_MODE_FLAGS_FULLRANGE2 2
#define CSC_MODE_FLAGS_FULLRANGE3 3
  int                 csc_mode;
  int                 have_user_csc_matrix;
  float               user_csc_matrix[12];

  /* keep last frame surface alive (video_out shamelessy uses it ...) */
  /* XXX maybe this issue could be solved with some kind of release callback.
   * Such callback could be useful with dropped frames too. */
  vo_frame_t        *recent_frames[VO_NUM_RECENT_FRAMES];

  /* */
  VASurfaceID         va_soft_surface_ids_storage[SOFT_SURFACES + 1];
  VAImage             va_soft_images_storage[SOFT_SURFACES + 1];
  vaapi_context_impl_t *va;
};

/* import common color matrix stuff */
#define CM_LUT
#define CM_HAVE_YCGCO_SUPPORT 1
#define CM_HAVE_BT2020_SUPPORT 1
#define CM_DRIVER_T vaapi_driver_t
#include "color_matrix.c"

static void vaapi_destroy_subpicture(vaapi_driver_t *this);
static int vaapi_ovl_associate(vaapi_driver_t *this, int format, int bShow);
static VAStatus vaapi_destroy_soft_surfaces(vaapi_driver_t *this);
static int vaapi_set_property (vo_driver_t *this_gen, int property, int value);

#ifdef ENABLE_VA_GLX
void (GLAPIENTRY *mpglBindTexture)(GLenum, GLuint);
void (GLAPIENTRY *mpglXBindTexImage)(Display *, GLXDrawable, int, const int *);
void (GLAPIENTRY *mpglXReleaseTexImage)(Display *, GLXDrawable, int);
GLXPixmap (GLAPIENTRY *mpglXCreatePixmap)(Display *, GLXFBConfig, Pixmap, const int *);
void (GLAPIENTRY *mpglXDestroyPixmap)(Display *, GLXPixmap);
const GLubyte *(GLAPIENTRY *mpglGetString)(GLenum);
void (GLAPIENTRY *mpglGenPrograms)(GLsizei, GLuint *);
#endif

#if defined(LOG) || defined(DEBUG)
static const char *string_of_VAImageFormat(VAImageFormat *imgfmt)
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

static int vaapi_check_status(vaapi_driver_t *this, VAStatus vaStatus, const char *msg)
{
  if (vaStatus != VA_STATUS_SUCCESS) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " Error : %s: %s\n", msg, vaErrorStr(vaStatus));
    return 0;
  }
  return 1;
}

static int vaapi_lock_decode_guarded(vo_frame_t *frame_gen)
{
  vaapi_driver_t  *this = (vaapi_driver_t *) frame_gen->driver;

  pthread_mutex_lock(&this->vaapi_lock);
  return 1;
}

static void vaapi_unlock_decode_guarded(vo_frame_t *frame_gen)
{
  vaapi_driver_t  *this = (vaapi_driver_t *) frame_gen->driver;

  /* unconditional unlock - this is called only if lock was acquired */
  pthread_mutex_unlock(&this->vaapi_lock);
}

typedef struct {
  video_driver_class_t driver_class;

  xine_t              *xine;
  unsigned             visual_type;
} vaapi_class_t;

static void vaapi_x11_wait_event(Display *dpy, Window w, int type)
{
  XEvent e;
  while (!XCheckTypedWindowEvent(dpy, w, type, &e))
    xine_usec_sleep(10);
}

/* X11 Error handler and error functions */
static int vaapi_x11_error_code = 0;
static int (*vaapi_x11_old_error_handler)(Display *, XErrorEvent *);

static int vaapi_x11_error_handler(Display *dpy, XErrorEvent *error)
{
    (void)dpy;
    vaapi_x11_error_code = error->error_code;
    return 0;
}

static void vaapi_x11_trap_errors(void)
{
    vaapi_x11_error_code    = 0;
    vaapi_x11_old_error_handler = XSetErrorHandler(vaapi_x11_error_handler);
}

static int vaapi_x11_untrap_errors(void)
{
    XSetErrorHandler(vaapi_x11_old_error_handler);
    return vaapi_x11_error_code;
}

#ifdef ENABLE_VA_GLX

static void vaapi_appendstr(char **dst, const char *str)
{
    int newsize;
    char *newstr;
    if (!str)
        return;
    newsize = strlen(*dst) + 1 + strlen(str) + 1;
    newstr = realloc(*dst, newsize);
    if (!newstr)
        return;
    *dst = newstr;
    strcat(*dst, " ");
    strcat(*dst, str);
}

/* Return the address of a linked function */
static void *vaapi_getdladdr (const char *s) {
  void *ret = NULL;
  void *handle = dlopen(NULL, RTLD_LAZY);
  if (!handle)
    return NULL;
  ret = dlsym(handle, s);
  dlclose(handle);

  return ret;
}

/* Resolve opengl functions. */
static void vaapi_get_functions(void *(*getProcAddress)(const GLubyte *),
                                const char *ext2)
{
  static const struct {
    void       *funcptr;
    const char *extstr;
    const char *funcnames[4];
  } extfuncs[] = {
    { &mpglBindTexture,
      NULL,
      { "glBindTexture", "glBindTextureARB", "glBindTextureEXT", NULL } },
    { &mpglXBindTexImage,
      "GLX_EXT_texture_from_pixmap",
      {" glXBindTexImageEXT", NULL }, },
    { &mpglXReleaseTexImage,
      "GLX_EXT_texture_from_pixmap",
      { "glXReleaseTexImageEXT", NULL} },
    { &mpglXCreatePixmap,
      "GLX_EXT_texture_from_pixmap",
      { "glXCreatePixmap", NULL } },
    { &mpglXDestroyPixmap,
      "GLX_EXT_texture_from_pixmap",
      { "glXDestroyPixmap", NULL } },
    { &mpglGenPrograms, "_program",
      { "glGenProgramsARB", NULL } },
};

  const char *extensions;
  char *allexts;
  size_t ext;

  if (!getProcAddress)
    getProcAddress = (void *)vaapi_getdladdr;

  /* special case, we need glGetString before starting to find the other functions */
  mpglGetString = getProcAddress("glGetString");
  if (!mpglGetString)
      mpglGetString = glGetString;

  extensions = (const char *)mpglGetString(GL_EXTENSIONS);
  if (!extensions) extensions = "";
  if (!ext2) ext2 = "";
  allexts = malloc(strlen(extensions) + strlen(ext2) + 2);
  strcpy(allexts, extensions);
  strcat(allexts, " ");
  strcat(allexts, ext2);
  lprintf("vaapi_get_functions: OpenGL extensions string:\n%s\n", allexts);
  for (ext = 0; ext < sizeof(extfuncs) / sizeof(extfuncs[0]); ext++) {
    void *ptr = NULL;
    int i;
    if (!extfuncs[ext].extstr || strstr(allexts, extfuncs[ext].extstr)) {
      for (i = 0; !ptr && extfuncs[ext].funcnames[i]; i++)
        ptr = getProcAddress((const GLubyte *)extfuncs[ext].funcnames[i]);
    }
    *(void **)extfuncs[ext].funcptr = ptr;
  }
  lprintf("\n");
  free(allexts);
}

#define VAAPI_GLX_VISUAL_ATTR \
  {                           \
    GLX_RGBA,                 \
    GLX_RED_SIZE, 1,          \
    GLX_GREEN_SIZE, 1,        \
    GLX_BLUE_SIZE, 1,         \
    GLX_DOUBLEBUFFER,         \
    GL_NONE                   \
  }                           \

/* Check if opengl indirect/software rendering is used */
static int vaapi_opengl_verify_direct (const x11_visual_t *vis) {
  Window        root, win;
  XVisualInfo  *visinfo;
  GLXContext    ctx;
  XSetWindowAttributes xattr;
  int           ret = 0;
  int           gl_visual_attr[] = VAAPI_GLX_VISUAL_ATTR;

  if (!vis || !vis->display || ! (root = RootWindow (vis->display, vis->screen))) {
    lprintf ("vaapi_opengl_verify_direct: Don't have a root window to verify\n");
    return 0;
  }

  if (! (visinfo = glXChooseVisual (vis->display, vis->screen, gl_visual_attr)))
    return 0;

  if (! (ctx = glXCreateContext (vis->display, visinfo, NULL, 1))) {
    XFree(visinfo);
    return 0;
  }

  memset (&xattr, 0, sizeof (xattr));
  xattr.colormap = XCreateColormap(vis->display, root, visinfo->visual, AllocNone);
  xattr.event_mask = StructureNotifyMask | ExposureMask;

  if ( (win = XCreateWindow (vis->display, root, 0, 0, 1, 1, 0, visinfo->depth,
                             InputOutput, visinfo->visual,
                             CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
                             &xattr))) {
    if (glXMakeCurrent (vis->display, win, ctx)) {
      const char *renderer = (const char *) glGetString(GL_RENDERER);
      if (glXIsDirect (vis->display, ctx) &&
          ! strstr (renderer, "Software") &&
          ! strstr (renderer, "Indirect"))
        ret = 1;
      glXMakeCurrent (vis->display, None, NULL);
    }
    XDestroyWindow (vis->display, win);
  }
  glXDestroyContext (vis->display, ctx);
  XFreeColormap     (vis->display, xattr.colormap);
  XFree(visinfo);

  return ret;
}

static int vaapi_glx_bind_texture(vaapi_driver_t *this)
{
  glEnable(GL_TEXTURE_2D);
  mpglBindTexture(GL_TEXTURE_2D, this->gl_texture);

  if (this->opengl_use_tfp) {
    vaapi_x11_trap_errors();
    mpglXBindTexImage(this->display, this->gl_pixmap, GLX_FRONT_LEFT_EXT, NULL);
    XSync(this->display, False);
    if (vaapi_x11_untrap_errors())
      xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_bind_texture : Update bind_tex_image failed\n");
  }

  return 0;
}

static int vaapi_glx_unbind_texture(vaapi_driver_t *this)
{
  if (this->opengl_use_tfp) {
    vaapi_x11_trap_errors();
    mpglXReleaseTexImage(this->display, this->gl_pixmap, GLX_FRONT_LEFT_EXT);
    if (vaapi_x11_untrap_errors())
      xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_unbind_texture : Failed to release?\n");
  }

  mpglBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  return 0;
}

static void vaapi_glx_render_frame(vaapi_driver_t *this, mem_frame_t *frame, int left, int top, int right, int bottom)
{
  ff_vaapi_context_t    *va_context = this->va_context;
  int             x1, x2, y1, y2;
  float           tx, ty;

  (void)left;
  (void)top;
  (void)right;
  (void)bottom;
  if (vaapi_glx_bind_texture(this) < 0)
    return;

  /* Calc texture/rectangle coords */
  x1 = this->sc.output_xoffset;
  y1 = this->sc.output_yoffset;
  x2 = x1 + this->sc.output_width;
  y2 = y1 + this->sc.output_height;
  tx = (float) frame->width  / va_context->width;
  ty = (float) frame->height / va_context->height;

  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  /* Draw quad */
  glBegin (GL_QUADS);

    glTexCoord2f (tx, ty);   glVertex2i (x2, y2);
    glTexCoord2f (0,  ty);   glVertex2i (x1, y2);
    glTexCoord2f (0,  0);    glVertex2i (x1, y1);
    glTexCoord2f (tx, 0);    glVertex2i (x2, y1);
    lprintf("render_frame left %d top %d right %d bottom %d\n", x1, y1, x2, y2);

  glEnd ();

  if (vaapi_glx_unbind_texture(this) < 0)
    return;
}

static void vaapi_glx_flip_page(vaapi_driver_t *this, mem_frame_t *frame, int left, int top, int right, int bottom)
{
  glClear(GL_COLOR_BUFFER_BIT);

  vaapi_glx_render_frame(this, frame, left, top, right, bottom);

  //if (gl_finish)
  //  glFinish();

  glXSwapBuffers(this->display, this->window);

}

static void destroy_glx(vaapi_driver_t *this)
{
  ff_vaapi_context_t    *va_context = this->va_context;

  if(!this->opengl_render || !va_context->valid_context)
    return;

  //if (gl_finish)
  //  glFinish();

  if (this->gl_surface) {
    VAStatus vaStatus = vaDestroySurfaceGLX(va_context->va_display, this->gl_surface);
    vaapi_check_status(this, vaStatus, "vaDestroySurfaceGLX()");
    this->gl_surface = NULL;
  }

  if(this->gl_context)
    glXMakeCurrent(this->display, None, NULL);

  if(this->gl_pixmap) {
    vaapi_x11_trap_errors();
    mpglXDestroyPixmap(this->display, this->gl_pixmap);
    XSync(this->display, False);
    if (vaapi_x11_untrap_errors())
      xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_destroy_glx : mpglXDestroyPixmap failed\n");
    this->gl_pixmap = None;
  }

  if(this->gl_image_pixmap) {
    XFreePixmap(this->display, this->gl_image_pixmap);
    this->gl_image_pixmap = None;
  }

  if(this->gl_texture) {
    glDeleteTextures(1, &this->gl_texture);
    this->gl_texture = GL_NONE;
  }

  if(this->gl_context) {
    glXDestroyContext(this->display, this->gl_context);
    this->gl_context = NULL;
  }
}

static GLXFBConfig *get_fbconfig_for_depth(vaapi_driver_t *this, int depth)
{
    GLXFBConfig *fbconfigs, *ret = NULL;
    int          n_elements, i, found;
    int          db, stencil, alpha, rgba, value;

    static GLXFBConfig *cached_config = NULL;
    static int          have_cached_config = 0;

    if (have_cached_config)
        return cached_config;

    fbconfigs = glXGetFBConfigs(this->display, this->screen, &n_elements);

    db      = SHRT_MAX;
    stencil = SHRT_MAX;
    rgba    = 0;

    found = n_elements;

    for (i = 0; i < n_elements; i++) {
        XVisualInfo *vi;
        int          visual_depth;

        vi = glXGetVisualFromFBConfig(this->display, fbconfigs[i]);
        if (!vi)
            continue;

        visual_depth = vi->depth;
        XFree(vi);

        if (visual_depth != depth)
            continue;

        glXGetFBConfigAttrib(this->display, fbconfigs[i], GLX_ALPHA_SIZE, &alpha);
        glXGetFBConfigAttrib(this->display, fbconfigs[i], GLX_BUFFER_SIZE, &value);
        if (value != depth && (value - alpha) != depth)
            continue;

        value = 0;
        if (depth == 32) {
            glXGetFBConfigAttrib(this->display, fbconfigs[i],
                                 GLX_BIND_TO_TEXTURE_RGBA_EXT, &value);
            if (value)
                rgba = 1;
        }

        if (!value) {
            if (rgba)
                continue;

            glXGetFBConfigAttrib(this->display, fbconfigs[i],
                                 GLX_BIND_TO_TEXTURE_RGB_EXT, &value);
            if (!value)
                continue;
        }

        glXGetFBConfigAttrib(this->display, fbconfigs[i], GLX_DOUBLEBUFFER, &value);
        if (value > db)
            continue;
        db = value;

        glXGetFBConfigAttrib(this->display, fbconfigs[i], GLX_STENCIL_SIZE, &value);
        if (value > stencil)
            continue;
        stencil = value;

        found = i;
    }

    if (found != n_elements) {
        ret = malloc(sizeof(*ret));
        *ret = fbconfigs[found];
    }

    if (n_elements)
        XFree(fbconfigs);

    have_cached_config = 1;
    cached_config = ret;
    return ret;
}

static int vaapi_glx_config_tfp(vaapi_driver_t *this, unsigned int width, unsigned int height)
{
  GLXFBConfig *fbconfig;
  int attribs[7], i = 0;
  const int depth = 24;

  if (!mpglXBindTexImage || !mpglXReleaseTexImage) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_tfp : No GLX texture-from-pixmap extension available\n");
    return 0;
  }

  if (depth != 24 && depth != 32) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_tfp : colour depth wrong.\n");
    return 0;
  }

  this->gl_image_pixmap = XCreatePixmap(this->display, this->window, width, height, depth);
  if (!this->gl_image_pixmap) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_tfp : Could not create X11 pixmap\n");
    return 0;
  }

  fbconfig = get_fbconfig_for_depth(this, depth);
  if (!fbconfig) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_tfp : Could not find an FBConfig for 32-bit pixmap\n");
    return 0;
  }

  attribs[i++] = GLX_TEXTURE_TARGET_EXT;
  attribs[i++] = GLX_TEXTURE_2D_EXT;
  attribs[i++] = GLX_TEXTURE_FORMAT_EXT;
  if (depth == 24)
    attribs[i++] = GLX_TEXTURE_FORMAT_RGB_EXT;
  else if (depth == 32)
    attribs[i++] = GLX_TEXTURE_FORMAT_RGBA_EXT;
  attribs[i++] = GLX_MIPMAP_TEXTURE_EXT;
  attribs[i++] = GL_FALSE;
  attribs[i++] = None;

  vaapi_x11_trap_errors();
  this->gl_pixmap = mpglXCreatePixmap(this->display, *fbconfig, this->gl_image_pixmap, attribs);
  XSync(this->display, False);
  if (vaapi_x11_untrap_errors()) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_tfp : Could not create GLX pixmap\n");
    return 0;
  }

  return 1;
}

static int vaapi_glx_config_glx(vaapi_driver_t *this, unsigned int width, unsigned int height)
{
  ff_vaapi_context_t    *va_context = this->va_context;
  int                    gl_visual_attr[] = VAAPI_GLX_VISUAL_ATTR;
  XVisualInfo           *gl_vinfo;

  gl_vinfo = glXChooseVisual(this->display, this->screen, gl_visual_attr);
  if(!gl_vinfo) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_glx : error glXChooseVisual\n");
    this->opengl_render = 0;
  }

  glXMakeCurrent(this->display, None, NULL);
  this->gl_context = glXCreateContext (this->display, gl_vinfo, NULL, True);
  XFree(gl_vinfo);
  if (this->gl_context) {
    if(!glXMakeCurrent (this->display, this->window, this->gl_context)) {
      xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_glx : error glXMakeCurrent\n");
      goto error;
    }
  } else {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_glx : error glXCreateContext\n");
    goto error;
  }

  void *(*getProcAddress)(const GLubyte *);
  const char *(*glXExtStr)(Display *, int);
  char *glxstr = strdup(" ");

  getProcAddress = vaapi_getdladdr("glXGetProcAddress");
  if (!getProcAddress)
    getProcAddress = vaapi_getdladdr("glXGetProcAddressARB");
  glXExtStr = vaapi_getdladdr("glXQueryExtensionsString");
  if (glXExtStr)
      vaapi_appendstr(&glxstr, glXExtStr(this->display, this->screen));
  glXExtStr = vaapi_getdladdr("glXGetClientString");
  if (glXExtStr)
      vaapi_appendstr(&glxstr, glXExtStr(this->display, GLX_EXTENSIONS));
  glXExtStr = vaapi_getdladdr("glXGetServerString");
  if (glXExtStr)
      vaapi_appendstr(&glxstr, glXExtStr(this->display, GLX_EXTENSIONS));

  vaapi_get_functions(getProcAddress, glxstr);
  if (!mpglGenPrograms && mpglGetString &&
      getProcAddress &&
      strstr(mpglGetString(GL_EXTENSIONS), "GL_ARB_vertex_program")) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_glx : Broken glXGetProcAddress detected, trying workaround\n");
    vaapi_get_functions(NULL, glxstr);
  }
  free(glxstr);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glEnable(GL_TEXTURE_2D);
  glDrawBuffer(GL_BACK);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  /* Create TFP resources */
  if(this->opengl_use_tfp && vaapi_glx_config_tfp(this, width, height)) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_glx : Using GLX texture-from-pixmap extension\n");
  } else {
    this->opengl_use_tfp = 0;
  }

  /* Create OpenGL texture */
  /* XXX: assume GL_ARB_texture_non_power_of_two is available */
  glEnable(GL_TEXTURE_2D);
  glGenTextures(1, &this->gl_texture);
  mpglBindTexture(GL_TEXTURE_2D, this->gl_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  if (!this->opengl_use_tfp) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);
  }
  mpglBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);

  glClearColor(0.0, 0.0, 0.0, 1.0);
  glClear(GL_COLOR_BUFFER_BIT);

  if(!this->gl_texture) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_glx_config_glx : gl_texture NULL\n");
    goto error;
  }

  if(!this->opengl_use_tfp) {
    VAStatus vaStatus = vaCreateSurfaceGLX(va_context->va_display, GL_TEXTURE_2D, this->gl_texture, &this->gl_surface);
    if(!vaapi_check_status(this, vaStatus, "vaCreateSurfaceGLX()")) {
      this->gl_surface = NULL;
      goto error;
    }
  } else {
    this->gl_surface = NULL;
  }

  lprintf("vaapi_glx_config_glx : GL setup done\n");

  return 1;

error:
  destroy_glx(this);
  return 0;
}

#endif /* ENABLE_VA_GLX */

static uint32_t vaapi_get_capabilities (vo_driver_t *this_gen) {
  vaapi_driver_t *this = (vaapi_driver_t *) this_gen;

  return this->capabilities;
}

/* Init subpicture */
static void vaapi_init_subpicture(vaapi_driver_t *this) {
  this->va_subpic_width               = 0;
  this->va_subpic_height              = 0;
  this->va_subpic_id                  = VA_INVALID_ID;
  this->va_subpic_image.image_id      = VA_INVALID_ID;

  this->overlay_output_width = this->overlay_output_height = 0;
  this->ovl_changed = 0;
  this->has_overlay = 0;
  this->overlay_bitmap = NULL;
  this->overlay_bitmap_size = 0;

  this->va_subpic_formats     = NULL;
  this->va_num_subpic_formats = 0;
}

/* Close vaapi  */
static void vaapi_close(vaapi_driver_t *this) {
  ff_vaapi_context_t    *va_context = this->va_context;

  if(!va_context || !va_context->va_display || !va_context->valid_context)
    return;

  vaapi_ovl_associate(this, 0, 0);

#ifdef ENABLE_VA_GLX
  destroy_glx(this);
#endif

  vaapi_destroy_subpicture(this);
  vaapi_destroy_soft_surfaces(this);

  _x_va_close(this->va);
}

/* Deassociate and free subpicture */
static void vaapi_destroy_subpicture(vaapi_driver_t *this) {
  ff_vaapi_context_t    *va_context = this->va_context;
  VAStatus              vaStatus;

  lprintf("destroy sub 0x%08x 0x%08x 0x%08x\n", this->va_subpic_id,
      this->va_subpic_image.image_id, this->va_subpic_image.buf);

  if (this->va_subpic_id != VA_INVALID_ID) {
    vaStatus = vaDestroySubpicture(va_context->va_display, this->va_subpic_id);
    vaapi_check_status(this, vaStatus, "vaDestroySubpicture()");
  }
  this->va_subpic_id = VA_INVALID_ID;

  _x_va_destroy_image(this->va, &this->va_subpic_image);

}

/* Create VAAPI subpicture */
static VAStatus vaapi_create_subpicture(vaapi_driver_t *this, int width, int height) {
  ff_vaapi_context_t  *va_context = this->va_context;
  VAStatus            vaStatus;

  int i = 0;

  if(!va_context->valid_context || !this->va_subpic_formats || this->va_num_subpic_formats == 0)
    return VA_STATUS_ERROR_UNKNOWN;

  for (i = 0; i < this->va_num_subpic_formats; i++) {
    if ( this->va_subpic_formats[i].fourcc == VA_FOURCC('B','G','R','A')) {

      vaStatus = vaCreateImage( va_context->va_display, &this->va_subpic_formats[i], width, height, &this->va_subpic_image );
      if(!vaapi_check_status(this, vaStatus, "vaCreateImage()"))
        goto error;

      vaStatus = vaCreateSubpicture(va_context->va_display, this->va_subpic_image.image_id, &this->va_subpic_id );
      if(!vaapi_check_status(this, vaStatus, "vaCreateSubpicture()"))
        goto error;
    }
  }

  if (this->va_subpic_image.image_id == VA_INVALID_ID || this->va_subpic_id == VA_INVALID_ID)
    goto error;

  void *p_base = NULL;

  lprintf("create sub 0x%08x 0x%08x 0x%08x\n", this->va_subpic_id,
      this->va_subpic_image.image_id, this->va_subpic_image.buf);

  vaStatus = vaMapBuffer(va_context->va_display, this->va_subpic_image.buf, &p_base);
  if(!vaapi_check_status(this, vaStatus, "vaMapBuffer()"))
    goto error;

  memset((uint32_t *)p_base, 0x0, this->va_subpic_image.data_size);
  vaStatus = vaUnmapBuffer(va_context->va_display, this->va_subpic_image.buf);
  vaapi_check_status(this, vaStatus, "vaUnmapBuffer()");

  this->overlay_output_width  = width;
  this->overlay_output_height = height;

  lprintf("vaapi_create_subpicture 0x%08x format %s\n", this->va_subpic_image.image_id,
      string_of_VAImageFormat(&this->va_subpic_image.format));

  return VA_STATUS_SUCCESS;

error:
  /* house keeping */
  if(this->va_subpic_id != VA_INVALID_ID)
    vaapi_destroy_subpicture(this);
  this->va_subpic_id = VA_INVALID_ID;

  _x_va_destroy_image(this->va, &this->va_subpic_image);

  this->overlay_output_width  = 0;
  this->overlay_output_height = 0;

  return VA_STATUS_ERROR_UNKNOWN;
}

static void vaapi_set_csc_mode(vaapi_driver_t *this, int new_mode)
{
  if (new_mode == CSC_MODE_USER_MATRIX) {
    this->capabilities |= VO_CAP_BRIGHTNESS | VO_CAP_CONTRAST | VO_CAP_SATURATION | VO_CAP_HUE
      | VO_CAP_COLOR_MATRIX | VO_CAP_FULLRANGE;
  } else {
    this->capabilities &=
      ~(VO_CAP_BRIGHTNESS | VO_CAP_CONTRAST | VO_CAP_SATURATION | VO_CAP_HUE | VO_CAP_COLOR_MATRIX | VO_CAP_FULLRANGE);
    if (this->props[VO_PROP_BRIGHTNESS].atom)
      this->capabilities |= VO_CAP_BRIGHTNESS;
    if (this->props[VO_PROP_CONTRAST].atom)
      this->capabilities |= VO_CAP_CONTRAST;
    if (this->props[VO_PROP_SATURATION].atom)
      this->capabilities |= VO_CAP_SATURATION;
    if (this->props[VO_PROP_HUE].atom)
      this->capabilities |= VO_CAP_HUE;
#if (defined VA_SRC_BT601) && ((defined VA_SRC_BT709) || (defined VA_SRC_SMPTE_240))
    this->capabilities |= VO_CAP_COLOR_MATRIX;
#endif
    if (new_mode != CSC_MODE_FLAGS) {
      if ((this->capabilities & (VO_CAP_BRIGHTNESS | VO_CAP_CONTRAST)) == (VO_CAP_BRIGHTNESS | VO_CAP_CONTRAST))
        this->capabilities |= VO_CAP_FULLRANGE;
    }
  }

  this->csc_mode = new_mode;
  this->color_matrix = 0;
}

static const char * const vaapi_csc_mode_labels[] = {
  "user_matrix", "simple", "simple+2", "simple+3", NULL
};

/* normalize to 0.0 ~ 2.0 */
static float vaapi_normalized_prop (vaapi_driver_t *this, int prop) {
  int range = (this->props[prop].max - this->props[prop].min) >> 1;

  if (range)
    return (float)(this->props[prop].value - this->props[prop].min) / (float)range;
  return 1.0;
}

static void vaapi_update_csc (vaapi_driver_t *that, mem_frame_t *frame) {
  int color_matrix;
  int i;

  color_matrix = cm_from_frame (&frame->vo_frame);

  if (that->color_matrix != color_matrix) {

    /* revert unsupported modes */
    i = that->csc_mode;
    if (i == CSC_MODE_USER_MATRIX && !that->have_user_csc_matrix)
      i = CSC_MODE_FLAGS_FULLRANGE3;
    if (i == CSC_MODE_FLAGS_FULLRANGE3 && !that->props[VO_PROP_SATURATION].atom)
      i = CSC_MODE_FLAGS_FULLRANGE2;
    if (i == CSC_MODE_FLAGS_FULLRANGE2 && !that->props[VO_PROP_BRIGHTNESS].atom)
      i = CSC_MODE_FLAGS;
    if (i != that->csc_mode) {
      xprintf (that->xine, XINE_VERBOSITY_LOG,
        _("video_out_vaapi: driver does not support \"%s\" colourspace conversion mode\n"),
        vaapi_csc_mode_labels[that->csc_mode]);
      vaapi_set_csc_mode (that, i);
    }

    that->color_matrix = color_matrix;

    if (that->csc_mode == CSC_MODE_USER_MATRIX) {
      /* WOW - full support */
      float hue = (vaapi_normalized_prop (that, VO_PROP_HUE) - 1.0) * M_PI;
      float saturation = vaapi_normalized_prop (that, VO_PROP_SATURATION);
      float contrast = vaapi_normalized_prop (that, VO_PROP_CONTRAST);
      float brightness = (vaapi_normalized_prop (that, VO_PROP_BRIGHTNESS) - 1.0) * 128.0;
      float *matrix = that->user_csc_matrix;
      VADisplayAttribute attr;

      cm_fill_matrix(matrix, color_matrix, hue, saturation, contrast, brightness);

      attr.type   = VADisplayAttribCSCMatrix;
      /* libva design bug: VADisplayAttribute.value is plain int.
        On 64bit system, a pointer value put here will overwrite the following "flags" field too. */
      memcpy (&(attr.value), &matrix, sizeof (float *));
      vaSetDisplayAttributes (that->va_context->va_display, &attr, 1);

      xprintf (that->xine, XINE_VERBOSITY_LOG,"video_out_vaapi: b %d c %d s %d h %d [%s]\n",
        that->props[VO_PROP_BRIGHTNESS].value,
        that->props[VO_PROP_CONTRAST].value,
        that->props[VO_PROP_SATURATION].value,
        that->props[VO_PROP_HUE].value,
        cm_names[color_matrix]);

    } else {
      /* fall back to old style */
      int brightness = that->props[VO_PROP_BRIGHTNESS].value;
      int contrast   = that->props[VO_PROP_CONTRAST].value;
      int saturation = that->props[VO_PROP_SATURATION].value;
      int hue        = that->props[VO_PROP_HUE].value;
      int i;
      VADisplayAttribute attr[4];

      /* The fallback rhapsody */
#if defined(VA_SRC_BT601) && (defined(VA_SRC_BT709) || defined(VA_SRC_SMPTE_240))
      i = color_matrix >> 1;
      switch (i) {
        case 1:
#if defined(VA_SRC_BT709)
          that->vaapi_cm_flags = VA_SRC_BT709;
#elif defined(VA_SRC_SMPTE_240)
          that->vaapi_cm_flags = VA_SRC_SMPTE_240;
          i = 7;
#endif
        break;
        case 7:
#if defined(VA_SRC_SMPTE_240)
          that->vaapi_cm_flags = VA_SRC_SMPTE_240;
#elif defined(VA_SRC_BT709)
          that->vaapi_cm_flags = VA_SRC_BT709;
          i = 1;
#endif
        break;
        default:
          that->vaapi_cm_flags = VA_SRC_BT601;
          i = 5;
      }
#else
      that->vaapi_cm_flags = 0;
      i = 2; /* undefined */
#endif
      color_matrix &= 1;
      color_matrix |= i << 1;

      if ((that->csc_mode != CSC_MODE_FLAGS) && (color_matrix & 1)) {
        int a, b;
        /* fullrange mode. XXX assuming TV set style bcs controls 0% - 200% */
        if (that->csc_mode == CSC_MODE_FLAGS_FULLRANGE3) {
          saturation -= that->props[VO_PROP_SATURATION].min;
          saturation  = (saturation * (112 * 255) + (127 * 219 / 2)) / (127 * 219);
          saturation += that->props[VO_PROP_SATURATION].min;
          if (saturation > that->props[VO_PROP_SATURATION].max)
            saturation = that->props[VO_PROP_SATURATION].max;
        }

        contrast -= that->props[VO_PROP_CONTRAST].min;
        contrast  = (contrast * 219 + 127) / 255;
        a         = contrast * (that->props[VO_PROP_BRIGHTNESS].max - that->props[VO_PROP_BRIGHTNESS].min);
        contrast += that->props[VO_PROP_CONTRAST].min;
        b         = 256 * (that->props[VO_PROP_CONTRAST].max - that->props[VO_PROP_CONTRAST].min);

        brightness += (16 * a + b / 2) / b;
        if (brightness > that->props[VO_PROP_BRIGHTNESS].max)
          brightness = that->props[VO_PROP_BRIGHTNESS].max;
      }

      i = 0;
      if (that->props[VO_PROP_BRIGHTNESS].atom) {
        attr[i].type  = that->props[VO_PROP_BRIGHTNESS].type;
        attr[i].value = brightness;
        i++;
      }
      if (that->props[VO_PROP_CONTRAST].atom) {
        attr[i].type  = that->props[VO_PROP_CONTRAST].type;
        attr[i].value = contrast;
        i++;
      }
      if (that->props[VO_PROP_SATURATION].atom) {
        attr[i].type  = that->props[VO_PROP_SATURATION].type;
        attr[i].value = saturation;
        i++;
      }
      if (that->props[VO_PROP_HUE].atom) {
        attr[i].type  = that->props[VO_PROP_HUE].type;
        attr[i].value = hue;
        i++;
      }
      if (i)
        vaSetDisplayAttributes (that->va_context->va_display, attr, i);

      xprintf (that->xine, XINE_VERBOSITY_LOG,"video_out_vaapi: %s b %d c %d s %d h %d [%s]\n",
        color_matrix & 1 ? "modified" : "",
        brightness, contrast, saturation, hue, cm_names[color_matrix]);
    }
  }
}

static void vaapi_property_callback (void *property_gen, xine_cfg_entry_t *entry) {
  va_property_t       *property   = (va_property_t *) property_gen;
  vaapi_driver_t      *this       = property->this;
  ff_vaapi_context_t  *va_context = this->va_context;

  pthread_mutex_lock(&this->vaapi_lock);
  LOCK_DISPLAY (this);

  VADisplayAttribute attr;

  attr.type   = property->type;
  attr.value  = entry->num_value;

  lprintf("vaapi_property_callback property=%d, value=%d\n", property->type, entry->num_value );

  /*VAStatus vaStatus = */ vaSetDisplayAttributes(va_context->va_display, &attr, 1);
  //vaapi_check_status(this, vaStatus, "vaSetDisplayAttributes()");

  UNLOCK_DISPLAY (this);
  pthread_mutex_unlock(&this->vaapi_lock);
}

/* called xlocked */
static void vaapi_check_capability (vaapi_driver_t *this,
         int property, VADisplayAttribute attr, 
         const char *config_name,
         const char *config_desc,
         const char *config_help) {
  config_values_t *config = this->xine->config;
  int int_default = 0;
  cfg_entry_t *entry;

  this->props[property].type   = attr.type;
  this->props[property].min    = attr.min_value;
  this->props[property].max    = attr.max_value;
  int_default                  = attr.value;
  this->props[property].atom   = 1;

  if (config_name) {
    /* is this a boolean property ? */
    if ((attr.min_value == 0) && (attr.max_value == 1)) {
      config->register_bool (config, config_name, int_default,
           config_desc,
           config_help, 20, vaapi_property_callback, &this->props[property]);

    } else {
      config->register_range (config, config_name, int_default,
            this->props[property].min, this->props[property].max,
            config_desc,
            config_help, 20, vaapi_property_callback, &this->props[property]);
    }

    entry = config->lookup_entry (config, config_name);
    if((entry->num_value < this->props[property].min) ||
       (entry->num_value > this->props[property].max)) {

      config->update_num(config, config_name,
             ((this->props[property].min + this->props[property].max) >> 1));

      entry = config->lookup_entry (config, config_name);
    }

    this->props[property].entry = entry;

    vaapi_set_property(&this->vo_driver, property, entry->num_value);
  } else {
    this->props[property].value  = int_default;
  }
}

/* VAAPI display attributes. */
static void vaapi_display_attribs(vaapi_driver_t *this) {
  ff_vaapi_context_t  *va_context = this->va_context;

  int num_display_attrs, max_display_attrs;
  VAStatus vaStatus;
  VADisplayAttribute *display_attrs;
  int i;

  max_display_attrs = vaMaxNumDisplayAttributes(va_context->va_display);
  display_attrs = calloc(max_display_attrs, sizeof(*display_attrs));

  if (display_attrs) {
    num_display_attrs = 0;
    vaStatus = vaQueryDisplayAttributes(va_context->va_display,
                                        display_attrs, &num_display_attrs);
    if(vaapi_check_status(this, vaStatus, "vaQueryDisplayAttributes()")) {
      for (i = 0; i < num_display_attrs; i++) {
        xprintf (this->xine, XINE_VERBOSITY_DEBUG,
          "video_out_vaapi: display attribute #%d = %d [%d .. %d], flags %d\n",
          (int)display_attrs[i].type,
          display_attrs[i].value, display_attrs[i].min_value, display_attrs[i].max_value,
          display_attrs[i].flags);
        switch (display_attrs[i].type) {
          case VADisplayAttribBrightness:
            if( ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_GETTABLE ) &&
                ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_SETTABLE ) ) {
              this->capabilities |= VO_CAP_BRIGHTNESS;
              vaapi_check_capability(this, VO_PROP_BRIGHTNESS, display_attrs[i], "video.output.vaapi_brightness", "Brightness setting", "Brightness setting");
            }
            break;
          case VADisplayAttribContrast:
            if( ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_GETTABLE ) &&
                ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_SETTABLE ) ) {
              this->capabilities |= VO_CAP_CONTRAST;
              vaapi_check_capability(this, VO_PROP_CONTRAST, display_attrs[i], "video.output.vaapi_contrast", "Contrast setting", "Contrast setting");
            }
            break;
          case VADisplayAttribHue:
            if( ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_GETTABLE ) &&
                ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_SETTABLE ) ) {
              this->capabilities |= VO_CAP_HUE;
              vaapi_check_capability(this, VO_PROP_HUE, display_attrs[i], "video.output.vaapi_hue", "Hue setting", "Hue setting");
            }
            break;
          case VADisplayAttribSaturation:
            if( ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_GETTABLE ) &&
                ( display_attrs[i].flags & VA_DISPLAY_ATTRIB_SETTABLE ) ) {
              this->capabilities |= VO_CAP_SATURATION;
              vaapi_check_capability(this, VO_PROP_SATURATION, display_attrs[i], "video.output.vaapi_saturation", "Saturation setting", "Saturation setting");
            }
            break;
          case VADisplayAttribCSCMatrix:
            if (display_attrs[i].flags & VA_DISPLAY_ATTRIB_SETTABLE) {
              this->have_user_csc_matrix = 1;
            }
            break;
          default:
            break;
        }
      }
    }
    free(display_attrs);
  }

  if (this->have_user_csc_matrix) {
    /* make sure video eq is full usable for user matrix mode */
    if (!this->props[VO_PROP_BRIGHTNESS].atom) {
      this->props[VO_PROP_BRIGHTNESS].min   = -1000;
      this->props[VO_PROP_BRIGHTNESS].max   =  1000;
      this->props[VO_PROP_BRIGHTNESS].value =  0;
    }
    if (!this->props[VO_PROP_CONTRAST].atom) {
      this->props[VO_PROP_CONTRAST].min = this->props[VO_PROP_BRIGHTNESS].min;
      this->props[VO_PROP_CONTRAST].max = this->props[VO_PROP_BRIGHTNESS].max;
      this->props[VO_PROP_CONTRAST].value
        = (this->props[VO_PROP_CONTRAST].max - this->props[VO_PROP_CONTRAST].min) >> 1;
    }
    if (!this->props[VO_PROP_SATURATION].atom) {
      this->props[VO_PROP_SATURATION].min = this->props[VO_PROP_CONTRAST].min;
      this->props[VO_PROP_SATURATION].max = this->props[VO_PROP_CONTRAST].max;
      this->props[VO_PROP_SATURATION].value
        = (this->props[VO_PROP_CONTRAST].max - this->props[VO_PROP_CONTRAST].min) >> 1;
    }
    if (!this->props[VO_PROP_HUE].atom) {
      this->props[VO_PROP_HUE].min = this->props[VO_PROP_BRIGHTNESS].min;
      this->props[VO_PROP_HUE].max = this->props[VO_PROP_BRIGHTNESS].max;
      this->props[VO_PROP_HUE].value
        = (this->props[VO_PROP_BRIGHTNESS].max - this->props[VO_PROP_BRIGHTNESS].min) >> 1;
    }
  }
}

static void vaapi_set_background_color(vaapi_driver_t *this) {
  ff_vaapi_context_t  *va_context = this->va_context;
  //VAStatus            vaStatus;

  if(!va_context->valid_context)
    return;

  VADisplayAttribute attr;
  memset( &attr, 0, sizeof(attr) );

  attr.type  = VADisplayAttribBackgroundColor;
  attr.value = 0x000000;

  /*vaStatus =*/ vaSetDisplayAttributes(va_context->va_display, &attr, 1);
  //vaapi_check_status(this, vaStatus, "vaSetDisplayAttributes()");
}

static VAStatus vaapi_destroy_soft_surfaces(vaapi_driver_t *this) {
  ff_vaapi_context_t  *va_context = this->va_context;
  int                 i;
  VAStatus            vaStatus;


  for(i = 0; i < SOFT_SURFACES; i++) {
    if (this->va_soft_images[i].image_id != VA_INVALID_ID)
      _x_va_destroy_image(this->va, &this->va_soft_images[i]);
    this->va_soft_images[i].image_id = VA_INVALID_ID;

    if (this->va_soft_surface_ids[i] != VA_INVALID_SURFACE) {
#ifdef DEBUG_SURFACE
      printf("vaapi_close destroy render surface 0x%08x\n", this->va_soft_surface_ids[i]);
#endif
      vaStatus = vaSyncSurface(va_context->va_display, this->va_soft_surface_ids[i]);
      vaapi_check_status(this, vaStatus, "vaSyncSurface()");
      vaStatus = vaDestroySurfaces(va_context->va_display, &this->va_soft_surface_ids[i], 1);
      vaapi_check_status(this, vaStatus, "vaDestroySurfaces()");
      this->va_soft_surface_ids[i] = VA_INVALID_SURFACE;
    }
  }

  this->sw_width  = 0;
  this->sw_height = 0;

  return VA_STATUS_SUCCESS;
}

static VAStatus vaapi_init_soft_surfaces(vaapi_driver_t *this, int width, int height) {
  ff_vaapi_context_t  *va_context = this->va_context;
  VAStatus            vaStatus;
  int                 i;

  vaapi_destroy_soft_surfaces(this);

  vaStatus = vaCreateSurfaces(va_context->va_display, VA_RT_FORMAT_YUV420, width, height, this->va_soft_surface_ids, SOFT_SURFACES, NULL, 0);
  if(!vaapi_check_status(this, vaStatus, "vaCreateSurfaces()"))
    goto error;

  /* allocate software surfaces */
  for(i = 0; i < SOFT_SURFACES; i++) {

    vaStatus = _x_va_create_image(this->va, this->va_soft_surface_ids[i], &this->va_soft_images[i], width, height, 1, &this->soft_image_is_bound);
    if (!vaapi_check_status(this, vaStatus, "_x_va_create_image()")) {
      this->va_soft_images[i].image_id = VA_INVALID_ID;
      goto error;
    }

    if (!this->soft_image_is_bound) {
      vaStatus = vaPutImage(va_context->va_display, this->va_soft_surface_ids[i], this->va_soft_images[i].image_id,
               0, 0, this->va_soft_images[i].width, this->va_soft_images[i].height,
               0, 0, this->va_soft_images[i].width, this->va_soft_images[i].height);
      vaapi_check_status(this, vaStatus, "vaPutImage()");
    }
#ifdef DEBUG_SURFACE
    printf("vaapi_init_soft_surfaces 0x%08x\n", this->va_soft_surface_ids[i]);
#endif
  }

  this->sw_width  = width;
  this->sw_height = height;
  this->va_soft_head = 0;
  return VA_STATUS_SUCCESS;

error:
  this->sw_width  = 0;
  this->sw_height = 0;
  vaapi_destroy_soft_surfaces(this);
  return VA_STATUS_ERROR_UNKNOWN;
}

static int _flush_recent_frames (vaapi_driver_t *this) {
  int i, n = 0;
  for (i = 0; i < VO_NUM_RECENT_FRAMES; i++) {
    if (this->recent_frames[i]) {
      if (this->guarded_render && this->recent_frames[i]->format == XINE_IMGFMT_VAAPI)
        _x_va_frame_displayed(this->recent_frames[i]);
      this->recent_frames[i]->free (this->recent_frames[i]);
      this->recent_frames[i] = NULL;
      n++;
    }
  }
  return n;
}

static VAStatus vaapi_init_internal(vaapi_driver_t *this, int va_profile, int width, int height) {
  VAStatus            vaStatus;

  vaapi_close(this);

  _flush_recent_frames (this);

  vaStatus = _x_va_init(this->va, va_profile, width, height);
  if (vaStatus != VA_STATUS_SUCCESS)
    goto error;

#if 0
  int i;
  for(i = 0; i < RENDER_SURFACES; i++) {
    if(this->va->frames[i]) {
      /* this seems to break decoding to the surface ? */
      VAImage va_image;
      int is_bound;
      vaStatus = _x_va_create_image(this->va, va_context->va_surface_ids[i], &va_image, width, height, 1, &is_bound);
      if (vaapi_check_status(this, vaStatus, "_x_va_create_image()") && !is_bound) {
        vaStatus = vaPutImage(va_context->va_display, va_context->va_surface_ids[i], va_image.image_id,
                              0, 0, va_image.width, va_image.height,
                              0, 0, va_image.width, va_image.height);
        _x_va_destroy_image(this->va, &va_image);
      }
    }
#ifdef DEBUG_SURFACE
    printf("vaapi_init_internal 0x%08x\n", va_context->va_surface_ids[i]);
#endif
  }
#endif

  vaStatus = vaapi_init_soft_surfaces(this, width, height);
  if(!vaapi_check_status(this, vaStatus, "vaapi_init_soft_surfaces()")) {
    vaapi_destroy_soft_surfaces(this);
    goto error;
  }

  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : guarded render : %d\n", this->guarded_render);

#ifdef ENABLE_VA_GLX
  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : glxrender      : %d\n", this->opengl_render);
  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : glxrender tfp  : %d\n", this->opengl_use_tfp);
#endif
  //xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : is_bound       : %d\n", this->is_bound);
  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : scaling level  : name %s value 0x%08x\n", scaling_level_enum_names[this->scaling_level_enum], this->scaling_level);

#ifdef ENABLE_VA_GLX
  this->init_opengl_render = 1;
#endif

  return VA_STATUS_SUCCESS;

error:
  vaapi_close(this);
  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_init : error init vaapi\n");

  return VA_STATUS_ERROR_UNKNOWN;
}

/* 
 * Init VAAPI. This function is called from the decoder side.
 * When the decoder uses software decoding vaapi_init is not called.
 * Therefore we do it in vaapi_display_frame to get a valid VAAPI context
 */ 
static VAStatus vaapi_init(vo_frame_t *frame_gen, int va_profile, int width, int height) {
  if(!frame_gen)
    return VA_STATUS_ERROR_UNKNOWN;

  vaapi_driver_t *this = (vaapi_driver_t *) frame_gen->driver;
  VAStatus        vaStatus;
  unsigned int    last_sub_img_fmt = this->last_sub_image_fmt;

  if(last_sub_img_fmt)
    vaapi_ovl_associate(this, frame_gen->format, 0);

  if(!this->guarded_render) {
    pthread_mutex_lock(&this->vaapi_lock);
    LOCK_DISPLAY (this);
  }

  vaStatus = vaapi_init_internal(this, va_profile, width, height);

  if(!this->guarded_render) {
    UNLOCK_DISPLAY (this);
    pthread_mutex_unlock(&this->vaapi_lock);
  }

  if(last_sub_img_fmt)
    vaapi_ovl_associate(this, frame_gen->format, this->has_overlay);

  return vaStatus;
}

static vo_frame_t *vaapi_alloc_frame (vo_driver_t *this_gen) {
  vaapi_driver_t  *this = xine_container_of(this_gen, vaapi_driver_t, vo_driver);
  vaapi_frame_t   *frame;
  static const struct vaapi_accel_funcs_s accel_funcs = {
    .vaapi_init                = vaapi_init,
    .profile_from_imgfmt       = _x_va_accel_profile_from_imgfmt,
    .get_context               = _x_va_accel_get_context,
    .lock_vaapi                = _x_va_accel_lock_decode_dummy,
    .unlock_vaapi              = NULL,

    .get_vaapi_surface         = _x_va_accel_get_vaapi_surface,
    .render_vaapi_surface      = NULL,
    .release_vaapi_surface     = NULL,
    .guarded_render            = _x_va_accel_guarded_render,
  };
  static const struct vaapi_accel_funcs_s accel_funcs_guarded = {
    .vaapi_init                = vaapi_init,
    .profile_from_imgfmt       = _x_va_accel_profile_from_imgfmt,
    .get_context               = _x_va_accel_get_context,
    .lock_vaapi                = vaapi_lock_decode_guarded,
    .unlock_vaapi              = vaapi_unlock_decode_guarded,

    .get_vaapi_surface         = _x_va_accel_alloc_vaapi_surface,
    .render_vaapi_surface      = _x_va_accel_render_vaapi_surface,
    .release_vaapi_surface     = _x_va_accel_release_vaapi_surface,
    .guarded_render            = _x_va_accel_guarded_render,
  };

  frame = _x_va_frame_alloc_frame(this->va, this_gen, this->guarded_render);
  if (!frame)
    return NULL;

  /* override accel functions */
  frame->vaapi_accel_data.f = this->guarded_render ? &accel_funcs_guarded : &accel_funcs;

  lprintf("alloc frame\n");

  return &frame->mem_frame.vo_frame;
}


/* Display OSD */
static int vaapi_ovl_associate(vaapi_driver_t *this, int format, int bShow) {
  ff_vaapi_context_t  *va_context = this->va_context;
  VAStatus vaStatus;

  if(!va_context->valid_context)
    return 0;

  if (this->last_sub_image_fmt && !bShow) {
    if (this->va_subpic_id != VA_INVALID_ID) {
      if (this->last_sub_image_fmt == XINE_IMGFMT_VAAPI) {
        vaStatus = vaDeassociateSubpicture(va_context->va_display, this->va_subpic_id,
                                va_context->va_surface_ids, RENDER_SURFACES);
        vaapi_check_status(this, vaStatus, "vaDeassociateSubpicture()");
      } else if (this->last_sub_image_fmt == XINE_IMGFMT_YV12 ||
                 this->last_sub_image_fmt == XINE_IMGFMT_YUY2) {
        vaStatus = vaDeassociateSubpicture(va_context->va_display, this->va_subpic_id,
                                this->va_soft_surface_ids, SOFT_SURFACES);
        vaapi_check_status(this, vaStatus, "vaDeassociateSubpicture()");
      }
    }
    this->last_sub_image_fmt = 0;
    return 1;
  }
  
  if (!this->last_sub_image_fmt && bShow) {
    unsigned int flags = 0;
    unsigned int output_width = va_context->width;
    unsigned int output_height = va_context->height;
    unsigned char *p_dest;
    uint32_t *p_src;
    void *p_base = NULL;

    VAStatus vaStatus;
    uint32_t i;

    vaapi_destroy_subpicture(this);
    vaStatus = vaapi_create_subpicture(this, this->overlay_bitmap_width, this->overlay_bitmap_height);
    if(!vaapi_check_status(this, vaStatus, "vaapi_create_subpicture()"))
      return 0;

    vaStatus = vaMapBuffer(va_context->va_display, this->va_subpic_image.buf, &p_base);
    if(!vaapi_check_status(this, vaStatus, "vaMapBuffer()"))
      return 0;

    p_src = this->overlay_bitmap;
    p_dest = p_base;
    for (i = 0; i < this->overlay_bitmap_height; i++) {
        xine_fast_memcpy(p_dest, p_src, this->overlay_bitmap_width * sizeof(uint32_t));
        p_dest += this->va_subpic_image.pitches[0];
        p_src += this->overlay_bitmap_width;
    }

    vaStatus = vaUnmapBuffer(va_context->va_display, this->va_subpic_image.buf);
    vaapi_check_status(this, vaStatus, "vaUnmapBuffer()");

    lprintf( "vaapi_ovl_associate: overlay_width=%d overlay_height=%d unscaled %d va_subpic_id 0x%08x ovl_changed %d has_overlay %d bShow %d overlay_bitmap_width %d overlay_bitmap_height %d va_context->width %d va_context->height %d\n", 
           this->overlay_output_width, this->overlay_output_height, this->has_overlay, 
           this->va_subpic_id, this->ovl_changed, this->has_overlay, bShow,
           this->overlay_bitmap_width, this->overlay_bitmap_height,
           va_context->width, va_context->height);

    if(format == XINE_IMGFMT_VAAPI) {
      lprintf("vaapi_ovl_associate hw\n");
      vaStatus = vaAssociateSubpicture(va_context->va_display, this->va_subpic_id,
                              va_context->va_surface_ids, RENDER_SURFACES,
                              0, 0, this->va_subpic_image.width, this->va_subpic_image.height,
                              0, 0, output_width, output_height, flags);
    } else {
      lprintf("vaapi_ovl_associate sw\n");
      vaStatus = vaAssociateSubpicture(va_context->va_display, this->va_subpic_id,
                              this->va_soft_surface_ids, SOFT_SURFACES,
                              0, 0, this->va_subpic_image.width, this->va_subpic_image.height,
                              0, 0, this->va_soft_images[0].width, this->va_soft_images[0].height, flags);
    }

    if(vaapi_check_status(this, vaStatus, "vaAssociateSubpicture()")) {
      this->last_sub_image_fmt = format;
      return 1;
    }
  }
  return 0;
}

static void vaapi_overlay_begin (vo_driver_t *this_gen,
                              vo_frame_t *frame_gen, int changed) {
  vaapi_driver_t      *this       = (vaapi_driver_t *) this_gen;
  ff_vaapi_context_t  *va_context = this->va_context;

  if ( !changed )
    return;

  this->has_overlay = 0;
  ++this->ovl_changed;

  /* Apply OSD layer. */
  if(va_context->valid_context) {
    lprintf("vaapi_overlay_begin chaned %d\n", changed);

    pthread_mutex_lock(&this->vaapi_lock);
    LOCK_DISPLAY (this);

    vaapi_ovl_associate(this, frame_gen->format, this->has_overlay);

    UNLOCK_DISPLAY (this);
    pthread_mutex_unlock(&this->vaapi_lock);
  }
}

static void vaapi_overlay_blend (vo_driver_t *this_gen,
                                 vo_frame_t *frame_gen, vo_overlay_t *overlay) {
  vaapi_driver_t  *this = (vaapi_driver_t *) this_gen;

  int i = this->ovl_changed;

  (void)frame_gen;
  if (!i)
    return;

  if (--i >= XINE_VORAW_MAX_OVL)
    return;

  if (overlay->width <= 0 || overlay->height <= 0 || (!overlay->rle && (!overlay->argb_layer || !overlay->argb_layer->buffer)))
    return;

  if (overlay->rle)
    lprintf("overlay[%d] rle %s%s %dx%d@%d,%d hili rect %d,%d-%d,%d\n", i,
            overlay->unscaled ? " unscaled ": " scaled ",
            (overlay->rgb_clut > 0 || overlay->hili_rgb_clut > 0) ? " rgb ": " ycbcr ",
            overlay->width, overlay->height, overlay->x, overlay->y,
            overlay->hili_left, overlay->hili_top,
            overlay->hili_right, overlay->hili_bottom);
  if (overlay->argb_layer && overlay->argb_layer->buffer)
    lprintf("overlay[%d] argb %s %dx%d@%d,%d dirty rect %d,%d-%d,%d\n", i,
            overlay->unscaled ? " unscaled ": " scaled ",
            overlay->width, overlay->height, overlay->x, overlay->y,
            overlay->argb_layer->x1, overlay->argb_layer->y1,
            overlay->argb_layer->x2, overlay->argb_layer->y2);


  this->overlays[i] = overlay;

  ++this->ovl_changed;
}

static void _merge_rects(vaapi_rect_t *rect, const vo_overlay_t *ovl)
{
  if (ovl->x < rect->x1)
    rect->x1 = ovl->x;
  if (ovl->y < rect->y1)
    rect->y1 = ovl->y;
  if ((ovl->x + ovl->width) > rect->x2)
    rect->x2 = ovl->x + ovl->width;
  if ((ovl->y + ovl->height) > rect->y2)
    rect->y2 = ovl->y + ovl->height;
}

static void vaapi_overlay_end (vo_driver_t *this_gen, vo_frame_t *frame_gen) {
  vaapi_driver_t      *this       = (vaapi_driver_t *) this_gen;
  mem_frame_t         *frame      = xine_container_of(frame_gen, mem_frame_t, vo_frame);
  ff_vaapi_context_t  *va_context = this->va_context;

  int novls = this->ovl_changed;
  if (novls < 2) {
    this->ovl_changed = 0;
    return;
  }
  --novls;

  uint32_t output_width = frame->width, output_height = frame->height;
  uint32_t unscaled_width = 0, unscaled_height = 0;
  vo_overlay_t *first_scaled = NULL, *first_unscaled = NULL;
  /* calm down compiler */
  vaapi_rect_t dirty_rect = { 0, 0, 0, 0};
  vaapi_rect_t unscaled_dirty_rect = {0, 0, 0, 0};
  int has_rle = 0;

  int i;
  for (i = 0; i < novls; ++i) {
    vo_overlay_t *ovl = this->overlays[i];

    if (ovl->rle)
      has_rle = 1;

    if (ovl->unscaled) {
      if (first_unscaled) {
        _merge_rects(&unscaled_dirty_rect, ovl);
      } else {
        first_unscaled = ovl;
        unscaled_dirty_rect.x1 = ovl->x;
        unscaled_dirty_rect.y1 = ovl->y;
        unscaled_dirty_rect.x2 = ovl->x + ovl->width;
        unscaled_dirty_rect.y2 = ovl->y + ovl->height;
      }

      unscaled_width = unscaled_dirty_rect.x2;
      unscaled_height = unscaled_dirty_rect.y2;
    } else {
      if (first_scaled) {
        _merge_rects(&dirty_rect, ovl);
      } else {
        first_scaled = ovl;
        dirty_rect.x1 = ovl->x;
        dirty_rect.y1 = ovl->y;
        dirty_rect.x2 = ovl->x + ovl->width;
        dirty_rect.y2 = ovl->y + ovl->height;
      }

      if (dirty_rect.x2 > (int)output_width)
        output_width = dirty_rect.x2;
      if (dirty_rect.y2 > (int)output_height)
        output_height = dirty_rect.y2;

    }
  }

  int need_init = 0;

  lprintf("dirty_rect.x1 %d dirty_rect.y1 %d dirty_rect.x2 %d dirty_rect.y2 %d output_width %d output_height %d\n",
      dirty_rect.x1, dirty_rect.y1, dirty_rect.x2, dirty_rect.y2, output_width, output_height);

  if (first_scaled) {
    vaapi_rect_t dest;
    dest.x1 = first_scaled->x;
    dest.y1 = first_scaled->y;
    dest.x2 = first_scaled->x + first_scaled->width;
    dest.y2 = first_scaled->y + first_scaled->height;
    if (!RECT_IS_EQ(dest, dirty_rect))
      need_init = 1;
  }

  int need_unscaled_init = (first_unscaled &&
                                  (first_unscaled->x != unscaled_dirty_rect.x1 ||
                                   first_unscaled->y != unscaled_dirty_rect.y1 ||
                                   (first_unscaled->x + first_unscaled->width) != unscaled_dirty_rect.x2 ||
                                   (first_unscaled->y + first_unscaled->height) != unscaled_dirty_rect.y2));

  if (first_scaled) {
    this->overlay_output_width = output_width;
    this->overlay_output_height = output_height;

    need_init = 1;

    this->overlay_dirty_rect = dirty_rect;
  }

  if (first_unscaled) {
    need_unscaled_init = 1;
  }

  if (has_rle || need_init || need_unscaled_init) {
    lprintf("has_rle %d need_init %d need_unscaled_init %d unscaled_width %d unscaled_height %d output_width %d output_height %d\n", 
        has_rle, need_init, need_unscaled_init, unscaled_width, unscaled_height, output_width, output_height);
    if (need_init) {
      this->overlay_bitmap_width = output_width;
      this->overlay_bitmap_height = output_height;
    }
#define UMAX(a,b) ((a) > (uint32_t)(b) ? (a) : (uint32_t)(b))
    if (need_unscaled_init) {

      if(this->vdr_osd_width) 
        this->overlay_bitmap_width =  UMAX (this->vdr_osd_width, this->sc.gui_width);
      else
        this->overlay_bitmap_width =  UMAX (unscaled_width, this->sc.gui_width);

      if(this->vdr_osd_height) 
        this->overlay_bitmap_height = UMAX (this->vdr_osd_height, this->sc.gui_height);
      else
        this->overlay_bitmap_height = UMAX (unscaled_height, this->sc.gui_height);

    } else if (need_init) {

      if(this->vdr_osd_width) 
        this->overlay_bitmap_width =  UMAX (this->vdr_osd_width, this->sc.gui_width);
      else
        this->overlay_bitmap_width =  UMAX (output_width, this->sc.gui_width);

      if(this->vdr_osd_height) 
        this->overlay_bitmap_height = UMAX (this->vdr_osd_height, this->sc.gui_height);
      else
        this->overlay_bitmap_height = UMAX (output_height, this->sc.gui_height);

    }
  }

  if ((this->overlay_bitmap_width * this->overlay_bitmap_height) > this->overlay_bitmap_size) {
    this->overlay_bitmap_size = this->overlay_bitmap_width * this->overlay_bitmap_height;
    free(this->overlay_bitmap);
    this->overlay_bitmap = calloc( this->overlay_bitmap_size, sizeof(uint32_t));
  } else {
    memset(this->overlay_bitmap, 0x0, this->overlay_bitmap_size * sizeof(uint32_t));
  }

  for (i = 0; i < novls; ++i) {
    vo_overlay_t *ovl = this->overlays[i];
    uint32_t *bitmap = NULL;

    if (ovl->rle) {
      if(ovl->width<=0 || ovl->height<=0)
        continue;

      if (!ovl->rgb_clut || !ovl->hili_rgb_clut)
        _x_overlay_clut_yuv2rgb (ovl, this->color_matrix);

      bitmap = malloc(ovl->width * ovl->height * sizeof(uint32_t));

      _x_overlay_to_argb32(ovl, bitmap, ovl->width, "BGRA");

      lprintf("width %d height %d\n", ovl->width, ovl->height);
    } else {
      pthread_mutex_lock(&ovl->argb_layer->mutex);
      bitmap = ovl->argb_layer->buffer;
    }

    /* Blit overlay to destination */
    uint32_t pitch = ovl->width * sizeof(uint32_t);
    uint32_t *copy_dst = this->overlay_bitmap;
    uint32_t *copy_src = NULL;
    uint32_t height = 0;

    copy_src = bitmap;
  
    copy_dst += ovl->y * this->overlay_bitmap_width;

    lprintf("overlay_bitmap_width %d overlay_bitmap_height %d  ovl->x %d ovl->y %d ovl->width %d ovl->height %d width %d height %d\n",
      this->overlay_bitmap_width, this->overlay_bitmap_height, ovl->x, ovl->y, ovl->width, ovl->height, this->overlay_bitmap_width, this->overlay_bitmap_height);

    for(height = 0; (int)height < ovl->height; height++) {
      if((height + ovl->y) >= this->overlay_bitmap_height)
        break;

      xine_fast_memcpy(copy_dst + ovl->x, copy_src, pitch);
      copy_dst += this->overlay_bitmap_width;
      copy_src += ovl->width;
    }

    if (ovl->rle) {
      free(bitmap);
      bitmap = NULL;
    }

    if (!ovl->rle)
      pthread_mutex_unlock(&ovl->argb_layer->mutex);

  }

  this->ovl_changed = 0;
  this->has_overlay = (first_scaled != NULL) | (first_unscaled != NULL);

  lprintf("this->has_overlay %d\n", this->has_overlay);
  /* Apply OSD layer. */
  if(va_context->valid_context) {
    pthread_mutex_lock(&this->vaapi_lock);
    LOCK_DISPLAY (this);

    vaapi_ovl_associate(this, frame_gen->format, this->has_overlay);

    UNLOCK_DISPLAY (this);
    pthread_mutex_unlock(&this->vaapi_lock);
  }
}

#ifdef ENABLE_VA_GLX
#ifndef HAVE_GLU
#define gluPerspective myGluPerspective
static void myGluPerspective (GLdouble fovy, GLdouble aspect,
                              GLdouble zNear, GLdouble zFar) {
  double ymax = zNear * tan(fovy * M_PI / 360.0);
  double ymin = -ymax;
  glFrustum (ymin * aspect, ymax * aspect, ymin, ymax, zNear, zFar);
}
#endif
static void vaapi_resize_glx_window (vaapi_driver_t *this, int width, int height) {

  if (this->gl_context) {
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(FOVY, ASPECT, Z_NEAR, Z_FAR);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(-0.5f, -0.5f, -Z_CAMERA);
    glScalef(1.0f / (GLfloat)width, 
             -1.0f / (GLfloat)height,
             1.0f / (GLfloat)width);
    glTranslatef(0.0f, -1.0f * (GLfloat)height, 0.0f);
  }
}
#endif

static int vaapi_redraw_needed (vo_driver_t *this_gen) {
  vaapi_driver_t      *this       = (vaapi_driver_t *) this_gen;
  int                 ret = 0;

  _x_vo_scale_compute_ideal_size( &this->sc );

  if ( _x_vo_scale_redraw_needed( &this->sc ) ) {
    _x_vo_scale_compute_output_size( &this->sc );

    XMoveResizeWindow(this->display, this->window, 
                      0, 0, this->sc.gui_width, this->sc.gui_height);
#ifdef ENABLE_VA_GLX
    vaapi_resize_glx_window(this, this->sc.gui_width, this->sc.gui_height);
#endif
    ret = 1;
  }

  if (this->color_matrix == 0)
    ret = 1;

  return ret;
}

static VAStatus vaapi_software_render_frame(vaapi_driver_t *this, mem_frame_t *frame,
                                            VAImage *va_image, int is_bound, VASurfaceID va_surface_id) {
  ff_vaapi_context_t *va_context      = this->va_context;
  void               *p_base          = NULL;
  VAStatus           vaStatus;

  if(va_image == NULL || va_image->image_id == VA_INVALID_ID ||
     va_surface_id == VA_INVALID_SURFACE || !va_context->valid_context)
    return VA_STATUS_ERROR_UNKNOWN;

  lprintf("vaapi_software_render_frame : va_surface_id 0x%08x va_image.image_id 0x%08x width %d height %d f_width %d f_height %d sw_width %d sw_height %d\n", 
      va_surface_id, va_image->image_id, va_image->width, va_image->height, frame->width, frame->height,
      this->sw_width, this->sw_height);

  if(frame->width != va_image->width || frame->height != va_image->height)
    return VA_STATUS_SUCCESS;

  vaStatus = vaMapBuffer( va_context->va_display, va_image->buf, &p_base ) ;
  if(!vaapi_check_status(this, vaStatus, "vaMapBuffer()"))
    return vaStatus;


  uint8_t *dst[3] = { NULL, };
  uint32_t  pitches[3];

  if(this->swap_uv_planes) {
    dst[0] = (uint8_t *)p_base + va_image->offsets[0]; pitches[0] = va_image->pitches[0];
    dst[1] = (uint8_t *)p_base + va_image->offsets[1]; pitches[1] = va_image->pitches[1];
    dst[2] = (uint8_t *)p_base + va_image->offsets[2]; pitches[2] = va_image->pitches[2];
  } else {
    dst[0] = (uint8_t *)p_base + va_image->offsets[0]; pitches[0] = va_image->pitches[0];
    dst[1] = (uint8_t *)p_base + va_image->offsets[2]; pitches[1] = va_image->pitches[1];
    dst[2] = (uint8_t *)p_base + va_image->offsets[1]; pitches[2] = va_image->pitches[2];
  }

  /* Copy xine frames into VAAPI images */
  if(frame->format == XINE_IMGFMT_YV12) {

    if (va_image->format.fourcc == VA_FOURCC( 'Y', 'V', '1', '2' ) ||
        va_image->format.fourcc == VA_FOURCC( 'I', '4', '2', '0' ) ) {
      lprintf("vaapi_software_render_frame yv12 -> yv12 convert\n");

      yv12_to_yv12(
              /* Y */
              frame->vo_frame.base[0], frame->vo_frame.pitches[0],
              dst[0], pitches[0],
              /* U */
              frame->vo_frame.base[1], frame->vo_frame.pitches[1],
              dst[1], pitches[1],
              /* V */
              frame->vo_frame.base[2], frame->vo_frame.pitches[2],
              dst[2], pitches[2],
              /* width x height */
              frame->vo_frame.width, frame->vo_frame.height);

    } else if (va_image->format.fourcc == VA_FOURCC( 'N', 'V', '1', '2' )) {
      lprintf("vaapi_software_render_frame yv12 -> nv12 convert\n");

      _x_yv12_to_nv12(frame->vo_frame.base[0], frame->vo_frame.pitches[0],
                      frame->vo_frame.base[1], frame->vo_frame.pitches[1],
                      frame->vo_frame.base[2], frame->vo_frame.pitches[2],
                      (uint8_t *)p_base + va_image->offsets[0], va_image->pitches[0],
                      (uint8_t *)p_base + va_image->offsets[1], va_image->pitches[1],
                      frame->vo_frame.width, frame->vo_frame.height);

    }
  } else if (frame->format == XINE_IMGFMT_YUY2) {

    if (va_image->format.fourcc == VA_FOURCC( 'Y', 'V', '1', '2' ) ||
        va_image->format.fourcc == VA_FOURCC( 'I', '4', '2', '0' ) ) {
      lprintf("vaapi_software_render_frame yuy2 -> yv12 convert\n");

      yuy2_to_yv12(frame->vo_frame.base[0], frame->vo_frame.pitches[0],
                  dst[0], pitches[0],
                  dst[1], pitches[1],
                  dst[2], pitches[2],
                  frame->vo_frame.width, frame->vo_frame.height);

    } else if (va_image->format.fourcc == VA_FOURCC( 'N', 'V', '1', '2' )) {
      lprintf("vaapi_software_render_frame yuy2 -> nv12 convert\n");

      _x_yuy2_to_nv12(frame->vo_frame.base[0], frame->vo_frame.pitches[0],
                      (uint8_t *)p_base + va_image->offsets[0], va_image->pitches[0],
                      (uint8_t *)p_base + va_image->offsets[1], va_image->pitches[1],
                      frame->vo_frame.width, frame->vo_frame.height);
    }

  }

  vaStatus = vaUnmapBuffer(va_context->va_display, va_image->buf);
  if(!vaapi_check_status(this, vaStatus, "vaUnmapBuffer()"))
    return vaStatus;

  if (!is_bound) {
    vaStatus = vaPutImage(va_context->va_display, va_surface_id, va_image->image_id,
                        0, 0, va_image->width, va_image->height,
                        0, 0, va_image->width, va_image->height);
    if(!vaapi_check_status(this, vaStatus, "vaPutImage()"))
      return vaStatus;
  }

  return VA_STATUS_SUCCESS;
}

static VAStatus vaapi_hardware_render_frame (vaapi_driver_t *this, mem_frame_t *frame,
                                             VASurfaceID va_surface_id) {
  ff_vaapi_context_t *va_context      = this->va_context;
  VAStatus           vaStatus         = VA_STATUS_ERROR_UNKNOWN; 
  int                i                = 0;
  int                interlaced_frame = !frame->vo_frame.progressive_frame;
  int                top_field_first  = frame->vo_frame.top_field_first;
#if defined(ENABLE_VA_GLX) || defined(LOG)
  int                width, height;

  if(frame->format == XINE_IMGFMT_VAAPI) {
    width  = va_context->width;
    height = va_context->height;
  } else {
    width  = (frame->width > this->sw_width) ? this->sw_width : frame->width;
    height = (frame->height > this->sw_height) ? this->sw_height : frame->height;
  }
#endif

  if(!va_context->valid_context || va_surface_id == VA_INVALID_SURFACE)
    return VA_STATUS_ERROR_UNKNOWN;

#ifdef ENABLE_VA_GLX
  if(this->opengl_render && !this->gl_context)
    return VA_STATUS_ERROR_UNKNOWN;
#endif

  /* Final VAAPI rendering. The deinterlacing can be controled by xine config.*/
  unsigned int deint = this->deinterlace;
  for(i = 0; i <= !!((deint > 1) && interlaced_frame); i++) {
    unsigned int flags = (deint && (interlaced_frame) ? (((!!(top_field_first)) ^ i) == 0 ? VA_BOTTOM_FIELD : VA_TOP_FIELD) : VA_FRAME_PICTURE);

    vaapi_update_csc (this, frame);
    flags |= this->vaapi_cm_flags;

    flags |= VA_CLEAR_DRAWABLE;
    flags |= this->scaling_level;

    lprintf("Putsrfc srfc 0x%08X flags 0x%08x %dx%d -> %dx%d interlaced %d top_field_first %d\n", 
            va_surface_id, flags, width, height, 
            this->sc.output_width, this->sc.output_height,
            interlaced_frame, top_field_first);

#ifdef ENABLE_VA_GLX
    if(this->opengl_render) {
      const char *msg;
      vaapi_x11_trap_errors();

      if(this->opengl_use_tfp) {
        lprintf("opengl render tfp\n");
        vaStatus = vaPutSurface(va_context->va_display, va_surface_id, this->gl_image_pixmap,
                 0, 0, width, height, 0, 0, width, height, NULL, 0, flags);
        msg = "vaPutSurface()";
      } else {
        lprintf("opengl render\n");
        vaStatus = vaCopySurfaceGLX(va_context->va_display, this->gl_surface, va_surface_id, flags);
        msg = "vaCopySurfaceGLX()";
      }

      if(vaapi_x11_untrap_errors())
        return VA_STATUS_ERROR_UNKNOWN;
      if(!vaapi_check_status(this, vaStatus, msg))
        return vaStatus;

      vaapi_glx_flip_page(this, frame, 0, 0, va_context->width, va_context->height);

    } else
#endif
    {

      vaStatus = vaPutSurface(va_context->va_display, va_surface_id, this->window,
                   this->sc.displayed_xoffset, this->sc.displayed_yoffset,
                   this->sc.displayed_width, this->sc.displayed_height,
                   this->sc.output_xoffset, this->sc.output_yoffset,
                   this->sc.output_width, this->sc.output_height,
                   NULL, 0, flags);
      if(!vaapi_check_status(this, vaStatus, "vaPutSurface()"))
        return vaStatus;
    }
    // workaround by johns from vdrportal.de
    usleep(1 * 1000);
  }
  return VA_STATUS_SUCCESS;
}

/* Used in vaapi_display_frame to determine how long displaying a frame takes
   - if slower than 60fps, print a message
*/
/*
static double timeOfDay()
{
    struct timeval t;
    gettimeofday( &t, NULL );
    return ((double)t.tv_sec) + (((double)t.tv_usec)/1000000.0);
}
*/

static void _add_recent_frame (vaapi_driver_t *this, vo_frame_t *vo_frame) {
  int i;

  i = VO_NUM_RECENT_FRAMES-1;
  if (this->recent_frames[i]) {
    if (this->guarded_render && this->recent_frames[i]->format == XINE_IMGFMT_VAAPI)
      _x_va_frame_displayed(this->recent_frames[i]);
    this->recent_frames[i]->free (this->recent_frames[i]);
  }

  for( ; i ; i-- )
    this->recent_frames[i] = this->recent_frames[i-1];

  this->recent_frames[0] = vo_frame;
}

static void vaapi_display_frame (vo_driver_t *this_gen, vo_frame_t *frame_gen) {
  vaapi_driver_t     *this          = (vaapi_driver_t *) this_gen;
  vaapi_accel_t      *accel         = frame_gen->accel_data;
  mem_frame_t        *frame         = xine_container_of(frame_gen, mem_frame_t, vo_frame);
  ff_vaapi_context_t *va_context    = this->va_context;
  VASurfaceID        va_surface_id  = VA_INVALID_SURFACE;
  VAImage            *va_image      = NULL;
  VAStatus           vaStatus;

  lprintf("vaapi_display_frame\n");

  if (frame->format != XINE_IMGFMT_VAAPI && frame->format != XINE_IMGFMT_YV12 && frame->format != XINE_IMGFMT_YUY2) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " unsupported image format %x width %d height %d valid_context %d\n",
            frame->format, frame->width, frame->height, va_context->valid_context);
    frame_gen->free (frame_gen);
    return;
  }

  /*
  if((frame->height < 17 || frame->width < 17) && ((frame->format == XINE_IMGFMT_YV12) || (frame->format == XINE_IMGFMT_YUY2))) {
    frame->vo_frame.free( frame_gen );
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " frame size to small width %d height %d\n", frame->height, frame->width);
    return;
  }
  */

  /*
   * let's see if this frame is different in size / aspect
   * ratio from the previous one
   */

  pthread_mutex_lock(&this->vaapi_lock);
  LOCK_DISPLAY (this);

  if ( (frame->width != this->sc.delivered_width)
       || (frame->height != this->sc.delivered_height)
       || (frame->ratio != this->sc.delivered_ratio)
       || (frame->vo_frame.crop_left != this->sc.crop_left)
       || (frame->vo_frame.crop_right != this->sc.crop_right)
       || (frame->vo_frame.crop_top != this->sc.crop_top)
       || (frame->vo_frame.crop_bottom != this->sc.crop_bottom) ) {
    lprintf("frame format changed\n");
    this->sc.force_redraw = 1;
  }

  /*
   * tell gui that we are about to display a frame,
   * ask for offset and output size
   */
  this->sc.delivered_height = frame->height;
  this->sc.delivered_width  = frame->width;
  this->sc.delivered_ratio  = frame->ratio;

  this->sc.crop_left        = frame->vo_frame.crop_left;
  this->sc.crop_right       = frame->vo_frame.crop_right;
  this->sc.crop_top         = frame->vo_frame.crop_top;
  this->sc.crop_bottom      = frame->vo_frame.crop_bottom;

  lprintf("vaapi_display_frame %s frame->width %d frame->height %d va_context->sw_width %d va_context->sw_height %d valid_context %d\n",
        (frame->format == XINE_IMGFMT_VAAPI) ? "XINE_IMGFMT_VAAPI" : ((frame->format == XINE_IMGFMT_YV12) ? "XINE_IMGFMT_YV12" : "XINE_IMGFMT_YUY2") ,
        frame->width, frame->height, this->sw_width, this->sw_height, va_context->valid_context);

  if( ((frame->format == XINE_IMGFMT_YV12) || (frame->format == XINE_IMGFMT_YUY2)) 
      && ((frame->width != this->sw_width) ||(frame->height != this->sw_height )) ) {

    lprintf("vaapi_display_frame %s frame->width %d frame->height %d\n", 
        (frame->format == XINE_IMGFMT_VAAPI) ? "XINE_IMGFMT_VAAPI" : ((frame->format == XINE_IMGFMT_YV12) ? "XINE_IMGFMT_YV12" : "XINE_IMGFMT_YUY2") ,
        frame->width, frame->height);

    unsigned int last_sub_img_fmt = this->last_sub_image_fmt;

    if(last_sub_img_fmt)
      vaapi_ovl_associate(this, frame_gen->format, 0);

    if(!va_context->valid_context) {
      lprintf("vaapi_display_frame init full context\n");
      vaapi_init_internal(this, SW_CONTEXT_INIT_FORMAT, frame->width, frame->height);
    } else {
      lprintf("vaapi_display_frame init soft surfaces\n");
      vaapi_init_soft_surfaces(this, frame->width, frame->height);
    }

    this->sc.force_redraw = 1;
#ifdef ENABLE_VA_GLX
    this->init_opengl_render = 1;
#endif

    if(last_sub_img_fmt)
      vaapi_ovl_associate(this, frame_gen->format, this->has_overlay);
  }

  UNLOCK_DISPLAY (this);
  pthread_mutex_unlock(&this->vaapi_lock);

  vaapi_redraw_needed (this_gen);

  pthread_mutex_lock(&this->vaapi_lock);
  LOCK_DISPLAY (this);

  /* posible race could happen while the lock is opened */
  if (!va_context->valid_context) {
    UNLOCK_DISPLAY (this);
    pthread_mutex_unlock(&this->vaapi_lock);
    frame_gen->free (frame_gen);
    return;
  }

#ifdef ENABLE_VA_GLX
  /* initialize opengl rendering */
  if(this->opengl_render && this->init_opengl_render) {
    unsigned int last_sub_img_fmt = this->last_sub_image_fmt;

    if(last_sub_img_fmt)
      vaapi_ovl_associate(this, frame_gen->format, 0);

    destroy_glx(this);

    vaapi_glx_config_glx(this, va_context->width, va_context->height);

    vaapi_resize_glx_window(this, this->sc.gui_width, this->sc.gui_height);

    if(last_sub_img_fmt)
      vaapi_ovl_associate(this, frame_gen->format, this->has_overlay);

    this->sc.force_redraw = 1;
    this->init_opengl_render = 0;
  }
#endif

  /*
  double start_time;
  double end_time;
  double elapse_time;
  int factor;

  start_time = timeOfDay();
  */

  if (frame->format != XINE_IMGFMT_VAAPI) {
    va_surface_id = this->va_soft_surface_ids[this->va_soft_head];
    va_image = &this->va_soft_images[this->va_soft_head];
    this->va_soft_head = (this->va_soft_head + 1) % (SOFT_SURFACES);
  } else if (accel->index < RENDER_SURFACES) { // (frame->format == XINE_IMGFMT_VAAPI)
    ff_vaapi_surface_t *va_surface = &va_context->va_render_surfaces[accel->index];
    if (this->guarded_render) {
      if (va_surface->status == SURFACE_RENDER || va_surface->status == SURFACE_RENDER_RELEASE) {
        va_surface_id = va_surface->va_surface_id;
      }
#ifdef DEBUG_SURFACE
      printf("vaapi_display_frame va_surface 0x%08x status %d index %d\n", va_surface_id, va_surface->status, accel->index);
#endif
    } else {
      va_surface_id = va_surface->va_surface_id;
    }
    va_image      = NULL;
  }

  lprintf("2: 0x%08x\n", va_surface_id);

  if (va_surface_id != VA_INVALID_SURFACE) {
    VASurfaceStatus surf_status = 0;
    if (this->va->query_va_status) {
      vaStatus = vaQuerySurfaceStatus(va_context->va_display, va_surface_id, &surf_status);
      vaapi_check_status(this, vaStatus, "vaQuerySurfaceStatus()");
    } else {
      surf_status = VASurfaceReady;
    }

    if (surf_status != VASurfaceReady) {
      va_surface_id = VA_INVALID_SURFACE;
      va_image = NULL;
#ifdef DEBUG_SURFACE
      printf("Surface srfc 0x%08X not ready for render\n", va_surface_id);
#endif
    }
  } else {
#ifdef DEBUG_SURFACE
    printf("Invalid srfc 0x%08X\n", va_surface_id);
#endif
  }

  if (va_surface_id != VA_INVALID_SURFACE) {
    lprintf("vaapi_display_frame: 0x%08x %d %d\n", va_surface_id, va_context->width, va_context->height);

    vaStatus = vaSyncSurface(va_context->va_display, va_surface_id);
    vaapi_check_status(this, vaStatus, "vaSyncSurface()");

    /* transfer image data to a VAAPI surface */
    if (frame->format != XINE_IMGFMT_VAAPI) {
      vaapi_software_render_frame(this, frame, va_image, this->soft_image_is_bound, va_surface_id);
    }
    vaapi_hardware_render_frame(this, frame, va_surface_id);
  }

  XSync(this->display, False);

  //end_time = timeOfDay();

  _add_recent_frame (this, frame_gen);

  pthread_mutex_unlock(&this->vaapi_lock);
  UNLOCK_DISPLAY (this);

  /*
  elapse_time = end_time - start_time;
  factor = (int)(elapse_time/(1.0/60.0));

  if( factor > 1 )
  {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " PutImage %dX interval (%fs)\n", factor, elapse_time );
  }
  */
}

static int vaapi_get_property (vo_driver_t *this_gen, int property) {
  vaapi_driver_t *this = (vaapi_driver_t *) this_gen;

  if ((property < 0) || (property >= VO_NUM_PROPERTIES)) return 0;

  switch (property) {
    case VO_PROP_WINDOW_WIDTH:
      this->props[property].value = this->sc.gui_width;
      break;
    case VO_PROP_WINDOW_HEIGHT:
      this->props[property].value = this->sc.gui_height;
      break;
    case VO_PROP_OUTPUT_WIDTH:
      this->props[property].value = this->sc.output_width;
      break;
    case VO_PROP_OUTPUT_HEIGHT:
      this->props[property].value = this->sc.output_height;
      break;
    case VO_PROP_OUTPUT_XOFFSET:
      this->props[property].value = this->sc.output_xoffset;
      break;
    case VO_PROP_OUTPUT_YOFFSET:
      this->props[property].value = this->sc.output_yoffset;
      break;
    case VO_PROP_MAX_NUM_FRAMES:
      /* Split surfaces between decoding and output.
       * Needed to prevent crashes with heavy seeking,
       * bright green flashes, and frame jumping with some h.264. */
      this->props[property].value = RENDER_SURFACES / 2;
      break;
  } 

  lprintf("vaapi_get_property property=%d, value=%d\n", property, this->props[property].value );

  return this->props[property].value;
}

static int vaapi_set_property (vo_driver_t *this_gen, int property, int value) {

  vaapi_driver_t      *this       = (vaapi_driver_t *) this_gen;
  ff_vaapi_context_t  *va_context = this->va_context;

  lprintf("vaapi_set_property property=%d, value=%d\n", property, value );

  if ((property < 0) || (property >= VO_NUM_PROPERTIES)) return 0;

  if ((property == VO_PROP_BRIGHTNESS)
    || (property == VO_PROP_CONTRAST)
    || (property == VO_PROP_SATURATION)
    || (property == VO_PROP_HUE)) {
    /* defer these to vaapi_update_csc () */
    if((value < this->props[property].min) || (value > this->props[property].max))
      value = (this->props[property].min + this->props[property].max) >> 1;
    this->props[property].value = value;
    this->color_matrix = 0;
    return value;
  }

  if(this->props[property].atom) {
    VADisplayAttribute attr;

    if((value < this->props[property].min) || (value > this->props[property].max))
      value = (this->props[property].min + this->props[property].max) >> 1;

    this->props[property].value = value;
    attr.type   = this->props[property].type;
    attr.value  = value;

    if(va_context && va_context->valid_context) {
      vaSetDisplayAttributes(va_context->va_display, &attr, 1);
      //vaapi_check_status(this, vaStatus, "vaSetDisplayAttributes()");
    }

    if (this->props[property].entry)
      this->props[property].entry->num_value = this->props[property].value;

    return this->props[property].value;
  } else {
    switch (property) {

      case VO_PROP_ASPECT_RATIO:
        if (value>=XINE_VO_ASPECT_NUM_RATIOS)
              value = XINE_VO_ASPECT_AUTO;
        this->props[property].value = value;
        this->sc.user_ratio = value;
        _x_vo_scale_compute_ideal_size (&this->sc);
        this->sc.force_redraw = 1;
        break;

      case VO_PROP_ZOOM_X:
        if ((value >= XINE_VO_ZOOM_MIN) && (value <= XINE_VO_ZOOM_MAX)) {
          this->props[property].value = value;
          this->sc.zoom_factor_x = (double)value / (double)XINE_VO_ZOOM_STEP;
          _x_vo_scale_compute_ideal_size (&this->sc);
          this->sc.force_redraw = 1;
        }
        break;

      case VO_PROP_ZOOM_Y:
        if ((value >= XINE_VO_ZOOM_MIN) && (value <= XINE_VO_ZOOM_MAX)) {
          this->props[property].value = value;
          this->sc.zoom_factor_y = (double)value / (double)XINE_VO_ZOOM_STEP;
          _x_vo_scale_compute_ideal_size (&this->sc);
          this->sc.force_redraw = 1;
        }
        break;

      case VO_PROP_DISCARD_FRAMES:
        this->props[property].value = _flush_recent_frames (this);
        break;
    }
  }
  return value;
}

static void vaapi_get_property_min_max (vo_driver_t *this_gen,
                                        int property, int *min, int *max) {
  vaapi_driver_t *this = (vaapi_driver_t *) this_gen;

  *min = this->props[property].min;
  *max = this->props[property].max;
}

static int vaapi_gui_data_exchange (vo_driver_t *this_gen,
                                    int data_type, void *data) {
  vaapi_driver_t     *this       = (vaapi_driver_t *) this_gen;

  lprintf("vaapi_gui_data_exchange %d\n", data_type);

  switch (data_type) {
#ifndef XINE_DISABLE_DEPRECATED_FEATURES
  case XINE_GUI_SEND_COMPLETION_EVENT:
    break;
#endif

  case XINE_GUI_SEND_EXPOSE_EVENT: {
    /* We should get this
     * 1. after initial video window open, and
     * 2. when video window gets revealed behind an other window
     *    while no desktop compositor is running.
     * This works with opengl2 and vdpau.
     * FIXME: With vaapi here, 2. does _not_ work. Why? */
    pthread_mutex_lock(&this->vaapi_lock);
    lprintf("XINE_GUI_SEND_EXPOSE_EVENT:\n");
    this->sc.force_redraw = 1;
#ifdef ENABLE_VA_GLX
    this->init_opengl_render = 1;
#endif
    pthread_mutex_unlock(&this->vaapi_lock);
  }
  break;

  case XINE_GUI_SEND_WILL_DESTROY_DRAWABLE: {
    printf("XINE_GUI_SEND_WILL_DESTROY_DRAWABLE\n");
  }
  break;

  case XINE_GUI_SEND_DRAWABLE_CHANGED: {
    pthread_mutex_lock(&this->vaapi_lock);
    LOCK_DISPLAY (this);
    lprintf("XINE_GUI_SEND_DRAWABLE_CHANGED\n");

    this->drawable = (Drawable) data;

    XReparentWindow(this->display, this->window, this->drawable, 0, 0);

    this->sc.force_redraw = 1;
#ifdef ENABLE_VA_GLX
    this->init_opengl_render = 1;
#endif

    UNLOCK_DISPLAY (this);
    pthread_mutex_unlock(&this->vaapi_lock);
  }
  break;

  case XINE_GUI_SEND_TRANSLATE_GUI_TO_VIDEO: {
    int x1, y1, x2, y2;
    x11_rectangle_t *rect = data;

    _x_vo_scale_translate_gui2video(&this->sc, rect->x, rect->y, &x1, &y1);
    _x_vo_scale_translate_gui2video(&this->sc, rect->x + rect->w, rect->y + rect->h, &x2, &y2);
    rect->x = x1;
    rect->y = y1;
    rect->w = x2-x1;
    rect->h = y2-y1;
  } 
  break;

  default:
    return -1;
  }

  return 0;
}

static void vaapi_dispose_locked (vaapi_driver_t *this) {
  config_values_t     *config = this->xine->config;

  /* cm_close already does this.
  config->unregister_callbacks (config, NULL, NULL, this, sizeof (*this));
  */
  cm_close (this);

  _x_vo_scale_cleanup (&this->sc, config);

  // vaapi_lock is locked at this point, either from vaapi_dispose or vaapi_open_plugin

  LOCK_DISPLAY (this);

  vaapi_close(this);

  _x_va_free(&this->va);

  _x_freep(&this->overlay_bitmap);

  if (this->window != None) {
    vaapi_x11_trap_errors();
    XDestroyWindow(this->display, this->window);
    XSync(this->display, False);
    if (vaapi_x11_untrap_errors()) {
      xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " XDestroyWindow() failed\n");
    }
  }

  UNLOCK_DISPLAY (this);

  _x_freep(&this->va_subpic_formats);
  this->va_num_subpic_formats = 0;

  pthread_mutex_unlock(&this->vaapi_lock);
  pthread_mutex_destroy(&this->vaapi_lock);

  _x_assert(this->recent_frames[0] == NULL);

  free (this);
}

static void vaapi_dispose (vo_driver_t *this_gen) {
  vaapi_driver_t  *this = (vaapi_driver_t *) this_gen;

  lprintf("vaapi_dispose\n");
  pthread_mutex_lock(&this->vaapi_lock);
  vaapi_dispose_locked(this);
}

static void vaapi_vdr_osd_width_flag( void *this_gen, xine_cfg_entry_t *entry )
{
  vaapi_driver_t  *this  = (vaapi_driver_t *) this_gen;

  this->vdr_osd_width = entry->num_value < 0 ? 0 : entry->num_value;
}

static void vaapi_vdr_osd_height_flag( void *this_gen, xine_cfg_entry_t *entry )
{
  vaapi_driver_t  *this  = (vaapi_driver_t *) this_gen;

  this->vdr_osd_height = entry->num_value < 0 ? 0 : entry->num_value;
}

static void vaapi_deinterlace_flag( void *this_gen, xine_cfg_entry_t *entry )
{
  vaapi_driver_t  *this  = (vaapi_driver_t *) this_gen;

  this->deinterlace = entry->num_value;
  if(this->deinterlace > 2)
    this->deinterlace = 2;
}

#ifdef ENABLE_VA_GLX
static void vaapi_opengl_use_tfp( void *this_gen, xine_cfg_entry_t *entry )
{
  vaapi_driver_t  *this  = (vaapi_driver_t *) this_gen;

  this->opengl_use_tfp = entry->num_value;
}
#endif /* ENABLE_VA_GLX */

static void vaapi_scaling_level( void *this_gen, xine_cfg_entry_t *entry )
{
  vaapi_driver_t  *this  = (vaapi_driver_t *) this_gen;

  this->scaling_level = entry->num_value;
}

static void vaapi_swap_uv_planes(void *this_gen, xine_cfg_entry_t *entry) 
{
  vaapi_driver_t  *this  = (vaapi_driver_t *) this_gen;

  this->swap_uv_planes = entry->num_value;
}

static void vaapi_csc_mode(void *this_gen, xine_cfg_entry_t *entry) 
{
  vaapi_driver_t  *this  = (vaapi_driver_t *) this_gen;
  int new_mode = entry->num_value;

  /* skip unchanged */
  if (new_mode == this->csc_mode)
    return;

  vaapi_set_csc_mode (this, new_mode);
}

static int vaapi_init_x11(vaapi_driver_t *this)
{
  XSetWindowAttributes    xswa;
  unsigned long           xswa_mask;
  XWindowAttributes       wattr;
  unsigned long           black_pixel;
  XVisualInfo             visualInfo;
  XVisualInfo             *vi;
  int                     depth;
  int                     result = 0;
  const int               x11_event_mask = ExposureMask |
                                           StructureNotifyMask;

  LOCK_DISPLAY (this);

  black_pixel = BlackPixel(this->display, this->screen);

  XGetWindowAttributes(this->display, this->drawable, &wattr);

  depth = wattr.depth;
  if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
    depth = 24;

  vi = &visualInfo;
  XMatchVisualInfo(this->display, this->screen, depth, TrueColor, vi);

  xswa_mask             = CWBorderPixel | CWBackPixel | CWColormap;
  xswa.border_pixel     = black_pixel;
  xswa.background_pixel = black_pixel;
  xswa.colormap         = CopyFromParent;

  vaapi_x11_trap_errors();
  this->window = XCreateWindow(this->display, this->drawable,
                             0, 0, 1, 1, 0, depth,
                             InputOutput, vi->visual, xswa_mask, &xswa);
  XSync(this->display, False);
  if (vaapi_x11_untrap_errors() || this->window == None) {
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " XCreateWindow() failed\n");
    goto out;
  }

  XSelectInput(this->display, this->window, x11_event_mask);

  XMapWindow(this->display, this->window);
  vaapi_x11_wait_event(this->display, this->window, MapNotify);

  result = 1;

 out:
  UNLOCK_DISPLAY (this);

  if (vi != &visualInfo)
    XFree(vi);

  return result;
}

static int vaapi_initialize(vaapi_driver_t *this, int visual_type, const void *visual)
{
  VAStatus vaStatus;
  int fmt_count = 0;
  unsigned interop_flags = XINE_VA_DISPLAY_X11;

#ifdef ENABLE_VA_GLX
  if (this->opengl_render)
    interop_flags = XINE_VA_DISPLAY_GLX;
#endif

  this->va = _x_va_new(this->xine, visual_type, visual, interop_flags);
  if (!this->va)
    return 0;

  this->va_context = &this->va->c;
  this->va_context->driver = &this->vo_driver;

#ifdef ENABLE_VA_GLX
  {
    const char *p, *vendor;
    size_t i;

    vendor = vaQueryVendorString(this->va_context->va_display);
    xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Vendor : %s\n", vendor);

    for (p = vendor, i = strlen (vendor); i > 0; i--, p++) {
      if(strncmp(p, "VDPAU", strlen("VDPAU")) == 0) {
        xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Enable Splitted-Desktop Systems VDPAU-VIDEO workarounds.\n");
        this->opengl_use_tfp = 0;
        break;
      }
    }
  }
#endif

  vaapi_set_background_color(this);
  vaapi_display_attribs(this);

  fmt_count = vaMaxNumSubpictureFormats( this->va_context->va_display );
  this->va_subpic_formats = calloc( fmt_count, sizeof(*this->va_subpic_formats) );

  vaStatus = vaQuerySubpictureFormats( this->va_context->va_display, this->va_subpic_formats, 0, &this->va_num_subpic_formats );
  if(!vaapi_check_status(this, vaStatus, "vaQuerySubpictureFormats()"))
    return 0;

  if(vaapi_init_internal(this, SW_CONTEXT_INIT_FORMAT, SW_WIDTH, SW_HEIGHT) != VA_STATUS_SUCCESS)
    return 0;

  vaapi_close(this);

  return 1;
}

static vo_driver_t *vaapi_open_plugin (video_driver_class_t *class_gen, const void *visual_gen) {

  vaapi_class_t           *class  = (vaapi_class_t *) class_gen;
  const x11_visual_t      *visual = (const x11_visual_t *) visual_gen;
  vaapi_driver_t          *this;
  config_values_t         *config = class->xine->config;
  int                     i;

  this = (vaapi_driver_t *) calloc(1, sizeof(vaapi_driver_t));
  if (!this)
    return NULL;

  pthread_mutex_init(&this->vaapi_lock, NULL);
  pthread_mutex_lock(&this->vaapi_lock);

  this->xine                    = class->xine;

  this->display                 = visual->display;
  this->screen                  = visual->screen;
  this->drawable                = visual->d;

  /* number of video frames from config - register it with the default value. */
  int frame_num = config->register_num (config, "engine.buffers.video_num_frames", MIN_SURFACES, /* default */
       _("default number of video frames"),
       _("The default number of video frames to request "
         "from xine video out driver. Some drivers will "
         "override this setting with their own values."),
      20, NULL, NULL);

  /* now make sure we have at least 22 frames, to prevent
   * locks with vdpau_h264 */
  if (frame_num < MIN_SURFACES)
    config->update_num (config,"engine.buffers.video_num_frames", MIN_SURFACES);

#ifdef ENABLE_VA_GLX
  /* This is not really live switchable. */
  this->opengl_render = config->register_bool( config, "video.output.vaapi_opengl_render", 0,
        _("vaapi: opengl output rendering"),
        _("vaapi: opengl output rendering"),
        20, NULL, NULL);

  this->init_opengl_render = 1;

  this->opengl_use_tfp = config->register_bool( config, "video.output.vaapi_opengl_use_tfp", 0,
        _("vaapi: opengl rendering tfp"),
        _("vaapi: opengl rendering tfp"),
        20, vaapi_opengl_use_tfp, this );


  if(this->opengl_render) {
      LOCK_DISPLAY (this);
      this->opengl_render = vaapi_opengl_verify_direct (visual);
      UNLOCK_DISPLAY (this);
      if(!this->opengl_render)
        xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Opengl indirect/software rendering does not work. Fallback to plain VAAPI output !!!!\n");
  }

  this->gl_context                      = NULL;
  this->gl_pixmap                       = None;
  this->gl_image_pixmap                 = None;
  this->gl_texture                      = GL_NONE;
#endif /* ENABLE_VA_GLX */

  this->va_soft_surface_ids             = this->va_soft_surface_ids_storage;
  this->va_soft_images                  = this->va_soft_images_storage;
  for (i = 0; i < SOFT_SURFACES; i++) {
    this->va_soft_surface_ids[i]        = VA_INVALID_SURFACE;
    this->va_soft_images[i].image_id    = VA_INVALID_ID;
  }

  vaapi_init_subpicture(this);

  _x_vo_scale_init (&this->sc, 1, 0, config );

  this->sc.frame_output_cb      = visual->frame_output_cb;
  this->sc.dest_size_cb         = visual->dest_size_cb;
  this->sc.user_data            = visual->user_data;
  this->sc.user_ratio           = XINE_VO_ASPECT_AUTO;

  this->capabilities            = VO_CAP_YV12 | VO_CAP_YUY2 | VO_CAP_CROP | VO_CAP_UNSCALED_OVERLAY | VO_CAP_ARGB_LAYER_OVERLAY | VO_CAP_VAAPI | VO_CAP_CUSTOM_EXTENT_OVERLAY;

  this->vo_driver.get_capabilities     = vaapi_get_capabilities;
  this->vo_driver.alloc_frame          = vaapi_alloc_frame;
  this->vo_driver.update_frame_format  = _x_va_frame_update_frame_format;
  this->vo_driver.overlay_begin        = vaapi_overlay_begin;
  this->vo_driver.overlay_blend        = vaapi_overlay_blend;
  this->vo_driver.overlay_end          = vaapi_overlay_end;
  this->vo_driver.display_frame        = vaapi_display_frame;
  this->vo_driver.get_property         = vaapi_get_property;
  this->vo_driver.set_property         = vaapi_set_property;
  this->vo_driver.get_property_min_max = vaapi_get_property_min_max;
  this->vo_driver.gui_data_exchange    = vaapi_gui_data_exchange;
  this->vo_driver.dispose              = vaapi_dispose;
  this->vo_driver.redraw_needed        = vaapi_redraw_needed;

  this->deinterlace                    = 0;
  this->vdr_osd_width                  = 0;
  this->vdr_osd_height                 = 0;

  i = config->register_num( config, "video.output.vaapi_vdr_osd_width", 0,
        _("vaapi: VDR osd width workaround."),
        _("vaapi: VDR osd width workaround."),
        10, vaapi_vdr_osd_width_flag, this );
  this->vdr_osd_width = i < 0 ? 0 : i;

  i = config->register_num( config, "video.output.vaapi_vdr_osd_height", 0,
        _("vaapi: VDR osd height workaround."),
        _("vaapi: VDR osd height workaround."),
        10, vaapi_vdr_osd_height_flag, this );
  this->vdr_osd_height = i < 0 ? 0 : i;

  this->deinterlace = config->register_num( config, "video.output.vaapi_deinterlace", 0,
        _("vaapi: set deinterlace to 0 ( none ), 1 ( top field ), 2 ( bob )."),
        _("vaapi: set deinterlace to 0 ( none ), 1 ( top field ), 2 ( bob )."),
        10, vaapi_deinterlace_flag, this );

  this->guarded_render = config->register_num( config, "video.output.vaapi_guarded_render", 1,
        _("vaapi: set vaapi_guarded_render to 0 ( no ) 1 ( yes )"),
        _("vaapi: set vaapi_guarded_render to 0 ( no ) 1 ( yes )"),
        10, NULL, NULL );

  this->scaling_level_enum = config->register_enum(config, "video.output.vaapi_scaling_level", 0,
    (char **)scaling_level_enum_names,
        _("vaapi: set scaling level to : default (default) fast (fast) hq (HQ) nla (anamorphic)"),
        _("vaapi: set scaling level to : default (default) fast (fast) hq (HQ) nla (anamorphic)"),
    10, vaapi_scaling_level, this);

  this->scaling_level = scaling_level_enum_values[this->scaling_level_enum];

  this->swap_uv_planes = config->register_bool( config, "video.output.vaapi_swap_uv_planes", 0,
    _("vaapi: swap UV planes."),
    _("vaapi: this is a workaround for buggy drivers ( intel IronLake ).\n"
      "There the UV planes are swapped.\n"),
    10, vaapi_swap_uv_planes, this);


  for (i = 0; i < VO_NUM_PROPERTIES; i++) {
    this->props[i].value = 0;
    this->props[i].min   = 0;
    this->props[i].max   = 0;
    this->props[i].atom  = 0;
    this->props[i].entry = NULL;
    this->props[i].this  = this;
  }

  cm_init (this);

  this->sc.user_ratio                        =
    this->props[VO_PROP_ASPECT_RATIO].value  = XINE_VO_ASPECT_AUTO;
  this->props[VO_PROP_ZOOM_X].value          = 100;
  this->props[VO_PROP_ZOOM_Y].value          = 100;

  this->last_sub_image_fmt                   = 0;

  this->csc_mode = this->xine->config->register_enum (this->xine->config, "video.output.vaapi_csc_mode", 3,
    (char **)vaapi_csc_mode_labels,
    _("VAAPI colour conversion method"),
    _("How to handle colour conversion in VAAPI:\n\n"
      "user_matrix: The best way - if your driver supports it.\n"
      "simple:      Switch SD/HD colour spaces, and let decoders convert fullrange video.\n"
      "simple+2:    Switch SD/HD colour spaces, and emulate full-range colour by modifying\n"
      "             brightness/contrast settings.\n"
      "simple+3:    Like above, but adjust saturation as well.\n\n"
      "Hint: play \"test://rgb_levels.bmp\" while trying this.\n"),
    10,
    vaapi_csc_mode, this);
  vaapi_set_csc_mode (this, this->csc_mode);

  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Deinterlace : %d\n", this->deinterlace);
  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Render surfaces : %d\n", RENDER_SURFACES);
#ifdef ENABLE_VA_GLX
  xprintf(this->xine, XINE_VERBOSITY_LOG, LOG_MODULE " vaapi_open: Opengl render : %d\n", this->opengl_render);
#endif

  if (!vaapi_init_x11(this) ||
      !vaapi_initialize(this, XINE_VISUAL_TYPE_X11, visual_gen)) {

    vaapi_dispose_locked(this);
    return NULL;
  }

  pthread_mutex_unlock(&this->vaapi_lock);

  return &this->vo_driver;
}

/*
 * class functions
 */
static void *vaapi_init_class (xine_t *xine, const void *visual_gen) {
  vaapi_class_t *this;

  (void)visual_gen;

  this = calloc(1, sizeof(*this));
  if (!this)
    return NULL;

  this->driver_class.open_plugin     = vaapi_open_plugin;
  this->driver_class.identifier      = "vaapi";
  this->driver_class.description     = N_("xine video output plugin using VAAPI");
  this->driver_class.dispose         = default_video_driver_class_dispose;
  this->xine                         = xine;

  return this;
}

static const vo_info_t vo_info_vaapi = {
  .priority    = 9,
  .visual_type = XINE_VISUAL_TYPE_X11,
};

/*
 * exported plugin catalog entry
 */

const plugin_info_t xine_plugin_info[] EXPORTED = {
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_VIDEO_OUT, 22, "vaapi", XINE_VERSION_CODE, &vo_info_vaapi, vaapi_init_class },
  { PLUGIN_NONE, 0, NULL, 0, NULL, NULL }
};

