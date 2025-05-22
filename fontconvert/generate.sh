#! /bin/sh

./fontconvert /usr/share/fonts/misc/ter-x28b.pcf.gz 28 32-128 160-255 1024-1119 > ../main/ter_x28b_pcf28pt.h
./fontconvert /usr/share/fonts/misc/ter-x32b.pcf.gz 32 32-126 160-255 1024-1119 > ../main/ter_x32b_pcf32pt.h
./fontconvert /usr/share/fonts/gsfonts/C059-Bold.otf 37 45 48-58 > ../main/c509_bold37pt.h
./fontconvert /usr/share/fonts/gsfonts/C059-Bold.otf 29 48-58 > ../main/c509_bold29pt.h
