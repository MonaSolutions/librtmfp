OS := $(shell uname -s)

# Variables with default values
GPP?=g++

# Variables extendable
ifeq ($(OS),FreeBSD)
	CFLAGS+=-D_GLIBCXX_USE_C99 -std=c++11
else
	CFLAGS+=-std=c++11
endif
override INCLUDES+=-I./../MonaServer/MonaBase/include/
LIBDIRS+=-L./../MonaServer/MonaBase/lib/
LIBS+=-lMonaBase -lcrypto -lssl

# Variables fixed
ifeq ($(OS),Darwin)
	LIB=lib/librtmfp.dylib
	SHARED=-dynamiclib -install_name ./../LibRTMFP/$(LIB)
else
	LIB=lib/librtmfp.so
	SHARED=-shared
endif
SOURCES = $(wildcard ./*.cpp)
OBJECT = $(addprefix tmp/Release/,$(notdir $(SOURCES:%.cpp=%.o)))
OBJECTD = $(addprefix tmp/Debug/,$(notdir $(SOURCES:%.cpp=%.o)))

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

$(OBJECT):
	@echo compiling $(filter %/$(subst .o,.cpp,$(notdir $(@))),$(SOURCES))
	@$(GPP) $(CFLAGS) -fpic $(INCLUDES) -c -o $(@) $(filter %/$(subst .o,.cpp,$(notdir $(@))),$(SOURCES))

$(OBJECTD):
	@echo compiling $(filter %/$(subst .o,.cpp,$(notdir $(@))),$(SOURCES))
	@$(GPP) -g -D_DEBUG $(CFLAGS) -fpic $(INCLUDES) -c -o $(@) $(filter %/$(subst .o,.cpp,$(notdir $(@))),$(SOURCES))

clean:
	@echo cleaning project librtmfp
	@rm -f $(OBJECT) $(LIB)
	@rm -f $(OBJECTD) $(LIB)
