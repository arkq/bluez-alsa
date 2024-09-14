#!/usr/bin/env python3
#
# Copyright (c) 2016-2022 Arkadiusz Bokowy
#
# This file is a part of bluez-alsa.
#
# This project is licensed under the terms of the MIT license.

import argparse
import signal
import subprocess
import sys
import time
from csv import DictWriter
from math import ceil, floor
from random import randint
from struct import Struct

# Format mapping between BlueALSA
# and Python struct module
FORMATS = {
    'U8': ('<', 'B'),
    'S16_LE': ('<', 'h'),
    'S24_3LE': ('<', None),  # not supported
    'S24_LE': ('<', 'i'),
    'S32_LE': ('<', 'i'),
}

# Value ranges for different formats
LIMITS = {
    'U8': (0, 255),
    'S16_LE': (-32768, 32767),
    'S24_LE': (-8388608, 8388607),
    'S32_LE': (-2147483648, 2147483647),
}


def rate_sync(t0: float, frames: int, rate: int):
    delta = frames / rate - time.monotonic() + t0
    if (delta > 0):
        print(f"Rate sync: {delta:.6f}")
        time.sleep(delta)
    else:
        print(f"Rate sync overdue: {-delta:.6f}")


def test_pcm_write(pcm, pcm_format, pcm_channels, pcm_rate, interval):
    """Write PCM test signal.

    This function generates test signal when real time modulo interval is zero
    (within sample rate resolution capabilities). Providing that both devices
    are in sync with NTP, this should be a reliable way to detect end-to-end
    latency.
    """

    fmt = FORMATS[pcm_format]
    # Structure for single PCM frame
    struct = Struct(fmt[0] + fmt[1] * pcm_channels)

    # Time quantum in seconds
    t_quantum = 1.0 / pcm_rate

    # Noise PCM value range
    v_noise_min = int(LIMITS[pcm_format][0] * 0.05)
    v_noise_max = int(LIMITS[pcm_format][1] * 0.05)

    # Signal PCM value range
    v_signal_min = int(LIMITS[pcm_format][1] * 0.8)
    v_signal_max = int(LIMITS[pcm_format][1] * 1.0)

    signal_frames = int(0.1 * pcm_rate)
    print(f"Signal frames: {signal_frames}")

    frames = 0
    t0 = time.monotonic()

    while True:

        # Time until next signal
        t = time.time()
        t_delta = ceil(t / interval) * interval - t - t_quantum
        print(f"Next signal at: {t:.6f} + {t_delta:.6f} -> {t + t_delta:.6f}")

        # Write random data to keep encoder busy
        noise_frames = int(t_delta * pcm_rate)
        print(f"Noise frames: {noise_frames}")
        pcm.writelines(
            struct.pack(*[randint(v_noise_min, v_noise_max)] * pcm_channels)
            for _ in range(noise_frames))
        pcm.flush()

        frames += noise_frames
        rate_sync(t0, frames, pcm_rate)

        # Write signal data
        pcm.writelines(
            struct.pack(*[randint(v_signal_min, v_signal_max)] * pcm_channels)
            for _ in range(signal_frames))
        pcm.flush()

        frames += signal_frames
        rate_sync(t0, frames, pcm_rate)


def test_pcm_read(pcm, pcm_format, pcm_channels, pcm_rate, interval):
    """Read PCM test signal."""

    fmt = FORMATS[pcm_format]
    # Structure for single PCM frame
    struct = Struct(fmt[0] + fmt[1] * pcm_channels)

    # Minimal value for received PCM signal
    v_signal_min = int(LIMITS[pcm_format][1] * 0.51)

    csv = DictWriter(sys.stdout, fieldnames=[
        'time', 'expected', 'latency', 'duration'])
    csv.writeheader()

    t_signal = 0
    while True:

        pcm_frame = struct.unpack(pcm.read(struct.size))
        if pcm_frame[0] < v_signal_min:
            if t_signal > 0:
                t = time.time()
                t_expected = floor(t / interval) * interval
                csv.writerow({'time': t,
                              'expected': float(t_expected),
                              'latency': t - t_expected,
                              'duration': t - t_signal})
                t_signal = 0
            continue

        if t_signal == 0:
            t_signal = time.time()


parser = argparse.ArgumentParser()
parser.add_argument('-B', '--dbus', type=str, metavar='NAME',
                    help='BlueALSA service name suffix')
parser.add_argument('-i', '--interval', type=int, metavar='SEC', default=2,
                    help='signal interval in seconds; default: 2')
parser.add_argument('-t', '--timeout', type=int, metavar='SEC', default=60,
                    help='test timeout in seconds; default: 60')
parser.add_argument('PCM_PATH', type=str,
                    help='D-Bus path of the BlueALSA PCM device')

args = parser.parse_args()
signal.alarm(args.timeout)

options = ["--verbose"]
if args.dbus:
    options.append(f'--dbus={args.dbus}')

try:  # Get info for given BlueALSA PCM device
    cmd = ['bluealsactl', *options, 'info', args.PCM_PATH]
    output = subprocess.check_output(cmd, text=True)
except subprocess.CalledProcessError:
    sys.exit(1)
info = {key.lower(): value.strip()
        for key, value in (line.split(':', 1)
                           for line in output.splitlines())}
channels = int(info['channels'])
rate = int(info['rate'].split()[0])

print(f"Bluetooth: {info['transport']} {info['selected codec']}")
print(f"PCM: {info['format']} {channels} channels {rate} Hz")
print("==========")

cmd = ['bluealsactl', *options, 'open', args.PCM_PATH]
client = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

# Wait for BlueALSA to open the PCM device
time.sleep(1)

if info['mode'] == 'sink':
    test_pcm_write(client.stdin, info['format'], channels, rate, args.interval)

if info['mode'] == 'source':
    test_pcm_read(client.stdout, info['format'], channels, rate, args.interval)
