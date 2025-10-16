#!/bin/bash

# auto-grades the incoming photo using GMIC's tensiongreen1 color preset
# It shows the original and when done, closes it and shows the graded one instead

cmd="$0"
cmdBase="$(basename $cmd)"
cmdDir="$(dirname $cmd)"
file="$1"
fileExt="${file##*.}"
fileExt="${fileExt,,}"   # convert extension to lowercase
suffix="${cmdBase%.*}"
showCmd="$cmdDir/show_single.sh"
outfile="${file%.*}_${suffix}.$fileExt"

brightness=0
contrast=15
gamma=8
saturation=0
tensiongreen=40
vignette=50

# show original
eval "${showCmd} $1" &

gmic ${file} \
     -fx_color_presets 5,14,74,1,1,1,31,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,18,1,1,1,1,1,1,31,512,${tensiongreen},${brightness},${contrast},${gamma},0,${saturation},0,0,50,50,"0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0" \
     -fx_vignette ${vignette},70,95,0,0,0,255 \
     -o ${outfile} \
    &> /dev/null

# post processing
case "${fileExt,,}" in
  jpg|jpeg)
    # drop the compression to acceptable levels (422 subsampling and 92%)
    # strip out exif data for security reasons
    # optimize for web to load progressively
    #mogrify -sampling-factor 4:2:2 -quality 92 -strip -interlace JPEG "${outfile}"
    # mmmm, listmonk doesn't like subsampling, let's drop it
    mogrify -quality 92 -strip -interlace JPEG "${outfile}"
    ;;
  png)
    # NOOP
    ;;
  *)
    # NOOP
    ;;
esac

# now close the original and show the graded one instead
eval "${showCmd} $outfile"
