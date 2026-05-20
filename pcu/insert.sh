#!/bin/sh

make -j$(nproc)
insmod pcu.ko

