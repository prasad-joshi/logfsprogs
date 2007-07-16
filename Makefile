BIN	:= mklogfs
SRC	:= mkfs.c
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


$(BIN):	$(OBJ)
	$(CC) $(CFLAGS) -lz -o $@ $^


%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<


install: all ~/bin
	cp $(BIN) ~/bin/

distclean: clean
	$(RM) core

clean:
	$(RM) $(BIN) $(OBJ) $(BB) $(BBG) $(COV) $(DA)
