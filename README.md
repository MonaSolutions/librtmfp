librtmfp README
===============

librtmfp is a multi-platform and LGPL library implementing the client part of the RTMFP protocol. 

It is made to allow softwares to connect to RTMFP servers and publish and receive media streams.

librtmfp can be integrated with FFmpeg to get the best experience.

# Simple Installation

- First you must download and compile the MonaBase library,
- Clone MonaServer :
  git clone https://github.com/MonaSolutions/MonaServer.git
- Cd into the MonaBase directory,
- Compile MonaBase using the following command :
  make debug
- Compile librtmfp (must be at the same hierarchy level than the directory MonaServer) :
  make debug
- And then install it using the same command as the sudoer :
  make install

Note: you need g++ to compile librtmfp

# Integration in FFmpeg

A temporary repository of FFmpeg is available with a wrapper to librtmfp : https://github.com/thomasjammet/FFmpeg.git

To use it just do a git clone and run the following command in FFmpeg directory :

./configure --disable-yasm --enable-librtmp --enable-librtmfp --enable-libspeex --enable-debug && make

Notes: 
 - You must install first librtmp and libspeex developer versions (-dev or -devel)
 - You can remove --enable-debug if you doesn't want to debug ffmpeg

# How to install librtmp and libspeex developer versions?

On Fedora you can use the RPM Fusion repository, to install it run the following command as root :

  dnf install http://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm http://download1.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm

Then just install it with dnf :

  dnf install librtmp-devel && dnf install speex-devel
 
# Sample commands
 
Then you can run ffmpeg:
./ffmpeg -i rtmfp://127.0.0.1/test123 -c:a copy -f flv test123.flv

Note: ffmpeg_g is the debug version of ffmpeg with debuging symbolic links.