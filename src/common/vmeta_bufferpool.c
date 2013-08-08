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


#include <codecVC.h>
#include <string.h>
#include "vmeta_bufferpool.h"


GST_DEBUG_CATEGORY (vmetabufferpool_debug);
#define GST_CAT_DEFAULT vmetabufferpool_debug


static gboolean gst_vmeta_buffer_meta_init(GstMeta *meta, G_GNUC_UNUSED gpointer params, G_GNUC_UNUSED GstBuffer *buffer);
static void gst_vmeta_buffer_meta_free(GstMeta *meta, G_GNUC_UNUSED GstBuffer *buffer);

static const gchar ** gst_vmeta_buffer_pool_get_options(GstBufferPool *pool);
static gboolean gst_vmeta_buffer_pool_set_config(GstBufferPool *pool, GstStructure *config);
static GstFlowReturn gst_vmeta_buffer_pool_alloc_buffer(GstBufferPool *pool, GstBuffer **buffer, GstBufferPoolAcquireParams *params);
static void gst_vmeta_buffer_pool_finalize(GObject *object);


G_DEFINE_TYPE(GstVmetaBufferPool, gst_vmeta_buffer_pool, GST_TYPE_BUFFER_POOL)




static gboolean gst_vmeta_buffer_meta_init(GstMeta *meta, G_GNUC_UNUSED gpointer params, G_GNUC_UNUSED GstBuffer *buffer)
{
	GstVmetaBufferMeta *vmeta_meta = (GstVmetaBufferMeta *)meta;
	vmeta_meta->mvl_ipp_data = NULL;
	vmeta_meta->mvl_ipp_data_size = 0;
	return TRUE;
}


static void gst_vmeta_buffer_meta_free(GstMeta *meta, G_GNUC_UNUSED GstBuffer *buffer)
{
	GstVmetaBufferMeta *vmeta_meta = (GstVmetaBufferMeta *)meta;
	if (vmeta_meta->mvl_ipp_data != NULL)
	{
		g_slice_free1(vmeta_meta->mvl_ipp_data_size, vmeta_meta->mvl_ipp_data);
		vmeta_meta->mvl_ipp_data = NULL;
		vmeta_meta->mvl_ipp_data_size = 0;
	}
}


GType gst_vmeta_buffer_meta_api_get_type(void)
{
	static volatile GType type;
	static gchar const *tags[] = { "memory", "vmeta", NULL };

	if (g_once_init_enter(&type))
	{
		GType _type = gst_meta_api_type_register("GstVmetaBufferMetaAPI", tags);
		g_once_init_leave(&type, _type);
	}

	return type;
}


GstMetaInfo const * gst_vmeta_buffer_meta_get_info(void)
{
	static GstMetaInfo const *meta_buffer_vmeta_info = NULL;

	if (g_once_init_enter(&meta_buffer_vmeta_info))
	{
		GstMetaInfo const *meta = gst_meta_register(
			gst_vmeta_buffer_meta_api_get_type(),
			"GstVmetaBufferMeta",
			sizeof(GstVmetaBufferMeta),
			GST_DEBUG_FUNCPTR(gst_vmeta_buffer_meta_init),
			GST_DEBUG_FUNCPTR(gst_vmeta_buffer_meta_free),
			(GstMetaTransformFunction)NULL
		);
		g_once_init_leave(&meta_buffer_vmeta_info, meta);
	}

	return meta_buffer_vmeta_info;
}




static const gchar ** gst_vmeta_buffer_pool_get_options(G_GNUC_UNUSED GstBufferPool *pool)
{
	static const gchar *options[] =
	{
		GST_BUFFER_POOL_OPTION_VIDEO_META,
		GST_BUFFER_POOL_OPTION_MVL_VMETA,
		NULL
	};

	return options;
}


static gboolean gst_vmeta_buffer_pool_set_config(GstBufferPool *pool, GstStructure *config)
{
	GstVmetaBufferPool *vmeta_pool;
	GstVideoInfo info;
	GstCaps *caps;
	gsize size;

	vmeta_pool = GST_VMETA_BUFFER_POOL(pool);

	if (!gst_buffer_pool_config_get_params(config, &caps, &size, NULL, NULL))
	{
		GST_ERROR_OBJECT(pool, "pool configuration invalid");
		return FALSE;
	}

	if (caps == NULL)
	{
		GST_ERROR_OBJECT(pool, "configuration contains no caps");
		return FALSE;
	}

	if (!gst_video_info_from_caps(&info, caps))
	{
		GST_ERROR_OBJECT(pool, "caps cannot be parsed for video info");
		return FALSE;
	}

	/* This vMeta decoded uses UYVY as the output format. For UYVY, only one plane is used.
	 * -> Only the first stride value has to be set. */
	vmeta_pool->video_info = info;
	vmeta_pool->video_info.stride[0] = vmeta_pool->dis_stride;
	vmeta_pool->video_info.size = size;

	vmeta_pool->dis_size = size;
	vmeta_pool->add_videometa = gst_buffer_pool_config_has_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);

	GST_INFO_OBJECT(pool, "pool configured:  video info stride: %u  dis size: %u", vmeta_pool->dis_stride, size);

	return GST_BUFFER_POOL_CLASS(gst_vmeta_buffer_pool_parent_class)->set_config(pool, config);
}


