CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -std=gnu11 $(shell pkg-config --cflags libdrm x11 xext xfixes)
LDLIBS += $(shell pkg-config --libs libdrm x11 xext xfixes)

dl-mirror: dl-mirror.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f dl-mirror

.PHONY: clean
