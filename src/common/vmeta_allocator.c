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


#include <string.h>
#include <codecVC.h>
#include <glib.h>
#include "vmeta_allocator.h"


GST_DEBUG_CATEGORY_STATIC(vmetaallocator_debug);
#define GST_CAT_DEFAULT vmetaallocator_debug

#define VMETA_PADDING_BYTE 0x88          /* the vmeta decoder needs a padding of 0x88 at the end of a frame */



static char const * gst_vmeta_get_alloctype_string(GstVmetaAllocatorType type);

static GstMemory* gst_vmeta_allocator_alloc(GstAllocator *allocator, gsize size, GstAllocationParams *params);
static void gst_vmeta_allocator_free(GstAllocator *allocator, GstMemory *memory);
static gpointer gst_vmeta_allocator_map(GstMemory *mem, gsize maxsize, GstMapFlags flags);
static void gst_vmeta_allocator_unmap(GstMemory *mem);
static GstMemory* gst_vmeta_allocator_copy(GstMemory *mem, gssize offset, gssize size);
static GstMemory* gst_vmeta_allocator_share(GstMemory *mem, gssize offset, gssize size);
static gboolean gst_vmeta_allocator_is_span(GstMemory *mem1, GstMemory *mem2, gsize *offset);


G_DEFINE_TYPE(GstVmetaAllocator, gst_vmeta_allocator, GST_TYPE_ALLOCATOR)


GstAllocator* gst_vmeta_allocator_new(GstVmetaAllocatorType type)
{
	GstAllocator *allocator;
	allocator = g_object_new(gst_vmeta_allocator_get_type(), NULL);

	GST_VMETA_ALLOCATOR(allocator)->type = type;

	return gst_object_ref(allocator);
}


static char const * gst_vmeta_get_alloctype_string(GstVmetaAllocatorType type)
{
	switch (type)
	{
		case GST_VMETA_ALLOCATOR_TYPE_NORMAL: return GST_VMETA_ALLOCATOR_MEMTYPE_NORMAL;
		case GST_VMETA_ALLOCATOR_TYPE_CACHEABLE: return GST_VMETA_ALLOCATOR_MEMTYPE_CACHABLE;
		case GST_VMETA_ALLOCATOR_TYPE_BUFFERABLE: return GST_VMETA_ALLOCATOR_MEMTYPE_BUFFERABLE;
		default:
			g_assert_not_reached();
			return "<invalid>";
	}
}


static void gst_vmeta_allocator_class_init(GstVmetaAllocatorClass *klass)
{
	GstAllocatorClass *parent_class = GST_ALLOCATOR_CLASS(klass);

	parent_class->alloc = GST_DEBUG_FUNCPTR(gst_vmeta_allocator_alloc);
	parent_class->free  = GST_DEBUG_FUNCPTR(gst_vmeta_allocator_free);

	GST_DEBUG_CATEGORY_INIT(vmetaallocator_debug, "vmetaallocator", 0, "vMeta DMA memory/allocator");
}


static void gst_vmeta_allocator_init(GstVmetaAllocator *allocator)
{
	GstAllocator *parent = GST_ALLOCATOR(allocator);

	parent->mem_type    = gst_vmeta_get_alloctype_string(allocator->type);
	parent->mem_map     = GST_DEBUG_FUNCPTR(gst_vmeta_allocator_map);
	parent->mem_unmap   = GST_DEBUG_FUNCPTR(gst_vmeta_allocator_unmap);
	parent->mem_copy    = GST_DEBUG_FUNCPTR(gst_vmeta_allocator_copy);
	parent->mem_share   = GST_DEBUG_FUNCPTR(gst_vmeta_allocator_share);
	parent->mem_is_span = GST_DEBUG_FUNCPTR(gst_vmeta_allocator_is_span);
}


static GstVmetaMemory* gst_vmeta_mem_new_internal(GstVmetaAllocator *vmeta_alloc, GstMemory *parent, gsize maxsize, GstMemoryFlags flags, gsize align, gsize offset, gsize size)
{
	GstVmetaMemory *vmeta_mem;
	vmeta_mem = g_slice_alloc(sizeof(GstVmetaMemory));
	vmeta_mem->virt_addr = NULL;

	gst_memory_init(GST_MEMORY_CAST(vmeta_mem), flags, GST_ALLOCATOR_CAST(vmeta_alloc), parent, maxsize, align, offset, size);

	return vmeta_mem;
}


