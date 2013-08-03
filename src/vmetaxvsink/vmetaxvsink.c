/* GStreamer
 * Copyright (C) <2005> Julien Moutte <julien@moutte.net>
 *               <2009>,<2010> Stefan Kost <stefan.kost@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-vmetaxvsink
 *
 * VmetaXvSink renders video frames to a drawable (XWindow) on a local display
 * using the XVideo extension. Rendering to a remote display is theoretically
 * possible but i doubt that the XVideo extension is actually available when
 * connecting to a remote display. This element can receive a Window ID from the
 * application through the #GstVideoOverlay interface and will then render
 * video frames in this drawable. If no Window ID was provided by the
 * application, the element will create its own internal window and render
 * into it.
 *
 * <refsect2>
 * <title>Scaling</title>
 * <para>
 * The XVideo extension, when it's available, handles hardware accelerated
 * scaling of video frames. This means that the element will just accept
 * incoming video frames no matter their geometry and will then put them to the
 * drawable scaling them on the fly. Using the #GstVmetaXvSink:force-aspect-ratio
 * property it is possible to enforce scaling with a constant aspect ratio,
 * which means drawing black borders around the video frame.
 * </para>
 * </refsect2>
 * <refsect2>
 * <title>Events</title>
 * <para>
 * VmetaXvSink creates a thread to handle events coming from the drawable. There
 * are several kind of events that can be grouped in 2 big categories: input
 * events and window state related events. Input events will be translated to
 * navigation events and pushed upstream for other elements to react on them.
 * This includes events such as pointer moves, key press/release, clicks etc...
 * Other events are used to handle the drawable appearance even when the data
 * is not flowing (GST_STATE_PAUSED). That means that even when the element is
 * paused, it will receive expose events from the drawable and draw the latest
 * frame with correct borders/aspect-ratio.
 * </para>
 * </refsect2>
 * <refsect2>
 * <title>Pixel aspect ratio</title>
 * <para>
 * When changing state to GST_STATE_READY, VmetaXvSink will open a connection to
 * the display specified in the #GstVmetaXvSink:display property or the
 * default display if nothing specified. Once this connection is open it will
 * inspect the display configuration including the physical display geometry and
 * then calculate the pixel aspect ratio. When receiving video frames with a
 * different pixel aspect ratio, VmetaXvSink will use hardware scaling to
 * display the video frames correctly on display's pixel aspect ratio.
 * Sometimes the calculated pixel aspect ratio can be wrong, it is
 * then possible to enforce a specific pixel aspect ratio using the
 * #GstVmetaXvSink:pixel-aspect-ratio property.
 * </para>
 * </refsect2>
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch -v videotestsrc ! vmetaxvsink
 * ]| A pipeline to test hardware scaling.
 * When the test video signal appears you can resize the window and see that
 * video frames are scaled through hardware (no extra CPU cost).
 * |[
 * gst-launch -v videotestsrc ! vmetaxvsink force-aspect-ratio=true
 * ]| Same pipeline with #GstVmetaXvSink:force-aspect-ratio property set to true
 * You can observe the borders drawn around the scaled image respecting aspect
 * ratio.
 * |[
 * gst-launch -v videotestsrc ! navigationtest ! vmetaxvsink
 * ]| A pipeline to test navigation events.
 * While moving the mouse pointer over the test signal you will see a black box
 * following the mouse pointer. If you press the mouse button somewhere on the
 * video and release it somewhere else a green box will appear where you pressed
 * the button and a red one where you released it. (The navigationtest element
 * is part of gst-plugins-good.) You can observe here that even if the images
 * are scaled through hardware the pointer coordinates are converted back to the
 * original video frame geometry so that the box can be drawn to the correct
 * position. This also handles borders correctly, limiting coordinates to the
 * image area
 * |[
 * gst-launch -v videotestsrc ! video/x-raw, pixel-aspect-ratio=(fraction)4/3 ! vmetaxvsink
 * ]| This is faking a 4/3 pixel aspect ratio caps on video frames produced by
 * videotestsrc, in most cases the pixel aspect ratio of the display will be
 * 1/1. This means that VmetaXvSink will have to do the scaling to convert
 * incoming frames to a size that will match the display pixel aspect ratio
 * (from 320x240 to 320x180 in this case). Note that you might have to escape
 * some characters for your shell like '\(fraction\)'.
 * |[
 * gst-launch -v videotestsrc ! vmetaxvsink hue=100 saturation=-100 brightness=100
 * ]| Demonstrates how to use the colorbalance interface.
 * </refsect2>
 */

/* for developers: there are two useful tools : xvinfo and xvattr */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Our interfaces */
#include <gst/video/navigation.h>
#include <gst/video/videooverlay.h>
#include <gst/video/colorbalance.h>
/* Helper functions */
#include <gst/video/gstvideometa.h>

/* Object header */
#include "vmetaxvsink.h"

/* Debugging category */
#include <gst/gstinfo.h>

/* for XkbKeycodeToKeysym */
#include <X11/XKBlib.h>

GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);
GST_DEBUG_CATEGORY_EXTERN (gst_debug_vmetaxvsink);
#define GST_CAT_DEFAULT gst_debug_vmetaxvsink

typedef struct
{
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
}
MotifWmHints, MwmHints;

#define MWM_HINTS_DECORATIONS   (1L << 1)

static void gst_vmetaxvsink_reset (GstVmetaXvSink * vmetaxvsink);
static void gst_vmetaxvsink_xwindow_update_geometry (GstVmetaXvSink *
    vmetaxvsink);
static void gst_vmetaxvsink_expose (GstVideoOverlay * overlay);

/* Default template - initiated with class struct to allow gst-register to work
   without X running */
static GstStaticPadTemplate gst_vmetaxvsink_sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, "
        "framerate = (fraction) [ 0, MAX ], "
        "width = (int) [ 1, MAX ], " "height = (int) [ 1, MAX ]")
    );

enum
{
  PROP_0,
  PROP_CONTRAST,
  PROP_BRIGHTNESS,
  PROP_HUE,
  PROP_SATURATION,
  PROP_DISPLAY,
  PROP_SYNCHRONOUS,
  PROP_PIXEL_ASPECT_RATIO,
  PROP_FORCE_ASPECT_RATIO,
  PROP_HANDLE_EVENTS,
  PROP_DEVICE,
  PROP_DEVICE_NAME,
  PROP_HANDLE_EXPOSE,
  PROP_DOUBLE_BUFFER,
  PROP_AUTOPAINT_COLORKEY,
  PROP_COLORKEY,
  PROP_DRAW_BORDERS,
  PROP_WINDOW_WIDTH,
  PROP_WINDOW_HEIGHT
};

/* ============================================================= */
/*                                                               */
/*                       Public Methods                          */
/*                                                               */
/* ============================================================= */

/* =========================================== */
/*                                             */
/*          Object typing & Creation           */
/*                                             */
/* =========================================== */
static void gst_vmetaxvsink_navigation_init (GstNavigationInterface * iface);
static void gst_vmetaxvsink_video_overlay_init (GstVideoOverlayInterface *
    iface);
static void gst_vmetaxvsink_colorbalance_init (GstColorBalanceInterface *
    iface);
#define gst_vmetaxvsink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVmetaXvSink, gst_vmetaxvsink, GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_NAVIGATION,
        gst_vmetaxvsink_navigation_init);
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_vmetaxvsink_video_overlay_init);
    G_IMPLEMENT_INTERFACE (GST_TYPE_COLOR_BALANCE,
        gst_vmetaxvsink_colorbalance_init))


/* ============================================================= */
/*                                                               */
/*                       Private Methods                         */
/*                                                               */
/* ============================================================= */


/* We are called with the x_lock taken */
static void
gst_vmetaxvsink_xwindow_draw_borders (GstVmetaXvSink * vmetaxvsink,
    GstXWindow * xwindow, GstVideoRectangle rect)
{
  gint t1, t2;

  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));
  g_return_if_fail (xwindow != NULL);

  XSetForeground (vmetaxvsink->xcontext->disp, xwindow->gc,
      vmetaxvsink->xcontext->black);

  /* Left border */
  if (rect.x > vmetaxvsink->render_rect.x) {
    XFillRectangle (vmetaxvsink->xcontext->disp, xwindow->win, xwindow->gc,
        vmetaxvsink->render_rect.x, vmetaxvsink->render_rect.y,
        rect.x - vmetaxvsink->render_rect.x, vmetaxvsink->render_rect.h);
  }

  /* Right border */
  t1 = rect.x + rect.w;
  t2 = vmetaxvsink->render_rect.x + vmetaxvsink->render_rect.w;
  if (t1 < t2) {
    XFillRectangle (vmetaxvsink->xcontext->disp, xwindow->win, xwindow->gc,
        t1, vmetaxvsink->render_rect.y, t2 - t1, vmetaxvsink->render_rect.h);
  }

  /* Top border */
  if (rect.y > vmetaxvsink->render_rect.y) {
    XFillRectangle (vmetaxvsink->xcontext->disp, xwindow->win, xwindow->gc,
        vmetaxvsink->render_rect.x, vmetaxvsink->render_rect.y,
        vmetaxvsink->render_rect.w, rect.y - vmetaxvsink->render_rect.y);
  }

  /* Bottom border */
  t1 = rect.y + rect.h;
  t2 = vmetaxvsink->render_rect.y + vmetaxvsink->render_rect.h;
  if (t1 < t2) {
    XFillRectangle (vmetaxvsink->xcontext->disp, xwindow->win, xwindow->gc,
        vmetaxvsink->render_rect.x, t1, vmetaxvsink->render_rect.w, t2 - t1);
  }
}

/* This function puts a GstVmetaXv on a GstVmetaXvSink's window. Returns FALSE
 * if no window was available  */
static gboolean
gst_vmetaxvsink_xvimage_put (GstVmetaXvSink * vmetaxvsink, GstBuffer * xvimage)
{
  GstVmetaXvMeta *meta;
  GstVideoCropMeta *crop;
  GstVideoRectangle result;
  gboolean draw_border = FALSE;
  GstVideoRectangle src, dst;

  /* We take the flow_lock. If expose is in there we don't want to run
     concurrently from the data flow thread */
  g_mutex_lock (&vmetaxvsink->flow_lock);

  if (G_UNLIKELY (vmetaxvsink->xwindow == NULL)) {
    g_mutex_unlock (&vmetaxvsink->flow_lock);
    return FALSE;
  }

  /* Draw borders when displaying the first frame. After this
     draw borders only on expose event or after a size change. */
  if (!vmetaxvsink->cur_image || vmetaxvsink->redraw_border) {
    draw_border = TRUE;
  }

  /* Store a reference to the last image we put, lose the previous one */
  if (xvimage && vmetaxvsink->cur_image != xvimage) {
    if (vmetaxvsink->cur_image) {
      GST_LOG_OBJECT (vmetaxvsink, "unreffing %p", vmetaxvsink->cur_image);
      gst_buffer_unref (vmetaxvsink->cur_image);
    }
    GST_LOG_OBJECT (vmetaxvsink, "reffing %p as our current image", xvimage);
    vmetaxvsink->cur_image = gst_buffer_ref (xvimage);
  }

  /* Expose sends a NULL image, we take the latest frame */
  if (!xvimage) {
    if (vmetaxvsink->cur_image) {
      draw_border = TRUE;
      xvimage = vmetaxvsink->cur_image;
    } else {
      g_mutex_unlock (&vmetaxvsink->flow_lock);
      return TRUE;
    }
  }

  meta = gst_buffer_get_vmetaxv_meta (xvimage);

  crop = gst_buffer_get_video_crop_meta (xvimage);

  if (crop) {
    src.x = crop->x + meta->x;
    src.y = crop->y + meta->y;
    src.w = crop->width;
    src.h = crop->height;
    GST_LOG_OBJECT (vmetaxvsink,
        "crop %dx%d-%dx%d", crop->x, crop->y, crop->width, crop->height);
  } else {
    src.x = meta->x;
    src.y = meta->y;
    src.w = meta->width;
    src.h = meta->height;
  }

  if (vmetaxvsink->keep_aspect) {
    GstVideoRectangle s;

    /* We take the size of the source material as it was negotiated and
     * corrected for DAR. This size can be different from the cropped size in
     * which case the image will be scaled to fit the negotiated size. */
    s.w = GST_VIDEO_SINK_WIDTH (vmetaxvsink);
    s.h = GST_VIDEO_SINK_HEIGHT (vmetaxvsink);
    dst.w = vmetaxvsink->render_rect.w;
    dst.h = vmetaxvsink->render_rect.h;

    gst_video_sink_center_rect (s, dst, &result, TRUE);
    result.x += vmetaxvsink->render_rect.x;
    result.y += vmetaxvsink->render_rect.y;
  } else {
    memcpy (&result, &vmetaxvsink->render_rect, sizeof (GstVideoRectangle));
  }

  g_mutex_lock (&vmetaxvsink->x_lock);

  if (draw_border && vmetaxvsink->draw_borders) {
    gst_vmetaxvsink_xwindow_draw_borders (vmetaxvsink, vmetaxvsink->xwindow,
        result);
    vmetaxvsink->redraw_border = FALSE;
  }
#ifdef HAVE_XSHM
  if (vmetaxvsink->xcontext->use_xshm) {
    GST_LOG_OBJECT (vmetaxvsink,
        "XvShmPutImage with image %dx%d and window %dx%d, from xvimage %"
        GST_PTR_FORMAT, meta->width, meta->height,
        vmetaxvsink->render_rect.w, vmetaxvsink->render_rect.h, xvimage);

    XvShmPutImage (vmetaxvsink->xcontext->disp,
        vmetaxvsink->xcontext->xv_port_id,
        vmetaxvsink->xwindow->win,
        vmetaxvsink->xwindow->gc, meta->xvimage,
        src.x, src.y, src.w, src.h,
        result.x, result.y, result.w, result.h, FALSE);
  } else
#endif /* HAVE_XSHM */
  {
    XvPutImage (vmetaxvsink->xcontext->disp,
        vmetaxvsink->xcontext->xv_port_id,
        vmetaxvsink->xwindow->win,
        vmetaxvsink->xwindow->gc, meta->xvimage,
        src.x, src.y, src.w, src.h, result.x, result.y, result.w, result.h);
  }

  XSync (vmetaxvsink->xcontext->disp, FALSE);

  g_mutex_unlock (&vmetaxvsink->x_lock);

  g_mutex_unlock (&vmetaxvsink->flow_lock);

  return TRUE;
}

