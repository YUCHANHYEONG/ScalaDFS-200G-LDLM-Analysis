#!/bin/bash

module load mpi/openmpi-x86_64
mpirun --allow-run-as-root --np 32 mdtest -n 31000 -i 1 -u -d /mnt/client/mdtest_file_easy -Y
