#!/bin/sh
(exec bwrap \
--ro-bind /usr/bin /usr/bin \
--ro-bind /usr/lib /usr/lib \
--symlink usr/lib /lib64 \
--tmpfs /usr/lib/gcc \
--tmpfs $HOME \
--ro-bind $HOME/Documents $HOME/Documents \
--tmpfs /tmp \
--share-net \
/usr/bin/sh "$@")