CFLAGS ?= -O2 -Wall -Wextra -std=gnu11 -fPIC
PREFIX ?= /usr/local

dl-fakerandr.so: dl-fakerandr.c
	$(CC) $(CFLAGS) -shared -o $@ $< -ldl -lpthread

install: dl-fakerandr.so
	install -D -m 0755 dl-fakerandr.so $(DESTDIR)$(PREFIX)/lib/dl-fakerandr.so

clean:
	rm -f dl-fakerandr.so

.PHONY: install clean
