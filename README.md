gst-vmeta
=========

About
-----

This is a set of [GStreamer 1.0](http://gstreamer.freedesktop.org/) plugins for plugins for Marvell's
hardware video accelerator, called "vMeta". Currently, a decoder is implemented. Encoders and a modified
xvimagesink will follow soon.

Unlike Marvell's official GStreamer 0.10 vMeta plugins, these make use of GStreamer's base classes, thus
integrating better with the other elements. These plugins also allow for passing on output data directly,
without having to copy it from the DMA buffers. This is possible thanks to GStreamer 1.0's new allocator
and buffer pool features.

Currently, this software has been tested on the [SolidRun CuBox](http://solid-run.com/cubox) only.


License
-------

These plugins are licensed under the LGPL v2.


Available plugins
-----------------

* `vmetadec` : video decoder plugin


Dependencies
------------

You'll need a GStreamer 1.0 installation, and Marvell's IPP package. 


Building and installing
-----------------------

This project uses the [waf meta build system](https://code.google.com/p/waf/). To configure , first set
the following environment variables to whatever is necessary for cross compilation for your platform:

* `CC`
* `CFLAGS`
* `LDFLAGS`
* `PKG_CONFIG_PATH`
* `PKG_CONFIG_SYSROOT_DIR`

Then, run:

    ./waf configure --prefix=PREFIX

(The aforementioned environment variables are only necessary for this configure call.)
PREFIX defines the installation prefix, that is, where the built binaries will be installed.

Once configuration is complete, run:

    ./waf

This builds the plugins.
Finally, to install, run:

    ./waf install

Note that there is a shared object that is _not_ a plugin, called `libgstvmetacommon.so`. This shared
object contains common functionality used in all plugins.

