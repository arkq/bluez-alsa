#!/bin/bash
#
# Copyright (c) 2016-2024 Arkadiusz Bokowy
#
# This file is a part of bluez-alsa.
#
# This project is licensed under the terms of the MIT license.

if [[ $# -ne 1 ]]; then
	echo "usage: $0 <pcm-path>"
	exit 1
fi

# open PCM and close it right away
: |bluealsactl open "$1" || exit

# check if open is possible right after close
if ! dd status=none if=/dev/zero count=10 |bluealsactl open "$1" ; then
	echo "error: Couldn't open BlueALSA PCM"
	exit 1
fi