static gboolean
gst_vmetaxvsink_xwindow_decorate (GstVmetaXvSink * vmetaxvsink,
    GstXWindow * window)
{
  Atom hints_atom = None;
  MotifWmHints *hints;

  g_return_val_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink), FALSE);
  g_return_val_if_fail (window != NULL, FALSE);

  g_mutex_lock (&vmetaxvsink->x_lock);

  hints_atom = XInternAtom (vmetaxvsink->xcontext->disp, "_MOTIF_WM_HINTS",
      True);
  if (hints_atom == None) {
    g_mutex_unlock (&vmetaxvsink->x_lock);
    return FALSE;
  }

  hints = g_malloc0 (sizeof (MotifWmHints));

  hints->flags |= MWM_HINTS_DECORATIONS;
  hints->decorations = 1 << 0;

  XChangeProperty (vmetaxvsink->xcontext->disp, window->win,
      hints_atom, hints_atom, 32, PropModeReplace,
      (guchar *) hints, sizeof (MotifWmHints) / sizeof (long));

  XSync (vmetaxvsink->xcontext->disp, FALSE);

  g_mutex_unlock (&vmetaxvsink->x_lock);

  g_free (hints);

  return TRUE;
}

static void
gst_vmetaxvsink_xwindow_set_title (GstVmetaXvSink * vmetaxvsink,
    GstXWindow * xwindow, const gchar * media_title)
{
  if (media_title) {
    g_free (vmetaxvsink->media_title);
    vmetaxvsink->media_title = g_strdup (media_title);
  }
  if (xwindow) {
    /* we have a window */
    if (xwindow->internal) {
      XTextProperty xproperty;
      const gchar *app_name;
      const gchar *title = NULL;
      gchar *title_mem = NULL;

      /* set application name as a title */
      app_name = g_get_application_name ();

      if (app_name && vmetaxvsink->media_title) {
        title = title_mem = g_strconcat (vmetaxvsink->media_title, " : ",
            app_name, NULL);
      } else if (app_name) {
        title = app_name;
      } else if (vmetaxvsink->media_title) {
        title = vmetaxvsink->media_title;
      }

      if (title) {
        if ((XStringListToTextProperty (((char **) &title), 1,
                    &xproperty)) != 0) {
          XSetWMName (vmetaxvsink->xcontext->disp, xwindow->win, &xproperty);
          XFree (xproperty.value);
        }

        g_free (title_mem);
      }
    }
  }
}

/* This function handles a GstXWindow creation
 * The width and height are the actual pixel size on the display */
static GstXWindow *
gst_vmetaxvsink_xwindow_new (GstVmetaXvSink * vmetaxvsink,
    gint width, gint height)
{
  GstXWindow *xwindow = NULL;
  XGCValues values;

  g_return_val_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink), NULL);

  xwindow = g_new0 (GstXWindow, 1);

  vmetaxvsink->render_rect.x = vmetaxvsink->render_rect.y = 0;
  vmetaxvsink->render_rect.w = width;
  vmetaxvsink->render_rect.h = height;

  xwindow->width = width;
  xwindow->height = height;
  xwindow->internal = TRUE;

  g_mutex_lock (&vmetaxvsink->x_lock);

  xwindow->win = XCreateSimpleWindow (vmetaxvsink->xcontext->disp,
      vmetaxvsink->xcontext->root,
      0, 0, width, height, 0, 0, vmetaxvsink->xcontext->black);

  /* We have to do that to prevent X from redrawing the background on
   * ConfigureNotify. This takes away flickering of video when resizing. */
  XSetWindowBackgroundPixmap (vmetaxvsink->xcontext->disp, xwindow->win, None);

  /* set application name as a title */
  gst_vmetaxvsink_xwindow_set_title (vmetaxvsink, xwindow, NULL);

  if (vmetaxvsink->handle_events) {
    Atom wm_delete;

    XSelectInput (vmetaxvsink->xcontext->disp, xwindow->win, ExposureMask |
        StructureNotifyMask | PointerMotionMask | KeyPressMask |
        KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);

    /* Tell the window manager we'd like delete client messages instead of
     * being killed */
    wm_delete = XInternAtom (vmetaxvsink->xcontext->disp,
        "WM_DELETE_WINDOW", True);
    if (wm_delete != None) {
      (void) XSetWMProtocols (vmetaxvsink->xcontext->disp, xwindow->win,
          &wm_delete, 1);
    }
  }

  xwindow->gc = XCreateGC (vmetaxvsink->xcontext->disp,
      xwindow->win, 0, &values);

  XMapRaised (vmetaxvsink->xcontext->disp, xwindow->win);

  XSync (vmetaxvsink->xcontext->disp, FALSE);

  g_mutex_unlock (&vmetaxvsink->x_lock);

  gst_vmetaxvsink_xwindow_decorate (vmetaxvsink, xwindow);

  gst_video_overlay_got_window_handle (GST_VIDEO_OVERLAY (vmetaxvsink),
      xwindow->win);

  return xwindow;
}

/* This function destroys a GstXWindow */
static void
gst_vmetaxvsink_xwindow_destroy (GstVmetaXvSink * vmetaxvsink,
    GstXWindow * xwindow)
{
  g_return_if_fail (xwindow != NULL);
  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));

  g_mutex_lock (&vmetaxvsink->x_lock);

  /* If we did not create that window we just free the GC and let it live */
  if (xwindow->internal)
    XDestroyWindow (vmetaxvsink->xcontext->disp, xwindow->win);
  else
    XSelectInput (vmetaxvsink->xcontext->disp, xwindow->win, 0);

  XFreeGC (vmetaxvsink->xcontext->disp, xwindow->gc);

  XSync (vmetaxvsink->xcontext->disp, FALSE);

  g_mutex_unlock (&vmetaxvsink->x_lock);

  g_free (xwindow);
}

static void
gst_vmetaxvsink_xwindow_update_geometry (GstVmetaXvSink * vmetaxvsink)
{
  XWindowAttributes attr;

  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));

  /* Update the window geometry */
  g_mutex_lock (&vmetaxvsink->x_lock);
  if (G_UNLIKELY (vmetaxvsink->xwindow == NULL)) {
    g_mutex_unlock (&vmetaxvsink->x_lock);
    return;
  }

  XGetWindowAttributes (vmetaxvsink->xcontext->disp,
      vmetaxvsink->xwindow->win, &attr);

  vmetaxvsink->xwindow->width = attr.width;
  vmetaxvsink->xwindow->height = attr.height;

  if (!vmetaxvsink->have_render_rect) {
    vmetaxvsink->render_rect.x = vmetaxvsink->render_rect.y = 0;
    vmetaxvsink->render_rect.w = attr.width;
    vmetaxvsink->render_rect.h = attr.height;
  }

  g_mutex_unlock (&vmetaxvsink->x_lock);
}

static void
gst_vmetaxvsink_xwindow_clear (GstVmetaXvSink * vmetaxvsink,
    GstXWindow * xwindow)
{
  g_return_if_fail (xwindow != NULL);
  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));

  g_mutex_lock (&vmetaxvsink->x_lock);

  XvStopVideo (vmetaxvsink->xcontext->disp, vmetaxvsink->xcontext->xv_port_id,
      xwindow->win);

  XSync (vmetaxvsink->xcontext->disp, FALSE);

  g_mutex_unlock (&vmetaxvsink->x_lock);
}

/* This function commits our internal colorbalance settings to our grabbed Xv
   port. If the xcontext is not initialized yet it simply returns */
static void
gst_vmetaxvsink_update_colorbalance (GstVmetaXvSink * vmetaxvsink)
{
  GList *channels = NULL;

  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));

  /* If we haven't initialized the X context we can't update anything */
  if (vmetaxvsink->xcontext == NULL)
    return;

  /* Don't set the attributes if they haven't been changed, to avoid
   * rounding errors changing the values */
  if (!vmetaxvsink->cb_changed)
    return;

  /* For each channel of the colorbalance we calculate the correct value
     doing range conversion and then set the Xv port attribute to match our
     values. */
  channels = vmetaxvsink->xcontext->channels_list;

  while (channels) {
    if (channels->data && GST_IS_COLOR_BALANCE_CHANNEL (channels->data)) {
      GstColorBalanceChannel *channel = NULL;
      Atom prop_atom;
      gint value = 0;
      gdouble convert_coef;

      channel = GST_COLOR_BALANCE_CHANNEL (channels->data);
      g_object_ref (channel);

      /* Our range conversion coef */
      convert_coef = (channel->max_value - channel->min_value) / 2000.0;

      if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
        value = vmetaxvsink->hue;
      } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
        value = vmetaxvsink->saturation;
      } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
        value = vmetaxvsink->contrast;
      } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
        value = vmetaxvsink->brightness;
      } else {
        g_warning ("got an unknown channel %s", channel->label);
        g_object_unref (channel);
        return;
      }

      /* Committing to Xv port */
      g_mutex_lock (&vmetaxvsink->x_lock);
      prop_atom =
          XInternAtom (vmetaxvsink->xcontext->disp, channel->label, True);
      if (prop_atom != None) {
        int xv_value;
        xv_value =
            floor (0.5 + (value + 1000) * convert_coef + channel->min_value);
        XvSetPortAttribute (vmetaxvsink->xcontext->disp,
            vmetaxvsink->xcontext->xv_port_id, prop_atom, xv_value);
      }
      g_mutex_unlock (&vmetaxvsink->x_lock);

      g_object_unref (channel);
    }
    channels = g_list_next (channels);
  }
}

/* This function handles XEvents that might be in the queue. It generates
   GstEvent that will be sent upstream in the pipeline to handle interactivity
   and navigation. It will also listen for configure events on the window to
   trigger caps renegotiation so on the fly software scaling can work. */
