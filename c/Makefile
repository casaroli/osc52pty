CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

osc52pty: osc52pty.c
	$(CC) $(CFLAGS) -o $@ osc52pty.c

test: test_osc52pty
	./test_osc52pty

test_osc52pty: test.c osc52pty.c
	$(CC) $(CFLAGS) -g -o $@ test.c

install: osc52pty
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 osc52pty $(DESTDIR)$(PREFIX)/bin/osc52pty

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/osc52pty

clean:
	rm -f osc52pty test_osc52pty

.PHONY: test install uninstall clean