static GstVmetaMemory* gst_vmeta_alloc_internal(GstAllocator *allocator, GstMemory *parent, gsize maxsize, GstMemoryFlags flags, gsize align, gsize offset, gsize size)
{
	GstVmetaAllocator *vmeta_alloc;
	GstVmetaMemory *vmeta_mem;
	gsize padding;

	vmeta_alloc = GST_VMETA_ALLOCATOR(allocator);

	GST_DEBUG_OBJECT(
		allocator,
		"allocating block with maxsize: %u, align: %u, offset: %u, size: %u",
		maxsize,
		align,
		offset,
		size
	);

	vmeta_mem = gst_vmeta_mem_new_internal(vmeta_alloc, parent, maxsize, flags, align, offset, size);

	/* vdec calls ensure the pointer is aligned */

	switch (vmeta_alloc->type)
	{
		case GST_VMETA_ALLOCATOR_TYPE_NORMAL:
			vmeta_mem->virt_addr = vdec_os_api_dma_alloc(maxsize, align, &(vmeta_mem->phys_addr));
			break;
		case GST_VMETA_ALLOCATOR_TYPE_CACHEABLE:
			vmeta_mem->virt_addr = vdec_os_api_dma_alloc_cached(maxsize, align, &(vmeta_mem->phys_addr));
			break;
		case GST_VMETA_ALLOCATOR_TYPE_BUFFERABLE:
			vmeta_mem->virt_addr = vdec_os_api_dma_alloc_writecombine(maxsize, align, &(vmeta_mem->phys_addr));
			break;
		default:
			break;
	}

	if (vmeta_mem->virt_addr == NULL)
	{
		GST_ERROR_OBJECT(allocator, "could not allocate %u byte of DMA memory for vMeta", maxsize);
		g_slice_free1(sizeof(GstVmetaMemory), vmeta_mem);
		return NULL;
	}

	padding = maxsize - (offset + size);

	if ((offset > 0) && (flags & GST_MEMORY_FLAG_ZERO_PREFIXED))
		memset(vmeta_mem->virt_addr, 0, offset);

	if (padding > 0)
	{
		memset(
			(guint8*)(vmeta_mem->virt_addr) + offset + size,
			(flags & GST_MEMORY_FLAG_ZERO_PADDED) ? 0 : VMETA_PADDING_BYTE,
			padding
		);
	}

	return vmeta_mem;
}


static GstMemory* gst_vmeta_allocator_alloc(GstAllocator *allocator, gsize size, GstAllocationParams *params)
{
	gsize maxsize;
	GstVmetaMemory *vmeta_mem;

	maxsize = size + params->prefix + params->padding;

	vmeta_mem = gst_vmeta_alloc_internal(allocator, NULL, maxsize, params->flags, params->align, params->prefix, size);

	GST_DEBUG_OBJECT(
		allocator,
		"allocated block at virtual address %p, physical address 0x%x, maxsize: %u, align: %u, offset: %u, size: %u",
		vmeta_mem->virt_addr,
		vmeta_mem->phys_addr,
		vmeta_mem->mem.maxsize,
		vmeta_mem->mem.align,
		vmeta_mem->mem.offset,
		vmeta_mem->mem.size
	);

	return (GstMemory *)vmeta_mem;
}


static void gst_vmeta_allocator_free(GstAllocator *allocator, GstMemory *memory)
{
	GstVmetaMemory *vmeta_mem = (GstVmetaMemory *)memory;

	GST_DEBUG_OBJECT(
		allocator,
		"deallocated block at virtual address %p, maxsize: %u, align: %u, offset: %u, size: %u",
		vmeta_mem->virt_addr,
		vmeta_mem->mem.maxsize,
		vmeta_mem->mem.align,
		vmeta_mem->mem.offset,
		vmeta_mem->mem.size
	);

	vdec_os_api_dma_free(vmeta_mem->virt_addr);

	vmeta_mem->virt_addr = (void*)0xDDDDDDDD;
	vmeta_mem->phys_addr = 0xDDDDDDDD;

	g_slice_free1(sizeof(GstVmetaMemory), memory);
}