static void
gst_vmetaxvsink_handle_xevents (GstVmetaXvSink * vmetaxvsink)
{
  XEvent e;
  guint pointer_x = 0, pointer_y = 0;
  gboolean pointer_moved = FALSE;
  gboolean exposed = FALSE, configured = FALSE;

  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));

  /* Handle Interaction, produces navigation events */

  /* We get all pointer motion events, only the last position is
     interesting. */
  g_mutex_lock (&vmetaxvsink->flow_lock);
  g_mutex_lock (&vmetaxvsink->x_lock);
  while (XCheckWindowEvent (vmetaxvsink->xcontext->disp,
          vmetaxvsink->xwindow->win, PointerMotionMask, &e)) {
    g_mutex_unlock (&vmetaxvsink->x_lock);
    g_mutex_unlock (&vmetaxvsink->flow_lock);

    switch (e.type) {
      case MotionNotify:
        pointer_x = e.xmotion.x;
        pointer_y = e.xmotion.y;
        pointer_moved = TRUE;
        break;
      default:
        break;
    }
    g_mutex_lock (&vmetaxvsink->flow_lock);
    g_mutex_lock (&vmetaxvsink->x_lock);
  }

  if (pointer_moved) {
    g_mutex_unlock (&vmetaxvsink->x_lock);
    g_mutex_unlock (&vmetaxvsink->flow_lock);

    GST_DEBUG ("vmetaxvsink pointer moved over window at %d,%d",
        pointer_x, pointer_y);
    gst_navigation_send_mouse_event (GST_NAVIGATION (vmetaxvsink),
        "mouse-move", 0, e.xbutton.x, e.xbutton.y);

    g_mutex_lock (&vmetaxvsink->flow_lock);
    g_mutex_lock (&vmetaxvsink->x_lock);
  }

  /* We get all events on our window to throw them upstream */
  while (XCheckWindowEvent (vmetaxvsink->xcontext->disp,
          vmetaxvsink->xwindow->win,
          KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask,
          &e)) {
    KeySym keysym;
    const char *key_str = NULL;

    /* We lock only for the X function call */
    g_mutex_unlock (&vmetaxvsink->x_lock);
    g_mutex_unlock (&vmetaxvsink->flow_lock);

    switch (e.type) {
      case ButtonPress:
        /* Mouse button pressed over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("vmetaxvsink button %d pressed over window at %d,%d",
            e.xbutton.button, e.xbutton.x, e.xbutton.y);
        gst_navigation_send_mouse_event (GST_NAVIGATION (vmetaxvsink),
            "mouse-button-press", e.xbutton.button, e.xbutton.x, e.xbutton.y);
        break;
      case ButtonRelease:
        /* Mouse button released over our window. We send upstream
           events for interactivity/navigation */
        GST_DEBUG ("vmetaxvsink button %d released over window at %d,%d",
            e.xbutton.button, e.xbutton.x, e.xbutton.y);
        gst_navigation_send_mouse_event (GST_NAVIGATION (vmetaxvsink),
            "mouse-button-release", e.xbutton.button, e.xbutton.x, e.xbutton.y);
        break;
      case KeyPress:
      case KeyRelease:
        /* Key pressed/released over our window. We send upstream
           events for interactivity/navigation */
        g_mutex_lock (&vmetaxvsink->x_lock);
        keysym = XkbKeycodeToKeysym (vmetaxvsink->xcontext->disp,
            e.xkey.keycode, 0, 0);
        if (keysym != NoSymbol) {
          key_str = XKeysymToString (keysym);
        } else {
          key_str = "unknown";
        }
        g_mutex_unlock (&vmetaxvsink->x_lock);
        GST_DEBUG_OBJECT (vmetaxvsink,
            "key %d pressed over window at %d,%d (%s)",
            e.xkey.keycode, e.xkey.x, e.xkey.y, key_str);
        gst_navigation_send_key_event (GST_NAVIGATION (vmetaxvsink),
            e.type == KeyPress ? "key-press" : "key-release", key_str);
        break;
      default:
        GST_DEBUG_OBJECT (vmetaxvsink, "vmetaxvsink unhandled X event (%d)",
            e.type);
    }
    g_mutex_lock (&vmetaxvsink->flow_lock);
    g_mutex_lock (&vmetaxvsink->x_lock);
  }

  /* Handle Expose */
  while (XCheckWindowEvent (vmetaxvsink->xcontext->disp,
          vmetaxvsink->xwindow->win, ExposureMask | StructureNotifyMask, &e)) {
    switch (e.type) {
      case Expose:
        exposed = TRUE;
        break;
      case ConfigureNotify:
        g_mutex_unlock (&vmetaxvsink->x_lock);
        gst_vmetaxvsink_xwindow_update_geometry (vmetaxvsink);
        g_mutex_lock (&vmetaxvsink->x_lock);
        configured = TRUE;
        break;
      default:
        break;
    }
  }

  if (vmetaxvsink->handle_expose && (exposed || configured)) {
    g_mutex_unlock (&vmetaxvsink->x_lock);
    g_mutex_unlock (&vmetaxvsink->flow_lock);

    gst_vmetaxvsink_expose (GST_VIDEO_OVERLAY (vmetaxvsink));

    g_mutex_lock (&vmetaxvsink->flow_lock);
    g_mutex_lock (&vmetaxvsink->x_lock);
  }

  /* Handle Display events */
  while (XPending (vmetaxvsink->xcontext->disp)) {
    XNextEvent (vmetaxvsink->xcontext->disp, &e);

    switch (e.type) {
      case ClientMessage:{
        Atom wm_delete;

        wm_delete = XInternAtom (vmetaxvsink->xcontext->disp,
            "WM_DELETE_WINDOW", True);
        if (wm_delete != None && wm_delete == (Atom) e.xclient.data.l[0]) {
          /* Handle window deletion by posting an error on the bus */
          GST_ELEMENT_ERROR (vmetaxvsink, RESOURCE, NOT_FOUND,
              ("Output window was closed"), (NULL));

          g_mutex_unlock (&vmetaxvsink->x_lock);
          gst_vmetaxvsink_xwindow_destroy (vmetaxvsink, vmetaxvsink->xwindow);
          vmetaxvsink->xwindow = NULL;
          g_mutex_lock (&vmetaxvsink->x_lock);
        }
        break;
      }
      default:
        break;
    }
  }

  g_mutex_unlock (&vmetaxvsink->x_lock);
  g_mutex_unlock (&vmetaxvsink->flow_lock);
}

static void
gst_lookup_xv_port_from_adaptor (GstXContext * xcontext,
    XvAdaptorInfo * adaptors, int adaptor_no)
{
  gint j;
  gint res;

  /* Do we support XvImageMask ? */
  if (!(adaptors[adaptor_no].type & XvImageMask)) {
    GST_DEBUG ("XV Adaptor %s has no support for XvImageMask",
        adaptors[adaptor_no].name);
    return;
  }

  /* We found such an adaptor, looking for an available port */
  for (j = 0; j < adaptors[adaptor_no].num_ports && !xcontext->xv_port_id; j++) {
    /* We try to grab the port */
    res = XvGrabPort (xcontext->disp, adaptors[adaptor_no].base_id + j, 0);
    if (Success == res) {
      xcontext->xv_port_id = adaptors[adaptor_no].base_id + j;
      GST_DEBUG ("XV Adaptor %s with %ld ports", adaptors[adaptor_no].name,
          adaptors[adaptor_no].num_ports);
    } else {
      GST_DEBUG ("GrabPort %d for XV Adaptor %s failed: %d", j,
          adaptors[adaptor_no].name, res);
    }
  }
}

/* This function generates a caps with all supported format by the first
   Xv grabable port we find. We store each one of the supported formats in a
   format list and append the format to a newly created caps that we return
   If this function does not return NULL because of an error, it also grabs
   the port via XvGrabPort */
static GstCaps *
gst_vmetaxvsink_get_xv_support (GstVmetaXvSink * vmetaxvsink,
    GstXContext * xcontext)
{
  gint i;
  XvAdaptorInfo *adaptors;
  gint nb_formats;
  XvImageFormatValues *formats = NULL;
  guint nb_encodings;
  XvEncodingInfo *encodings = NULL;
  gulong max_w = G_MAXINT, max_h = G_MAXINT;
  GstCaps *caps = NULL;
  GstCaps *rgb_caps = NULL;

  g_return_val_if_fail (xcontext != NULL, NULL);

  /* First let's check that XVideo extension is available */
  if (!XQueryExtension (xcontext->disp, "XVideo", &i, &i, &i)) {
    GST_ELEMENT_ERROR (vmetaxvsink, RESOURCE, SETTINGS,
        ("Could not initialise Xv output"),
        ("XVideo extension is not available"));
    return NULL;
  }

  /* Then we get adaptors list */
  if (Success != XvQueryAdaptors (xcontext->disp, xcontext->root,
          &xcontext->nb_adaptors, &adaptors)) {
    GST_ELEMENT_ERROR (vmetaxvsink, RESOURCE, SETTINGS,
        ("Could not initialise Xv output"),
        ("Failed getting XV adaptors list"));
    return NULL;
  }

  xcontext->xv_port_id = 0;

  GST_DEBUG ("Found %u XV adaptor(s)", xcontext->nb_adaptors);

  xcontext->adaptors =
      (gchar **) g_malloc0 (xcontext->nb_adaptors * sizeof (gchar *));

  /* Now fill up our adaptor name array */
  for (i = 0; i < xcontext->nb_adaptors; i++) {
    xcontext->adaptors[i] = g_strdup (adaptors[i].name);
  }

  if (vmetaxvsink->adaptor_no != -1 &&
      vmetaxvsink->adaptor_no < xcontext->nb_adaptors) {
    /* Find xv port from user defined adaptor */
    gst_lookup_xv_port_from_adaptor (xcontext, adaptors,
        vmetaxvsink->adaptor_no);
  }

  if (!xcontext->xv_port_id) {
    /* Now search for an adaptor that supports XvImageMask */
    for (i = 0; i < xcontext->nb_adaptors && !xcontext->xv_port_id; i++) {
      gst_lookup_xv_port_from_adaptor (xcontext, adaptors, i);
      vmetaxvsink->adaptor_no = i;
    }
  }

  XvFreeAdaptorInfo (adaptors);

  if (!xcontext->xv_port_id) {
    vmetaxvsink->adaptor_no = -1;
    GST_ELEMENT_ERROR (vmetaxvsink, RESOURCE, BUSY,
        ("Could not initialise Xv output"), ("No port available"));
    return NULL;
  }

  /* Set XV_AUTOPAINT_COLORKEY and XV_DOUBLE_BUFFER and XV_COLORKEY */
  {
    int count, todo = 3;
    XvAttribute *const attr = XvQueryPortAttributes (xcontext->disp,
        xcontext->xv_port_id, &count);
    static const char autopaint[] = "XV_AUTOPAINT_COLORKEY";
    static const char dbl_buffer[] = "XV_DOUBLE_BUFFER";
    static const char colorkey[] = "XV_COLORKEY";

    GST_DEBUG_OBJECT (vmetaxvsink, "Checking %d Xv port attributes", count);

    vmetaxvsink->have_autopaint_colorkey = FALSE;
    vmetaxvsink->have_double_buffer = FALSE;
    vmetaxvsink->have_colorkey = FALSE;

    for (i = 0; ((i < count) && todo); i++)
      if (!strcmp (attr[i].name, autopaint)) {
        const Atom atom = XInternAtom (xcontext->disp, autopaint, False);

        /* turn on autopaint colorkey */
        XvSetPortAttribute (xcontext->disp, xcontext->xv_port_id, atom,
            (vmetaxvsink->autopaint_colorkey ? 1 : 0));
        todo--;
        vmetaxvsink->have_autopaint_colorkey = TRUE;
      } else if (!strcmp (attr[i].name, dbl_buffer)) {
        const Atom atom = XInternAtom (xcontext->disp, dbl_buffer, False);

        XvSetPortAttribute (xcontext->disp, xcontext->xv_port_id, atom,
            (vmetaxvsink->double_buffer ? 1 : 0));
        todo--;
        vmetaxvsink->have_double_buffer = TRUE;
      } else if (!strcmp (attr[i].name, colorkey)) {
        /* Set the colorkey, default is something that is dark but hopefully
         * won't randomly appear on the screen elsewhere (ie not black or greys)
         * can be overridden by setting "colorkey" property
         */
        const Atom atom = XInternAtom (xcontext->disp, colorkey, False);
        guint32 ckey = 0;
        gboolean set_attr = TRUE;
        guint cr, cg, cb;

        /* set a colorkey in the right format RGB565/RGB888
         * We only handle these 2 cases, because they're the only types of
         * devices we've encountered. If we don't recognise it, leave it alone
         */
        cr = (vmetaxvsink->colorkey >> 16);
        cg = (vmetaxvsink->colorkey >> 8) & 0xFF;
        cb = (vmetaxvsink->colorkey) & 0xFF;
        switch (xcontext->depth) {
          case 16:             /* RGB 565 */
            cr >>= 3;
            cg >>= 2;
            cb >>= 3;
            ckey = (cr << 11) | (cg << 5) | cb;
            break;
          case 24:
          case 32:             /* RGB 888 / ARGB 8888 */
            ckey = (cr << 16) | (cg << 8) | cb;
            break;
          default:
            GST_DEBUG_OBJECT (vmetaxvsink,
                "Unknown bit depth %d for Xv Colorkey - not adjusting",
                xcontext->depth);
            set_attr = FALSE;
            break;
        }

        if (set_attr) {
          ckey = CLAMP (ckey, (guint32) attr[i].min_value,
              (guint32) attr[i].max_value);
          GST_LOG_OBJECT (vmetaxvsink,
              "Setting color key for display depth %d to 0x%x",
              xcontext->depth, ckey);

          XvSetPortAttribute (xcontext->disp, xcontext->xv_port_id, atom,
              (gint) ckey);
        }
        todo--;
        vmetaxvsink->have_colorkey = TRUE;
      }

    XFree (attr);
  }

  /* Get the list of encodings supported by the adapter and look for the
   * XV_IMAGE encoding so we can determine the maximum width and height
   * supported */
  XvQueryEncodings (xcontext->disp, xcontext->xv_port_id, &nb_encodings,
      &encodings);

  for (i = 0; i < nb_encodings; i++) {
    GST_LOG_OBJECT (vmetaxvsink,
        "Encoding %d, name %s, max wxh %lux%lu rate %d/%d",
        i, encodings[i].name, encodings[i].width, encodings[i].height,
        encodings[i].rate.numerator, encodings[i].rate.denominator);
    if (strcmp (encodings[i].name, "XV_IMAGE") == 0) {
      max_w = encodings[i].width;
      max_h = encodings[i].height;
    }
  }

  XvFreeEncodingInfo (encodings);

  /* We get all image formats supported by our port */
  formats = XvListImageFormats (xcontext->disp,
      xcontext->xv_port_id, &nb_formats);
  caps = gst_caps_new_empty ();
  for (i = 0; i < nb_formats; i++) {
    GstCaps *format_caps = NULL;
    gboolean is_rgb_format = FALSE;
    GstVideoFormat vformat;

    /* We set the image format of the xcontext to an existing one. This
       is just some valid image format for making our xshm calls check before
       caps negotiation really happens. */
    xcontext->im_format = formats[i].id;

    switch (formats[i].type) {
      case XvRGB:
      {
        XvImageFormatValues *fmt = &(formats[i]);
        gint endianness;

        endianness =
            (fmt->byte_order == LSBFirst ? G_LITTLE_ENDIAN : G_BIG_ENDIAN);

        vformat = gst_video_format_from_masks (fmt->depth, fmt->bits_per_pixel,
            endianness, fmt->red_mask, fmt->green_mask, fmt->blue_mask, 0);
        if (vformat == GST_VIDEO_FORMAT_UNKNOWN)
          break;

        format_caps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, gst_video_format_to_string (vformat),
            "width", GST_TYPE_INT_RANGE, 1, max_w,
            "height", GST_TYPE_INT_RANGE, 1, max_h,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

        is_rgb_format = TRUE;
        break;
      }
      case XvYUV:
      {
        vformat = gst_video_format_from_fourcc (formats[i].id);
        if (vformat == GST_VIDEO_FORMAT_UNKNOWN)
          break;

        format_caps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, gst_video_format_to_string (vformat),
            "width", GST_TYPE_INT_RANGE, 1, max_w,
            "height", GST_TYPE_INT_RANGE, 1, max_h,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
        break;
      }
      default:
        vformat = GST_VIDEO_FORMAT_UNKNOWN;
        g_assert_not_reached ();
        break;
    }

    if (format_caps) {
      GstVmetaXvFormat *format = NULL;

      format = g_new0 (GstVmetaXvFormat, 1);
      if (format) {
        format->format = formats[i].id;
        format->vformat = vformat;
        format->caps = gst_caps_copy (format_caps);
        xcontext->formats_list = g_list_append (xcontext->formats_list, format);
      }

      if (is_rgb_format) {
        if (rgb_caps == NULL)
          rgb_caps = format_caps;
        else
          gst_caps_append (rgb_caps, format_caps);
      } else
        gst_caps_append (caps, format_caps);
    }
  }

  /* Collected all caps into either the caps or rgb_caps structures.
   * Append rgb_caps on the end of YUV, so that YUV is always preferred */
  if (rgb_caps)
    gst_caps_append (caps, rgb_caps);

  if (formats)
    XFree (formats);

  GST_DEBUG ("Generated the following caps: %" GST_PTR_FORMAT, caps);

  if (gst_caps_is_empty (caps)) {
    gst_caps_unref (caps);
    XvUngrabPort (xcontext->disp, xcontext->xv_port_id, 0);
    GST_ELEMENT_ERROR (vmetaxvsink, STREAM, WRONG_TYPE, (NULL),
        ("No supported format found"));
    return NULL;
  }

  return caps;
}

