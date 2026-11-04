#!/bin/sh
# Cross-compiles a tiny ffmpeg.exe (Linux + mingw-w64) used by Vysion's "Save as copy" trimming.
# It can only copy streams between containers (no encoders/decoders), which keeps it small and
# makes cuts lossless and instant. LGPL build (no --enable-gpl). Output: build/ffmpeg.exe
set -e
cd "$(dirname "$0")/.."
VER="${FFMPEG_VERSION:-7.1}"
mkdir -p build/ffmpeg-src
if [ ! -f "build/ffmpeg-src/ffmpeg-$VER/configure" ]; then
  curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz" | tar -xJ -C build/ffmpeg-src
fi
cd "build/ffmpeg-src/ffmpeg-$VER"

DEMUX=mov,matroska,avi,mpegts,mpegps,flv,asf,ogg,mxf,h264,hevc,aac,mp3,ac3,wav,flac
MUX=mp4,mov,ipod,matroska,webm,avi,mpegts,flv,asf,ogg,mxf,mp3,adts,ac3,wav,flac,framecrc,null
PARSE=h264,hevc,av1,vp8,vp9,mpeg4video,mpegvideo,vc1,mjpeg,aac,aac_latm,ac3,mpegaudio,opus,vorbis,flac,dca
BSF=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,vp9_superframe,vp9_superframe_split,extract_extradata,av1_frame_merge,av1_frame_split,mpeg4_unpack_bframes,setts,null

./configure --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32- \
  --pkg-config=false --enable-static --disable-shared --enable-small \
  --disable-everything --disable-autodetect --disable-network --disable-doc \
  --disable-ffplay --disable-ffprobe --disable-avdevice --disable-swscale --disable-postproc \
  --enable-protocol=file,pipe \
  --enable-demuxer=$DEMUX --enable-muxer=$MUX --enable-parser=$PARSE --enable-bsf=$BSF \
  --extra-ldflags="-static -static-libgcc"
make -j"$(nproc)" ffmpeg.exe
x86_64-w64-mingw32-strip ffmpeg.exe
cp ffmpeg.exe ../../ffmpeg.exe
ls -la ../../ffmpeg.exe
