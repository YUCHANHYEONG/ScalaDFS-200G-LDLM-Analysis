#!/bin/bash
echo "Running commands as a root user..."
sudo sync; echo 3 > /proc/sys/vm/drop_caches
sudo umount /mnt/client
sudo rmmod lustre
sudo rmmod lmv
sudo rmmod mdc
sudo rmmod lov
sudo rmmod mgc
sudo rmmod fid
sudo rmmod osc
sudo rmmod fld
sudo rmmod ptlrpc
echo "All done."

sudo rmmod obdclass
