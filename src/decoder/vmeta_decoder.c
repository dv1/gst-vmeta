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


#include <config.h>
#include <string.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <vdec_os_api.h>
#include <misc.h>
#include "vmeta_decoder.h"
#include "../common/vmeta_bufferpool.h"



/* Marvell's vMeta is a hardware video engine for en/decoding.
 * 
 * While there is a direct vMeta API for it, it is undocumented, no example code exists for it,
 * and many essential constants are missing. Instead, the video engine is controlled using
 * the IPP API, which is placed on top of the vMeta one.
 *
 * Data transmission from/to the engine is done using DMA buffers, allocated with the
 * vdec_os_api_dma_alloc* calls. There are two types of DMA buffers: pictures and streams.
 * Since GstBuffers, DMA buffers etc. can be easily confused, the following terminology is established:
 * - DMA buffer: Memory block allocate with the vdec_os_api_dma_alloc* calls. There is a virtual and a physical
 *   address for each DMA buffer.
 * - Picture: An IPP struct which points to a DMA buffer that is used for unencoded frames.
 * - Streams: An IPP struct which points to a DMA buffer that is used for encoded data.
 * - GstBuffer: GStreamer buffer structure. Contains one or more GstMemory instances, which in turn
 *   contain the actual memory blocks.
 * - Picture buffer: GstBuffer with one GstMemory block. This GstMemory block wraps a picture.
 * - Stream buffer: GstBuffer with one GstMemory block. This GstMemory block wraps a stream.
 *
 * Since it is not possible to pass GstBuffers to the video engine, a trick is used. The picture and stream
 * structs contain user data pointers. The first user data pointer is set to point to the GstBuffer.
 * Then, the picture or stream can be sent to vMeta. Once a picture or streams comes out of vMeta, its
 * associated GstBuffer can be retrieved by looking at the first user data pointer.
 * The reverse also exists: the GstBuffer structure used to wrap pictures and streams also contains metadata of
 * type GstVmetaBufferMeta, which in turn contains fields for storing picture and stream structs.
 * This way, it becomes possible to find out the picture or stream associated with a GstBuffer, and vice-versa.
 * The gst_vmeta_dec_get_buffer_from_ipp_picture() and gst_vmeta_dec_get_ipp_picture_from_buffer() functions
 * exist for this very purpose.
 *
 * For picture buffers, a custom GStreamer buffer pool is used, which in turn uses a custom allocator.
 * This makes sure the decoder does not have to memcpy decoded frames when pushing them downstream.
 *
 * Streams do not use a GStreamer buffer pool, since these require all buffers to be of the same size, which cannot
 * be guaranteed for streams. instead, they are stored in three GLists. The first, "streams" always contains pointers
 * to all streams. It is iterated over to deallocate all streams during shutdown. The second, "streams_available",
 * contains all streams that can be used to fill in input data. The third, "streams_ready", contains all
 * streams which can be pushed to the video engine (they have been previously filled with input data).
 * 
 * TODO: Limit the picture buffer pool size.
 */



GST_DEBUG_CATEGORY_STATIC(vmetadec_debug);
#define GST_CAT_DEFAULT vmetadec_debug



/* Defines and utility macros */

#define ALIGN_VAL_TO(LENGTH, ALIGN_SIZE)  ( (((LENGTH) + (ALIGN_SIZE) - 1) / (ALIGN_SIZE)) * (ALIGN_SIZE) )
#define ALIGN_OFFSET(x, n)  ( (-(x)) & ((n) - 1) )
#define PADDED_SIZE(x) ALIGN_VAL_TO((x), 128)
#define PADDING_LEN(x) ALIGN_OFFSET((x), 128)
#define PADDING_BYTE 0x88          /* the vmeta decoder needs a padding of 0x88 at the end of a frame */
#define NUM_STREAMS 7
#define STREAM_VDECBUF_SIZE (512 * 1024U)     /* must equal to or greater than 64k and multiple of 128 */






/* The following formats are NOT supported:
 * WMV1 & 2 (aka wmv7 & 8)
 * h-263 (not supported by Dove; perhaps supported by other Marvell platforms?)
 */
static GstStaticPadTemplate static_sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		/* IPP_VIDEO_STRM_FMT_H264 */
		"video/x-h264, "
		"parsed = (boolean) true, "
		"stream-format = (string) byte-stream, "
		"alignment = (string) au, "
		"width = (int) [ 16, 2048 ], "
		"height = (int) [ 16, 2048 ], "
		"framerate = (fraction) [ 0, MAX ]; "

		/* IPP_VIDEO_STRM_FMT_MPG1 and IPP_VIDEO_STRM_FMT_MPG2 */
		"video/mpeg, "
		"parsed = (boolean) true, "
		"systemstream = (boolean) false, "
		"mpegversion = (int) { 1, 2 }, "
		"width = (int) [ 16, 2048 ], "
		"height = (int) [ 16, 2048 ], "
		"framerate = (fraction) [ 0, MAX ]; "

		/* IPP_VIDEO_STRM_FMT_MPG4 */
		/* xvid and divx are supported as MPEG-4 streams */
		"video/mpeg, "
		"parsed = (boolean) true, "
		"mpegversion = (int) 4, "
		"width = (int) [ 16, 2048 ], "
		"height = (int) [ 16, 2048 ], "
		"framerate = (fraction) [ 0, MAX ]; "

		/* IPP_VIDEO_STRM_FMT_VC1 and IPP_VIDEO_STRM_FMT_VC1M */
		/* WVC1 = VC1-AP (IPP_VIDEO_STRM_FMT_VC1) */
		/* WMV3 = VC1-SPMP (IPP_VIDEO_STRM_FMT_VC1M) */
		"video/x-wmv, "
		"wmvversion = (int) 3, "
		"format = (string) { WVC1, WMV3 }, "
		"width = (int) [ 16, 2048 ], "
		"height = (int) [ 16, 2048 ], "
		"framerate = (fraction) [ 0, MAX ]; "

		/* IPP_VIDEO_STRM_FMT_MJPG */
		"image/jpeg, "
		"width = (int) [ 16, 2048 ], "
		"height = (int) [ 16, 2048 ], "
		"framerate = (fraction) [ 0, MAX ]; "
	)
);

static GstStaticPadTemplate static_src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS(
		"video/x-raw,"
		"format = (string) UYVY, "
		"width = (int) [ 16, 2048 ], "
		"height = (int) [ 16, 2048 ], "
		"framerate = (fraction) [ 0, MAX ]"
	)
);


G_DEFINE_TYPE(GstVmetaDec, gst_vmeta_dec, GST_TYPE_VIDEO_DECODER)



/* miscellaneous */
static gchar const * gst_vmeta_dec_strstatus(IppCodecStatus status);
static void gst_vmeta_dec_free_decoder(GstVmetaDec *vmeta_dec);
static gboolean gst_vmeta_dec_fill_param_set(GstVmetaDec *vmeta_dec, GstVideoCodecState *state, GstBuffer **codec_data);
static gboolean gst_vmeta_dec_suspend_and_resume(GstVmetaDec *vmeta_dec);
static gboolean gst_vmeta_dec_suspend(GstVmetaDec *vmeta_dec, gboolean suspend);