static gpointer
gst_vmetaxvsink_event_thread (GstVmetaXvSink * vmetaxvsink)
{
  g_return_val_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink), NULL);

  GST_OBJECT_LOCK (vmetaxvsink);
  while (vmetaxvsink->running) {
    GST_OBJECT_UNLOCK (vmetaxvsink);

    if (vmetaxvsink->xwindow) {
      gst_vmetaxvsink_handle_xevents (vmetaxvsink);
    }
    /* FIXME: do we want to align this with the framerate or anything else? */
    g_usleep (G_USEC_PER_SEC / 20);

    GST_OBJECT_LOCK (vmetaxvsink);
  }
  GST_OBJECT_UNLOCK (vmetaxvsink);

  return NULL;
}

static void
gst_vmetaxvsink_manage_event_thread (GstVmetaXvSink * vmetaxvsink)
{
  GThread *thread = NULL;

  /* don't start the thread too early */
  if (vmetaxvsink->xcontext == NULL) {
    return;
  }

  GST_OBJECT_LOCK (vmetaxvsink);
  if (vmetaxvsink->handle_expose || vmetaxvsink->handle_events) {
    if (!vmetaxvsink->event_thread) {
      /* Setup our event listening thread */
      GST_DEBUG_OBJECT (vmetaxvsink, "run xevent thread, expose %d, events %d",
          vmetaxvsink->handle_expose, vmetaxvsink->handle_events);
      vmetaxvsink->running = TRUE;
      vmetaxvsink->event_thread = g_thread_try_new ("vmetaxvsink-events",
          (GThreadFunc) gst_vmetaxvsink_event_thread, vmetaxvsink, NULL);
    }
  } else {
    if (vmetaxvsink->event_thread) {
      GST_DEBUG_OBJECT (vmetaxvsink, "stop xevent thread, expose %d, events %d",
          vmetaxvsink->handle_expose, vmetaxvsink->handle_events);
      vmetaxvsink->running = FALSE;
      /* grab thread and mark it as NULL */
      thread = vmetaxvsink->event_thread;
      vmetaxvsink->event_thread = NULL;
    }
  }
  GST_OBJECT_UNLOCK (vmetaxvsink);

  /* Wait for our event thread to finish */
  if (thread)
    g_thread_join (thread);

}


/* This function calculates the pixel aspect ratio based on the properties
 * in the xcontext structure and stores it there. */
static void
gst_vmetaxvsink_calculate_pixel_aspect_ratio (GstXContext * xcontext)
{
  static const gint par[][2] = {
    {1, 1},                     /* regular screen */
    {16, 15},                   /* PAL TV */
    {11, 10},                   /* 525 line Rec.601 video */
    {54, 59},                   /* 625 line Rec.601 video */
    {64, 45},                   /* 1280x1024 on 16:9 display */
    {5, 3},                     /* 1280x1024 on 4:3 display */
    {4, 3}                      /*  800x600 on 16:9 display */
  };
  gint i;
  gint index;
  gdouble ratio;
  gdouble delta;

#define DELTA(idx) (ABS (ratio - ((gdouble) par[idx][0] / par[idx][1])))

  /* first calculate the "real" ratio based on the X values;
   * which is the "physical" w/h divided by the w/h in pixels of the display */
  ratio = (gdouble) (xcontext->widthmm * xcontext->height)
      / (xcontext->heightmm * xcontext->width);

  /* DirectFB's X in 720x576 reports the physical dimensions wrong, so
   * override here */
  if (xcontext->width == 720 && xcontext->height == 576) {
    ratio = 4.0 * 576 / (3.0 * 720);
  }
  GST_DEBUG ("calculated pixel aspect ratio: %f", ratio);

  /* now find the one from par[][2] with the lowest delta to the real one */
  delta = DELTA (0);
  index = 0;

  for (i = 1; i < sizeof (par) / (sizeof (gint) * 2); ++i) {
    gdouble this_delta = DELTA (i);

    if (this_delta < delta) {
      index = i;
      delta = this_delta;
    }
  }

  GST_DEBUG ("Decided on index %d (%d/%d)", index,
      par[index][0], par[index][1]);

  g_free (xcontext->par);
  xcontext->par = g_new0 (GValue, 1);
  g_value_init (xcontext->par, GST_TYPE_FRACTION);
  gst_value_set_fraction (xcontext->par, par[index][0], par[index][1]);
  GST_DEBUG ("set xcontext PAR to %d/%d",
      gst_value_get_fraction_numerator (xcontext->par),
      gst_value_get_fraction_denominator (xcontext->par));
}

/* This function gets the X Display and global info about it. Everything is
   stored in our object and will be cleaned when the object is disposed. Note
   here that caps for supported format are generated without any window or
   image creation */
static GstXContext *
gst_vmetaxvsink_xcontext_get (GstVmetaXvSink * vmetaxvsink)
{
  GstXContext *xcontext = NULL;
  XPixmapFormatValues *px_formats = NULL;
  gint nb_formats = 0, i, j, N_attr;
  XvAttribute *xv_attr;
  Atom prop_atom;
  const char *channels[4] = { "XV_HUE", "XV_SATURATION",
    "XV_BRIGHTNESS", "XV_CONTRAST"
  };

  g_return_val_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink), NULL);

  xcontext = g_new0 (GstXContext, 1);
  xcontext->im_format = 0;

  g_mutex_lock (&vmetaxvsink->x_lock);

  xcontext->disp = XOpenDisplay (vmetaxvsink->display_name);

  if (!xcontext->disp) {
    g_mutex_unlock (&vmetaxvsink->x_lock);
    g_free (xcontext);
    GST_ELEMENT_ERROR (vmetaxvsink, RESOURCE, WRITE,
        ("Could not initialise Xv output"), ("Could not open display"));
    return NULL;
  }

  xcontext->screen = DefaultScreenOfDisplay (xcontext->disp);
  xcontext->screen_num = DefaultScreen (xcontext->disp);
  xcontext->visual = DefaultVisual (xcontext->disp, xcontext->screen_num);
  xcontext->root = DefaultRootWindow (xcontext->disp);
  xcontext->white = XWhitePixel (xcontext->disp, xcontext->screen_num);
  xcontext->black = XBlackPixel (xcontext->disp, xcontext->screen_num);
  xcontext->depth = DefaultDepthOfScreen (xcontext->screen);

  xcontext->width = DisplayWidth (xcontext->disp, xcontext->screen_num);
  xcontext->height = DisplayHeight (xcontext->disp, xcontext->screen_num);
  xcontext->widthmm = DisplayWidthMM (xcontext->disp, xcontext->screen_num);
  xcontext->heightmm = DisplayHeightMM (xcontext->disp, xcontext->screen_num);

  GST_DEBUG_OBJECT (vmetaxvsink, "X reports %dx%d pixels and %d mm x %d mm",
      xcontext->width, xcontext->height, xcontext->widthmm, xcontext->heightmm);

  gst_vmetaxvsink_calculate_pixel_aspect_ratio (xcontext);
  /* We get supported pixmap formats at supported depth */
  px_formats = XListPixmapFormats (xcontext->disp, &nb_formats);

  if (!px_formats) {
    XCloseDisplay (xcontext->disp);
    g_mutex_unlock (&vmetaxvsink->x_lock);
    g_free (xcontext->par);
    g_free (xcontext);
    GST_ELEMENT_ERROR (vmetaxvsink, RESOURCE, SETTINGS,
        ("Could not initialise Xv output"), ("Could not get pixel formats"));
    return NULL;
  }

  /* We get bpp value corresponding to our running depth */
  for (i = 0; i < nb_formats; i++) {
    if (px_formats[i].depth == xcontext->depth)
      xcontext->bpp = px_formats[i].bits_per_pixel;
  }

  XFree (px_formats);

  xcontext->endianness =
      (ImageByteOrder (xcontext->disp) ==
      LSBFirst) ? G_LITTLE_ENDIAN : G_BIG_ENDIAN;

  /* our caps system handles 24/32bpp RGB as big-endian. */
  if ((xcontext->bpp == 24 || xcontext->bpp == 32) &&
      xcontext->endianness == G_LITTLE_ENDIAN) {
    xcontext->endianness = G_BIG_ENDIAN;
    xcontext->visual->red_mask = GUINT32_TO_BE (xcontext->visual->red_mask);
    xcontext->visual->green_mask = GUINT32_TO_BE (xcontext->visual->green_mask);
    xcontext->visual->blue_mask = GUINT32_TO_BE (xcontext->visual->blue_mask);
    if (xcontext->bpp == 24) {
      xcontext->visual->red_mask >>= 8;
      xcontext->visual->green_mask >>= 8;
      xcontext->visual->blue_mask >>= 8;
    }
  }

  xcontext->caps = gst_vmetaxvsink_get_xv_support (vmetaxvsink, xcontext);

  /* Search for XShm extension support */
#ifdef HAVE_XSHM
  if (XShmQueryExtension (xcontext->disp) &&
      gst_vmetaxvsink_check_xshm_calls (vmetaxvsink, xcontext)) {
    xcontext->use_xshm = TRUE;
    GST_DEBUG ("vmetaxvsink is using XShm extension");
  } else
