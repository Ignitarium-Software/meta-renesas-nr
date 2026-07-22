#!/bin/sh

#Flash DSP application

modprobe rcar-dsp-rproc.ko

echo VpxAweRuntime.elf > /sys/class/remoteproc/remoteproc1/firmware
echo start > /sys/class/remoteproc/remoteproc1/state

echo "Dsp firmware loaded"

sleep 1

# Load CR firmware
echo audio_server.elf > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state

sleep 1

echo "Loaded CR firmware"
