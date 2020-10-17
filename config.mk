# shttproxy version
VERSION = 0

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/man

# flags
CPPFLAGS = -DVERSION=\"$(VERSION)\" -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 -D_BSD_SOURCE
CFLAGS   = -std=c99 -pedantic -Wall -Os
LDFLAGS  = -s

# compiler and linker
CC = cc