#endif /* HAVE_XSHM */
  {
    xcontext->use_xshm = FALSE;
    GST_DEBUG ("vmetaxvsink is not using XShm extension");
  }

  if (!xcontext->caps) {
    XCloseDisplay (xcontext->disp);
    g_mutex_unlock (&vmetaxvsink->x_lock);
    g_free (xcontext->par);
    g_free (xcontext);
    /* GST_ELEMENT_ERROR is thrown by gst_vmetaxvsink_get_xv_support */
    return NULL;
  }

  xv_attr = XvQueryPortAttributes (xcontext->disp,
      xcontext->xv_port_id, &N_attr);


  /* Generate the channels list */
  for (i = 0; i < (sizeof (channels) / sizeof (char *)); i++) {
    XvAttribute *matching_attr = NULL;

    /* Retrieve the property atom if it exists. If it doesn't exist,
     * the attribute itself must not either, so we can skip */
    prop_atom = XInternAtom (xcontext->disp, channels[i], True);
    if (prop_atom == None)
      continue;

    if (xv_attr != NULL) {
      for (j = 0; j < N_attr && matching_attr == NULL; ++j)
        if (!g_ascii_strcasecmp (channels[i], xv_attr[j].name))
          matching_attr = xv_attr + j;
    }

    if (matching_attr) {
      GstColorBalanceChannel *channel;

      channel = g_object_new (GST_TYPE_COLOR_BALANCE_CHANNEL, NULL);
      channel->label = g_strdup (channels[i]);
      channel->min_value = matching_attr ? matching_attr->min_value : -1000;
      channel->max_value = matching_attr ? matching_attr->max_value : 1000;

      xcontext->channels_list = g_list_append (xcontext->channels_list,
          channel);

      /* If the colorbalance settings have not been touched we get Xv values
         as defaults and update our internal variables */
      if (!vmetaxvsink->cb_changed) {
        gint val;

        XvGetPortAttribute (xcontext->disp, xcontext->xv_port_id,
            prop_atom, &val);
        /* Normalize val to [-1000, 1000] */
        val = floor (0.5 + -1000 + 2000 * (val - channel->min_value) /
            (double) (channel->max_value - channel->min_value));

        if (!g_ascii_strcasecmp (channels[i], "XV_HUE"))
          vmetaxvsink->hue = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_SATURATION"))
          vmetaxvsink->saturation = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_BRIGHTNESS"))
          vmetaxvsink->brightness = val;
        else if (!g_ascii_strcasecmp (channels[i], "XV_CONTRAST"))
          vmetaxvsink->contrast = val;
      }
    }
  }

  if (xv_attr)
    XFree (xv_attr);

  g_mutex_unlock (&vmetaxvsink->x_lock);

  return xcontext;
}

/* This function cleans the X context. Closing the Display, releasing the XV
   port and unrefing the caps for supported formats. */
static void
gst_vmetaxvsink_xcontext_clear (GstVmetaXvSink * vmetaxvsink)
{
  GList *formats_list, *channels_list;
  GstXContext *xcontext;
  gint i = 0;

  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));

  GST_OBJECT_LOCK (vmetaxvsink);
  if (vmetaxvsink->xcontext == NULL) {
    GST_OBJECT_UNLOCK (vmetaxvsink);
    return;
  }

  /* Take the XContext from the sink and clean it up */
  xcontext = vmetaxvsink->xcontext;
  vmetaxvsink->xcontext = NULL;

  GST_OBJECT_UNLOCK (vmetaxvsink);


  formats_list = xcontext->formats_list;

  while (formats_list) {
    GstVmetaXvFormat *format = formats_list->data;

    gst_caps_unref (format->caps);
    g_free (format);
    formats_list = g_list_next (formats_list);
  }

  if (xcontext->formats_list)
    g_list_free (xcontext->formats_list);

  channels_list = xcontext->channels_list;

  while (channels_list) {
    GstColorBalanceChannel *channel = channels_list->data;

    g_object_unref (channel);
    channels_list = g_list_next (channels_list);
  }

  if (xcontext->channels_list)
    g_list_free (xcontext->channels_list);

  gst_caps_unref (xcontext->caps);
  if (xcontext->last_caps)
    gst_caps_replace (&xcontext->last_caps, NULL);

  for (i = 0; i < xcontext->nb_adaptors; i++) {
    g_free (xcontext->adaptors[i]);
  }

  g_free (xcontext->adaptors);

  g_free (xcontext->par);

  g_mutex_lock (&vmetaxvsink->x_lock);

  GST_DEBUG_OBJECT (vmetaxvsink, "Closing display and freeing X Context");

  XvUngrabPort (xcontext->disp, xcontext->xv_port_id, 0);

  XCloseDisplay (xcontext->disp);

  g_mutex_unlock (&vmetaxvsink->x_lock);

  g_free (xcontext);
}

/* Element stuff */

static GstCaps *
gst_vmetaxvsink_getcaps (GstBaseSink * bsink, GstCaps * filter)
{
  GstVmetaXvSink *vmetaxvsink;
  GstCaps *caps;

  vmetaxvsink = GST_VMETAXVSINK (bsink);

  if (vmetaxvsink->xcontext) {
    if (filter)
      return gst_caps_intersect_full (filter, vmetaxvsink->xcontext->caps,
          GST_CAPS_INTERSECT_FIRST);
    else
      return gst_caps_ref (vmetaxvsink->xcontext->caps);
  }

  caps = gst_pad_get_pad_template_caps (GST_VIDEO_SINK_PAD (vmetaxvsink));
  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }
  return caps;
}

static gboolean
gst_vmetaxvsink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstVmetaXvSink *vmetaxvsink;
  GstStructure *structure;
  GstBufferPool *newpool, *oldpool;
  GstVideoInfo info;
  guint32 im_format = 0;
  gint video_par_n, video_par_d;        /* video's PAR */
  gint display_par_n, display_par_d;    /* display's PAR */
  guint num, den;
  gint size;
  static GstAllocationParams params = { 0, 15, 0, 0, };

  vmetaxvsink = GST_VMETAXVSINK (bsink);

  GST_DEBUG_OBJECT (vmetaxvsink,
      "In setcaps. Possible caps %" GST_PTR_FORMAT ", setting caps %"
      GST_PTR_FORMAT, vmetaxvsink->xcontext->caps, caps);

  if (!gst_caps_can_intersect (vmetaxvsink->xcontext->caps, caps))
    goto incompatible_caps;

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_format;

  vmetaxvsink->fps_n = info.fps_n;
  vmetaxvsink->fps_d = info.fps_d;

  vmetaxvsink->video_width = info.width;
  vmetaxvsink->video_height = info.height;

  im_format = gst_vmetaxvsink_get_format_from_info (vmetaxvsink, &info);
  if (im_format == -1)
    goto invalid_format;

  size = info.size;

  /* get aspect ratio from caps if it's present, and
   * convert video width and height to a display width and height
   * using wd / hd = wv / hv * PARv / PARd */

  /* get video's PAR */
  video_par_n = info.par_n;
  video_par_d = info.par_d;

  /* get display's PAR */
  if (vmetaxvsink->par) {
    display_par_n = gst_value_get_fraction_numerator (vmetaxvsink->par);
    display_par_d = gst_value_get_fraction_denominator (vmetaxvsink->par);
  } else {
    display_par_n = 1;
    display_par_d = 1;
  }

  if (!gst_video_calculate_display_ratio (&num, &den, info.width,
          info.height, video_par_n, video_par_d, display_par_n, display_par_d))
    goto no_disp_ratio;

  GST_DEBUG_OBJECT (vmetaxvsink,
      "video width/height: %dx%d, calculated display ratio: %d/%d",
      info.width, info.height, num, den);

  /* now find a width x height that respects this display ratio.
   * prefer those that have one of w/h the same as the incoming video
   * using wd / hd = num / den */

  /* start with same height, because of interlaced video */
  /* check hd / den is an integer scale factor, and scale wd with the PAR */
  if (info.height % den == 0) {
    GST_DEBUG_OBJECT (vmetaxvsink, "keeping video height");
    GST_VIDEO_SINK_WIDTH (vmetaxvsink) = (guint)
        gst_util_uint64_scale_int (info.height, num, den);
    GST_VIDEO_SINK_HEIGHT (vmetaxvsink) = info.height;
  } else if (info.width % num == 0) {
    GST_DEBUG_OBJECT (vmetaxvsink, "keeping video width");
    GST_VIDEO_SINK_WIDTH (vmetaxvsink) = info.width;
    GST_VIDEO_SINK_HEIGHT (vmetaxvsink) = (guint)
        gst_util_uint64_scale_int (info.width, den, num);
  } else {
    GST_DEBUG_OBJECT (vmetaxvsink, "approximating while keeping video height");
    GST_VIDEO_SINK_WIDTH (vmetaxvsink) = (guint)
        gst_util_uint64_scale_int (info.height, num, den);
    GST_VIDEO_SINK_HEIGHT (vmetaxvsink) = info.height;
  }
  GST_DEBUG_OBJECT (vmetaxvsink, "scaling to %dx%d",
      GST_VIDEO_SINK_WIDTH (vmetaxvsink), GST_VIDEO_SINK_HEIGHT (vmetaxvsink));

  /* Notify application to set xwindow id now */
  g_mutex_lock (&vmetaxvsink->flow_lock);
  if (!vmetaxvsink->xwindow) {
    g_mutex_unlock (&vmetaxvsink->flow_lock);
    gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (vmetaxvsink));
  } else {
    g_mutex_unlock (&vmetaxvsink->flow_lock);
  }

  /* Creating our window and our image with the display size in pixels */
  if (GST_VIDEO_SINK_WIDTH (vmetaxvsink) <= 0 ||
      GST_VIDEO_SINK_HEIGHT (vmetaxvsink) <= 0)
    goto no_display_size;

  g_mutex_lock (&vmetaxvsink->flow_lock);
  if (!vmetaxvsink->xwindow) {
    vmetaxvsink->xwindow = gst_vmetaxvsink_xwindow_new (vmetaxvsink,
        GST_VIDEO_SINK_WIDTH (vmetaxvsink),
        GST_VIDEO_SINK_HEIGHT (vmetaxvsink));
  }

  vmetaxvsink->info = info;

  /* After a resize, we want to redraw the borders in case the new frame size
   * doesn't cover the same area */
  vmetaxvsink->redraw_border = TRUE;

  /* create a new pool for the new configuration */
  newpool = gst_vmetaxv_buffer_pool_new (vmetaxvsink);

  structure = gst_buffer_pool_get_config (newpool);
  gst_buffer_pool_config_set_params (structure, caps, size, 2, 0);
  gst_buffer_pool_config_set_allocator (structure, NULL, &params);
  if (!gst_buffer_pool_set_config (newpool, structure))
    goto config_failed;

  oldpool = vmetaxvsink->pool;
  /* we don't activate the pool yet, this will be done by downstream after it
   * has configured the pool. If downstream does not want our pool we will
   * activate it when we render into it */
  vmetaxvsink->pool = newpool;
  g_mutex_unlock (&vmetaxvsink->flow_lock);

  /* unref the old sink */
  if (oldpool) {
    /* we don't deactivate, some elements might still be using it, it will
     * be deactivated when the last ref is gone */
    gst_object_unref (oldpool);
  }

  return TRUE;

  /* ERRORS */
incompatible_caps:
  {
    GST_ERROR_OBJECT (vmetaxvsink, "caps incompatible");
    return FALSE;
  }