/* list helper function */
static void gst_vmeta_dec_push_to_list(GList **list, gpointer data);
static gpointer gst_vmeta_dec_pop_from_list(GList **list);

/* stream buffer functions */
static gboolean gst_vmeta_dec_copy_to_stream(GstVmetaDec *vmeta_dececoder, IppVmetaBitstream *stream, guint8 *in_data, gsize in_size);
static gboolean gst_vmeta_dec_return_stream_buffers(GstVmetaDec *vmeta_dec);

/* picture buffer functions */
static gboolean gst_vmeta_dec_return_picture_buffers(GstVmetaDec *vmeta_dec);
static IppVmetaPicture* gst_vmeta_dec_get_ipp_picture_from_buffer(GstVmetaDec *vmeta_decoder, GstBuffer *buffer);
static GstBuffer* gst_vmeta_dec_get_buffer_from_ipp_picture(GstVmetaDec *vmeta_decoder, IppVmetaPicture *picture);

/* functions for the base class */
static gboolean gst_vmeta_dec_start(GstVideoDecoder *decoder);
static gboolean gst_vmeta_dec_stop(GstVideoDecoder *decoder);
static gboolean gst_vmeta_dec_set_format(GstVideoDecoder *decoder, GstVideoCodecState *state);
static GstFlowReturn gst_vmeta_dec_handle_frame(GstVideoDecoder *decoder, GstVideoCodecFrame *frame);
static gboolean gst_vmeta_dec_reset(GstVideoDecoder *decoder, gboolean hard);
static gboolean gst_vmeta_dec_decide_allocation(GstVideoDecoder *decoder, GstQuery *query);
static GstStateChangeReturn gst_vmeta_dec_change_state(GstElement * element, GstStateChange transition);




/* required function declared by G_DEFINE_TYPE */

void gst_vmeta_dec_class_init(GstVmetaDecClass *klass)
{
	GstVideoDecoderClass *base_class;
	GstElementClass *element_class;

	GST_DEBUG_CATEGORY_INIT(vmetadec_debug, "vmetadec", 0, "Marvell vMeta video decoder");

	base_class = GST_VIDEO_DECODER_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);

	gst_element_class_set_static_metadata(
		element_class,
		"vMeta video decoder",
		"Codec/Decoder/Video",
		"hardware-accelerated video decoding using the Marvell vMeta engine",
		"Carlos Rafael Giani <dv@pseudoterminal.org>"
	);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_sink_template));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_src_template));

	base_class->start             = GST_DEBUG_FUNCPTR(gst_vmeta_dec_start);
	base_class->stop              = GST_DEBUG_FUNCPTR(gst_vmeta_dec_stop);
	base_class->set_format        = GST_DEBUG_FUNCPTR(gst_vmeta_dec_set_format);
	base_class->handle_frame      = GST_DEBUG_FUNCPTR(gst_vmeta_dec_handle_frame);
	base_class->reset             = GST_DEBUG_FUNCPTR(gst_vmeta_dec_reset);
	base_class->decide_allocation = GST_DEBUG_FUNCPTR(gst_vmeta_dec_decide_allocation);
	element_class->change_state   = GST_DEBUG_FUNCPTR(gst_vmeta_dec_change_state);
}


void gst_vmeta_dec_init(GstVmetaDec *vmeta_dec)
{
	vmeta_dec->callback_table = NULL;
	vmeta_dec->dec_state = NULL;
	vmeta_dec->is_suspended = FALSE;

	vmeta_dec->streams = NULL;
	vmeta_dec->streams_available = NULL;
	vmeta_dec->streams_ready = NULL;

	vmeta_dec->upload_before_loop = FALSE;

	vmeta_dec->codec_data = NULL;
}







/*****************/
/* miscellaneous */

static gchar const * gst_vmeta_dec_strstatus(IppCodecStatus status)
{
	switch (status)
	{
		case IPP_STATUS_INIT_ERR: return "initialization error";
		case IPP_STATUS_INIT_OK: return "initialization ok";
		case IPP_STATUS_BUFFER_UNDERRUN: return "buffer underrun";
		case IPP_STATUS_FRAME_COMPLETE: return "frame complete";
		case IPP_STATUS_BS_END: return "bs end";
		case IPP_STATUS_FRAME_ERR: return "frame error";
		case IPP_STATUS_FRAME_HEADER_INVALID: return "frame header invalid";
		case IPP_STATUS_FRAME_UNDERRUN: return "frame underrun";

		case IPP_STATUS_MP4_SHORTHEAD: return "mp4 short head";

		case IPP_STATUS_READEVENT: return "read event";

		case IPP_STATUS_DTMF_NOTSUPPORTEDFS: return "dtmf not supported fs";

		case IPP_STATUS_TIMEOUT_ERR: return "timeout";
		case IPP_STATUS_STREAMFLUSH_ERR: return "stream flush error";
		case IPP_STATUS_BUFOVERFLOW_ERR: return "buffer overflow";
		case IPP_STATUS_NOTSUPPORTED_ERR: return "not supported";
		case IPP_STATUS_MISALIGNMENT_ERR: return "misalignment";
		case IPP_STATUS_BITSTREAM_ERR: return "bitstream error";
		case IPP_STATUS_INPUT_ERR: return "input error";
		case IPP_STATUS_SYNCNOTFOUND_ERR: return "sync not found";
		case IPP_STATUS_BADARG_ERR: return "bad argument";
		case IPP_STATUS_NOMEM_ERR: return "no memory";
		case IPP_STATUS_ERR: return "unspecified error";
		case IPP_STATUS_NOERR: return "no error";

		case IPP_STATUS_NOTSUPPORTED: return "not supported";

		case IPP_STATUS_JPEG_EOF: return "jpeg EOF";
		case IPP_STATUS_JPEG_CONTINUE: return "jpeg continue";
		case IPP_STATUS_OUTPUT_DATA: return "output data";
		case IPP_STATUS_NEED_INPUT: return "need input";

		case IPP_STATUS_NEW_VIDEO_SEQ: return "new video sequence";
		case IPP_STATUS_BUFFER_FULL: return "buffer full";

		case IPP_STATUS_GIF_FINISH: return "gif finish";
		case IPP_STATUS_GIF_MORE: return "gif more";
		case IPP_STATUS_GIF_NOIMAGE: return "gif no image";

		case IPP_STATUS_FATAL_ERR: return "fatal error";
		case IPP_STATUS_FIELD_PICTURE_TOP: return "field picture top";
		case IPP_STATUS_FIELD_PICTURE_BOTTOM: return "field picture bottom";

		case IPP_STATUS_NEED_OUTPUT_BUF: return "need output buffer";
		case IPP_STATUS_RETURN_INPUT_BUF: return "return input buffer";
		case IPP_STATUS_END_OF_STREAM: return "end of stream";
		case IPP_STATUS_WAIT_FOR_EVENT: return "wait for event";
		case IPP_STATUS_END_OF_PICTURE: return "end of picture";

		default: return "<unknown>";
	}
}


