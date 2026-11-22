CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra
LDFLAGS ?=

PREFIX  ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin

BINS := lsbv svcreate svchange svremove

all: $(BINS)

$(BINS): %: %.c ssvm_common.c ssvm_mkfs.c ssvm.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $@.c ssvm_common.c ssvm_mkfs.c

clean:
	rm -f $(BINS)

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(BINS) $(DESTDIR)$(SBINDIR)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(SBINDIR)/,$(BINS))

test: all
	bash tests/run-tests.sh

.PHONY: all clean install uninstall test
