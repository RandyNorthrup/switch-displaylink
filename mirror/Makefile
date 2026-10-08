CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -std=gnu11 $(shell pkg-config --cflags libdrm x11 xext xfixes)
LDLIBS += $(shell pkg-config --libs libdrm x11 xext xfixes)

dl-mirror: dl-mirror.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

PREFIX ?= /usr/local

install: dl-mirror
	install -Dm0755 dl-mirror $(DESTDIR)$(PREFIX)/bin/dl-mirror
	install -Dm0644 dl-mirror.service $(DESTDIR)/etc/systemd/user/dl-mirror.service

clean:
	rm -f dl-mirror

.PHONY: install clean
