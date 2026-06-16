#!/bin/sh

#Flash DSP application
cd /home/root/dsp_flash
chmod +x ./devmemcpy
./run_dspss.sh
cd

sleep 10

# Load DSP firmware
echo "Loading dsp firmware"
echo dspss_sample_kernel_cl0_c0_x5h.elf > /sys/class/remoteproc/remoteproc1/firmware
echo start > /sys/class/remoteproc/remoteproc1/state
