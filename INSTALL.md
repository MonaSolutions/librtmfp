Installing librtmfp
===================

## Linux Installation

### Prerequisites

- [g++](https://gcc.gnu.org/) version >=5 (or compliant clang...)
- [OpenSSL](https://www.openssl.org/) libraries with headers, usually dev-package named `libssl-dev` or `openssl-devel`

### Installation steps

- Clone librtmfp :

	git clone https://github.com/MonaSolutions/librtmfp.git

- Compile librtmfp :

	make
	
- [Optional] And then install it using the same command as the sudoer :

	make install

**Note:** You need g++ to compile librtmfp.

## Windows Installation

- First, install Visual Studio Express 2015 (or newer) for Windows Desktop,
- Clone librtmfp :

	git clone https://github.com/MonaSolutions/librtmfp.git

- Open the *librtmfp.sln* project file, and start to build the project,
- The project *TestClient* is a sample of client using librtmfp for test purpose.

## Integration in FFmpeg

A temporary repository of FFmpeg is available with a wrapper to librtmfp : https://github.com/thomasjammet/FFmpeg.git

### Installing FFmepg with librtmfp on Linux

**Prerequisites:**

- You must install first libx264, libx265, librtmp and libspeex developer versions (see next point) to have h264, HEVC, RTMP and speex working,
- You can remove --enable-debug if you doesn't want to debug ffmpeg,
- You can remove --enable-librtmp and libspeex if you don't need them,
- You can remove --enable-libx265 if you don't need the HEVC encoder/decoder
- You can also remove --enable-libx264 and --enable-gpl if you don't need the h264 encoder/decoder,
- Install SDL2-devel (or libsdl2-dev) if you want to compile ffplay (on older linux distributions SDL is used instead).

To compile it just do a git clone and run the following command in FFmpeg directory :

	./configure --disable-x86asm --enable-librtmp --enable-librtmfp --enable-libspeex --enable-libx264 --enable-libx265 --enable-debug --enable-gpl && make

**Note:** ffmpeg_g is the debug version of ffmpeg with debugging symbolic links.
	
Then you can install it with the following command as root :

	make install

#### How to install libx264, librtmp and libspeex developer versions?

**On Fedora** you can use the RPM Fusion repository, to install it run the following command as root :

    dnf install http://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm http://download1.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm

Then just install the packages with dnf :

    dnf install librtmp-devel speex-devel x264-devel x265-devel

### Installating FFmpeg with librtmfp on Windows

This is not planned for now since it will need to support cygwin, contact us if you want this feature.
	
## Usage

To know how to use librtmfp and ffmpeg read the README.md file (in this directory).
