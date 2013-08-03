/* GStreamer
 * Copyright (C) <2003> Julien Moutte <julien@moutte.net>
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

#include "config.h"

#include "vmetaxvsink.h"

GST_DEBUG_CATEGORY (gst_debug_vmetaxvpool);
GST_DEBUG_CATEGORY (gst_debug_vmetaxvsink);
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "vmetaxvsink",
          GST_RANK_PRIMARY, GST_TYPE_VMETAXVSINK))
    return FALSE;

  GST_DEBUG_CATEGORY_INIT (gst_debug_vmetaxvsink, "vmetaxvsink", 0,
      "vmetaxvsink element");
  GST_DEBUG_CATEGORY_INIT (gst_debug_vmetaxvpool, "vmetaxvpool", 0,
      "vmetaxvpool object");

  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    vmetaxvsink,
    "XFree86 video output plugin using Xv extension, modified for vMeta",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)

