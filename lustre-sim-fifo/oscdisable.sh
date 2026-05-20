#!/bin/bash
echo "Running commands as a root user..."
sudo rmmod lmv
sudo rmmod mdc
sudo rmmod lov
sudo rmmod mgc
sudo rmmod fid
sudo rmmod osc
sudo rmmod fld
sudo rmmod ptlrpc
echo "All done."

