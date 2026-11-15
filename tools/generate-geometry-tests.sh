#!/bin/sh
#
# generate-geometry-tests.sh [output-dir]
#
# Generate H.264 Baseline + AAC-LC test clips for checking mp4play display
# geometry on the 1024x600 panel.  Every clip has a red border on the very
# edge of its display rectangle, a green border just inside it, a grid,
# edge/center labels and a short 440/660 Hz tone.
#
# Expected on the panel:
#
#   1024x600 -> full screen, red border touches all four edges
#   1280x720 -> 1024x576, 12 px black bars top/bottom
#    854x480 -> 1024x576, 12 px black bars top/bottom, right padding clipped
#   1920x1080-> 1024x576, 12 px black bars top/bottom (decode is slow)
#   1920x816 -> 1024x435, 82/83 px black bars top/bottom (2.35:1)
#    640x480 ->  800x600, 112 px black bars left/right (4:3)
#    320x240 ->  800x600, 112 px black bars left/right (4:3, coded 256)
#
# The *-embedded-16x9 clip is a 1024x600 file that already contains a
# 1024x576 picture plus 12 px source black bars.  If the player fills the
# panel, the red outer border touches the edges, while the green content
# border stays 12 px inside the top/bottom edges.
#
set -eu

out=${1:-geometry-tests}
font=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf

command -v ffmpeg >/dev/null 2>&1 || {
	echo "ffmpeg not found" >&2
	exit 1
}
[ -f "$font" ] || {
	echo "font not found: $font" >&2
	exit 1
}

mkdir -p "$out"

label()
{
	w=$1
	h=$2
	fs=$((w / 20))
	[ "$fs" -lt 20 ] && fs=20
	[ "$fs" -gt 56 ] && fs=56

	printf '%s' \
"drawbox=x=0:y=0:w=iw:h=ih:color=red@0.95:t=4,\
drawbox=x=8:y=8:w=iw-16:h=ih-16:color=lime@0.9:t=2,\
drawgrid=w=iw/16:h=ih/10:t=1:c=white@0.20,\
drawbox=x=iw/4:y=ih/2-1:w=iw/2:h=2:color=yellow@0.9:t=fill,\
drawbox=x=iw/2-1:y=ih/4:w=2:h=ih/2:color=yellow@0.9:t=fill,\
drawtext=fontfile=$font:text=${w}x${h}:x=(w-tw)/2:y=24:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.65,\
drawtext=fontfile=$font:text=BOTTOM:x=(w-tw)/2:y=h-th-24:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.65,\
drawtext=fontfile=$font:text=L:x=16:y=(h-th)/2:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.65,\
drawtext=fontfile=$font:text=R:x=w-tw-16:y=(h-th)/2:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.65,\
drawtext=fontfile=$font:text=CENTER:x=(w-tw)/2:y=(h-th)/2:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.65"
}

gen()
{
	name=$1
	w=$2
	h=$3
	level=$4

	echo "== $name (${w}x${h}) =="
	ffmpeg -y -hide_banner -loglevel error \
		-f lavfi -i "testsrc2=size=${w}x${h}:rate=30:duration=6" \
		-f lavfi -i "sine=frequency=440:sample_rate=48000:duration=6" \
		-vf "$(label "$w" "$h")" \
		-c:v libx264 -profile:v baseline -level "$level" \
		-pix_fmt yuv420p -bf 0 -refs 1 -g 30 -crf 24 \
		-preset veryfast \
		-c:a aac -b:a 96k -ac 2 -ar 48000 \
		-movflags +faststart -shortest \
		"$out/$name"
}

gen_embedded_16x9()
{
	name=$1
	w=1024
	h=576
	fs=32

	echo "== $name (1024x576 inside 1024x600) =="
	ffmpeg -y -hide_banner -loglevel error \
		-f lavfi -i "testsrc2=size=${w}x${h}:rate=30:duration=6" \
		-f lavfi -i "sine=frequency=660:sample_rate=48000:duration=6" \
		-vf "drawbox=x=0:y=0:w=iw:h=ih:color=lime@0.9:t=4,\
drawgrid=w=iw/16:h=ih/10:t=1:c=white@0.20,\
drawtext=fontfile=$font:text=1024x576 source:x=(w-tw)/2:y=24:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.65,\
drawtext=fontfile=$font:text=12px source bar:x=(w-tw)/2:y=h-th-24:fontsize=$fs:fontcolor=white:box=1:boxcolor=black@0.65,\
pad=1024:600:0:12:black,\
drawbox=x=0:y=0:w=iw:h=ih:color=red@0.95:t=4,\
drawtext=fontfile=$font:text=1024x600 frame:x=(w-tw)/2:y=2:fontsize=16:fontcolor=white:box=1:boxcolor=black@0.65" \
		-c:v libx264 -profile:v baseline -level 3.1 \
		-pix_fmt yuv420p -bf 0 -refs 1 -g 30 -crf 24 \
		-preset veryfast \
		-c:a aac -b:a 96k -ac 2 -ar 48000 \
		-movflags +faststart -shortest \
		"$out/$name"
}

gen test-1024x600.mp4 1024 600 3.1
gen test-1280x720.mp4 1280 720 3.1
gen test-854x480.mp4 854 480 3.1
gen test-640x480.mp4 640 480 3.1
gen test-320x240.mp4 320 240 3.1
gen test-1920x816.mp4 1920 816 4.0
gen test-1920x1080.mp4 1920 1080 4.0
gen_embedded_16x9 test-1024x600-embedded-16x9.mp4

echo
echo "== output =="
ls -l "$out"
