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

read -r -a CODECS < <(bluealsactl codec "$1" |grep Available |cut -d: -f2)
if [[ ! ${#CODECS[@]} -gt 1 ]]; then
	echo "error: This test requires PCM with at least two codecs"
	exit 1
fi

# try to select the last codec from the codec list
echo "Select codec: ${CODECS[-1]}"
bluealsactl codec "$1" "${CODECS[-1]}"
sleep 1

# check whether codec was selected correctly
read -r -a SELECTED < <(bluealsactl codec "$1" |grep Selected |cut -d: -f2)
if [[ "${SELECTED[0]}" != "${CODECS[-1]}" ]]; then
	echo "error: Codec selection mismatch: ${SELECTED[0]} != ${CODECS[-1]}"
	exit 1
fi

# check for race condition in codec selection
for CODEC in "${CODECS[@]}"; do
	echo "Select codec: ${CODEC}"
	bluealsactl codec "$1" "${CODEC}" &
done
wait

# check whether selected codec is the last from the array
read -r -a SELECTED < <(bluealsactl codec "$1" |grep Selected |cut -d: -f2)
if [[ "${SELECTED[0]}" != "${CODECS[-1]}" ]]; then
	echo "error: Codec selection mismatch: ${SELECTED[0]} != ${CODECS[-1]}"
	exit 1
fi
