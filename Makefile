QOS_OBJ = qos.o
QOS_LIBNAME = libqos

LDLIBS = -lm

LIBVERSION = 1.0.0

PREFIX ?= /opt/arm/nx1s/libqos
INCLUDE_PATH ?= include
LIBRARY_PATH ?= lib

INSTALL_INCLUDE_PATH = $(DESTDIR)$(PREFIX)/$(INCLUDE_PATH)
INSTALL_LIBRARY_PATH = $(DESTDIR)$(PREFIX)/$(LIBRARY_PATH)

INSTALL ?= cp -a

CC = /opt/arm-toolchain/ext-toolchain/bin/arm-linux-gnueabi-gcc

#CFLAGS += -g -O2
PIC_FLAGS = -fPIC
R_CFLAGS = $(PIC_FLAGS) $(CFLAGS)

#library file extensions
SHARED = so
STATIC = a

QOS_STATIC = $(QOS_LIBNAME).$(STATIC)

SHARED_CMD = $(CC) -shared -o

.PHONY: all static clean

all: static install

static: $(QOS_STATIC)

.c.o:
	$(CC) -c $(R_CFLAGS) $<

#static libraries
$(QOS_STATIC): $(QOS_OBJ)
	$(AR) rcs $@ $<
install:
	-@mkdir -p $(INSTALL_INCLUDE_PATH) $(INSTALL_LIBRARY_PATH)
	cp -rf qos.h $(INSTALL_INCLUDE_PATH)
	cp -rf libqos.a $(INSTALL_LIBRARY_PATH)
#objects
$(QOS_OBJ): qos.c qos.h


clean:
	$(RM) $(QOS_OBJ)
	$(RM) $(QOS_STATIC)
