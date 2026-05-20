#!/bin/bash

#################################
# Different Grant Size		#
# small(<= 16)		- 256GB	#
# medium(16 < && 64 <=)	- 16GB	#
# large( 64 <)		- 8MB	#
#################################

fio r_4.fio
#sudo sync; 
#rm -rf /mnt/client/*
#fio r_4.fio
#sudo sync; echo 3 > /proc/sys/vm/drop_caches
#rm -rf /mnt/client/*
#fio r_4.fio
#sudo sync; echo 3 > /proc/sys/vm/drop_caches
#rm -rf /mnt/client/*
#fio r_4.fio
fio r_512.fio	
#sudo sync; echo 3 > /proc/sys/vm/drop_caches
fio r_64.fio
#sudo sync; echo 3 > /proc/sys/vm/drop_caches
#fio r_256.fio
#fio r_1024.fio	
#fio r_512.fio
fio r_128.fio
#fio r_32.fio
fio r_16.fio	
#fio r_8.fio
#fio r_4.fio
#fio r_64.fio

