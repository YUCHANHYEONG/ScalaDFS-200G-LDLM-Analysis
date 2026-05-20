#!/bin/bash

LOGFILE="ptlrpcd_log_proposed.txt"
TMPFILE="r_count_per_second.txt"

awk '
  /^--- [0-9]{4}-/ {
    timestamp = $0
    count = 0
    next
  }
  /ptlrpcd/ && $4 == "R" {
    count++
  }
  NF == 0 && timestamp {
    print timestamp, count
    timestamp = ""
  }
' "$LOGFILE" > "$TMPFILE"

echo "✅ 각 초별 R 상태 개수 요약:"
cat "$TMPFILE"

echo
echo "✅ 가장 많은 R 상태가 있었던 시점:"
sort -k5 -nr "$TMPFILE" | head -1

