#!/bin/sh

# Base addresses for command area
NUM_CORES=4
NUM_CLUSTER=5
CTRL_CMD=0x96700000
ELF_ADDRESSES_BASE=0x96700004
ELF_SIZE_BASE=$(printf "0x%X" $((ELF_ADDRESSES_BASE + NUM_CORES * NUM_CLUSTER * 4)))
ELF_BASE=0x96701000

CORE_INDEX=0
ELF_OFFSET=0
# Load all cluster and core ELFs
for cl in 0 1 2 3 4; do
    for c in 0 1 2 3; do
        ELF_FILE="dspss_sample_kernel_cl${cl}_c${c}_x5h.elf"
        ELF_FILE_SIZE=$(stat -c %s $ELF_FILE)
        if [ -f "$ELF_FILE" ]; then
            # Calculate addresses for this core
            ELF_ADDR=$(printf "0x%X" $((ELF_BASE + ELF_OFFSET)))
            ELF_ADDR_CMD=$(printf "0x%X" $((ELF_ADDRESSES_BASE + CORE_INDEX * 0x4)))
            
            echo "Loading CL${cl}_C${c}: $ELF_FILE to $ELF_ADDR"
            
            # Copy ELF to memory
            ./devmemcpy $ELF_ADDR $ELF_FILE
            
            # Write ELF load address
            devmem2 $ELF_ADDR_CMD w $ELF_ADDR
        else
            echo "Warning: $ELF_FILE not found, skipping CL${cl}_C${c}"
        fi
        CORE_INDEX=$((CORE_INDEX + 1))
        ELF_OFFSET=$((ELF_OFFSET + ELF_FILE_SIZE))
    done
done

# Start processing on all cores
echo "Starting processing..."
devmem2 $CTRL_CMD w 0xCAFECAFE

# Wait for processing
sleep 10s

# Signal processing done on all cores
echo "Signaling stop..."
devmem2 $CTRL_CMD w 0xDEADDEAD

echo "Done."
