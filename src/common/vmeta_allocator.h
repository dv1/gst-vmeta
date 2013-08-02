/* vMeta video memory allocator for GStreamer
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


#ifndef VMETA_ALLOCATOR_H
#define VMETA_ALLOCATOR_H

#include <glib.h>
#include <gst/gst.h>
#include <gst/gstallocator.h>

#include <vdec_os_api.h>


G_BEGIN_DECLS


typedef struct _GstVmetaAllocator GstVmetaAllocator;
typedef struct _GstVmetaAllocatorClass GstVmetaAllocatorClass;
typedef struct _GstVmetaMemory GstVmetaMemory;


#define GST_TYPE_VMETA_ALLOCATOR             (gst_vmeta_allocator_get_type())
#define GST_VMETA_ALLOCATOR(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VMETA_ALLOCATOR, GstVmetaAllocator))
#define GST_VMETA_ALLOCATOR_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VMETA_ALLOCATOR, GstVmetaAllocatorClass))
#define GST_IS_VMETA_ALLOCATOR(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_VMETA_ALLOCATOR))
#define GST_IS_VMETA_ALLOCATOR_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_VMETA_ALLOCATOR))

#define GST_VMETA_ALLOCATOR_MEMTYPE_NORMAL       "VmetaDMAMemoryNormal"
#define GST_VMETA_ALLOCATOR_MEMTYPE_CACHABLE     "VmetaDMAMemoryCacheable"
#define GST_VMETA_ALLOCATOR_MEMTYPE_BUFFERABLE   "VmetaDMAMemoryBufferable"


typedef enum
{
	GST_VMETA_ALLOCATOR_TYPE_NORMAL = 0,
	GST_VMETA_ALLOCATOR_TYPE_CACHEABLE,
	GST_VMETA_ALLOCATOR_TYPE_BUFFERABLE,
	NUM_GST_VMETA_ALLOCATOR_TYPES
}
GstVmetaAllocatorType;


struct _GstVmetaAllocator
{
	GstAllocator parent;

	GstVmetaAllocatorType type;
};


struct _GstVmetaAllocatorClass
{
	GstAllocatorClass parent_class;
};


struct _GstVmetaMemory
{
	GstMemory mem;

	void *virt_addr;
	UNSG32 phys_addr;
};


GType gst_vmeta_allocator_get_type(void);

GstAllocator* gst_vmeta_allocator_new(GstVmetaAllocatorType type);


G_END_DECLS


#endif
