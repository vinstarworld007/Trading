#!/usr/bin/env bash
# Downloads one Nasdaq TotalView-ITCH 5.0 sample day and checks its md5.
#
#   scripts/fetch-itch-sample.sh                         default: 01302019 (4.8 GB)
#   scripts/fetch-itch-sample.sh 08302019.NASDAQ_ITCH50.gz
#
# Files are full trading days, 3.5 to 18 GB compressed. Downloads resume if
# interrupted. Keep them gzipped: the lab reads .gz directly.
set -euo pipefail

BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
FILE="${1:-01302019.NASDAQ_ITCH50.gz}"
DATA_DIR="${DATA_DIR:-$HOME/data/itch}"

mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

echo "Downloading $FILE into $DATA_DIR (resumable)"
curl --fail --location --continue-at - --output "$FILE" "$BASE_URL/$FILE"

if curl --fail --silent --location --output "$FILE.md5sum" "$BASE_URL/$FILE.md5sum"; then
  expected="$(awk '{print $1}' "$FILE.md5sum")"
  echo "Checking md5 (takes a minute for a multi-GB file)"
  actual="$(md5sum "$FILE" | awk '{print $1}')"
  if [ "$expected" != "$actual" ]; then
    echo "md5 mismatch: expected $expected, got $actual. Delete the file and download again." >&2
    exit 1
  fi
  echo "md5 OK"
else
  rm -f "$FILE.md5sum"
  echo "No .md5sum published for $FILE; skipped the check."
fi
ls -lh "$DATA_DIR/$FILE"
