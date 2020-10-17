# See LICENSE file for copyright and license details
# shttproxy - simple web server
.POSIX:

include config.mk

all: shttproxy

shttproxy: shttproxy.c arg.h config.h config.mk
	$(CC) -o $@ $(CPPFLAGS) $(CFLAGS) shttproxy.c $(LDFLAGS)

config.h:
	cp config.def.h $@

clean:
	rm -f shttproxy

dist:
	rm -rf "shttproxy-$(VERSION)"
	mkdir -p "shttproxy-$(VERSION)"
	cp -R LICENSE Makefile arg.h config.def.h config.mk shttproxy.1 \
		shttproxy.c "shttproxy-$(VERSION)"
	tar -cf - "shttproxy-$(VERSION)" | gzip -c > "shttproxy-$(VERSION).tar.gz"
	rm -rf "shttproxy-$(VERSION)"

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	cp -f shttproxy "$(DESTDIR)$(PREFIX)/bin"
	chmod 755 "$(DESTDIR)$(PREFIX)/bin/shttproxy"
	mkdir -p "$(DESTDIR)$(MANPREFIX)/man1"
	cp shttproxy.1 "$(DESTDIR)$(MANPREFIX)/man1/shttproxy.1"
	chmod 644 "$(DESTDIR)$(MANPREFIX)/man1/shttproxy.1"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/shttproxy"
	rm -f "$(DESTDIR)$(MANPREFIX)/man1/shttproxy.1"
