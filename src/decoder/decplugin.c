#include <gst/gst.h>
#include "config.h"
#include "vmeta_decoder.h"



static gboolean plugin_init(GstPlugin *plugin)
{
	return gst_element_register(plugin, "vmetadec", GST_RANK_PRIMARY + 1, gst_vmeta_dec_get_type());
}



GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	vmetadec,
	"Hardware-accelerated video decoding using the Marvell vMeta engine",
	plugin_init,
	VERSION,
	"LGPL",
	GST_PACKAGE_NAME,
	GST_PACKAGE_ORIGIN
)

