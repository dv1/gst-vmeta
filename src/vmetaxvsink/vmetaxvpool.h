/* GStreamer
 * Copyright (C) <2005> Julien Moutte <julien@moutte.net>
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

#ifndef __GST_VMETAXVPOOL_H__
#define __GST_VMETAXVPOOL_H__

#ifdef HAVE_XSHM
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif /* HAVE_XSHM */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifdef HAVE_XSHM
#include <X11/extensions/XShm.h>
#endif /* HAVE_XSHM */

#include <string.h>
#include <math.h>


G_BEGIN_DECLS

typedef struct _GstVmetaXvMeta GstVmetaXvMeta;

typedef struct _GstVmetaXvBufferPool GstVmetaXvBufferPool;
typedef struct _GstVmetaXvBufferPoolClass GstVmetaXvBufferPoolClass;
typedef struct _GstVmetaXvBufferPoolPrivate GstVmetaXvBufferPoolPrivate;

#include "vmetaxvsink.h"

GType gst_vmetaxv_meta_api_get_type (void);
#define GST_VMETAXV_META_API_TYPE  (gst_vmetaxv_meta_api_get_type())
const GstMetaInfo * gst_vmetaxv_meta_get_info (void);
#define GST_VMETAXV_META_INFO  (gst_vmetaxv_meta_get_info())

#define gst_buffer_get_vmetaxv_meta(b) ((GstVmetaXvMeta*)gst_buffer_get_meta((b),GST_VMETAXV_META_API_TYPE))

/**
 * GstVmetaXvMeta:
 * @sink: a reference to the our #GstVmetaXvSink
 * @xvimage: the XvImage of this buffer
 * @width: the width in pixels of XvImage @xvimage
 * @height: the height in pixels of XvImage @xvimage
 * @im_format: the format of XvImage @xvimage
 * @size: the size in bytes of XvImage @xvimage
 *
 * Subclass of #GstMeta containing additional information about an XvImage.
 */
struct _GstVmetaXvMeta
{
  GstMeta meta;

  /* Reference to the xvimagesink we belong to */
  GstVmetaXvSink *sink;

  XvImage *xvimage;

#ifdef HAVE_XSHM
  XShmSegmentInfo SHMInfo;
#endif                          /* HAVE_XSHM */

  gint x, y;
  gint width, height;
  gint im_format;
  size_t size;
};

/* buffer pool functions */
#define GST_TYPE_VMETAXV_BUFFER_POOL      (gst_vmetaxv_buffer_pool_get_type())
#define GST_IS_VMETAXV_BUFFER_POOL(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_VMETAXV_BUFFER_POOL))
#define GST_VMETAXV_BUFFER_POOL(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_VMETAXV_BUFFER_POOL, GstVmetaXvBufferPool))
#define GST_VMETAXV_BUFFER_POOL_CAST(obj) ((GstVmetaXvBufferPool*)(obj))

struct _GstVmetaXvBufferPool
{
  GstBufferPool bufferpool;

  GstVmetaXvSink *sink;

  GstVmetaXvBufferPoolPrivate *priv;
};

struct _GstVmetaXvBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

GType gst_vmetaxv_buffer_pool_get_type (void);

GstBufferPool *gst_vmetaxv_buffer_pool_new (GstVmetaXvSink * vmetaxvsink);

gboolean gst_vmetaxvsink_check_xshm_calls (GstVmetaXvSink * vmetaxvsink,
      GstXContext * xcontext);

gint gst_vmetaxvsink_get_format_from_info (GstVmetaXvSink * vmetaxvsink,
    GstVideoInfo * info);

G_END_DECLS

#endif /*__GST_VMETAXVPOOL_H__*/