invalid_format:
  {
    GST_DEBUG_OBJECT (vmetaxvsink,
        "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
no_disp_ratio:
  {
    GST_ELEMENT_ERROR (vmetaxvsink, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }
no_display_size:
  {
    GST_ELEMENT_ERROR (vmetaxvsink, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }
config_failed:
  {
    GST_ERROR_OBJECT (vmetaxvsink, "failed to set config.");
    g_mutex_unlock (&vmetaxvsink->flow_lock);
    return FALSE;
  }
}

static GstStateChangeReturn
gst_vmetaxvsink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstVmetaXvSink *vmetaxvsink;
  GstXContext *xcontext = NULL;

  vmetaxvsink = GST_VMETAXVSINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      /* Initializing the XContext */
      if (vmetaxvsink->xcontext == NULL) {
        xcontext = gst_vmetaxvsink_xcontext_get (vmetaxvsink);
        if (xcontext == NULL) {
          ret = GST_STATE_CHANGE_FAILURE;
          goto beach;
        }
        GST_OBJECT_LOCK (vmetaxvsink);
        if (xcontext)
          vmetaxvsink->xcontext = xcontext;
        GST_OBJECT_UNLOCK (vmetaxvsink);
      }

      /* update object's par with calculated one if not set yet */
      if (!vmetaxvsink->par) {
        vmetaxvsink->par = g_new0 (GValue, 1);
        gst_value_init_and_copy (vmetaxvsink->par, vmetaxvsink->xcontext->par);
        GST_DEBUG_OBJECT (vmetaxvsink, "set calculated PAR on object's PAR");
      }
      /* call XSynchronize with the current value of synchronous */
      GST_DEBUG_OBJECT (vmetaxvsink, "XSynchronize called with %s",
          vmetaxvsink->synchronous ? "TRUE" : "FALSE");
      XSynchronize (vmetaxvsink->xcontext->disp, vmetaxvsink->synchronous);
      gst_vmetaxvsink_update_colorbalance (vmetaxvsink);
      gst_vmetaxvsink_manage_event_thread (vmetaxvsink);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      vmetaxvsink->fps_n = 0;
      vmetaxvsink->fps_d = 1;
      GST_VIDEO_SINK_WIDTH (vmetaxvsink) = 0;
      GST_VIDEO_SINK_HEIGHT (vmetaxvsink) = 0;
      g_mutex_lock (&vmetaxvsink->flow_lock);
      if (vmetaxvsink->pool)
        gst_buffer_pool_set_active (vmetaxvsink->pool, FALSE);
      g_mutex_unlock (&vmetaxvsink->flow_lock);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_vmetaxvsink_reset (vmetaxvsink);
      break;
    default:
      break;
  }

beach:
  return ret;
}

static void
gst_vmetaxvsink_get_times (GstBaseSink * bsink, GstBuffer * buf,
    GstClockTime * start, GstClockTime * end)
{
  GstVmetaXvSink *vmetaxvsink;

  vmetaxvsink = GST_VMETAXVSINK (bsink);

  if (GST_BUFFER_TIMESTAMP_IS_VALID (buf)) {
    *start = GST_BUFFER_TIMESTAMP (buf);
    if (GST_BUFFER_DURATION_IS_VALID (buf)) {
      *end = *start + GST_BUFFER_DURATION (buf);
    } else {
      if (vmetaxvsink->fps_n > 0) {
        *end = *start +
            gst_util_uint64_scale_int (GST_SECOND, vmetaxvsink->fps_d,
            vmetaxvsink->fps_n);
      }
    }
  }
}

static GstFlowReturn
gst_vmetaxvsink_show_frame (GstVideoSink * vsink, GstBuffer * buf)
{
  GstFlowReturn res;
  GstVmetaXvSink *vmetaxvsink;
  GstVmetaXvMeta *meta;
  GstBuffer *to_put;

  vmetaxvsink = GST_VMETAXVSINK (vsink);

  meta = gst_buffer_get_vmetaxv_meta (buf);

  if (meta && meta->sink == vmetaxvsink) {
    /* If this buffer has been allocated using our buffer management we simply
       put the ximage which is in the PRIVATE pointer */
    GST_LOG_OBJECT (vmetaxvsink, "buffer %p from our pool, writing directly",
        buf);
    to_put = buf;
    res = GST_FLOW_OK;
  } else {
    GstVideoFrame src, dest;
    GstBufferPoolAcquireParams params = { 0, };

    /* Else we have to copy the data into our private image, */
    /* if we have one... */
    GST_LOG_OBJECT (vmetaxvsink, "buffer %p not from our pool, copying", buf);

    /* we should have a pool, configured in setcaps */
    if (vmetaxvsink->pool == NULL)
      goto no_pool;

    if (!gst_buffer_pool_set_active (vmetaxvsink->pool, TRUE))
      goto activate_failed;

    /* take a buffer from our pool, if there is no buffer in the pool something
     * is seriously wrong, waiting for the pool here might deadlock when we try
     * to go to PAUSED because we never flush the pool then. */
    params.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT;
    res = gst_buffer_pool_acquire_buffer (vmetaxvsink->pool, &to_put, &params);
    if (res != GST_FLOW_OK)
      goto no_buffer;

    GST_CAT_LOG_OBJECT (GST_CAT_PERFORMANCE, vmetaxvsink,
        "slow copy into bufferpool buffer %p", to_put);

    if (!gst_video_frame_map (&src, &vmetaxvsink->info, buf, GST_MAP_READ))
      goto invalid_buffer;

    if (!gst_video_frame_map (&dest, &vmetaxvsink->info, to_put, GST_MAP_WRITE)) {
      gst_video_frame_unmap (&src);
      goto invalid_buffer;
    }

    gst_video_frame_copy (&dest, &src);

    gst_video_frame_unmap (&dest);
    gst_video_frame_unmap (&src);
  }

  if (!gst_vmetaxvsink_xvimage_put (vmetaxvsink, to_put))
    goto no_window;

done:
  if (to_put != buf)
    gst_buffer_unref (to_put);

  return res;

  /* ERRORS */
no_pool:
  {
    GST_ELEMENT_ERROR (vmetaxvsink, RESOURCE, WRITE,
        ("Internal error: can't allocate images"),
        ("We don't have a bufferpool negotiated"));
    return GST_FLOW_ERROR;
  }
no_buffer:
  {
    /* No image available. That's very bad ! */
    GST_WARNING_OBJECT (vmetaxvsink, "could not create image");
    return GST_FLOW_OK;
  }
invalid_buffer:
  {
    /* No Window available to put our image into */
    GST_WARNING_OBJECT (vmetaxvsink, "could not map image");
    res = GST_FLOW_OK;
    goto done;
  }
no_window:
  {
    /* No Window available to put our image into */
    GST_WARNING_OBJECT (vmetaxvsink, "could not output image - no window");
    res = GST_FLOW_ERROR;
    goto done;
  }
activate_failed:
  {
    GST_ERROR_OBJECT (vmetaxvsink, "failed to activate bufferpool.");
    res = GST_FLOW_ERROR;
    goto done;
  }
}

static gboolean
gst_vmetaxvsink_event (GstBaseSink * sink, GstEvent * event)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (sink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_TAG:{
      GstTagList *l;
      gchar *title = NULL;

      gst_event_parse_tag (event, &l);
      gst_tag_list_get_string (l, GST_TAG_TITLE, &title);

      if (title) {
        GST_DEBUG_OBJECT (vmetaxvsink, "got tags, title='%s'", title);
        gst_vmetaxvsink_xwindow_set_title (vmetaxvsink, vmetaxvsink->xwindow,
            title);

        g_free (title);
      }
      break;
    }
    default:
      break;
  }
  return GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
}

static gboolean
gst_vmetaxvsink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (bsink);
  GstBufferPool *pool;
  GstStructure *config;
  GstCaps *caps;
  guint size;
  gboolean need_pool;

  gst_query_parse_allocation (query, &caps, &need_pool);

  if (caps == NULL)
    goto no_caps;

  g_mutex_lock (&vmetaxvsink->flow_lock);
  if ((pool = vmetaxvsink->pool))
    gst_object_ref (pool);
  g_mutex_unlock (&vmetaxvsink->flow_lock);

  if (pool != NULL) {
    GstCaps *pcaps;

    /* we had a pool, check caps */
    GST_DEBUG_OBJECT (vmetaxvsink, "check existing pool caps");
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, &pcaps, &size, NULL, NULL);

    if (!gst_caps_is_equal (caps, pcaps)) {
      GST_DEBUG_OBJECT (vmetaxvsink, "pool has different caps");
      /* different caps, we can't use this pool */
      gst_object_unref (pool);
      pool = NULL;
    }
    gst_structure_free (config);
  }
  if (pool == NULL && need_pool) {
    GstVideoInfo info;

    if (!gst_video_info_from_caps (&info, caps))
      goto invalid_caps;

    GST_DEBUG_OBJECT (vmetaxvsink, "create new pool");
    pool = gst_vmetaxv_buffer_pool_new (vmetaxvsink);

    /* the normal size of a frame */
    size = info.size;

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
    if (!gst_buffer_pool_set_config (pool, config))
      goto config_failed;
  }
  if (pool) {
    /* we need at least 2 buffer because we hold on to the last one */
    gst_query_add_allocation_pool (query, pool, size, 2, 0);
    gst_object_unref (pool);
  }

  /* we also support various metadata */
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_CROP_META_API_TYPE, NULL);

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_DEBUG_OBJECT (bsink, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_DEBUG_OBJECT (bsink, "invalid caps specified");
    return FALSE;
  }
config_failed:
  {
    GST_DEBUG_OBJECT (bsink, "failed setting config");
    gst_object_unref (pool);
    return FALSE;
  }
}

/* Interfaces stuff */
static void
gst_vmetaxvsink_navigation_send_event (GstNavigation * navigation,
    GstStructure * structure)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (navigation);
  GstPad *peer;

  if ((peer = gst_pad_get_peer (GST_VIDEO_SINK_PAD (vmetaxvsink)))) {
    GstEvent *event;
    GstVideoRectangle src, dst, result;
    gdouble x, y, xscale = 1.0, yscale = 1.0;

    event = gst_event_new_navigation (structure);

    /* We take the flow_lock while we look at the window */
    g_mutex_lock (&vmetaxvsink->flow_lock);

    if (!vmetaxvsink->xwindow) {
      g_mutex_unlock (&vmetaxvsink->flow_lock);
      return;
    }

    if (vmetaxvsink->keep_aspect) {
      /* We get the frame position using the calculated geometry from _setcaps
         that respect pixel aspect ratios */
      src.w = GST_VIDEO_SINK_WIDTH (vmetaxvsink);
      src.h = GST_VIDEO_SINK_HEIGHT (vmetaxvsink);
      dst.w = vmetaxvsink->render_rect.w;
      dst.h = vmetaxvsink->render_rect.h;

      gst_video_sink_center_rect (src, dst, &result, TRUE);
      result.x += vmetaxvsink->render_rect.x;
      result.y += vmetaxvsink->render_rect.y;
    } else {
      memcpy (&result, &vmetaxvsink->render_rect, sizeof (GstVideoRectangle));
    }

    g_mutex_unlock (&vmetaxvsink->flow_lock);

    /* We calculate scaling using the original video frames geometry to include
       pixel aspect ratio scaling. */
    xscale = (gdouble) vmetaxvsink->video_width / result.w;
    yscale = (gdouble) vmetaxvsink->video_height / result.h;

    /* Converting pointer coordinates to the non scaled geometry */
    if (gst_structure_get_double (structure, "pointer_x", &x)) {
      x = MIN (x, result.x + result.w);
      x = MAX (x - result.x, 0);
      gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE,
          (gdouble) x * xscale, NULL);
    }
    if (gst_structure_get_double (structure, "pointer_y", &y)) {
      y = MIN (y, result.y + result.h);
      y = MAX (y - result.y, 0);
      gst_structure_set (structure, "pointer_y", G_TYPE_DOUBLE,
          (gdouble) y * yscale, NULL);
    }

    gst_pad_send_event (peer, event);
    gst_object_unref (peer);
  }
}

static void
gst_vmetaxvsink_navigation_init (GstNavigationInterface * iface)
{
  iface->send_event = gst_vmetaxvsink_navigation_send_event;
}

static void
gst_vmetaxvsink_set_window_handle (GstVideoOverlay * overlay, guintptr id)
{
  XID xwindow_id = id;
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (overlay);
  GstXWindow *xwindow = NULL;

  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));

  g_mutex_lock (&vmetaxvsink->flow_lock);

  /* If we already use that window return */
  if (vmetaxvsink->xwindow && (xwindow_id == vmetaxvsink->xwindow->win)) {
    g_mutex_unlock (&vmetaxvsink->flow_lock);
    return;
  }

  /* If the element has not initialized the X11 context try to do so */
  if (!vmetaxvsink->xcontext &&
      !(vmetaxvsink->xcontext = gst_vmetaxvsink_xcontext_get (vmetaxvsink))) {
    g_mutex_unlock (&vmetaxvsink->flow_lock);
    /* we have thrown a GST_ELEMENT_ERROR now */
    return;
  }

  gst_vmetaxvsink_update_colorbalance (vmetaxvsink);

  /* If a window is there already we destroy it */
  if (vmetaxvsink->xwindow) {
    gst_vmetaxvsink_xwindow_destroy (vmetaxvsink, vmetaxvsink->xwindow);
    vmetaxvsink->xwindow = NULL;
  }

  /* If the xid is 0 we go back to an internal window */
  if (xwindow_id == 0) {
    /* If no width/height caps nego did not happen window will be created
       during caps nego then */
    if (GST_VIDEO_SINK_WIDTH (vmetaxvsink)
        && GST_VIDEO_SINK_HEIGHT (vmetaxvsink)) {
      xwindow =
          gst_vmetaxvsink_xwindow_new (vmetaxvsink,
          GST_VIDEO_SINK_WIDTH (vmetaxvsink),
          GST_VIDEO_SINK_HEIGHT (vmetaxvsink));
    }
  } else {
    XWindowAttributes attr;

    xwindow = g_new0 (GstXWindow, 1);
    xwindow->win = xwindow_id;

    /* Set the event we want to receive and create a GC */
    g_mutex_lock (&vmetaxvsink->x_lock);

    XGetWindowAttributes (vmetaxvsink->xcontext->disp, xwindow->win, &attr);

    xwindow->width = attr.width;
    xwindow->height = attr.height;
    xwindow->internal = FALSE;
    if (!vmetaxvsink->have_render_rect) {
      vmetaxvsink->render_rect.x = vmetaxvsink->render_rect.y = 0;
      vmetaxvsink->render_rect.w = attr.width;
      vmetaxvsink->render_rect.h = attr.height;
    }
    if (vmetaxvsink->handle_events) {
      XSelectInput (vmetaxvsink->xcontext->disp, xwindow->win, ExposureMask |
          StructureNotifyMask | PointerMotionMask | KeyPressMask |
          KeyReleaseMask);
    }

    xwindow->gc = XCreateGC (vmetaxvsink->xcontext->disp,
        xwindow->win, 0, NULL);
    g_mutex_unlock (&vmetaxvsink->x_lock);
  }

  if (xwindow)
    vmetaxvsink->xwindow = xwindow;

  g_mutex_unlock (&vmetaxvsink->flow_lock);
}