static void gst_vmeta_dec_free_decoder(GstVmetaDec *vmeta_dec)
{
	if (vmeta_dec->dec_state == NULL)
		return;

	DecodeSendCmd_Vmeta(IPPVC_STOP_DECODE_STREAM, NULL, NULL, vmeta_dec->dec_state);
	gst_vmeta_dec_reset(GST_VIDEO_DECODER(vmeta_dec), TRUE);
	DecoderFree_Vmeta(&(vmeta_dec->dec_state));
	vmeta_dec->dec_state = NULL;
}


static gboolean gst_vmeta_dec_fill_param_set(GstVmetaDec *vmeta_dec, GstVideoCodecState *state, GstBuffer **codec_data)
{
	guint structure_nr;
	gboolean format_set;
	gboolean do_codec_data = FALSE;

	memset(&(vmeta_dec->dec_param_set), 0, sizeof(IppVmetaDecParSet));

	for (structure_nr = 0; structure_nr < gst_caps_get_size(state->caps); ++structure_nr)
	{
		GstStructure *s;
		gchar const *name;

		format_set = TRUE;
		s = gst_caps_get_structure(state->caps, structure_nr);
		name = gst_structure_get_name(s);

		if (g_strcmp0(name, "video/x-h264") == 0)
		{
			vmeta_dec->dec_param_set.strm_fmt = IPP_VIDEO_STRM_FMT_H264;
			GST_INFO_OBJECT(vmeta_dec, "setting h.264 as stream format");
		}
		else if (g_strcmp0(name, "video/mpeg") == 0)
		{
			gint mpegversion;
			if (gst_structure_get_int(s, "mpegversion", &mpegversion))
			{
				gboolean is_systemstream;
				switch (mpegversion)
				{
					case 1:
					case 2:
						if (gst_structure_get_boolean(s, "systemstream", &is_systemstream) && !is_systemstream)
						{
							vmeta_dec->dec_param_set.strm_fmt = (mpegversion == 1) ? IPP_VIDEO_STRM_FMT_MPG1 : IPP_VIDEO_STRM_FMT_MPG2;
						}
						else
						{
							GST_WARNING_OBJECT(vmeta_dec, "MPEG-%d system stream is not supported", mpegversion);
							format_set = FALSE;
						}
						break;
					case 4:
						vmeta_dec->dec_param_set.strm_fmt = IPP_VIDEO_STRM_FMT_MPG4;
						break;
					default:
						GST_WARNING_OBJECT(vmeta_dec, "unsupported MPEG version: %d", mpegversion);
						format_set = FALSE;
						break;
				}

				if (format_set)
					GST_INFO_OBJECT(vmeta_dec, "setting MPEG-%d as stream format", mpegversion);
			}

			do_codec_data = TRUE;
		}
		else if (g_strcmp0(name, "video/x-wmv") == 0)
		{
			gint wmvversion;
			gchar const *format_str;

			if (!gst_structure_get_int(s, "wmvversion", &wmvversion))
			{
				GST_WARNING_OBJECT(vmeta_dec, "wmvversion caps is missing");
				format_set = FALSE;
				break;
			}
			if (wmvversion != 3)
			{
				GST_WARNING_OBJECT(vmeta_dec, "unsupported WMV version %d (only version 3 is supported)", wmvversion);
				format_set = FALSE;
				break;
			}

			format_str = gst_structure_get_string(s, "format");
			if ((format_str == NULL) || g_str_equal(format_str, "WMV3"))
			{
				GST_INFO_OBJECT(vmeta_dec, "setting VC1M (= WMV3, VC1-SPMP) as stream format");
				vmeta_dec->dec_param_set.strm_fmt = IPP_VIDEO_STRM_FMT_VC1M;
			}
			else if (g_str_equal(format_str, "WVC1"))
			{
				GST_INFO_OBJECT(vmeta_dec, "setting VC1 (= WVC1, VC1-AP) as stream format");
				vmeta_dec->dec_param_set.strm_fmt = IPP_VIDEO_STRM_FMT_VC1;
			}
			else
			{
				GST_WARNING_OBJECT(vmeta_dec, "unsupported WMV format \"%s\"", format_str);
				format_set = FALSE;
			}

			do_codec_data = TRUE;
		}
		else if (g_strcmp0(name, "image/jpeg") == 0)
		{
			vmeta_dec->dec_param_set.strm_fmt = IPP_VIDEO_STRM_FMT_MJPG;
			GST_INFO_OBJECT(vmeta_dec, "setting Motion JPEG as stream format");
		}
		else
		{
			GST_WARNING_OBJECT(vmeta_dec, "unrecognized caps \"%s\"", name);
			format_set = FALSE;
		}

		if  (format_set)
		{
			if (do_codec_data)
			{
				GValue const *value = gst_structure_get_value(s, "codec_data");
				if (value != NULL)
				{
					GST_INFO_OBJECT(vmeta_dec, "codec data expected and found in caps");
					*codec_data = gst_value_get_buffer(value);
				}
				else
				{
					GST_WARNING_OBJECT(vmeta_dec, "codec data expected, but not found in caps");
					format_set = FALSE;
				}
			}

			break;
		}
	}

	if (!format_set)
		return FALSE;

	vmeta_dec->dec_param_set.opt_fmt = IPP_YCbCr422I;
	vmeta_dec->dec_param_set.no_reordering = 0;
	vmeta_dec->dec_param_set.bMultiIns = 0;
	vmeta_dec->dec_param_set.bFirstUser = 0;

	return TRUE;
}


static gboolean gst_vmeta_dec_suspend_and_resume(GstVmetaDec *vmeta_dec)
{
	IppCodecStatus ret;

	/* According to Marvell's GStreamer 0.10 plugins, These steps are necessary
	 * after a frame was completed when using Dove hardware
	 * TODO: is this really necessary? */
	if (!vdec_os_api_suspend_check())
		return TRUE;

	ret = DecodeSendCmd_Vmeta(IPPVC_PAUSE, NULL, NULL, vmeta_dec->dec_state);
	if (ret != IPP_STATUS_NOERR)
	{
		GST_ERROR_OBJECT(vmeta_dec, "pausing failed : %s", gst_vmeta_dec_strstatus(ret));
		return FALSE;
	}

	vdec_os_api_suspend_ready();

	ret = DecodeSendCmd_Vmeta(IPPVC_RESUME, NULL, NULL, vmeta_dec->dec_state);
	if (ret != IPP_STATUS_NOERR)
	{
		GST_ERROR_OBJECT(vmeta_dec, "resuming failed : %s", gst_vmeta_dec_strstatus(ret));
		return FALSE;
	}

	return TRUE;
}


