librtmfp README
===============

librtmfp is a multi-platform and LGPL library implementing the client part of the RTMFP protocol. 

It is made to allow softwares to connect to RTMFP servers and publish and receive media streams.

librtmfp can be integrated with FFmpeg to get the best experience.

# Installation

- First you must download and compile the MonaBase library,
- As root user install MonaBase using the following command :
  make install
- Then compile librtmfp :
  make
- And then install it using the following command as the sudoer:
  make install

Note: you need g++ to compile librtmfp

A temporary repository of FFmpeg is available with a wrapper to librtmfp : https://github.com/thomasjammet/FFmpeg.git

To use it just do a git clone and run the following command in FFmpeg directory :

./configure --disable-yasm --enable-librtmp --enable-librtmfp --enable-debug && make

