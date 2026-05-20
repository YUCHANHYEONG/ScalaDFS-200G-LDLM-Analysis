#!/bin/bash

scripts=("r_4.fio" "r_512.fio" "r_64.fio" "r_128.fio")

for script in "${scripts[@]}"; do
	fio "$script"
done

sleep 30

for script in "${scripts[@]}"; do
	fio "$script"
done

echo "[INFO] ALL FIO tests completed."