/* TODO:
 * For hardware other than Marvell's Dove platform, Marvell's GStreamer 0.10 plugins
 * suspend and resume the hardware in the PAUSED<->PLAYING state changes.
 * It is unclear why it isn't used with Dove. Initial tests with Dove didn't show any
 * issues. Still, keeping this code disabled for now until the reason for this
 * exclusion can be found.
 *
 * Not to be confused with the gst_vmeta_dec_suspend_and_resume() function above,
 * which *is* necessary for Dove.
 */
#if 0
static gboolean gst_vmeta_dec_suspend(GstVmetaDec *vmeta_dec, gboolean suspend)
{
	IppCodecStatus ret;

	if (vmeta_dec->dec_state == NULL)
		return TRUE;

	if (vmeta_dec->is_suspended == suspend)
	{
		GST_DEBUG_OBJECT(vmeta_dec, "decoder is already %s - ignoring call", suspend ? "suspended" : "resumed");
		return TRUE;
	}

	GST_DEBUG_OBJECT(vmeta_dec, "%s vMeta decoder", suspend ? "suspending" : "resuming");

	ret = DecodeSendCmd_Vmeta(suspend ? IPPVC_PAUSE : IPPVC_RESUME, NULL, NULL, vmeta_dec->dec_state);
	if (ret != IPP_STATUS_NOERR)
	{
		GST_ERROR_OBJECT(vmeta_dec, "could not %s decoder: %s", suspend ? "suspend" : "resume", gst_vmeta_dec_strstatus(ret));
		return FALSE;
	}

	vmeta_dec->is_suspended = suspend;

	return TRUE;
}
#else
static gboolean gst_vmeta_dec_suspend(G_GNUC_UNUSED GstVmetaDec *vmeta_dec, G_GNUC_UNUSED gboolean suspend)
{
	return TRUE;
}
#endif




/************************/
/* list helper function */

static void gst_vmeta_dec_push_to_list(GList **list, gpointer data)
{
	*list = g_list_append(*list, data);
}


static gpointer gst_vmeta_dec_pop_from_list(GList **list)
{
	GList *llink;
	llink = g_list_first(*list);
	if (llink == NULL)
	{
		return NULL;
	}
	else
	{
		gpointer data = llink->data;
		*list = g_list_delete_link(*list, llink);
		return data;
	}
}




/***************************/
/* stream buffer functions */

static gboolean gst_vmeta_dec_copy_to_stream(GstVmetaDec *vmeta_dec, IppVmetaBitstream *stream, guint8 *in_data, gsize in_size)
{
	unsigned int num_padding, extra_bytes, offset, in_size_total, codec_data_size;
	gboolean add_vc1_code;

	extra_bytes = 0;
	offset = 0;
	codec_data_size = 0;

	/* the VC1 frame start code is optional, but vMeta requires it.
	 * In case the input data is a VC1 stream, and there is no frame start code present,
	 * make room for one.
	 */

	add_vc1_code = (vmeta_dec->dec_param_set.strm_fmt == IPP_VIDEO_STRM_FMT_VC1) && ((in_size < 3) || (in_data[0] != 0) || (in_data[1] != 0) || (in_data[2] != 1));
	if (add_vc1_code)
		extra_bytes += 4;

	/* In case there is codec_data, make room for it.
	 * This is done only for the first frame; afterwards, codec_data is NULL. */
	if (vmeta_dec->codec_data != NULL)
	{
		codec_data_size = gst_buffer_get_size(vmeta_dec->codec_data);
		extra_bytes += codec_data_size;
	}

	/* Total size for the stream, including extra bytes added above */
	in_size_total = in_size + extra_bytes;

	GST_DEBUG_OBJECT(vmeta_dec, "VC1 start code: %s", add_vc1_code ? "yes" : "no");

	/* If the stream is not big enough (including padding), enlarge it */
	if (PADDED_SIZE(in_size_total) > stream->nBufSize)
	{
		/* The stream's DMA buffer size must always be aligned to 64kB boundaries */
		unsigned int new_buf_size = ALIGN_VAL_TO(in_size_total, 65536) + 65536;

		GST_DEBUG_OBJECT(
			vmeta_dec,
			"need to stream buffer: necessary stream buffer size: %u  current size: %u",
			PADDED_SIZE(in_size_total),
			stream->nBufSize
		);

		vdec_os_api_dma_free(stream->pBuf);
		stream->pBuf = vdec_os_api_dma_alloc_writecombine(new_buf_size, VMETA_STRM_BUF_ALIGN, &(stream->nPhyAddr));
		stream->nBufSize = new_buf_size;
		stream->nDataLen = 0;

		if (stream->pBuf == NULL)
		{
			GST_ERROR_OBJECT(vmeta_dec, "reallocating stream buffer failed");
			stream->nBufSize = 0;
			return FALSE;
		}
	}

	/* In case there is codec data, copy it over to the stream.
	 * This is done only for the first frame; after copying, the codec_data
	 * buffer is unref'd, and codec_data is set to NULL. */
	if (vmeta_dec->codec_data != NULL)
	{
		GstMapInfo in_map_info;

		gst_buffer_map(vmeta_dec->codec_data, &in_map_info, GST_MAP_READ);
		memcpy(stream->pBuf + offset, in_map_info.data, in_map_info.size);
		gst_buffer_unmap(vmeta_dec->codec_data, &in_map_info);

		offset += codec_data_size;

		gst_buffer_unref(vmeta_dec->codec_data);
		vmeta_dec->codec_data = NULL;
	}

	/* For VC1 streams, copy over the start frame code */
	if (add_vc1_code)
	{
		static guint8 const VC1FrameStartCode[4] = {0, 0, 1, 0xd};
		stream->pBuf[offset + 0] = VC1FrameStartCode[0];
		stream->pBuf[offset + 1] = VC1FrameStartCode[1];
		stream->pBuf[offset + 2] = VC1FrameStartCode[2];
		stream->pBuf[offset + 3] = VC1FrameStartCode[3];
		offset += 4;
	}

	/* Copy over the input frame data */
	memcpy(stream->pBuf + offset, in_data, in_size);

	stream->nDataLen = in_size_total;
	stream->nFlag = IPP_VMETA_STRM_BUF_END_OF_UNIT; /* Necessary flag for vMeta input */

	/* vMeta requires padded bytes to be of value 0x88
	 * (which is the value of PADDING_BYTE) */
	num_padding = PADDING_LEN(in_size_total);
	if (num_padding > 0)
		memset(stream->pBuf + in_size_total, PADDING_BYTE, num_padding);

	return TRUE;
}


