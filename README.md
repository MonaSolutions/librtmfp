librtmfp README
===============

**librtmfp** is a multi-platform and LGPL library implementing the client part of the RTMFP protocol. 

It is made to allow softwares to connect to RTMFP servers and publish or receive media streams with or without P2P.

Both MonaServer and AMS server are supported.

**librtmfp** can be integrated with [our fork of FFmpeg](https://github.com/thomasjammet/FFmpeg/) to get a reliable streaming experience.

## Installation

You will find the Installation instructions for **librtmfp** and FFmpeg in the INSTALL.md file (in this directory).

## Documentation

Coming soon...

## Usage

As **librtmfp** is a C++ LGPL library it can be included in many softwares.

A testing software (TestClient) is delivered with **librtmfp** as a sample of usage but we recommend [FFmpeg](https://github.com/thomasjammet/FFmpeg/) for production use.

 
### Sample FFmpeg commands
 
- Publishing an flv file to the server :
	
```
./ffmpeg -re -i in.flv -c copy -f flv rtmfp://<hostname and port + application name>/<stream name> < /dev/null
```

- Reading a stream from the server :

```
./ffmpeg -i rtmfp://<hostname and port + application name>/<stream name> -c copy -f flv out1.flv -y
```

- Publishing for P2P direct communication only :

```
./ffmpeg -re -i in.flv -p2pPublishing true -c copy -f flv rtmfp://<hostname and port + application name>/<stream name> < /dev/null
```

- Reading a stream from a peer (direct P2P) :
	
```
./ffmpeg -peerId <peer ID> -i rtmfp://<hostname and port + application name>/<stream name> -c copy -f flv out1.flv -y
```

- Publishing a stream into a NetGroup (P2P Multicast) :
	
```
./ffmpeg -re -i in.flv -c copy -netgroup <netgroup identifier> -f flv rtmfp://<hostname and port + application name>/<stream name> < /dev/null
```

- Reading a stream from a NetGroup (P2P Multicast) :

```
./ffmpeg -netgroup <netgroup identifier> -i rtmfp://<hostname and port + application name>/<stream name> -c copy -f flv out1.flv -y
```

**Notes:** 

- The netgroup identifier is the key obtained in AS3 code with GroupSpecifier.toString(), it can be *G:027f0201010103010c050e74657374011b00* for example,
- The *hostname and port + application name* field can be 127.0.0.1:1935/live for example,
- The *stream name* field is the name of the stream to read/publish (full example of url : rtmfp://127.0.0.1:1935/live/test),
- If you are using AMS you must specify an application name ("live" is the default one), with MonaServer you can ignore it.

### Sample ffplay commands

If you just want to play directly a stream you can use the ffplay command.

- Playing a stream from the server :

```
./ffplay -i rtmfp://<hostname and port + application name>/<stream name>
```

- Playing a stream from a peer (direct P2P) :
	
```
./ffplay -peerId <peer ID> -i rtmfp://<hostname and port + application name>/<stream name>
```

- Playing a stream from a NetGroup (P2P Multicast) :
	
```
./ffplay -netgroup <netgroup identifier> -i rtmfp://<hostname and port + application name>/<stream name>
```

### Sample TestClient commands (testing only)

- Publishing an flv file to the server :

```
./TestClient --log=8 --write --url=rtmfp://<hostname and port + application name>/<stream name> --mediaFile=in.flv
--logFile=testPubli.log
```
	
- Reading a stream from the server :

```
./TestClient --url=rtmfp://<hostname and port + application name>/<stream name> --mediaFile=out1.flv --logFile=testPlay1.log
```

- Publishing for P2P direct communication only :

```
./TestClient --p2pWrite --url=rtmfp://<hostname and port + application name>/<stream name> --mediaFile=in.flv --logFile=testPubli.log
```

- Reading a stream from a peer (direct P2P) :

```
./TestClient --peerId=<peer ID> --url=rtmfp://<hostname and port + application name>/<stream name> --mediaFile=out1.flv --logFile=testPlay1.log
```
	
- Publishing a stream into a NetGroup (P2P Multicast) :

```
./TestClient --p2pWrite --url=rtmfp://<hostname and port + application name>/<stream name> --netgroup=<netgroup identifier> --logFile=testPubli.log --mediaFile=in.flv
```

- Reading a stream from a NetGroup (P2P Multicast) :

```
./TestClient --url=rtmfp://<hostname and port + application name>/<stream name> --netgroup=<netgroup identifier> --logFile=testPlay1.log --mediaFile=out1.flv
```

### Additional arguments

Coming soon...
