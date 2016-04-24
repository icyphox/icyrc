build: clean
	cc irc.c -o irc -lncurses -Wall -std=c99 -Os
	strip irc

clean:
	rm -f irc