static gboolean gst_vmeta_dec_return_stream_buffers(GstVmetaDec *vmeta_dec)
{
	IppCodecStatus ret;
	IppVmetaBitstream *stream;

	while (TRUE)
	{
		ret = DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, (void **)(&stream), vmeta_dec->dec_state);
		if (ret != IPP_STATUS_NOERR)
		{
			GST_ERROR_OBJECT(vmeta_dec, "failed to pop stream : %s", gst_vmeta_dec_strstatus(ret));
			return FALSE;
		}

		if (stream == NULL)
		{
			GST_LOG_OBJECT(vmeta_dec, "popped NULL stream");
			break;
		}

		GST_LOG_OBJECT(vmeta_dec, "popped stream %p", stream);

		stream->nDataLen = 0;
		gst_vmeta_dec_push_to_list(&(vmeta_dec->streams_available), (gpointer)stream);
	}

	return TRUE;
}




/****************************/
/* picture buffer functions */

static gboolean gst_vmeta_dec_return_picture_buffers(GstVmetaDec *vmeta_dec)
{
	IppCodecStatus ret;
	IppVmetaPicture *picture;
	GstBuffer *picture_buffer;

	while (TRUE)
	{
		ret = DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, (void **)(&picture), vmeta_dec->dec_state);
		if (ret != IPP_STATUS_NOERR)
		{
			GST_ERROR_OBJECT(vmeta_dec, "popping picture failed : %s", gst_vmeta_dec_strstatus(ret));
			return FALSE;
		}

		if (picture == NULL)
		{
			GST_LOG_OBJECT(vmeta_dec, "popped NULL picture");
			break;
		}

		picture_buffer = gst_vmeta_dec_get_buffer_from_ipp_picture(vmeta_dec, picture);
		if (picture_buffer != NULL)
		{
			GST_LOG_OBJECT(vmeta_dec, "popped picture %p (gstreamer buffer %p)", picture, picture_buffer);
			gst_buffer_unref(picture_buffer);
		}
		else
			GST_LOG_OBJECT(vmeta_dec, "popped picture %p (no gstreamer buffer)", picture);
	}

	return TRUE;
}


static IppVmetaPicture* gst_vmeta_dec_get_ipp_picture_from_buffer(GstVmetaDec *vmeta_decoder, GstBuffer *buffer)
{
	GstVmetaBufferMeta *meta = GST_VMETA_BUFFER_META_GET(buffer);
	if (meta == NULL)
	{
		GST_ERROR_OBJECT(vmeta_decoder, "picture buffer has no vMeta metadata");
		return NULL;
	}

	return (IppVmetaPicture *)(meta->mvl_ipp_data);
}


static GstBuffer* gst_vmeta_dec_get_buffer_from_ipp_picture(GstVmetaDec *vmeta_decoder, IppVmetaPicture *picture)
{
	if (picture->pUsrData0 == NULL)
	{
		GST_ERROR_OBJECT(vmeta_decoder, "IPP picture %p is not associated with a gst buffer", picture);
		return NULL;
	}

	return (GstBuffer *)(picture->pUsrData0);
}




/********************************/
/* functions for the base class */

static gboolean gst_vmeta_dec_start(GstVideoDecoder *decoder)
{
	int i;
	GstVmetaDec *vmeta_dec = GST_VMETA_DEC(decoder);

	GST_LOG_OBJECT(vmeta_dec, "starting decoder");

	if (miscInitGeneralCallbackTable(&(vmeta_dec->callback_table)) != 0)
	{
		GST_ERROR_OBJECT(vmeta_dec, "could not initialize callback table");
		return FALSE;
	}

	/* Preallocate streams and fill "streams" and "streams_available" GLists */
	for (i = 0; i < NUM_STREAMS; ++i)
	{
		IppVmetaBitstream *stream = (IppVmetaBitstream *)g_try_malloc(sizeof(IppVmetaBitstream));
		if (stream == NULL)
		{
			GST_ERROR_OBJECT(vmeta_dec, "failed to allocate stream");
			return FALSE;
		}

		memset(stream, 0, sizeof(IppVmetaBitstream));
		stream->pBuf = (Ipp8u *)vdec_os_api_dma_alloc_writecombine(STREAM_VDECBUF_SIZE, VMETA_STRM_BUF_ALIGN, &(stream->nPhyAddr));
		stream->nBufSize = STREAM_VDECBUF_SIZE;
		stream->nDataLen = 0;

		if (stream->pBuf == NULL)
		{
			GST_ERROR_OBJECT(vmeta_dec, "allocating stream buffer failed");
			g_free(stream);
			stream->nBufSize = 0;
			return FALSE;
		}

		vmeta_dec->streams = g_list_append(vmeta_dec->streams, stream);
		vmeta_dec->streams_available = g_list_append(vmeta_dec->streams_available, stream);
	}

	/* The decoder is initialized in set_format, not here, since only then the input bitstream
	 * format is known (and this information is necessary for initialization).
	 * Also, streams can be allocated before the decoder is initialized, since the allocator
	 * does not depend on it. */

	return TRUE;
}


static gboolean gst_vmeta_dec_stop(GstVideoDecoder *decoder)
{
	GstVmetaDec *vmeta_dec = GST_VMETA_DEC(decoder);

	GST_LOG_OBJECT(vmeta_dec, "stopping decoder");

	/* First free the decoder, BEFORE freeing the DMA buffers */
	gst_vmeta_dec_free_decoder(vmeta_dec);

	if (vmeta_dec->callback_table != NULL)
	{
		miscFreeGeneralCallbackTable(&(vmeta_dec->callback_table));
		vmeta_dec->callback_table = NULL;
	}

	/* Free the stream DMA buffers */
	{
		GList *elem;

		for (elem = vmeta_dec->streams; elem != NULL; elem = elem->next)
		{
			IppVmetaBitstream *stream = (IppVmetaBitstream *)(elem->data);
			if (stream->pBuf != NULL)
				vdec_os_api_dma_free(stream->pBuf);
			g_free(stream);
		}

		g_list_free(vmeta_dec->streams);
		g_list_free(vmeta_dec->streams_available);
		g_list_free(vmeta_dec->streams_ready);

		vmeta_dec->streams = NULL;
		vmeta_dec->streams_available = NULL;
		vmeta_dec->streams_ready = NULL;
	}

	if (vmeta_dec->codec_data != NULL)
	{
		gst_buffer_unref(vmeta_dec->codec_data);
		vmeta_dec->codec_data = NULL;
	}

	return TRUE;
}


