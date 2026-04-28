#!/bin/sh

echo "Loading remoteproc firmware"

echo rpmsg_mfis0_cluster0_core0.elf > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
