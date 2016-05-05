BIN = irc

CFLAGS = -std=c99 -Os -D_POSIX_C_SOURCE=201112 -D_GNU_SOURCE
LDFLAGS = -lncursesw

all: ${BIN}

clean:
	rm -f ${BIN} *.o

.PHONY: all clean
