CXX ?= g++
MARCH ?= native
INCLUDES := -I/usr/local/include -I/opt/homebrew/include -Ilibs
LIBPATHS := -L/usr/local/lib -L/opt/homebrew/lib
LDFLAGS := $(LIBPATHS) -lwebp -lwebpdemux -lssl -lcrypto -lz
CFLAGS=-W -Wall -Wextra -Wno-unused-parameter -O3 -fPIC -march=$(MARCH)
FIRMWARE_VERSION ?= dev
CXXFLAGS :=$(CFLAGS) -fno-exceptions -std=c++23 -MMD -MP -DFIRMWARE_VERSION='"$(FIRMWARE_VERSION)"'
TARGET := tronberry
SRCS := main.cc startup.cc
RGB_LIB_DISTRIBUTION=libs/rpi-rgb-led-matrix
RGB_INCDIR=$(RGB_LIB_DISTRIBUTION)/include
RGB_LIBDIR=$(RGB_LIB_DISTRIBUTION)/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a
RGB_LDFLAGS+=-L$(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -lrt -lm -lpthread
CPPFLAGS=-D_FILE_OFFSET_BITS=64 -DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_NO_EXCEPTIONS -DCPPHTTPLIB_NO_DEFAULT_USER_AGENT -DCPPHTTPLIB_ZLIB_SUPPORT -DJSON_NOEXCEPTION -DJSON_NO_IO -DJSON_USE_IMPLICIT_CONVERSIONS=0 $(INCLUDES) -I$(RGB_INCDIR)

.PHONY: all clean $(RGB_LIBRARY) check-and-reinit-submodules

all: $(TARGET)

$(RGB_LIBRARY): check-and-reinit-submodules
	$(MAKE) -C $(RGB_LIBDIR) CFLAGS="$(CFLAGS) -DDEFAULT_HARDWARE='\"regular\"'"

OBJS := $(SRCS:.cc=.o)
DEPS := $(OBJS:.o=.d)

-include $(DEPS)

$(TARGET): $(OBJS) $(RGB_LIBRARY)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS) $(RGB_LDFLAGS)

clean: check-and-reinit-submodules
	rm -f $(TARGET)
	$(MAKE) -C $(RGB_LIBDIR) clean
	find . -name '*.o' -delete
	find . -name '*.a' -delete
	find . -name '*.d' -delete

check-and-reinit-submodules:
	@if [ -d .git ] && git submodule status | egrep -q '^[-+]' ; then \
		echo "INFO: Need to reinitialize git submodules"; \
		git submodule update --init; \
	fi
