#!/bin/sh
# Runs test_transcode_aac and, when ffprobe/ffmpeg are available, verifies the
# produced ADTS stream decodes to the expected amount of AAC-LC 44.1 kHz stereo.
set -u

bin="${TEST_BIN:-./tests/test_transcode_aac}"
tmp="$(mktemp -d 2>/dev/null || mktemp -d -t owntone-aac)"
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/owntone.conf" <<CONF
general {
	logfile = "$tmp/owntone.log"
}
library {
	name = "test"
	directories = { "$tmp" }
}
CONF

"$bin" "$tmp/out.adts" "$tmp/owntone.conf" || exit 1

if ! command -v ffprobe >/dev/null 2>&1 || ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffprobe/ffmpeg not found, skipping decode verification"
  exit 0
fi

probe="$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,profile,sample_rate,channels -of csv=p=0 "$tmp/out.adts")"
echo "ffprobe: $probe"
case "$probe" in
  aac,LC,44100,2) ;;
  *) echo "FAIL: unexpected stream description"; exit 1 ;;
esac

bytes="$(ffmpeg -v error -i "$tmp/out.adts" -f s16le -ac 2 -ar 44100 - | wc -c | tr -d ' ')"
samples=$((bytes / 4))
echo "decoded $samples samples"
# 2 s were encoded; the AAC encoder/decoder delay adds up to a couple of frames
if [ "$samples" -lt 88200 ] || [ "$samples" -gt 92296 ]; then
  echo "FAIL: expected 88200-92296 decoded samples"
  exit 1
fi
echo "ok"
