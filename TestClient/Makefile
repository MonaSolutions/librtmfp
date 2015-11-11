OS := $(shell uname -s)

# Variables with default values
GCC?=gcc
EXEC?=TestClient

override INCLUDES+=-I./../
LIBDIRS+=-L./../lib/
LDFLAGS+="-Wl,-rpath,/usr/local/lib/,-rpath,./../lib/"
LIBS+=-pthread -lrtmfp -lcrypto -lssl
ifeq ($(OS),Darwin)
	LBITS := $(shell getconf LONG_BIT)
	ifeq ($(LBITS),64)
	   # just require for OSX 64 buts
	   LIBS +=  -pagezero_size 10000 -image_base 100000000
	endif
endif

# Variables fixed
#SOURCES = $(wildcard ./*.c)
OBJECT = tmp/Release/Main.o
OBJECTD = tmp/Debug/Main.o

# This line is used to ignore possibly existing folders release/debug
.PHONY: release debug

release:	
	mkdir -p tmp/Release/
	@$(MAKE) -k $(OBJECT)
	@echo creating executable $(EXEC)
	@$(GCC) $(CFLAGS) $(LDFLAGS) $(LIBDIRS) -o $(EXEC) $(OBJECT) $(LIBS)

debug:	
	mkdir -p tmp/Debug/
	@$(MAKE) -k $(OBJECTD)
	@echo creating debugging executable $(EXEC)
	@$(GCC) -g -D_DEBUG $(CFLAGS) $(LDFLAGS) $(LIBDIRS) -o $(EXEC) $(OBJECTD) $(LIBS)

$(OBJECT): Main.c
	@echo compiling $(@:tmp/Release/%.o=%.c)
	@$(GCC) $(CFLAGS) $(INCLUDES) -c -o $(@) $(@:tmp/Release/%.o=%.c)

$(OBJECTD): Main.c
	@echo compiling $(@:tmp/Debug/%.o=%.c)
	@$(GCC) -g -D_DEBUG $(CFLAGS) $(INCLUDES) -c -o $(@) $(@:tmp/Debug/%.o=%.c)

clean:
	@echo cleaning project $(EXEC)
	@rm -f $(OBJECT) $(EXEC)
	@rm -f $(OBJECTD) $(EXEC)
