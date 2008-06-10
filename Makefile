BIN	:= mklogfs logfsck
SRC	:= mkfs.c fsck.c
OBJ	:= $(SRC:.c=.o)
BB	:= $(SRC:.c=.bb)
BBG	:= $(SRC:.c=.bbg)
DA	:= $(SRC:.c=.da)
COV	:= $(SRC:.c=.c.gcov)

CC	:= gcc
CFLAGS	:= -std=gnu99
CFLAGS	+= -Wall
CFLAGS	+= -Os
#CFLAGS	+= -g
#CFLAGS	+= -fprofile-arcs -ftest-coverage

all: $(BIN)


mklogfs: mkfs.o lib.o crc32.o deflate.o adler32.o compress.o trees.o zutil.o
	$(CC) $(CFLAGS) -static -lz -o $@ $^

logfsck: fsck.o lib.o crc32.o deflate.o adler32.o compress.o trees.o zutil.o
	$(CC) $(CFLAGS) -static -lz -o $@ $^

mkfs.o: kerncompat.h logfs.h
fsck.o: kerncompat.h logfs.h
lib.o: kerncompat.h logfs.h

%.o: %.c
	cgcc $(CFLAGS) -c -o $@ $<
	$(CC) $(CFLAGS) -c -o $@ $<


install: all ~/bin
	cp $(BIN) ~/bin/

distclean: clean
	$(RM) core

clean:
	$(RM) $(BIN) $(OBJ) $(BB) $(BBG) $(COV) $(DA)
