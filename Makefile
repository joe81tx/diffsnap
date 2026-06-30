PROG = diffsnap
SRCS = diffsnap.c

CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
ETCDIR ?= $(PREFIX)/etc
MANDIR ?= $(PREFIX)/share/man
INSTALL ?= install

.PHONY: all clean install uninstall

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

install: $(PROG)
	$(INSTALL) -d $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 0755 $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)
	$(INSTALL) -d $(DESTDIR)$(ETCDIR)
	@if [ ! -f "$(DESTDIR)$(ETCDIR)/diffsnap.conf" ]; then \
		$(INSTALL) -m 0644 examples/diffsnap.conf $(DESTDIR)$(ETCDIR)/diffsnap.conf; \
	fi

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(PROG)

clean:
	rm -f $(PROG) *.o
