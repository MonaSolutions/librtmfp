# librtmfp README


librtmfp is a multi-platform and LGPL library implementing the client part of the RTMFP protocol. 

It is made to allow softwares to connect to RTMFP servers and publish and receive media streams.

librtmfp can be integrated with FFmpeg to get the best experience.

## Simple Installation

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

## Integration in FFmpeg

A temporary repository of FFmpeg is available with a wrapper to librtmfp : https://github.com/thomasjammet/FFmpeg.git

### Installation on Linux

To use it just do a git clone and run the following command in FFmpeg directory :

./configure --disable-yasm --enable-librtmp --enable-librtmfp --enable-libspeex --enable-debug && make

**Notes:**
 - You must install first librtmp and libspeex developer versions (-dev or -devel)
 - You can remove --enable-debug if you doesn't want to debug ffmpeg

#### How to install librtmp and libspeex developer versions?

On Fedora you can use the RPM Fusion repository, to install it run the following command as root :

    dnf install http://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm http://download1.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm

Then just install it with dnf :

    dnf install librtmp-devel && dnf install speex-devel
 
#### Sample commands
 
Then you can run ffmpeg:

    ./ffmpeg -i rtmfp://127.0.0.1/test123 -c:a copy -f flv test123.flv

**Note:** ffmpeg_g is the debug version of ffmpeg with debugging symbolic links.

### Installation on Windows

Please read the following page to have all informations about compilation with MSVC : https://trac.ffmpeg.org/wiki/CompilationGuide/MSVC

The integration of FFmpeg on Windows require the use of mingw and yasm, so first install the prerequisites into a folder (for example *c:\99*) :

 * ​[C99-to-C89 Converter](https://github.com/libav/c99-to-c89/) & Wrapper if using MSVC 2012 or earlier.
 *​ [msinttypes](http://code.google.com/p/msinttypes/) if using MSVC 2012 or earlier.
 * ​[MSYS](http://www.mingw.org/)
 * ​[YASM](http://yasm.tortall.net/) (install the normal version, not the VS2010 version)

**Note:** For MSYS after installing mingw-get you must :
 * Install msys with the following command :
 
     mingw-get install msys
 * Copy the **pr.exe** file from [msys-coreutils](http://sourceforge.net/projects/mingw/files/MSYS/Base/msys-core/_obsolete/coreutils-5.97-MSYS-1.0.11-2/coreutils-5.97-MSYS-1.0.11-snapshot.tar.bz2/download) image to the msys/bin folder.
 
Then, you need to add the folder c:\c99 into your PATH environment variable.

Rename the yasm executable you will use to yasm.exe.

Create an INCLUDE environment variable, and point it to c:\c99; this is the location where the compiler will find inttypes.h.
Create a LIB environment variable, and point it to c:\c99; this is the location where the compiler will find the external libraries and includes.

Clone the [FFmpeg](https://github.com/thomasjammet/FFmpeg.git) repository, taking care that git option *core.autocrlf* is set to false.

TODO: Here we must be able to compile MonaBase with MinGW!

Finally open *msys.bat* in a MSVC command line tool and run the following commands :

    ./configure --toolchain=msvc --enable-librtmp --enable-librtmfp --enable-libspeex --enable-debug
	make
