PROG = diffsnap
SRCS = diffsnap.c

OS_NAME != uname -s
OS_ETCDIR != case "$(OS_NAME)" in FreeBSD) echo /usr/local/etc ;; *) echo /etc ;; esac
OS_RUNSTATEDIR != case "$(OS_NAME)" in FreeBSD) echo /var/run ;; *) echo /run ;; esac
OS_ZFS_PATH != case "$(OS_NAME)" in FreeBSD) echo /sbin/zfs ;; *) if [ -x /usr/sbin/zfs ]; then echo /usr/sbin/zfs; else echo /sbin/zfs; fi ;; esac
OS_LOGCONFDIR != case "$(OS_NAME)" in FreeBSD) echo /usr/local/etc/newsyslog.conf.d ;; *) echo /etc/logrotate.d ;; esac
OS_LOGCONF_SRC != case "$(OS_NAME)" in FreeBSD) echo newsyslog.conf.d/diffsnap.conf ;; *) echo logrotate.d/diffsnap ;; esac
OS_LOGCONF_NAME != case "$(OS_NAME)" in FreeBSD) echo diffsnap.conf ;; *) echo diffsnap ;; esac
BUILD_SHA != git describe --always --dirty --abbrev=12 2>/dev/null || echo unknown

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
LOGCONFDIR ?= $(OS_LOGCONFDIR)
LOGCONF_SRC ?= $(OS_LOGCONF_SRC)
LOGCONF_NAME ?= $(OS_LOGCONF_NAME)
INSTALL ?= install
INSTALL_PROGRAM ?= $(INSTALL) -m 0755
INSTALL_DATA ?= $(INSTALL) -m 0644

CONF_PATH ?= $(ETCDIR)/diffsnap.conf
LOG_PATH ?= $(LOGDIR)/diffsnap.log
LOCK_PATH ?= $(RUNSTATEDIR)/diffsnap.lock

CPPFLAGS += -DCONF_PATH='"$(CONF_PATH)"'
CPPFLAGS += -DLOG_PATH='"$(LOG_PATH)"'
CPPFLAGS += -DLOCK_PATH='"$(LOCK_PATH)"'
CPPFLAGS += -DZFS_PATH='"$(ZFS_PATH)"'
CPPFLAGS += -DBUILD_SHA='"$(BUILD_SHA)"'

.PHONY: all clean install uninstall

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

install: $(PROG)
	$(INSTALL) -d $(DESTDIR)$(SBINDIR)
	$(INSTALL_PROGRAM) $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)
	$(INSTALL) -d $(DESTDIR)$(ETCDIR)
	$(INSTALL_DATA) diffsnap.conf $(DESTDIR)$(ETCDIR)/diffsnap.conf.sample
	test -f "$(DESTDIR)$(ETCDIR)/diffsnap.conf" || \
		$(INSTALL_DATA) diffsnap.conf "$(DESTDIR)$(ETCDIR)/diffsnap.conf"
	$(INSTALL) -d $(DESTDIR)$(LOGCONFDIR)
	$(INSTALL_DATA) $(LOGCONF_SRC) $(DESTDIR)$(LOGCONFDIR)/$(LOGCONF_NAME)

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/$(PROG)
	rm -f $(DESTDIR)$(ETCDIR)/diffsnap.conf.sample
	rm -f $(DESTDIR)$(LOGCONFDIR)/$(LOGCONF_NAME)
clean:
	rm -f $(PROG) *.o
