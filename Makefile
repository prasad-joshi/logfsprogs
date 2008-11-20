#
# Use "make C=1 foo" to enable sparse checking
#
BIN	:= mklogfs logfsck
SRC	:= mkfs.c fsck.c lib.c journal.c
OBJ	:= $(SRC:.c=.o)
BB	:= $(SRC:.c=.bb)
BBG	:= $(SRC:.c=.bbg)
DA	:= $(SRC:.c=.da)
COV	:= $(SRC:.c=.c.gcov)
ZLIB_O	:= crc32.o deflate.o adler32.o compress.o trees.o zutil.o

CC	:= gcc
CHECK	:= cgcc
CFLAGS	:= -std=gnu99
CFLAGS	+= -Wall
CFLAGS	+= -Os
#CFLAGS	+= -g
#CFLAGS	+= -fprofile-arcs -ftest-coverage

all: $(BIN)

$(ZLIB_O): /usr/lib/libz.a
	ar -x /usr/lib/libz.a $@

mklogfs: $(ZLIB_O)
mklogfs: mkfs.o lib.o
	$(CC) $(CFLAGS) -static -lz -o $@ $^

logfsck: $(ZLIB_O)
logfsck: fsck.o lib.o journal.o super.o
	$(CC) $(CFLAGS) -static -lz -o $@ $^

fsck.o: kerncompat.h logfs.h
lib.o: kerncompat.h logfs.h
mkfs.o: kerncompat.h logfs.h
super.o: kerncompat.h logfs.h

%.o: %.c
ifdef C
	$(CHECK) $(CFLAGS) -c -o $@ $<
endif
	$(CC) $(CFLAGS) -c -o $@ $<


install: all ~/bin
	cp $(BIN) ~/bin/

distclean: clean
	$(RM) core

clean:
	$(RM) $(BIN) $(OBJ) $(BB) $(BBG) $(COV) $(DA) $(ZLIB_O)