static gboolean gst_vmeta_dec_set_format(GstVideoDecoder *decoder, GstVideoCodecState *state)
{
	IppCodecStatus ret;
	GstBuffer *codec_data = NULL;
	GstVmetaDec *vmeta_dec = GST_VMETA_DEC(decoder);

	GST_LOG_OBJECT(vmeta_dec, "setting new format");

	if (vmeta_dec->dec_state != NULL)
		gst_vmeta_dec_free_decoder(vmeta_dec);

	memset(&(vmeta_dec->dec_info), 0, sizeof(IppVmetaDecInfo));

	/* codec_data does not need to be unref'd after use; it is owned by the caps structure */
	if (!gst_vmeta_dec_fill_param_set(vmeta_dec, state, &codec_data))
	{
		GST_ERROR_OBJECT(vmeta_dec, "could not fill open params: state info incompatible");
		return FALSE;
	}

	/* The actual initialization; requires bitstream information (such as the codec type), which
	 * is determined by the fill_param_set call before */
	ret = DecoderInitAlloc_Vmeta(&(vmeta_dec->dec_param_set), vmeta_dec->callback_table, &(vmeta_dec->dec_state));
	if (ret != IPP_STATUS_NOERR)
	{
		GST_ERROR_OBJECT(vmeta_dec, "failed to initialize&alloc vMeta state : %s", gst_vmeta_dec_strstatus(ret));
		return FALSE;
	}

	gst_video_decoder_set_output_state(decoder, GST_VIDEO_FORMAT_UYVY, state->info.width, state->info.height, state);

	/* For WMV3, a special header has to be sent to the decoder first
	 * The codec_data buffer is consumed during this process */
	if (vmeta_dec->dec_param_set.strm_fmt == IPP_VIDEO_STRM_FMT_VC1M)
	{
		GstMapInfo codec_data_map;
		unsigned char* cdata;
		unsigned int csize;

		if (codec_data == NULL)
		{
			GST_ERROR_OBJECT(vmeta_dec, "WMV3/VC1-SPMP data without codec_data");
			return FALSE;
		}

		gst_buffer_map(codec_data, &codec_data_map, GST_MAP_READ);
		cdata = codec_data_map.data;
		csize = codec_data_map.size;

		vc1m_seq_header seq_header;
		seq_header.num_frames = 0xffffff;
		seq_header.vert_size = state->info.height;
		seq_header.horiz_size = state->info.width;
		seq_header.level = ((cdata[0] >> 4) == 4) ? 4 : 2;
		seq_header.cbr = 1;
		seq_header.hrd_buffer = 0x007fff;
		seq_header.hrd_rate = 0x00007fff;
		seq_header.frame_rate = 0xffffffff;
		memcpy(seq_header.exthdr, cdata, csize);
		seq_header.exthdrsize = csize;
		ret = DecodeSendCmd_Vmeta(IPPVC_SET_VC1M_SEQ_INFO, &seq_header, NULL, vmeta_dec->dec_state);

		gst_buffer_unmap(codec_data, &codec_data_map);

		/* codec_data buffer was used already ; make sure it is not sent again */
		codec_data = NULL;

		if (ret != IPP_STATUS_NOERR)
		{
			GST_ERROR_OBJECT(vmeta_dec, "failed to send WMV3/VC1-SPMP seq info to decoder: %s", gst_vmeta_dec_strstatus(ret));
			return FALSE;
		}
	}

	/* Copy the buffer, to make sure the codec_data lifetime does not depend on the caps */
	if (codec_data != NULL)
		vmeta_dec->codec_data = gst_buffer_copy(codec_data);

	return TRUE;
}


