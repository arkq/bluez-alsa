#!/bin/bash
# usage: test-pcm-open.sh <pcm-path>

# check whether it is possible to open
# a BlueALSA PCM right after it was closed

# open PCM and close it right away
: |bluealsa-cli open $1
: |bluealsa-cli open $1

if [ $? -ne 0 ]; then
	echo "error: Couldn't open BlueALSA PCM"
	exit 1
fi
