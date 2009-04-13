#
# Use "make C=1 foo" to enable sparse checking
# Use "make S=1 foo" to compile statically
#
BIN	:= mklogfs
SRC	:= mkfs.c fsck.c lib.c journal.c segment.c btree.c readwrite.c
OBJ	:= $(SRC:.c=.o)
BB	:= $(SRC:.c=.bb)
BBG	:= $(SRC:.c=.bbg)
DA	:= $(SRC:.c=.da)
COV	:= $(SRC:.c=.c.gcov)
ZLIB_O	:= crc32.o deflate.o adler32.o compress.o trees.o zutil.o

CC	:= gcc
CHECK	:= cgcc
CHECKFLAGS := -D__CHECK_ENDIAN__
CFLAGS	:= -std=gnu99
CFLAGS	+= -Wall
CFLAGS	+= -Os
CFLAGS	+= -D_FILE_OFFSET_BITS=64
CFLAGS	+= -g
#CFLAGS	+= -fprofile-arcs -ftest-coverage

all: $(BIN)

$(ZLIB_O): /usr/lib/libz.a
	ar -x /usr/lib/libz.a $@

ifdef S
EXTRA_OBJ := $(ZLIB_O)
CFLAGS += -static
else
CFLAGS += -lz
endif

mklogfs: $(EXTRA_OBJ)
mklogfs: mkfs.o lib.o btree.o segment.o readwrite.o
	$(CC) $(CFLAGS) -o $@ $^

logfsck: $(ZLIB_O)
logfsck: fsck.o lib.o journal.o super.o
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ): kerncompat.h logfs.h logfs_abi.h btree.h

%.o: %.c
ifdef C
	$(CHECK) $(CFLAGS) $(CHECKFLAGS) -c -o $@ $<
endif
	$(CC) $(CFLAGS) -c -o $@ $<


install: all ~/bin
	cp $(BIN) ~/bin/

distclean: clean
	$(RM) core

clean:
	$(RM) $(BIN) $(OBJ) $(BB) $(BBG) $(COV) $(DA) $(ZLIB_O)
