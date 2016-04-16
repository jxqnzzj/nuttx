/****************************************************************************
 * graphics/vnc/server/vnc_fbdev.c
 *
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/kthread.h>
#include <nuttx/video/fb.h>

#include "vnc_server.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure provides the frame buffer interface and also incapulates
 * information about the frame buffer instances for each display.
 */

struct vnc_fbinfo_s
{
  /* The publically visible frame buffer interface.  This must appear first
   * so that struct vnc_fbinfo_s is cast compatible with struct fb_vtable_s.
   */

  struct fb_vtable_s vtable;

  /* Our private per-display information */

  bool initialized;           /* True: This instance has been initialized */
  uint8_t display;            /* Display number */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Get information about the video controller configuration and the
 * configuration of each color plane.
 */

static int up_getvideoinfo(FAR struct fb_vtable_s *vtable, 
                           FAR struct fb_videoinfo_s *vinfo);
static int up_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                           FAR struct fb_planeinfo_s *pinfo);

/* The following are provided only if the video hardware supports RGB color
 * mapping.
 */

#ifdef CONFIG_FB_CMAP
static int up_getcmap(FAR struct fb_vtable_s *vtable,
                      FAR struct fb_cmap_s *cmap);
static int up_putcmap(FAR struct fb_vtable_s *vtable,
                      FAR const struct fb_cmap_s *cmap);
#endif

/* The following are provided only if the video hardware supports a hardware
 * cursor.
 */

#ifdef CONFIG_FB_HWCURSOR
static int up_getcursor(FAR struct fb_vtable_s *vtable,
                        FAR struct fb_cursorattrib_s *attrib);
static int up_setcursor(FAR struct fb_vtable_s *vtable,
                        FAR struct fb_setcursor_s *setttings);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Current cursor position */

#ifdef CONFIG_FB_HWCURSOR
static struct fb_cursorpos_s g_cpos;

/* Current cursor size */

#ifdef CONFIG_FB_HWCURSORSIZE
static struct fb_cursorsize_s g_csize;
#endif
#endif

/* The framebuffer object -- There is no private state information in this simple
 * framebuffer simulation.
 */

static struct vnc_fbinfo_s g_fbinfo[RFB_MAX_DISPLAYS];

/****************************************************************************
 * Public Data
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_getvideoinfo
 ****************************************************************************/

static int up_getvideoinfo(FAR struct fb_vtable_s *vtable,
                           FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct vnc_fbinfo_s *fbinfo = (FAR struct vnc_fbinfo_s *)vtable;
  FAR struct vnc_session_s *session;

  gvdbg("vtable=%p vinfo=%p\n", vtable, vinfo);

  DEBUGASSERT(fbinfo != NULL && vinfo != NULL);
  if (fbinfo != NULL && vinfo != NULL)
    {
      session = vnc_find_session(fbinfo->display);
      if (session == NULL || session->state != VNCSERVER_SCANNING)
        {
          gdbg("ERROR: session is not connected\n");
          return -ENOTCONN;
        }

      /* Return the requested video info */

      vinfo->fmt     = session->colorfmt;
      vinfo->xres    = session->screen.w;
      vinfo->yres    = session->screen.h;
      vinfo->nplanes = 1;

      return OK;
    }

  gdbg("ERROR: Invalid arguments\n");
  return -EINVAL;
}

/****************************************************************************
 * Name: up_getplaneinfo
 ****************************************************************************/

static int up_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                           FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct vnc_fbinfo_s *fbinfo = (FAR struct vnc_fbinfo_s *)vtable;
  FAR struct vnc_session_s *session;

  gvdbg("vtable=%p planeno=%d pinfo=%p\n", vtable, planeno, pinfo);

  DEBUGASSERT(fbinfo != NULL && pinfo != NULL && planeno == 0);
  if (fbinfo != NULL && pinfo != NULL && planeno == 0)
    {
      session = vnc_find_session(fbinfo->display);
      if (session == NULL || session->state != VNCSERVER_SCANNING)
        {
          gdbg("ERROR: session is not connected\n");
          return -ENOTCONN;
        }

      DEBUGASSERT(session->fb != NULL);

      pinfo->fbmem    = (FAR void *)&session->fb;
      pinfo->fblen    = (uint32_t)session->stride * CONFIG_VNCSERVER_SCREENWIDTH;
      pinfo->stride   = (fb_coord_t)session->stride;
      pinfo->bpp      = session->bpp;

      return OK;
    }

  gdbg("Returning EINVAL\n");
  return -EINVAL;
}

