BIN = irc

CFLAGS = -std=c99 -Os -D_POSIX_C_SOURCE=201112
LDFLAGS = -lncurses

all: ${BIN}

clean:
	rm -f ${BIN} *.o

.PHONY: all clean
