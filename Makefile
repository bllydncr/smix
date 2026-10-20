PREFIX = /usr/local
CC     = cc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L

all: smix

config.h:
	cp config.def.h $@

smix: smix.c config.h
	$(CC) $(CFLAGS) -o $@ smix.c $(LDFLAGS)

clean:
	rm -f smix

install: smix
	install -Dm755 smix $(DESTDIR)$(PREFIX)/bin/smix

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/smix

.PHONY: all clean install uninstall
