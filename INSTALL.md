Installing librtmfp
===================

## Simple Installation

- First you must download and compile the MonaBase library,
- Clone MonaServer :

	git clone https://github.com/MonaSolutions/MonaServer.git
- Cd into the MonaBase directory,
- Compile MonaBase using the following command :
	
	make
- Compile librtmfp (must be at the same hierarchy level than the directory MonaServer) :

	make
- And then install it using the same command as the sudoer :

	make install

**Note:** You need g++ to compile librtmfp.

## Integration in FFmpeg

A temporary repository of FFmpeg is available with a wrapper to librtmfp : https://github.com/thomasjammet/FFmpeg.git

### Installing FFmepg with librtmfp on Linux

**Prerequisites:**

- You must install first librtmp and libspeex developer versions (see next point),
- You can remove --enable-debug if you doesn't want to debug ffmpeg,
- Install SDL-devel (or libsdl-dev) if you want to compile ffplay.

To use it just do a git clone and run the following command in FFmpeg directory :

	./configure --disable-yasm --enable-librtmp --enable-librtmfp --enable-libspeex --enable-debug && make

**Note:** ffmpeg_g is the debug version of ffmpeg with debugging symbolic links.
	
Then you can install it with the following command as root :

	make install

#### How to install librtmp and libspeex developer versions?

**On Fedora** you can use the RPM Fusion repository, to install it run the following command as root :

    dnf install http://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm http://download1.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm

Then just install it with dnf :

    dnf install librtmp-devel && dnf install speex-devel

### Installating FFmpeg with librtmfp on Windows

Coming soon...
	
## Usage

To know how to use librtmfp and ffmpeg read the README.md file (in this directory).
