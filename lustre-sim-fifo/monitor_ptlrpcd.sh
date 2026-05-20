#!/bin/bash

# Init log file
LOGFILE="ptlrpcd_log_proposed.txt"
> "$LOGFILE"

# Background monitoring
(
    echo "=== Monitoring started at $(date) ===" >> "$LOGFILE"
    while true; do
        echo "--- $(date '+%Y-%m-%d %H:%M:%S') ---" >> "$LOGFILE"
        ps -e -o pid,psr,comm,state | grep ptlrpcd >> "$LOGFILE"
        echo >> "$LOGFILE"
        sleep 1
    done
) &
MON_PID=$!

# FIO run
fio r.fio

# End monitoring process
kill $MON_PID
wait $MON_PID 2>/dev/null

echo "=== Monitoring ended at $(date) ===" >> "$LOGFILE"

