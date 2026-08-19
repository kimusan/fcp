# fcp - Faster CP
# Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
# MIT License

PREFIX     ?= /usr/local
CC         ?= gcc
CFLAGS     ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS    ?=
DEPS       ?= -lpthread -lrt -lcrypto -lm

SOURCES = fcp.c util.c copy.c identical.c hash.c progress.c colors.c queue.c config.c
HEADERS = fcp.h util.h copy.h identical.h hash.h progress.h colors.h queue.h config.h
TARGET  = fcp

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(DEPS)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -m 644 man/$(TARGET).1 $(DESTDIR)$(PREFIX)/share/man/man1/$(TARGET).1
	mandb -q 2>/dev/null || true

clean:
	rm -f $(TARGET)

deb:
	dpkg-buildpackage -us -uc -b

.PHONY: all install clean deb