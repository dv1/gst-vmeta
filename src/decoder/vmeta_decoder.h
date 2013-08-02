/* vMeta video decoder plugin
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


#ifndef VMETA_DECODER_H
#define VMETA_DECODER_H

#include <glib.h>
#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include <codecVC.h>


G_BEGIN_DECLS


typedef struct _GstVmetaDec GstVmetaDec;
typedef struct _GstVmetaDecClass GstVmetaDecClass;


#define GST_TYPE_VMETA_DEC             (gst_vmeta_dec_get_type())
#define GST_VMETA_DEC(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VMETA_DEC, GstVmetaDec))
#define GST_VMETA_DEC_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VMETA_DEC, GstVmetaDecClass))
#define GST_IS_VMETA_DEC(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_VMETA_DEC))
#define GST_IS_VMETA_DEC_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_VMETA_DEC))


struct _GstVmetaDec
{
	GstVideoDecoder parent;

	MiscGeneralCallbackTable *callback_table;
	IppVmetaDecParSet dec_param_set;
	IppVmetaDecInfo dec_info;
	void *dec_state;
	gboolean is_suspended;

	GList *streams, *streams_available, *streams_ready;

	gboolean upload_before_loop;

	GstBuffer *codec_data;
};


struct _GstVmetaDecClass
{
	GstVideoDecoderClass parent_class;
};


GType gst_vmeta_dec_get_type(void);


G_END_DECLS


#endif

