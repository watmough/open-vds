# vds-thumbnailer
# Preview VDS file contents in the file icon

## Outline

The `vds-thumbnailer` application works as part of the Gnome desktop to 
provide small images, or thumbnails, of your VDS seismic file content.

Here's an example of the Gnome Nautilus file browser requesting creation of
a thumbnail for a network file.

  (nautilus:3817686): GnomeDesktop-DEBUG: 14:19:52.799: About to launch script: 
  bwrap --ro-bind /usr /usr --ro-bind /etc/ld.so.cache /etc/ld.so.cache 
  --symlink /usr//bin /bin --symlink /usr//lib64 /lib64 --symlink /usr//lib /lib 
  --symlink /usr//sbin /sbin --proc /proc --dev /dev --chdir / --setenv GIO_USE_VFS local 
  --unshare-all --die-with-parent --setenv G_MESSAGES_DEBUG all 
  --bind /tmp/gnome-desktop-thumbnailer-13CH52 /tmp 
  --ro-bind /run/user/1000/gvfs/smb-share:server=10.x.x.5,
    share=XXXxxx/RegressionTestData/filt_mig_small.vds /tmp/gnome-desktop-file-to-thumbnail.vds
  --seccomp 31 /usr/bin/vds-thumbnailer /tmp/gnome-desktop-file-to-thumbnail.vds /tmp/gnome-desktop-thumbnailer.png 256

Note that `bwrap` is used here to sandbox the `vds-thumbnailer`.

## Build Instructions

### Full open-vds 

For a full open-vds build, add -DBUILD_FOR_SANDBOX to the cmake configuration command.

open-vds will then build with many libs and tools, but without cloud capability.

### Python distribution or OpenVDS install

For a regular install of open-vds

## Installation

Register application/x-vds as a mime-type

    xdg-mime install --mode user --novendor VolumeDataStore-Mime-vds.xml

Ensure the user's local thumbnailer entries folder is created

    mkdir -p $HOME/.local/share/thumbnailers

Copy the thumbnailer entry file `vds.thumbnailer` to

    cp vds.thumbnailer $HOME/.local/share/thumbnailers

Restart Gnome Files with `nautilus -q`.`

## Debugging

If you have problems with thumbnails not appearing, consider running nautilus
in debug mode as follows:

    # See: https://askubuntu.com/a/1142299/23941
    G_MESSAGES_DEBUG="all" NAUTILUS_DEBUG="All" nautilus

Also, check the following:

* Right-click on a VDS file and check the properties correctly register a vds filetype,

* Confirm that the Gnome desktop understands the .vds filetype:
  xdg-mime query filetype \<some *actual* file\>.vds

* Reset your failed thumbnails by running:
  `rm ~/.cache/thumbnails/fail/*png`
  which will reset the cache only for previously failed thumbnails.