static gpointer gst_vmeta_allocator_map(GstMemory *mem, G_GNUC_UNUSED gsize maxsize, G_GNUC_UNUSED GstMapFlags flags)
{
	GstVmetaMemory *vmeta_mem = (GstVmetaMemory *)mem;
	return vmeta_mem->virt_addr;
}


static void gst_vmeta_allocator_unmap(GstMemory *mem)
{
	GstVmetaMemory *vmeta_mem = (GstVmetaMemory *)mem;
	GstVmetaAllocator *vmeta_alloc = GST_VMETA_ALLOCATOR(mem->allocator);
	if (vmeta_alloc->type == GST_VMETA_ALLOCATOR_TYPE_CACHEABLE)
		vdec_os_api_flush_cache((UNSG32)(vmeta_mem->virt_addr), mem->maxsize, DMA_TO_DEVICE);
}


static GstMemory* gst_vmeta_allocator_copy(GstMemory *mem, gssize offset, gssize size)
{
	GstVmetaMemory *vmeta_mem;
	GstVmetaMemory *copy;

	vmeta_mem = (GstVmetaMemory *)mem;

	if (size == -1)
		size = ((gssize)(mem->size) > offset) ? (mem->size - offset) : 0;

	// TODO: is flags = 0 correct?
	copy = gst_vmeta_alloc_internal(mem->allocator, NULL, mem->maxsize, 0, mem->align, mem->offset + offset, size);
	memcpy(copy->virt_addr, vmeta_mem->virt_addr, mem->maxsize);

	GST_DEBUG_OBJECT(
		mem->allocator,
		"copied block; offset: %d, size: %d; source block maxsize: %u, align: %u, offset: %u, size: %u",
		offset,
		size,
		mem->maxsize,
		mem->align,
		mem->offset,
		mem->size
	);

	return (GstMemory *)copy;
}


static GstMemory* gst_vmeta_allocator_share(GstMemory *mem, gssize offset, gssize size)
{
	GstVmetaMemory *vmeta_mem;
	GstVmetaMemory *sub;
	GstMemory *parent;

	vmeta_mem = (GstVmetaMemory *)mem;

	if (size == -1)
		size = ((gssize)(vmeta_mem->mem.size) > offset) ? (vmeta_mem->mem.size - offset) : 0;

	if ((parent = vmeta_mem->mem.parent) == NULL)
		parent = (GstMemory *)mem;

	sub = gst_vmeta_mem_new_internal(
		GST_VMETA_ALLOCATOR(vmeta_mem->mem.allocator),
		parent,
		vmeta_mem->mem.maxsize,
		GST_MINI_OBJECT_FLAGS(parent) | GST_MINI_OBJECT_FLAG_LOCK_READONLY,
		vmeta_mem->mem.align,
		vmeta_mem->mem.offset + offset,
		size
	);
	sub->virt_addr = vmeta_mem->virt_addr;
	sub->phys_addr = vmeta_mem->phys_addr;

	GST_DEBUG_OBJECT(
		mem->allocator,
		"shared block; offset: %d, size: %d; source block maxsize: %u, align: %u, offset: %u, size: %u",
		offset,
		size,
		mem->maxsize,
		mem->align,
		mem->offset,
		mem->size
	);

	return (GstMemory *)sub;
}


static gboolean gst_vmeta_allocator_is_span(GstMemory *mem1, GstMemory *mem2, gsize *offset)
{
	GstVmetaMemory *vmeta_mem1 = (GstVmetaMemory *)mem1;
	GstVmetaMemory *vmeta_mem2 = (GstVmetaMemory *)mem2;

	if (offset != NULL)
	{
		if (vmeta_mem1->mem.parent != NULL)
			*offset = vmeta_mem1->mem.offset - vmeta_mem1->mem.parent->offset;
		else
			*offset = vmeta_mem1->mem.offset;
	}

	return ((guint8*)(vmeta_mem1->virt_addr) + vmeta_mem1->mem.offset + vmeta_mem1->mem.size) == ((guint8*)(vmeta_mem2->virt_addr) + vmeta_mem2->mem.offset);
}

