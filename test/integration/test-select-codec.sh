#!/bin/bash
# usage: test-select-codec.sh <pcm-path>

CODECS=($(bluealsa-cli codec $1 |grep Available |cut -d: -f2))
if [[ ! ${#CODECS[@]} > 1 ]]; then
	echo "error: This test requires PCM with at least two codecs"
	exit 1
fi

# try to select the last codec from the codec list
echo "Select codec: ${CODECS[-1]}"
bluealsa-cli codec $1 ${CODECS[-1]}
sleep 1

# check whether codec was selected correctly
SELECTED=($(bluealsa-cli codec $1 |grep Selected |cut -d: -f2))
if [[ ${SELECTED[0]} != ${CODECS[-1]} ]]; then
	echo "error: Codec selection mismatch: ${SELECTED[0]} != ${CODECS[-1]}"
	exit 1
fi

# check for race condition in codec selection
for CODEC in "${CODECS[@]}"; do
	echo "Select codec: $CODEC"
	bluealsa-cli codec $1 $CODEC &
done
wait

# check whether selected codec is the last from the array
SELECTED=($(bluealsa-cli codec $1 |grep Selected |cut -d: -f2))
if [[ ${SELECTED[0]} != ${CODECS[-1]} ]]; then
	echo "error: Codec selection mismatch: ${SELECTED[0]} != ${CODECS[-1]}"
	exit 1
fi