static void
gst_vmetaxvsink_expose (GstVideoOverlay * overlay)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (overlay);

  GST_DEBUG ("doing expose");
  gst_vmetaxvsink_xwindow_update_geometry (vmetaxvsink);
  gst_vmetaxvsink_xvimage_put (vmetaxvsink, NULL);
}

static void
gst_vmetaxvsink_set_event_handling (GstVideoOverlay * overlay,
    gboolean handle_events)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (overlay);

  vmetaxvsink->handle_events = handle_events;

  g_mutex_lock (&vmetaxvsink->flow_lock);

  if (G_UNLIKELY (!vmetaxvsink->xwindow)) {
    g_mutex_unlock (&vmetaxvsink->flow_lock);
    return;
  }

  g_mutex_lock (&vmetaxvsink->x_lock);

  if (handle_events) {
    if (vmetaxvsink->xwindow->internal) {
      XSelectInput (vmetaxvsink->xcontext->disp, vmetaxvsink->xwindow->win,
          ExposureMask | StructureNotifyMask | PointerMotionMask |
          KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);
    } else {
      XSelectInput (vmetaxvsink->xcontext->disp, vmetaxvsink->xwindow->win,
          ExposureMask | StructureNotifyMask | PointerMotionMask |
          KeyPressMask | KeyReleaseMask);
    }
  } else {
    XSelectInput (vmetaxvsink->xcontext->disp, vmetaxvsink->xwindow->win, 0);
  }

  g_mutex_unlock (&vmetaxvsink->x_lock);

  g_mutex_unlock (&vmetaxvsink->flow_lock);
}

static void
gst_vmetaxvsink_set_render_rectangle (GstVideoOverlay * overlay, gint x, gint y,
    gint width, gint height)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (overlay);

  /* FIXME: how about some locking? */
  if (width >= 0 && height >= 0) {
    vmetaxvsink->render_rect.x = x;
    vmetaxvsink->render_rect.y = y;
    vmetaxvsink->render_rect.w = width;
    vmetaxvsink->render_rect.h = height;
    vmetaxvsink->have_render_rect = TRUE;
  } else {
    vmetaxvsink->render_rect.x = 0;
    vmetaxvsink->render_rect.y = 0;
    vmetaxvsink->render_rect.w = vmetaxvsink->xwindow->width;
    vmetaxvsink->render_rect.h = vmetaxvsink->xwindow->height;
    vmetaxvsink->have_render_rect = FALSE;
  }
}

static void
gst_vmetaxvsink_video_overlay_init (GstVideoOverlayInterface * iface)
{
  iface->set_window_handle = gst_vmetaxvsink_set_window_handle;
  iface->expose = gst_vmetaxvsink_expose;
  iface->handle_events = gst_vmetaxvsink_set_event_handling;
  iface->set_render_rectangle = gst_vmetaxvsink_set_render_rectangle;
}

static const GList *
gst_vmetaxvsink_colorbalance_list_channels (GstColorBalance * balance)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (balance);

  g_return_val_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink), NULL);

  if (vmetaxvsink->xcontext)
    return vmetaxvsink->xcontext->channels_list;
  else
    return NULL;
}

static void
gst_vmetaxvsink_colorbalance_set_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel, gint value)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (balance);

  g_return_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink));
  g_return_if_fail (channel->label != NULL);

  vmetaxvsink->cb_changed = TRUE;

  /* Normalize val to [-1000, 1000] */
  value = floor (0.5 + -1000 + 2000 * (value - channel->min_value) /
      (double) (channel->max_value - channel->min_value));

  if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
    vmetaxvsink->hue = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
    vmetaxvsink->saturation = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
    vmetaxvsink->contrast = value;
  } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
    vmetaxvsink->brightness = value;
  } else {
    g_warning ("got an unknown channel %s", channel->label);
    return;
  }

  gst_vmetaxvsink_update_colorbalance (vmetaxvsink);
}

static gint
gst_vmetaxvsink_colorbalance_get_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (balance);
  gint value = 0;

  g_return_val_if_fail (GST_IS_VMETAXVSINK (vmetaxvsink), 0);
  g_return_val_if_fail (channel->label != NULL, 0);

  if (g_ascii_strcasecmp (channel->label, "XV_HUE") == 0) {
    value = vmetaxvsink->hue;
  } else if (g_ascii_strcasecmp (channel->label, "XV_SATURATION") == 0) {
    value = vmetaxvsink->saturation;
  } else if (g_ascii_strcasecmp (channel->label, "XV_CONTRAST") == 0) {
    value = vmetaxvsink->contrast;
  } else if (g_ascii_strcasecmp (channel->label, "XV_BRIGHTNESS") == 0) {
    value = vmetaxvsink->brightness;
  } else {
    g_warning ("got an unknown channel %s", channel->label);
  }

  /* Normalize val to [channel->min_value, channel->max_value] */
  value = channel->min_value + (channel->max_value - channel->min_value) *
      (value + 1000) / 2000;

  return value;
}

static GstColorBalanceType
gst_vmetaxvsink_colorbalance_get_balance_type (GstColorBalance * balance)
{
  return GST_COLOR_BALANCE_HARDWARE;
}

static void
gst_vmetaxvsink_colorbalance_init (GstColorBalanceInterface * iface)
{
  iface->list_channels = gst_vmetaxvsink_colorbalance_list_channels;
  iface->set_value = gst_vmetaxvsink_colorbalance_set_value;
  iface->get_value = gst_vmetaxvsink_colorbalance_get_value;
  iface->get_balance_type = gst_vmetaxvsink_colorbalance_get_balance_type;
}

#if 0
static const GList *
gst_vmetaxvsink_probe_get_properties (GstPropertyProbe * probe)
{
  GObjectClass *klass = G_OBJECT_GET_CLASS (probe);
  static GList *list = NULL;

  if (!list) {
    list = g_list_append (NULL, g_object_class_find_property (klass, "device"));
    list =
        g_list_append (list, g_object_class_find_property (klass,
            "autopaint-colorkey"));
    list =
        g_list_append (list, g_object_class_find_property (klass,
            "double-buffer"));
    list =
        g_list_append (list, g_object_class_find_property (klass, "colorkey"));
  }

  return list;
}

static void
gst_vmetaxvsink_probe_probe_property (GstPropertyProbe * probe,
    guint prop_id, const GParamSpec * pspec)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (probe);

  switch (prop_id) {
    case PROP_DEVICE:
    case PROP_AUTOPAINT_COLORKEY:
    case PROP_DOUBLE_BUFFER:
    case PROP_COLORKEY:
      GST_DEBUG_OBJECT (vmetaxvsink,
          "probing device list and get capabilities");
      if (!vmetaxvsink->xcontext) {
        GST_DEBUG_OBJECT (vmetaxvsink, "generating xcontext");
        vmetaxvsink->xcontext = gst_vmetaxvsink_xcontext_get (vmetaxvsink);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (probe, prop_id, pspec);
      break;
  }
}

static gboolean
gst_vmetaxvsink_probe_needs_probe (GstPropertyProbe * probe,
    guint prop_id, const GParamSpec * pspec)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (probe);
  gboolean ret = FALSE;

  switch (prop_id) {
    case PROP_DEVICE:
    case PROP_AUTOPAINT_COLORKEY:
    case PROP_DOUBLE_BUFFER:
    case PROP_COLORKEY:
      if (vmetaxvsink->xcontext != NULL) {
        ret = FALSE;
      } else {
        ret = TRUE;
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (probe, prop_id, pspec);
      break;
  }

  return ret;
}

