#!/bin/bash
echo "Enable lnet"
modprobe lnet
lnetctl lnet configure
lnetctl net add --net tcp2 --if enp24s0f0
lnetctl net show
echo "Done"
