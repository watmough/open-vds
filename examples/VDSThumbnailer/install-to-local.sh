#!/bin/bash

# Register VolumeDataStore-Mime-vds.xml image/vds mime type,
# Install the 'desktop' vds.thumbnailer file to ~/.local/share/thumbnailers,
# Install the vds-thumbnailer executable to ~/.local/bin
# No sudo needed!

# Ensure mime type is registered to user
xdg-mime install --mode user ./VolumeDataStore-Mime-vds.xml

# Create .local/share/thumbnailers if needed
if [[ ! -d "$HOME/.local/share/thumbnailers" ]]; then
  mkdir -p $HOME/.local/share/thumbnailers
fi

# Copy the vds-thumbnailer 'desktop' file to $HOME/.local/share/thumbnailers
cp ./vds.thumbnailer $HOME/.local/share/thumbnailers

# Copy OpenVDS libs to ~/.local/lib
# Copy vds-thumbnailer executable to $HOME/.local/bin
# running from full open-vds build?
if [[ -f "./../../build/examples/VDSThumbnailer/vds-thumbnailer" ]]; then
  cp ./../../Dist/OpenVDS/lib64/libopenvds.so* ~/.local/lib
  cp ./../../build/examples/VDSThumbnailer/vds-thumbnailer ~/.local/bin/
# running from distribution?
elif [[ -f "./../../../../build/VDSThumbnailer/vds-thumbnailer" ]]; then
 cp ./../../../../lib64/libopenvds.so* ~/.local/lib/
 cp ./../../../../build/VDSThumbnailer/vds-thumbnailer ~/.local/bin/
else
  echo "Unable to find the vds-thumbnailer executable. Stopping."
  exit 1
fi

# Fix up RPATH on vds-thumbnailer
patchelf --remove-rpath ~/.local/bin/vds-thumbnailer
patchelf --set-rpath '$ORIGIN/../lib/' ~/.local/bin/vds-thumbnailer