static GValueArray *
gst_vmetaxvsink_probe_get_values (GstPropertyProbe * probe,
    guint prop_id, const GParamSpec * pspec)
{
  GstVmetaXvSink *vmetaxvsink = GST_VMETAXVSINK (probe);
  GValueArray *array = NULL;

  if (G_UNLIKELY (!vmetaxvsink->xcontext)) {
    GST_WARNING_OBJECT (vmetaxvsink, "we don't have any xcontext, can't "
        "get values");
    goto beach;
  }

  switch (prop_id) {
    case PROP_DEVICE:
    {
      guint i;
      GValue value = { 0 };

      array = g_value_array_new (vmetaxvsink->xcontext->nb_adaptors);
      g_value_init (&value, G_TYPE_STRING);

      for (i = 0; i < vmetaxvsink->xcontext->nb_adaptors; i++) {
        gchar *adaptor_id_s = g_strdup_printf ("%u", i);

        g_value_set_string (&value, adaptor_id_s);
        g_value_array_append (array, &value);
        g_free (adaptor_id_s);
      }
      g_value_unset (&value);
      break;
    }
    case PROP_AUTOPAINT_COLORKEY:
      if (vmetaxvsink->have_autopaint_colorkey) {
        GValue value = { 0 };

        array = g_value_array_new (2);
        g_value_init (&value, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value, FALSE);
        g_value_array_append (array, &value);
        g_value_set_boolean (&value, TRUE);
        g_value_array_append (array, &value);
        g_value_unset (&value);
      }
      break;
    case PROP_DOUBLE_BUFFER:
      if (vmetaxvsink->have_double_buffer) {
        GValue value = { 0 };

        array = g_value_array_new (2);
        g_value_init (&value, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value, FALSE);
        g_value_array_append (array, &value);
        g_value_set_boolean (&value, TRUE);
        g_value_array_append (array, &value);
        g_value_unset (&value);
      }
      break;
    case PROP_COLORKEY:
      if (vmetaxvsink->have_colorkey) {
        GValue value = { 0 };

        array = g_value_array_new (1);
        g_value_init (&value, GST_TYPE_INT_RANGE);
        gst_value_set_int_range (&value, 0, 0xffffff);
        g_value_array_append (array, &value);
        g_value_unset (&value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (probe, prop_id, pspec);
      break;
  }

beach:
  return array;
}

static void
gst_vmetaxvsink_property_probe_interface_init (GstPropertyProbeInterface *
    iface)
{
  iface->get_properties = gst_vmetaxvsink_probe_get_properties;
  iface->probe_property = gst_vmetaxvsink_probe_probe_property;
  iface->needs_probe = gst_vmetaxvsink_probe_needs_probe;
  iface->get_values = gst_vmetaxvsink_probe_get_values;
}
#endif

/* =========================================== */
/*                                             */
/*              Init & Class init              */
/*                                             */
/* =========================================== */

static void
gst_vmetaxvsink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVmetaXvSink *vmetaxvsink;

  g_return_if_fail (GST_IS_VMETAXVSINK (object));

  vmetaxvsink = GST_VMETAXVSINK (object);

  switch (prop_id) {
    case PROP_HUE:
      vmetaxvsink->hue = g_value_get_int (value);
      vmetaxvsink->cb_changed = TRUE;
      gst_vmetaxvsink_update_colorbalance (vmetaxvsink);
      break;
    case PROP_CONTRAST:
      vmetaxvsink->contrast = g_value_get_int (value);
      vmetaxvsink->cb_changed = TRUE;
      gst_vmetaxvsink_update_colorbalance (vmetaxvsink);
      break;
    case PROP_BRIGHTNESS:
      vmetaxvsink->brightness = g_value_get_int (value);
      vmetaxvsink->cb_changed = TRUE;
      gst_vmetaxvsink_update_colorbalance (vmetaxvsink);
      break;
    case PROP_SATURATION:
      vmetaxvsink->saturation = g_value_get_int (value);
      vmetaxvsink->cb_changed = TRUE;
      gst_vmetaxvsink_update_colorbalance (vmetaxvsink);
      break;
    case PROP_DISPLAY:
      vmetaxvsink->display_name = g_strdup (g_value_get_string (value));
      break;
    case PROP_SYNCHRONOUS:
      vmetaxvsink->synchronous = g_value_get_boolean (value);
      if (vmetaxvsink->xcontext) {
        XSynchronize (vmetaxvsink->xcontext->disp, vmetaxvsink->synchronous);
        GST_DEBUG_OBJECT (vmetaxvsink, "XSynchronize called with %s",
            vmetaxvsink->synchronous ? "TRUE" : "FALSE");
      }
      break;
    case PROP_PIXEL_ASPECT_RATIO:
      g_free (vmetaxvsink->par);
      vmetaxvsink->par = g_new0 (GValue, 1);
      g_value_init (vmetaxvsink->par, GST_TYPE_FRACTION);
      if (!g_value_transform (value, vmetaxvsink->par)) {
        g_warning ("Could not transform string to aspect ratio");
        gst_value_set_fraction (vmetaxvsink->par, 1, 1);
      }
      GST_DEBUG_OBJECT (vmetaxvsink, "set PAR to %d/%d",
          gst_value_get_fraction_numerator (vmetaxvsink->par),
          gst_value_get_fraction_denominator (vmetaxvsink->par));
      break;
    case PROP_FORCE_ASPECT_RATIO:
      vmetaxvsink->keep_aspect = g_value_get_boolean (value);
      break;
    case PROP_HANDLE_EVENTS:
      gst_vmetaxvsink_set_event_handling (GST_VIDEO_OVERLAY (vmetaxvsink),
          g_value_get_boolean (value));
      gst_vmetaxvsink_manage_event_thread (vmetaxvsink);
      break;
    case PROP_DEVICE:
      vmetaxvsink->adaptor_no = atoi (g_value_get_string (value));
      break;
    case PROP_HANDLE_EXPOSE:
      vmetaxvsink->handle_expose = g_value_get_boolean (value);
      gst_vmetaxvsink_manage_event_thread (vmetaxvsink);
      break;
    case PROP_DOUBLE_BUFFER:
      vmetaxvsink->double_buffer = g_value_get_boolean (value);
      break;
    case PROP_AUTOPAINT_COLORKEY:
      vmetaxvsink->autopaint_colorkey = g_value_get_boolean (value);
      break;
    case PROP_COLORKEY:
      vmetaxvsink->colorkey = g_value_get_int (value);
      break;
    case PROP_DRAW_BORDERS:
      vmetaxvsink->draw_borders = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vmetaxvsink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVmetaXvSink *vmetaxvsink;

  g_return_if_fail (GST_IS_VMETAXVSINK (object));

  vmetaxvsink = GST_VMETAXVSINK (object);

  switch (prop_id) {
    case PROP_HUE:
      g_value_set_int (value, vmetaxvsink->hue);
      break;
    case PROP_CONTRAST:
      g_value_set_int (value, vmetaxvsink->contrast);
      break;
    case PROP_BRIGHTNESS:
      g_value_set_int (value, vmetaxvsink->brightness);
      break;
    case PROP_SATURATION:
      g_value_set_int (value, vmetaxvsink->saturation);
      break;
    case PROP_DISPLAY:
      g_value_set_string (value, vmetaxvsink->display_name);
      break;
    case PROP_SYNCHRONOUS:
      g_value_set_boolean (value, vmetaxvsink->synchronous);
      break;
    case PROP_PIXEL_ASPECT_RATIO:
      if (vmetaxvsink->par)
        g_value_transform (vmetaxvsink->par, value);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      g_value_set_boolean (value, vmetaxvsink->keep_aspect);
      break;
    case PROP_HANDLE_EVENTS:
      g_value_set_boolean (value, vmetaxvsink->handle_events);
      break;
    case PROP_DEVICE:
    {
      char *adaptor_no_s = g_strdup_printf ("%u", vmetaxvsink->adaptor_no);

      g_value_set_string (value, adaptor_no_s);
      g_free (adaptor_no_s);
      break;
    }
    case PROP_DEVICE_NAME:
      if (vmetaxvsink->xcontext && vmetaxvsink->xcontext->adaptors) {
        g_value_set_string (value,
            vmetaxvsink->xcontext->adaptors[vmetaxvsink->adaptor_no]);
      } else {
        g_value_set_string (value, NULL);
      }
      break;
    case PROP_HANDLE_EXPOSE:
      g_value_set_boolean (value, vmetaxvsink->handle_expose);
      break;
    case PROP_DOUBLE_BUFFER:
      g_value_set_boolean (value, vmetaxvsink->double_buffer);
      break;
    case PROP_AUTOPAINT_COLORKEY:
      g_value_set_boolean (value, vmetaxvsink->autopaint_colorkey);
      break;
    case PROP_COLORKEY:
      g_value_set_int (value, vmetaxvsink->colorkey);
      break;
    case PROP_DRAW_BORDERS:
      g_value_set_boolean (value, vmetaxvsink->draw_borders);
      break;
    case PROP_WINDOW_WIDTH:
      if (vmetaxvsink->xwindow)
        g_value_set_uint64 (value, vmetaxvsink->xwindow->width);
      else
        g_value_set_uint64 (value, 0);
      break;
    case PROP_WINDOW_HEIGHT:
      if (vmetaxvsink->xwindow)
        g_value_set_uint64 (value, vmetaxvsink->xwindow->height);
      else
        g_value_set_uint64 (value, 0);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vmetaxvsink_reset (GstVmetaXvSink * vmetaxvsink)
{
  GThread *thread;

  GST_OBJECT_LOCK (vmetaxvsink);
  vmetaxvsink->running = FALSE;
  /* grab thread and mark it as NULL */
  thread = vmetaxvsink->event_thread;
  vmetaxvsink->event_thread = NULL;
  GST_OBJECT_UNLOCK (vmetaxvsink);

  /* Wait for our event thread to finish before we clean up our stuff. */
  if (thread)
    g_thread_join (thread);

  if (vmetaxvsink->cur_image) {
    gst_buffer_unref (vmetaxvsink->cur_image);
    vmetaxvsink->cur_image = NULL;
  }

  g_mutex_lock (&vmetaxvsink->flow_lock);

  if (vmetaxvsink->pool) {
    gst_object_unref (vmetaxvsink->pool);
    vmetaxvsink->pool = NULL;
  }

  if (vmetaxvsink->xwindow) {
    gst_vmetaxvsink_xwindow_clear (vmetaxvsink, vmetaxvsink->xwindow);
    gst_vmetaxvsink_xwindow_destroy (vmetaxvsink, vmetaxvsink->xwindow);
    vmetaxvsink->xwindow = NULL;
  }
  g_mutex_unlock (&vmetaxvsink->flow_lock);

  vmetaxvsink->render_rect.x = vmetaxvsink->render_rect.y =
      vmetaxvsink->render_rect.w = vmetaxvsink->render_rect.h = 0;
  vmetaxvsink->have_render_rect = FALSE;

  gst_vmetaxvsink_xcontext_clear (vmetaxvsink);
}

/* Finalize is called only once, dispose can be called multiple times.
 * We use mutexes and don't reset stuff to NULL here so let's register
 * as a finalize. */
static void
gst_vmetaxvsink_finalize (GObject * object)
{
  GstVmetaXvSink *vmetaxvsink;

  vmetaxvsink = GST_VMETAXVSINK (object);

  gst_vmetaxvsink_reset (vmetaxvsink);

  if (vmetaxvsink->display_name) {
    g_free (vmetaxvsink->display_name);
    vmetaxvsink->display_name = NULL;
  }

  if (vmetaxvsink->par) {
    g_free (vmetaxvsink->par);
    vmetaxvsink->par = NULL;
  }
  g_mutex_clear (&vmetaxvsink->x_lock);
  g_mutex_clear (&vmetaxvsink->flow_lock);
  g_free (vmetaxvsink->media_title);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vmetaxvsink_init (GstVmetaXvSink * vmetaxvsink)
{
  vmetaxvsink->display_name = NULL;
  vmetaxvsink->adaptor_no = 0;
  vmetaxvsink->xcontext = NULL;
  vmetaxvsink->xwindow = NULL;
  vmetaxvsink->cur_image = NULL;

  vmetaxvsink->hue = vmetaxvsink->saturation = 0;
  vmetaxvsink->contrast = vmetaxvsink->brightness = 0;
  vmetaxvsink->cb_changed = FALSE;

  vmetaxvsink->fps_n = 0;
  vmetaxvsink->fps_d = 0;
  vmetaxvsink->video_width = 0;
  vmetaxvsink->video_height = 0;

  g_mutex_init (&vmetaxvsink->x_lock);
  g_mutex_init (&vmetaxvsink->flow_lock);

  vmetaxvsink->pool = NULL;

  vmetaxvsink->synchronous = FALSE;
  vmetaxvsink->double_buffer = TRUE;
  vmetaxvsink->running = FALSE;
  vmetaxvsink->keep_aspect = TRUE;
  vmetaxvsink->handle_events = TRUE;
  vmetaxvsink->par = NULL;
  vmetaxvsink->handle_expose = TRUE;
  vmetaxvsink->autopaint_colorkey = TRUE;

  /* on 16bit displays this becomes r,g,b = 1,2,3
   * on 24bit displays this becomes r,g,b = 8,8,16
   * as a port atom value
   */
  vmetaxvsink->colorkey = (8 << 16) | (8 << 8) | 16;
  vmetaxvsink->draw_borders = TRUE;
}

static void
gst_vmetaxvsink_class_init (GstVmetaXvSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstVideoSinkClass *videosink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  videosink_class = (GstVideoSinkClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = gst_vmetaxvsink_set_property;
  gobject_class->get_property = gst_vmetaxvsink_get_property;

  g_object_class_install_property (gobject_class, PROP_CONTRAST,
      g_param_spec_int ("contrast", "Contrast", "The contrast of the video",
          -1000, 1000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_BRIGHTNESS,
      g_param_spec_int ("brightness", "Brightness",
          "The brightness of the video", -1000, 1000, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HUE,
      g_param_spec_int ("hue", "Hue", "The hue of the video", -1000, 1000, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SATURATION,
      g_param_spec_int ("saturation", "Saturation",
          "The saturation of the video", -1000, 1000, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DISPLAY,
      g_param_spec_string ("display", "Display", "X Display name", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SYNCHRONOUS,
      g_param_spec_boolean ("synchronous", "Synchronous",
          "When enabled, runs the X display in synchronous mode. "
          "(unrelated to A/V sync, used only for debugging)", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PIXEL_ASPECT_RATIO,
      g_param_spec_string ("pixel-aspect-ratio", "Pixel Aspect Ratio",
          "The pixel aspect ratio of the device", "1/1",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_FORCE_ASPECT_RATIO,
      g_param_spec_boolean ("force-aspect-ratio", "Force aspect ratio",
          "When enabled, scaling will respect original aspect ratio", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HANDLE_EVENTS,
      g_param_spec_boolean ("handle-events", "Handle XEvents",
          "When enabled, XEvents will be selected and handled", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE,
      g_param_spec_string ("device", "Adaptor number",
          "The number of the video adaptor", "0",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DEVICE_NAME,
      g_param_spec_string ("device-name", "Adaptor name",
          "The name of the video adaptor", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  /**
   * GstVmetaXvSink:handle-expose
   *
   * When enabled, the current frame will always be drawn in response to X
   * Expose.
   *
   * Since: 0.10.14
   */
  g_object_class_install_property (gobject_class, PROP_HANDLE_EXPOSE,
      g_param_spec_boolean ("handle-expose", "Handle expose",
          "When enabled, "
          "the current frame will always be drawn in response to X Expose "
          "events", TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVmetaXvSink:double-buffer
   *
   * Whether to double-buffer the output.
   *
   * Since: 0.10.14
   */
  g_object_class_install_property (gobject_class, PROP_DOUBLE_BUFFER,
      g_param_spec_boolean ("double-buffer", "Double-buffer",
          "Whether to double-buffer the output", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstVmetaXvSink:autopaint-colorkey
   *
   * Whether to autofill overlay with colorkey
   *
   * Since: 0.10.21
   */
  g_object_class_install_property (gobject_class, PROP_AUTOPAINT_COLORKEY,
      g_param_spec_boolean ("autopaint-colorkey", "Autofill with colorkey",
          "Whether to autofill overlay with colorkey", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstVmetaXvSink:colorkey
   *
   * Color to use for the overlay mask.
   *
   * Since: 0.10.21
   */
  g_object_class_install_property (gobject_class, PROP_COLORKEY,
      g_param_spec_int ("colorkey", "Colorkey",
          "Color to use for the overlay mask", G_MININT, G_MAXINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVmetaXvSink:draw-borders
   *
   * Draw black borders when using GstVmetaXvSink:force-aspect-ratio to fill
   * unused parts of the video area.
   *
   * Since: 0.10.21
   */
  g_object_class_install_property (gobject_class, PROP_DRAW_BORDERS,
      g_param_spec_boolean ("draw-borders", "Colorkey",
          "Draw black borders to fill unused area in force-aspect-ratio mode",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVmetaXvSink:window-width
   *
   * Actual width of the video window.
   *
   * Since: 0.10.32
   */
  g_object_class_install_property (gobject_class, PROP_WINDOW_WIDTH,
      g_param_spec_uint64 ("window-width", "window-width",
          "Width of the window", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVmetaXvSink:window-height
   *
   * Actual height of the video window.
   *
   * Since: 0.10.32
   */
  g_object_class_install_property (gobject_class, PROP_WINDOW_HEIGHT,
      g_param_spec_uint64 ("window-height", "window-height",
          "Height of the window", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  gobject_class->finalize = gst_vmetaxvsink_finalize;

  gst_element_class_set_static_metadata (gstelement_class,
      "Video sink", "Sink/Video",
      "A Xv based videosink", "Julien Moutte <julien@moutte.net>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_vmetaxvsink_sink_template_factory));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_vmetaxvsink_change_state);

  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_vmetaxvsink_getcaps);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_vmetaxvsink_setcaps);
  gstbasesink_class->get_times = GST_DEBUG_FUNCPTR (gst_vmetaxvsink_get_times);
  gstbasesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vmetaxvsink_propose_allocation);
  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_vmetaxvsink_event);

  videosink_class->show_frame = GST_DEBUG_FUNCPTR (gst_vmetaxvsink_show_frame);
}
