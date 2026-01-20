#!/bin/bash

# Register VolumeDataStore-Mime-vds.xml image/vds mime type,
# Install the 'desktop' vds.thumbnailer file to ~/.local/share/thumbnailers,
# Install the vds-thumbnailer executable to ~/.local/bin
# No sudo needed!

# Run this script from root of open-vds source tree / or from folder
# containing the required files.
exe=$( find . -iname 'vds-thumbnailer' )
opv=$( find . -iname 'libopenvds.so.3' )
mmt=$( find . -iname 'VolumeDataStore-Mime-vds.xml' )
tmb=$( find . -iname 'vds.thumbnailer' )
pel=$( which patchelf )

if [[ -z "$exe" ]]; then
  echo "Unable to find vds-thumbnailer. Did you build it in Release / RelWithDebInfo?"
  exit 1;
elif [[ -z "$opv" ]]; then
  echo "Unable to find openvds library shared object. Check your build is Release / RelWithDebInfo."
  exit 1;
elif [[ -z "$mmt" ]]; then
  echo "Unable to find mime-type file. Check git status or the unzipped files."
  exit 1;
elif [[ -z "$tmb" ]]; then
  echo "Unable to find thumbnailer entry file. Check git status or the unzipped files."
  exit 1;
elif [[ -z "$pel" ]]; then
  echo "Unable to find patchelf. It is required for patching rpath. Please install it e.g. # dnf install patchelf."
  exit 1;
fi

# Ensure mime type is registered to user
sudo xdg-mime install --mode system --novendor "$mmt"

# Create .local/share/thumbnailers if needed, and copy the thumbnailer entry
if [[ ! -d "/usr/share/thumbnailers" ]]; then
  mkdir -p /usr/share/thumbnailers
fi
cp "$tmb" /usr/share/thumbnailers

# copy other exe and lib
cp -f "$exe" /usr/bin/
cp -f "$opv" /usr/lib/

# Fix up RPATH on vds-thumbnailer
patchelf --remove-rpath /usr/bin/vds-thumbnailer
patchelf --set-rpath '$ORIGIN/../lib/' /usr/bin/vds-thumbnailer