/****************************************************************************
 * Name: up_getcmap
 ****************************************************************************/

#ifdef CONFIG_FB_CMAP
static int up_getcmap(FAR struct fb_vtable_s *vtable,
                      FAR struct fb_cmap_s *cmap)
{
  FAR struct vnc_fbinfo_s *fbinfo = (FAR struct vnc_fbinfo_s *)vtable;
  FAR struct vnc_session_s *session;
  int i;

  gvdbg("vtable=%p cmap=%p\n", vtable, cmap);

  DEBUGASSERT(fbinfo != NULL && cmap != NULL);

  if (fbinfo != NULL && cmap != NULL)
    {
      session = vnc_find_session(fbinfo->display);
      if (session == NULL || session->state != VNCSERVER_SCANNING)
        {
          gdbg("ERROR: session is not connected\n");
          return -ENOTCONN;
        }

      gvdbg("first=%d len=%d\n", vcmap->first, cmap->len);
#warning Missing logic

      return OK;
    }

  gdbg("Returning EINVAL\n");
  return -EINVAL;
}
#endif

/****************************************************************************
 * Name: up_putcmap
 ****************************************************************************/

#ifdef CONFIG_FB_CMAP
static int up_putcmap(FAR struct fb_vtable_s *vtable, FAR const struct fb_cmap_s *cmap)
{
  FAR struct vnc_fbinfo_s *fbinfo = (FAR struct vnc_fbinfo_s *)vtable;
  FAR struct vnc_session_s *session;
  int i;

  gvdbg("vtable=%p cmap=%p\n", vtable, cmap);

  DEBUGASSERT(fbinfo != NULL && cmap != NULL);

  if (fbinfo != NULL && cmap != NULL)
    {
      session = vnc_find_session(fbinfo->display);
      if (session == NULL || session->state != VNCSERVER_SCANNING)
        {
          gdbg("ERROR: session is not connected\n");
          return -ENOTCONN;
        }

      gvdbg("first=%d len=%d\n", vcmap->first, cmap->len);
#warning Missing logic

      return OK;
    }

  gdbg("Returning EINVAL\n");
  return -EINVAL;
}
#endif

/****************************************************************************
 * Name: up_getcursor
 ****************************************************************************/

#ifdef CONFIG_FB_HWCURSOR
static int up_getcursor(FAR struct fb_vtable_s *vtable,
                        FAR struct fb_cursorattrib_s *attrib)
{
  FAR struct vnc_fbinfo_s *fbinfo = (FAR struct vnc_fbinfo_s *)vtable;
  FAR struct vnc_session_s *session;
  int i;

  gvdbg("vtable=%p attrib=%p\n", vtable, attrib);

  DEBUGASSERT(fbinfo != NULL && attrib != NULL);

  if (fbinfo != NULL && attrib != NULL)
    {
      session = vnc_find_session(fbinfo->display);
      if (session == NULL || session->state != VNCSERVER_SCANNING)
        {
          gdbg("ERROR: session is not connected\n");
          return -ENOTCONN;
        }

#warning Missing logic

      return OK;
    }
  gdbg("Returning EINVAL\n");
  return -EINVAL;
}
#endif

/****************************************************************************
 * Name:
 ****************************************************************************/

