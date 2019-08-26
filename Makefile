# Generate version from RTMFP.h file
TMP=$(shell printf "%d" $(shell grep RTMFP_LIB_VERSION include/RTMFP.h | grep -o '\b0x\w*'))
MAJ=$(shell expr $(TMP) / 16777216)
INTER=$(shell expr \( $(TMP) - $(MAJ) \* 16777216 \) / 65536)
MIN=$(shell expr $(TMP) - $(MAJ) \* 16777216 - $(INTER) \* 65536)
VERSION="$(MAJ).$(INTER).$(MIN)"

prefix=/usr

LIBDIR=$(prefix)/lib/
ifeq ($(shell uname -m), x86_64)
	ifneq "$(wildcard $(prefix)/lib64)" ""
		LIBDIR="$(prefix)/lib64/"
	else ifneq "$(wildcard $(prefix)/lib/x86_64-linux-gnu)" ""
		LIBDIR="$(prefix)/lib/x86_64-linux-gnu/"
	endif
endif
PKGCONFIGPATH=$(shell pkg-config --variable pc_path pkg-config | cut -d ':' -f 1)
ifeq ($(PKGCONFIGPATH), )
	PKGCONFIGPATH=$(LIBDIR)/pkgconfig
endif

# pkgconfig description variables
CRYPTO_REQ=libssl,libcrypto
PUBLIC_LIBS=
PRIVATE_LIBS=

OS := $(shell uname -s)

# Variables with default values
GPP?=g++

# Variables extendable
CFLAGS+=-std=c++11 -Wall -Wno-reorder -Wno-terminate -Wno-unknown-pragmas -Wno-unknown-warning-option
ifeq ($(OS),FreeBSD)
	CFLAGS+=-D_GLIBCXX_USE_C99
endif
override INCLUDES+=-I./include/
LIBS+=-Wl,-Bdynamic -lcrypto -lssl -lpthread

INCDIR=/usr/include/librtmfp/

# Variables fixed
ifeq ($(OS),Darwin)
	LIBNAME=librtmfp.dylib
	SHARED=-dynamiclib -install_name ./../LibRTMFP/$(LIB)
else
	LIBNAME=librtmfp.so
	AR=lib/librtmfp.a
	SHARED=-shared
endif
LIB=lib/$(LIBNAME)

# Variables fixed
SOURCES = $(wildcard sources/*.cpp sources/Base/*.cpp)
OBJECT = $(SOURCES:sources/%.cpp=tmp/Release/%.o)
OBJECTD = $(SOURCES:sources/%.cpp=tmp/Debug/%.o)

.PHONY: debug release librtmfp.pc

release:
	mkdir -p tmp/Release/Base
	mkdir -p lib
	@$(MAKE) -k $(OBJECT)
	@echo creating dynamic lib $(LIB)
	@$(GPP) $(CFLAGS) $(LIBDIRS) -fPIC $(SHARED) -o $(LIB) $(OBJECT) $(LIBS)
	@if [ "$(OS)" != "Darwin" ]; then\
		echo "creating static lib $(AR)";\
		ar rcs $(AR) $(OBJECT);\
	fi

debug:
	mkdir -p tmp/Debug/Base
	mkdir -p lib
	@$(MAKE) -k $(OBJECTD)
	@echo creating dynamic debug lib $(LIB)
	@$(GPP) -g -D_DEBUG $(CFLAGS) $(LIBDIRS) -fPIC $(SHARED) -o $(LIB) $(OBJECTD) $(LIBS)
	@if [ "$(OS)" != "Darwin" ]; then\
		echo "creating static lib $(AR)";\
		ar rcs $(AR) $(OBJECTD);\
	fi

librtmfp.pc:
	@echo "compiling librtmfp.pc.in with version $(VERSION)"
	sed -e "s;@prefix@;$(prefix);" -e "s;@libdir@;$(LIBDIR);" \
	    -e "s;@VERSION@;$(VERSION);" \
	    -e "s;@CRYPTO_REQ@;$(CRYPTO_REQ);" \
	    -e "s;@PUBLIC_LIBS@;$(PUBLIC_LIBS);" \
	    -e "s;@PRIVATE_LIBS@;$(PRIVATE_LIBS);" librtmfp.pc.in > $@

install: librtmfp.pc
	-mkdir -p $(INCDIR)
	cp ./include/librtmfp.h $(INCDIR) && chmod 644 $(INCDIR)/librtmfp.h
	cp $(LIB) $(LIBDIR) && chmod 755 $(LIBDIR)/$(LIBNAME)
	test -d "$(PKGCONFIGPATH)" || mkdir -p "$(PKGCONFIGPATH)" && cp librtmfp.pc $(PKGCONFIGPATH) && chmod 644 $(PKGCONFIGPATH)/librtmfp.pc

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

