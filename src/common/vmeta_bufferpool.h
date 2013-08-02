/* vMeta video buffer pool for GStreamer
 * Copyright (C) 2013  Carlos Rafael Giani
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
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#ifndef VMETA_BUFFERPOOL_H
#define VMETA_BUFFERPOOL_H

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include "vmeta_allocator.h"


G_BEGIN_DECLS


typedef struct _GstVmetaBufferMeta GstVmetaBufferMeta;
typedef struct _GstVmetaBufferPool GstVmetaBufferPool;
typedef struct _GstVmetaBufferPoolClass GstVmetaBufferPoolClass;


#define GST_TYPE_VMETA_BUFFER_POOL             (gst_vmeta_buffer_pool_get_type())
#define GST_VMETA_BUFFER_POOL(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VMETA_BUFFER_POOL, GstVmetaBufferPool))
#define GST_VMETA_BUFFER_POOL_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VMETA_BUFFER_POOL, GstVmetaBufferPoolClass))

#define GST_VMETA_BUFFER_META_GET(buffer)      ((GstVmetaBufferMeta *)gst_buffer_get_meta((buffer), gst_vmeta_buffer_meta_api_get_type()))
#define GST_VMETA_BUFFER_META_ADD(buffer)      ((GstVmetaBufferMeta *)gst_buffer_add_meta((buffer), gst_vmeta_buffer_meta_get_info(), NULL))


#define GST_BUFFER_POOL_OPTION_MVL_VMETA "GstBufferPoolOptionMvlVmeta"


struct _GstVmetaBufferMeta
{
	GstMeta meta;

	GstVmetaMemory *dma_mem;

	/* IPP structures like IppVmetaPicture are stored here */
	void *mvl_ipp_data;
	gsize mvl_ipp_data_size;
};


struct _GstVmetaBufferPool
{
	GstBufferPool bufferpool;

	GstAllocator *allocator;
	gsize dis_size;
	gint dis_stride;
	GstVideoInfo video_info;
	gboolean add_videometa;
	gboolean read_only;
};


struct _GstVmetaBufferPoolClass
{
	GstBufferPoolClass parent_class;
};


GType gst_vmeta_buffer_meta_api_get_type(void);
GstMetaInfo const * gst_vmeta_buffer_meta_get_info(void);

GType gst_vmeta_buffer_pool_get_type(void);
GstBufferPool *gst_vmeta_buffer_pool_new(GstVmetaAllocatorType alloc_type, gboolean read_only);
void gst_vmeta_buffer_pool_set_dis_info(GstBufferPool *pool, gsize dis_size, gint dis_stride);


G_END_DECLS


#endif
