#!/bin/sh

if [ $# -ne 1 ]; then
    echo "Usage: $0 <ffmpeg-dir>"
    exit 1
fi

echo "static const char *presets[][2] = {"
for profile in ultrafast superfast veryfast faster fast medium slow slower veryslow placebo; do
    opts=`xargs -a $1/ffpresets/libx264-$profile.ffpreset`
    printf "    {%-12s %s},\n" "\"$profile\"," "\"$opts\""
done
echo "};"
