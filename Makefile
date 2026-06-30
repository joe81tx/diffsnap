PROG = diffsnap
SRCS = diffsnap.c

OS_NAME != uname -s
OS_ETCDIR != case "$(OS_NAME)" in FreeBSD) echo /usr/local/etc ;; *) echo /etc ;; esac
OS_RUNSTATEDIR != case "$(OS_NAME)" in FreeBSD) echo /var/run ;; *) echo /run ;; esac
OS_ZFS_PATH != case "$(OS_NAME)" in FreeBSD) echo /sbin/zfs ;; *) if [ -x /usr/sbin/zfs ]; then echo /usr/sbin/zfs; else echo /sbin/zfs; fi ;; esac

CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=

PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
ETCDIR ?= $(OS_ETCDIR)
LOGDIR ?= /var/log
RUNSTATEDIR ?= $(OS_RUNSTATEDIR)
ZFS_PATH ?= $(OS_ZFS_PATH)
INSTALL ?= install

CONF_PATH ?= $(ETCDIR)/diffsnap.conf
LOG_PATH ?= $(LOGDIR)/diffsnap.log
LOCK_PATH ?= $(RUNSTATEDIR)/diffsnap.lock

CPPFLAGS += -DCONF_PATH='"$(CONF_PATH)"'
CPPFLAGS += -DLOG_PATH='"$(LOG_PATH)"'
CPPFLAGS += -DLOCK_PATH='"$(LOCK_PATH)"'
CPPFLAGS += -DZFS_PATH='"$(ZFS_PATH)"'

.PHONY: all clean install uninstall

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

install: $(PROG)
	$(INSTALL) -d $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 0755 $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)
	$(INSTALL) -d $(DESTDIR)$(ETCDIR)
	@if [ ! -f "$(DESTDIR)$(ETCDIR)/diffsnap.conf" ]; then \
		$(INSTALL) -m 0644 diffsnap.conf $(DESTDIR)$(ETCDIR)/diffsnap.conf; \
	fi

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(PROG)

clean:
	rm -f $(PROG) *.o