static GstFlowReturn gst_vmeta_buffer_pool_alloc_buffer(GstBufferPool *pool, GstBuffer **buffer, G_GNUC_UNUSED GstBufferPoolAcquireParams *params)
{
	GstVmetaBufferPool *vmeta_pool;
	GstBuffer *buf;
	GstMemory *mem;
	GstVmetaMemory *vmeta_mem;
	GstVmetaBufferMeta *vmeta_meta;
	GstVideoInfo *info;
	GstAllocationParams alloc_params;
	IppVmetaPicture *picture;

	guint const align = VMETA_DIS_BUF_ALIGN;

	vmeta_pool = GST_VMETA_BUFFER_POOL(pool);

	memset(&alloc_params, 0, sizeof(GstAllocationParams));
	alloc_params.flags = vmeta_pool->read_only ? GST_MEMORY_FLAG_READONLY : 0;
	alloc_params.align = align - 1; /* -1 , since align works like a bitmask (internal alignment is align+1) */

	info = &vmeta_pool->video_info;

	buf = gst_buffer_new();
	if (buf == NULL)
	{
		GST_ERROR_OBJECT(pool, "could not create new buffer");
		return GST_FLOW_ERROR;
	}

	mem = gst_allocator_alloc(vmeta_pool->allocator, vmeta_pool->dis_size, &alloc_params);
	if (mem == NULL)
	{
		gst_buffer_unref(buf);
		GST_ERROR_OBJECT(pool, "could not allocate %u byte for new buffer", vmeta_pool->dis_size);
		return GST_FLOW_ERROR;
	}

	vmeta_mem = (GstVmetaMemory *)mem;
	vmeta_meta = GST_VMETA_BUFFER_META_ADD(buf);
	vmeta_meta->dma_mem = vmeta_mem;

	picture = g_slice_alloc(sizeof(IppVmetaPicture));
	memset(picture, 0, sizeof(IppVmetaPicture));
	picture->nPhyAddr = vmeta_mem->phys_addr;
	picture->pBuf = vmeta_mem->virt_addr;
	picture->nBufSize = vmeta_pool->dis_size;
	picture->pUsrData0 = buf;

	vmeta_meta->mvl_ipp_data = picture;
	vmeta_meta->mvl_ipp_data_size = sizeof(IppVmetaPicture);

	gst_buffer_append_memory(buf, mem);

	if (vmeta_pool->add_videometa)
	{
		gst_buffer_add_video_meta_full(
			buf,
			GST_VIDEO_FRAME_FLAG_NONE,
			GST_VIDEO_INFO_FORMAT(info),
			GST_VIDEO_INFO_WIDTH (info), GST_VIDEO_INFO_HEIGHT (info),
			GST_VIDEO_INFO_N_PLANES(info),
			info->offset,
			info->stride
		);
	}

	*buffer = buf;

	return GST_FLOW_OK;
}


static void gst_vmeta_buffer_pool_finalize(GObject *object)
{
	GstVmetaBufferPool *vmeta_pool = GST_VMETA_BUFFER_POOL(object);

	GST_DEBUG_OBJECT(vmeta_pool, "shutting down vMeta buffer pool");
	
	G_OBJECT_CLASS (gst_vmeta_buffer_pool_parent_class)->finalize(object);

	/* unref'ing AFTER calling the parent class' finalize function, since the parent
	 * class will shut down the allocated memory blocks, for which the allocator must
	 * exist */
	gst_object_unref(vmeta_pool->allocator);
}


static void gst_vmeta_buffer_pool_class_init(GstVmetaBufferPoolClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GstBufferPoolClass *parent_class = GST_BUFFER_POOL_CLASS(klass);

	GST_DEBUG_CATEGORY_INIT(vmetabufferpool_debug, "vmetabufferpool", 0, "vMeta buffer pool");

	object_class->finalize     = GST_DEBUG_FUNCPTR(gst_vmeta_buffer_pool_finalize);
	parent_class->get_options  = GST_DEBUG_FUNCPTR(gst_vmeta_buffer_pool_get_options);
	parent_class->set_config   = GST_DEBUG_FUNCPTR(gst_vmeta_buffer_pool_set_config);
	parent_class->alloc_buffer = GST_DEBUG_FUNCPTR(gst_vmeta_buffer_pool_alloc_buffer);
}


static void gst_vmeta_buffer_pool_init(GstVmetaBufferPool *pool)
{
	pool->dis_stride = -1;
	pool->add_videometa = FALSE;

	GST_DEBUG_OBJECT(pool, "initializing vMeta buffer pool");
}


GstBufferPool *gst_vmeta_buffer_pool_new(GstVmetaAllocatorType alloc_type, gboolean read_only)
{
	GstVmetaBufferPool *vmeta_pool;

	vmeta_pool = g_object_new(gst_vmeta_buffer_pool_get_type(), NULL);
	vmeta_pool->allocator = gst_vmeta_allocator_new(alloc_type);
	vmeta_pool->read_only = read_only;

	return GST_BUFFER_POOL_CAST(vmeta_pool);
}


void gst_vmeta_buffer_pool_set_dis_info(GstBufferPool *pool, gsize dis_size, gint dis_stride)
{
	GstVmetaBufferPool *vmeta_pool = GST_VMETA_BUFFER_POOL(pool);

	vmeta_pool->dis_size = dis_size;
	vmeta_pool->dis_stride = dis_stride;

	GST_LOG_OBJECT(pool, "set_dis_info:  video info stride: %u  dis size: %u", dis_stride, dis_size);

	vmeta_pool->video_info.stride[0] = dis_stride;
}