#ifdef CONFIG_FB_HWCURSOR
static int up_setcursor(FAR struct fb_vtable_s *vtable,
                       FAR struct fb_setcursor_s *settings)
{
  FAR struct vnc_fbinfo_s *fbinfo = (FAR struct vnc_fbinfo_s *)vtable;
  FAR struct vnc_session_s *session;
  int i;

  gvdbg("vtable=%p settings=%p\n", vtable, settings);

  DEBUGASSERT(fbinfo != NULL && settings != NULL);

  if (fbinfo != NULL && settings != NULL)
    {
      session = vnc_find_session(fbinfo->display);
      if (session == NULL || session->state != VNCSERVER_SCANNING)
        {
          gdbg("ERROR: session is not connected\n");
          return -ENOTCONN;
        }

      gvdbg("flags:   %02x\n", settings->flags);
      if ((settings->flags & FB_CUR_SETPOSITION) != 0)
        {
#warning Missing logic
        }

#ifdef CONFIG_FB_HWCURSORSIZE
      if ((settings->flags & FB_CUR_SETSIZE) != 0)
        {
#warning Missing logic
        }
#endif
#ifdef CONFIG_FB_HWCURSORIMAGE
      if ((settings->flags & FB_CUR_SETIMAGE) != 0)
        {
#warning Missing logic
        }
#endif
      return OK;
    }

  gdbg("Returning EINVAL\n");
  return -EINVAL;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_fbinitialize
 *
 * Description:
 *   Initialize the framebuffer video hardware associated with the display.
 *
 * Input parameters:
 *   display - In the case of hardware with multiple displays, this
 *     specifies the display.  Normally this is zero.
 *
 * Returned Value:
 *   Zero is returned on success; a negated errno value is returned on any
 *   failure.
 *
 ****************************************************************************/

int up_fbinitialize(int display)
{
  FAR char *argv[2];
  char str[8];
  pid_t pid;

  /* Start the VNC server kernel thread.
   * REVISIT: There is no protection for the case where this function is
   * called more that once.
   */

  gvdbg("Starting the VNC server for display %d\n", display);
  DEBUGASSERT(display >= 8 && display < RFB_MAX_DISPLAYS);

  (void)itoa(display, str, 10);
  argv[0] = str;
  argv[1] = NULL;

  pid = kernel_thread("vnc_server", CONFIG_VNCSERVER_PRIO,
                       CONFIG_VNCSERVER_STACKSIZE,
                       (main_t)vnc_server, argv);
  if (pid < 0)
    {
      gdbg("ERROR: Failed to start the VNC server: %d\n", (int)pid);
      return (int)pid;
    }

  /* Wait for the VNC client to connect and for the RFB to be ready */
#warning Missing logic

  return OK;
}

/****************************************************************************
 * Name: up_fbgetvplane
 *
 * Description:
 *   Return a a reference to the framebuffer object for the specified video
 *   plane of the specified plane.  Many OSDs support multiple planes of video.
 *
 * Input parameters:
 *   display - In the case of hardware with multiple displays, this
 *     specifies the display.  Normally this is zero.
 *   vplane - Identifies the plane being queried.
 *
 * Returned Value:
 *   A non-NULL pointer to the frame buffer access structure is returned on
 *   success; NULL is returned on any failure.
 *
 ****************************************************************************/

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  FAR struct vnc_session_s *session = vnc_find_session(display);
  FAR struct vnc_fbinfo_s *fbinfo;

  /* Verify that the session is still valid */

  if (session->state != VNCSERVER_SCANNING)
    {
      return NULL;
    }

  if (vplane == 0)
    {
      /* Has the framebuffer information been initialized for this display? */

      fbinfo = &g_fbinfo[display];
      if (!fbinfo->initialized)
        {
          fbinfo->vtable.getvideoinfo = up_getvideoinfo,
          fbinfo->vtable.getplaneinfo = up_getplaneinfo,
#ifdef CONFIG_FB_CMAP
          fbinfo->vtable.getcmap      = up_getcmap,
          fbinfo->vtable.putcmap      = up_putcmap,
#endif
#ifdef CONFIG_FB_HWCURSOR
          fbinfo->vtable.getcursor    = up_getcursor,
          fbinfo->vtable.setcursor    = up_setcursor,
#endif
          fbinfo->display             = display;
          fbinfo->initialized         = true;
        }

      return &fbinfo->vtable;
    }
  else
    {
      return NULL;
    }
}

/****************************************************************************
 * Name: up_fbuninitialize
 *
 * Description:
 *   Uninitialize the framebuffer support for the specified display.
 *
 * Input Parameters:
 *   display - In the case of hardware with multiple displays, this
 *     specifies the display.  Normally this is zero.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_fbuninitialize(int display)
{
  FAR struct vnc_session_s *session = vnc_find_session(display);
  FAR struct vnc_fbinfo_s *fbinfo;

  DEBUGASSERT(session != NULL);
  fbinfo = &g_fbinfo[display];
#warning Missing logic
  UNUSED(session);
  UNUSED(fbinfo);
}