static GstFlowReturn gst_vmeta_dec_handle_frame(GstVideoDecoder *decoder, GstVideoCodecFrame *frame)
{
	IppCodecStatus ret;
	IppVmetaBitstream *stream;
	IppVmetaPicture *picture;
	gboolean decode_only, do_finish, run_decoding_loop, input_already_delivered, do_eos, picture_decoded;
	GstVmetaDec *vmeta_dec = GST_VMETA_DEC(decoder);


	/* The code in here orients itself towards the IPP_STATUS_NEED_INPUT status codes.
	 * The first time IPP_STATUS_NEED_INPUT is returned by DecodeFrame_Vmeta(), the input data
	 * from the frame parameter is pushed to the video engine. Then, the loop continues.
	 * During the loops, the decoder may request pictures, and return completed pictures.
	 * If more than one completed picture is returned for the input data, all but the first
	 * are dropped (this is a current GStreamer limitation; see below). The looping continues
	 * until either EOS or an error is reported, or IPP_STATUS_NEED_INPUT is returned again.
	 * Looping stops then.
	 *
	 * The idea behind this is that the handle_frame function is "input-oriented", that is,
	 * every time it is called, it means there is a new input frame to decode. So the code inside
	 * here does as much as possible with the input data until the video engine is done with it
	 * and requires new input data.
	 *
	 * The upload_before_loop boolean is tied to this. Initially, it is set to FALSE.
	 * The very first time handle_frame is called, DecodeFrame_Vmeta() has not been called yet.
	 * Then, in the first loop, DecodeFrame_Vmeta() is called, IPP_STATUS_NEED_INPUT is returned the
	 * first time. The input data is pushed to the video engine, the loop does all it can, until the
	 * second IPP_STATUS_NEED_INPUT status code is returned. upload_before_loop is set to TRUE,
	 * the loop exits, and so does the handle_frame function.
	 * The next time handle_frame is called, upload_before_loop is TRUE, and the loop immediately
	 * pushes the input data to the video engine, effectively omitting the first IPP_STATUS_NEED_INPUT
	 * status code. This means upload_before_loop is FALSE only before the first handle_frame, and TRUE
	 * afterwards. This mechanism prevents unnecessary DecodeFrame_Vmeta() calls.
	 */


	/* Convenience macros */
#define PUSH_AVAILABLE_STREAM()   gst_vmeta_dec_push_to_list(&(vmeta_dec->streams_available),  (gpointer)stream)
#define PUSH_READY_STREAM()       gst_vmeta_dec_push_to_list(&(vmeta_dec->streams_ready),      (gpointer)stream)

#define POP_AVAILABLE_STREAM() \
	do { \
		stream = gst_vmeta_dec_pop_from_list(&(vmeta_dec->streams_available)); \
		if (stream == NULL) \
		{ \
			GST_ERROR_OBJECT(vmeta_dec, "no streams available"); \
			return GST_FLOW_ERROR; \
		} \
	} while (0)

#define POP_READY_STREAM() \
	do { \
		stream = gst_vmeta_dec_pop_from_list(&(vmeta_dec->streams_ready)); \
		if (stream == NULL) \
		{ \
			GST_ERROR_OBJECT(vmeta_dec, "no streams ready"); \
			return GST_FLOW_ERROR; \
		} \
	} while (0)


	/* Prepare a stream containing the input data (if there is input data) */
	if (frame->input_buffer != NULL)
	{
		gboolean copy_ok;
		GstMapInfo in_map_info;

		POP_AVAILABLE_STREAM();
		gst_buffer_map(frame->input_buffer, &in_map_info, GST_MAP_READ);
		copy_ok = gst_vmeta_dec_copy_to_stream(vmeta_dec, stream, in_map_info.data, in_map_info.size);
		gst_buffer_unmap(frame->input_buffer, &in_map_info);

		if (copy_ok)
			PUSH_READY_STREAM();
		else
		{
			stream->nDataLen = 0;
			PUSH_AVAILABLE_STREAM();
			GST_ERROR_OBJECT(vmeta_dec, "failed to upload input data to stream buffer");
			return GST_FLOW_ERROR;
		}
	}

	GST_LOG_OBJECT(vmeta_dec, "upload before running decode loop: %s", vmeta_dec->upload_before_loop ? "yes" : "no");

	if (vmeta_dec->upload_before_loop)
	{
		/* video engine requires more input, but nothing is coming anymore -> signal EOS */
		if (frame->input_buffer == NULL)
		{
			GST_INFO_OBJECT(vmeta_dec, "NULL input buffer received -> signaling EOS");
			return GST_FLOW_EOS;
		}

		POP_READY_STREAM();
		ret = DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, stream, vmeta_dec->dec_state);
		if (ret != IPP_STATUS_NOERR)
		{
			stream->nDataLen = 0;
			PUSH_AVAILABLE_STREAM();
			GST_ERROR_OBJECT(vmeta_dec, "failed to push stream buffer : %s", gst_vmeta_dec_strstatus(ret));
			return GST_FLOW_ERROR;
		}

		vmeta_dec->upload_before_loop = FALSE;
		input_already_delivered = TRUE;
	}
	else
		input_already_delivered = FALSE;

	decode_only = TRUE;
	do_finish = FALSE;
	run_decoding_loop = TRUE;
	do_eos = FALSE;
	picture_decoded = FALSE;

	while (run_decoding_loop)
	{
		ret = DecodeFrame_Vmeta(&(vmeta_dec->dec_info), vmeta_dec->dec_state);
		GST_LOG_OBJECT(vmeta_dec, "DecodeFrame_Vmeta() returned code %d (%s)", (gint)(ret), gst_vmeta_dec_strstatus(ret));
		switch (ret)
		{
			/* TODO:
			 * there are two status codes, IPP_STATUS_END_OF_PICTURE and
			 * IPP_STATUS_END_OF_STREAM , which never ever are returned by the
			 * DecodeFrame_Vmeta() function. What are these? */

			case IPP_STATUS_NEED_INPUT:
			{
				if (input_already_delivered)
				{
					/* the input has already been delivered
					 * -> exit, and wait until handle_frame() is called again, with
					 * new input; the block before the main loop then uploads the input */
					vmeta_dec->upload_before_loop = TRUE;
					run_decoding_loop = FALSE;
				}
				else
				{
					/* video engine requires more input, but nothing is coming anymore -> signal EOS */
					if (frame->input_buffer == NULL)
					{
						GST_INFO_OBJECT(vmeta_dec, "NULL input buffer received -> signaling EOS");
						return GST_FLOW_EOS;
					}

					POP_READY_STREAM();
					ret = DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, stream, vmeta_dec->dec_state);
					if (ret != IPP_STATUS_NOERR)
					{
						stream->nDataLen = 0;
						PUSH_AVAILABLE_STREAM();
						GST_ERROR_OBJECT(vmeta_dec, "failed to push stream buffer : %s", gst_vmeta_dec_strstatus(ret));
						return GST_FLOW_ERROR;
					}

					input_already_delivered = TRUE;
				}
				break;
			}
			case IPP_STATUS_RETURN_INPUT_BUF:
			{
				if (!gst_vmeta_dec_return_stream_buffers(vmeta_dec))
					return GST_FLOW_ERROR;
				break;
			}
			case IPP_STATUS_FRAME_COMPLETE:
			{
				ret = DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, (void **)(&picture), vmeta_dec->dec_state);
				if (ret != IPP_STATUS_NOERR)
				{
					GST_ERROR_OBJECT(vmeta_dec, "failed to pop picture : %s", gst_vmeta_dec_strstatus(ret));
					return GST_FLOW_ERROR;
				}

				/* DecoderPopBuffer_Vmeta() sometimes returns NULL after a completed frame.
				 * When this happens, this NULL frame has to be ignored. Return stream buffers
				 * and suspend-resume as usual, but that's it. The next frame returns non-NULL. */
				if (picture != NULL)
				{
					GstBuffer *picture_buffer;

					GST_LOG_OBJECT(vmeta_dec, "pic type: %u coded type: %d %d poc: %d %d offset: %u datalen: %u bufsize: %u", picture->PicDataInfo.pic_type, picture->PicDataInfo.coded_type[0], picture->PicDataInfo.coded_type[1], picture->PicDataInfo.poc[0], picture->PicDataInfo.poc[1], picture->nOffset, picture->nDataLen, picture->nBufSize);

					picture_buffer = gst_vmeta_dec_get_buffer_from_ipp_picture(vmeta_dec, picture);
					if (picture_buffer == NULL)
					{
						GST_ERROR_OBJECT(vmeta_dec, "IPP picture %p is not associated with a gstreamer buffer", picture);
						return GST_FLOW_ERROR;
					}

					GST_LOG_OBJECT(vmeta_dec, "popped picture %p (gstreamer buffer %p)", picture, picture_buffer);

					if (picture_decoded)
					{
						/*
						 * TODO: this is temporary
						 * currently, GStreamer cannot handle cases where one stream
						 * causes the decoder to produce more than one picture
						 * (the GstVideoDecoder base class would need a possibility to send
						 * more than one frame downstream)
						 * so far, this has only happened with h.264 MVC data; since GStreamer
						 * is currently also lacking proper MVC support, it is pointless to worry
						 * about how to send multiple output pictures downstream
						 * -> dropping extra pictures for now by returning them to the available
						 * picture list */
						GST_DEBUG_OBJECT(vmeta_dec, "more than one picture decoded for one stream - dropping additional picture to maintain 1:1 ratio");
						gst_buffer_unref(picture_buffer);
					}
					else
					{
						frame->output_buffer = picture_buffer;
						decode_only = FALSE;
						do_finish = TRUE;
						picture_decoded = TRUE;
					}
				}
				else
					GST_LOG_OBJECT(vmeta_dec, "popped NULL picture");

				if (!gst_vmeta_dec_return_stream_buffers(vmeta_dec))
					return GST_FLOW_ERROR;

				if (!gst_vmeta_dec_suspend_and_resume(vmeta_dec))
					return GST_FLOW_ERROR;

				break;
			}
			case IPP_STATUS_NEED_OUTPUT_BUF:
			{
				GstBuffer *picture_buffer = gst_video_decoder_allocate_output_buffer(decoder);
				picture = gst_vmeta_dec_get_ipp_picture_from_buffer(vmeta_dec, picture_buffer);

				g_assert(picture != NULL);

				GST_LOG_OBJECT(vmeta_dec, "pushing picture: %p", picture);

				ret = DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, picture, vmeta_dec->dec_state);
				if (ret != IPP_STATUS_NOERR)
				{
					GST_ERROR_OBJECT(vmeta_dec, "pushing picture failed : %s", gst_vmeta_dec_strstatus(ret));
					gst_buffer_unref(picture_buffer);
 					return GST_FLOW_ERROR;
				}

				break;
			}
			case IPP_STATUS_NEW_VIDEO_SEQ:
			{
				/* When a new sequence is started, pull all pictures from the video
				 * engine; completed ones have already been processed before anyway */
				if (!gst_vmeta_dec_return_picture_buffers(vmeta_dec))
					return GST_FLOW_ERROR;
				break;
			}
			case IPP_STATUS_END_OF_STREAM:
			{
				GST_DEBUG_OBJECT(vmeta_dec, "end of stream reached");

				/* TODO: There is a VC1 start code for end-of-sequence.
				 * It is unclear if this has to be sent to vMeta, or if it is optional,
				 * or if the data already contains it.
				 * The marvell plugins for GStreamer 0.10 seem to send it under some
				 * conditions. Omitting it here for now (decoder shutdown works fine
				 * without it). */

				do_eos = TRUE;
				run_decoding_loop = FALSE;
				break;
			}
			case IPP_STATUS_WAIT_FOR_EVENT:
				break;
			default:
			{
				GST_DEBUG_OBJECT(vmeta_dec, "DecodeFrame_Vmeta() returned unhandled code %d (%s)", (gint)(ret), gst_vmeta_dec_strstatus(ret));
			}
		}
	}

	if (do_finish)
	{
		if (decode_only)
			GST_VIDEO_CODEC_FRAME_SET_DECODE_ONLY(frame);

		gst_video_decoder_finish_frame(decoder, frame);
	}

	return do_eos ? GST_FLOW_EOS : GST_FLOW_OK;
}


