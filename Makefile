VERSION=0.1

prefix=/usr

LIBDIR=$(prefix)/lib
ifeq ($(shell uname -m), x86_64)
	LIBDIR = $(prefix)/lib64
endif
PKGCONFIGPATH=$(shell pkg-config --variable pc_path pkg-config | cut -d ':' -f 1)
ifeq ($(PKGCONFIGPATH), )
	PKGCONFIGPATH=$(LIBDIR)/pkgconfig
endif

CRYPTO_REQ=libssl,libcrypto
PUBLIC_LIBS=
PRIVATE_LIBS=

OS := $(shell uname -s)

# Variables with default values
GPP?=g++

# Variables extendable
ifeq ($(OS),FreeBSD)
	CFLAGS+=-D_GLIBCXX_USE_C99 -std=c++11
else
	CFLAGS+=-std=c++11
endif
override INCLUDES+=-I./../MonaBase/include/ -I./include/
LIBDIRS+=-L./../MonaBase/lib/
LIBS+=-Wl,-Bstatic -l:libMonaBase.ar -Wl,-Bdynamic -lcrypto -lssl

INCDIR=/usr/include/librtmfp/

# Variables fixed
ifeq ($(OS),Darwin)
	LIB=lib/librtmfp.dylib
	SHARED=-dynamiclib -install_name ./../LibRTMFP/$(LIB)
else
	LIB=lib/librtmfp.so
	SHARED=-shared
endif

# Variables fixed
SOURCES = $(wildcard */*.cpp)
OBJECT = $(SOURCES:sources/%.cpp=tmp/Release/%.o)
OBJECTD = $(SOURCES:sources/%.cpp=tmp/Debug/%.o)

.PHONY: debug release

release:
	mkdir -p tmp/Release
	mkdir -p lib
	@$(MAKE) -k $(OBJECT)
	@echo creating dynamic lib $(LIB)
	@$(GPP) $(CFLAGS) $(LIBDIRS) -fPIC $(SHARED) -o $(LIB) $(OBJECT) $(LIBS)

debug:
	mkdir -p tmp/Debug
	mkdir -p lib
	@$(MAKE) -k $(OBJECTD)
	@echo creating dynamic debug lib $(LIB)
	@$(GPP) -g -D_DEBUG $(CFLAGS) $(LIBDIRS) -fPIC $(SHARED) -o $(LIB) $(OBJECTD) $(LIBS)

librtmfp.pc: librtmfp.pc.in Makefile
	sed -e "s;@prefix@;$(prefix);" -e "s;@libdir@;$(LIBDIR);" \
	    -e "s;@VERSION@;$(VERSION);" \
	    -e "s;@CRYPTO_REQ@;$(CRYPTO_REQ);" \
	    -e "s;@PUBLIC_LIBS@;$(PUBLIC_LIBS);" \
	    -e "s;@PRIVATE_LIBS@;$(PRIVATE_LIBS);" librtmfp.pc.in > $@

install: librtmfp.pc
	-mkdir -p $(INCDIR)
	cp ./include/librtmfp.h $(INCDIR)
	cp $(LIB) $(LIBDIR)
	cp librtmfp.pc $(PKGCONFIGPATH)

$(OBJECT): tmp/Release/%.o: sources/%.cpp
	@echo compiling $(@:tmp/Release/%.o=sources/%.cpp)
	@$(GPP) $(CFLAGS) -fpic $(INCLUDES) -c -o $(@) $(@:tmp/Release/%.o=sources/%.cpp)

$(OBJECTD): tmp/Debug/%.o: sources/%.cpp
	@echo compiling $(@:tmp/Debug/%.o=sources/%.cpp)
	@$(GPP) -g -D_DEBUG $(CFLAGS) -fpic $(INCLUDES) -c -o $(@) $(@:tmp/Debug/%.o=sources/%.cpp)

clean:
	@echo cleaning project librtmfp
	@rm -f $(OBJECT) $(LIB)
	@rm -f $(OBJECTD) $(LIB)
