#!/bin/sh
#
# convert-for-f1c100s.sh [--720p] [--fill] input.mp4 [output.mp4]
#
# Convert an arbitrary MP4 to the format the F1C100s player supports:
#   H.264 Constrained Baseline, 30 fps, no B-frames, 1 reference frame,
#   AAC-LC 48 kHz stereo, faststart.
#
# Default output is 1024x600 (the panel size).  --720p produces 1280x720
# for the maximum real-time decode resolution.
#
# By default the source aspect ratio is preserved and the output is padded
# with black bars.  --fill scales to cover the target and crops the extra
# edges, so the picture fills the screen but loses a little on the sides
# (or top/bottom for a taller source).
#
set -eu

usage()
{
	echo "usage: $0 [--720p] [--fill] input.mp4 [output.mp4]" >&2
	echo "  default output: <input>_f1c100s.mp4 (1024x600, padded)" >&2
	echo "  --720p        : <input>_720p.mp4     (1280x720, padded)" >&2
	echo "  --fill        : crop to fill instead of padding, suffix _fill" >&2
}

use_720p=0
use_fill=0
input=""
output=""

for arg in "$@"; do
	case "$arg" in
	--720p|-7)
		use_720p=1
		;;
	--fill|-f)
		use_fill=1
		;;
	-h|--help)
		usage
		exit 0
		;;
	-*)
		echo "unknown option: $arg" >&2
		usage
		exit 1
		;;
	*)
		if [ -z "$input" ]; then
			input="$arg"
		elif [ -z "$output" ]; then
			output="$arg"
		else
			echo "too many arguments" >&2
			usage
			exit 1
		fi
		;;
	esac
done

if [ -z "$input" ]; then
	usage
	exit 1
fi
if [ ! -f "$input" ]; then
	echo "input not found: $input" >&2
	exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1 ||
   ! command -v ffprobe >/dev/null 2>&1; then
	echo "ffmpeg/ffprobe not found in PATH" >&2
	exit 1
fi

if [ -z "$output" ]; then
	dir=$(dirname "$input")
	base=$(basename "$input")
	stem=${base%.*}
	if [ "$use_720p" = 1 ]; then
		suffix="_720p"
	else
		suffix="_f1c100s"
	fi
	[ "$use_fill" = 1 ] && suffix="${suffix}_fill"
	output="$dir/${stem}${suffix}.mp4"
fi

if [ "$input" = "$output" ]; then
	echo "output must differ from input" >&2
	exit 1
fi

if [ "$use_720p" = 1 ]; then
	if [ "$use_fill" = 1 ]; then
		scale="scale=1280:720:force_original_aspect_ratio=increase,crop=1280:720,setsar=1"
	else
		scale="scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2,setsar=1"
	fi
	level="3.1"
else
	if [ "$use_fill" = 1 ]; then
		scale="scale=1024:600:force_original_aspect_ratio=increase,crop=1024:600,setsar=1"
	else
		scale="scale=1024:600:force_original_aspect_ratio=decrease,pad=1024:600:(ow-iw)/2:(oh-ih)/2,setsar=1"
	fi
	level="3.1"
fi

echo "== input =="
ffprobe -v error \
	-show_entries format=duration,size \
	-show_entries stream=index,codec_name,width,height,r_frame_rate,sample_rate,channels \
	-of default=noprint_wrappers=1 "$input"

echo "== converting to H.264 Baseline + AAC-LC =="
ffmpeg -y -i "$input" \
	-vf "$scale,fps=30" \
	-c:v libx264 \
	-profile:v baseline \
	-level "$level" \
	-pix_fmt yuv420p \
	-bf 0 \
	-refs 1 \
	-g 30 \
	-crf 23 \
	-preset fast \
	-c:a aac \
	-b:a 96k \
	-ac 2 \
	-ar 48000 \
	-movflags +faststart \
	"$output"

echo "== output =="
ffprobe -v error \
	-show_entries format=duration,size \
	-show_entries stream=index,codec_name,profile,level,width,height,pix_fmt,r_frame_rate,sample_rate,channels \
	-of default=noprint_wrappers=1 "$output"

ls -l "$output"