static gboolean gst_vmeta_dec_reset(GstVideoDecoder *decoder, G_GNUC_UNUSED gboolean hard)
{
	gboolean ret = TRUE;
	GstVmetaDec *vmeta_dec = GST_VMETA_DEC(decoder);

	if (vmeta_dec->dec_state == NULL)
	{
		GST_LOG_OBJECT(vmeta_dec, "decoder not initialized yet - ignoring reset call");
		return TRUE;
	}

	ret = gst_vmeta_dec_return_stream_buffers(vmeta_dec) && ret;
	ret = gst_vmeta_dec_return_picture_buffers(vmeta_dec) && ret;

	GST_DEBUG_OBJECT(
		vmeta_dec,
		"after reset:  available streams: %u",
		g_list_length(vmeta_dec->streams_available)
	);

	g_list_free(vmeta_dec->streams_ready);

	vmeta_dec->upload_before_loop = FALSE;

	return ret;
}


static gboolean gst_vmeta_dec_decide_allocation(GstVideoDecoder *decoder, GstQuery *query)
{
	GstVmetaDec *vmeta_dec = GST_VMETA_DEC(decoder);
	GstCaps *outcaps;
	GstBufferPool *pool = NULL;
	guint size, min = 0, max = 0;
	GstStructure *config;
	GstVideoInfo vinfo;
	gboolean update_pool;

	gst_query_parse_allocation(query, &outcaps, NULL);
	gst_video_info_init(&vinfo);
	gst_video_info_from_caps(&vinfo, outcaps);

	GST_DEBUG_OBJECT(decoder, "num allocation pools: %d", gst_query_get_n_allocation_pools(query));

	/* Look for an allocator which can allocate vMeta DMA buffers */
	if (gst_query_get_n_allocation_pools(query) > 0)
	{
		for (guint i = 0; i < gst_query_get_n_allocation_pools(query); ++i)
		{
			gst_query_parse_nth_allocation_pool(query, i, &pool, &size, &min, &max);
			if (gst_buffer_pool_has_option(pool, GST_BUFFER_POOL_OPTION_MVL_VMETA))
				break;
		}

		size = MAX(size, (guint)(vmeta_dec->dec_info.seq_info.dis_buf_size));
		size = MAX(size, vinfo.size);
		update_pool = TRUE;
	}
	else
	{
		pool = NULL;
		size = MAX(vinfo.size, (guint)(vmeta_dec->dec_info.seq_info.dis_buf_size));
		min = max = 0;
		update_pool = FALSE;
	}

	/* Either no pool or no pool with the ability to allocate vMeta DMA buffers
	 * has been found -> create a new pool */
	if ((pool == NULL) || !gst_buffer_pool_has_option(pool, GST_BUFFER_POOL_OPTION_MVL_VMETA))
	{
		if (pool == NULL)
			GST_DEBUG_OBJECT(decoder, "no pool present; creating new pool");
		else
			GST_DEBUG_OBJECT(decoder, "no pool supports vMeta buffers; creating new pool");
		pool = gst_vmeta_buffer_pool_new(GST_VMETA_ALLOCATOR_TYPE_CACHEABLE, TRUE);
	}

	GST_DEBUG_OBJECT(
		pool,
		"pool config:  outcaps: %" GST_PTR_FORMAT "  size: %u  min buffers: %u  max buffers: %u",
		outcaps,
		size,
		min,
		max
	);

	if ((vmeta_dec->dec_info.seq_info.dis_buf_size == 0) || (vmeta_dec->dec_info.seq_info.dis_stride == 0))
	{
		if (pool != NULL)
			gst_object_unref(pool);
		GST_ERROR_OBJECT(decoder, "%s is zero", (vmeta_dec->dec_info.seq_info.dis_stride == 0) ? "dis_stride" : "dis_buf_size");
		return FALSE;
	}

	/* Inform the pool about the required stride and DMA buffer size */
	gst_vmeta_buffer_pool_set_dis_info(
		pool,
		vmeta_dec->dec_info.seq_info.dis_buf_size,
		vmeta_dec->dec_info.seq_info.dis_stride
	);

	/* Now configure the pool. */
	config = gst_buffer_pool_get_config(pool);
	gst_buffer_pool_config_set_params(config, outcaps, size, min, max);
	gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_MVL_VMETA);
	gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
	gst_buffer_pool_set_config(pool, config);

	if (update_pool)
		gst_query_set_nth_allocation_pool(query, 0, pool, size, min, max);
	else
		gst_query_add_allocation_pool(query, pool, size, min, max);

	if (pool != NULL)
		gst_object_unref(pool);

	return TRUE;
}


static GstStateChangeReturn gst_vmeta_dec_change_state(GstElement * element, GstStateChange transition)
{
	GstStateChangeReturn ret;
	GstVmetaDec *vmeta_dec = GST_VMETA_DEC(element);

	switch (transition)
	{
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			if (!gst_vmeta_dec_suspend(vmeta_dec, FALSE))
				return GST_STATE_CHANGE_FAILURE;
			break;
		default:
			break;
	}

	ret = GST_ELEMENT_CLASS(gst_vmeta_dec_parent_class)->change_state(element, transition);
	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition)
	{
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			if (!gst_vmeta_dec_suspend(vmeta_dec, TRUE))
				return GST_STATE_CHANGE_FAILURE;
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			if (!gst_vmeta_dec_suspend(vmeta_dec, FALSE))
				return GST_STATE_CHANGE_FAILURE;
			break;
		default:
			break;
	}

	return ret;
}

