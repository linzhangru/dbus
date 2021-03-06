dbusdatadir=$(datadir)/dbus-1
legacydbusdatadir=$(sysconfdir)/dbus-1
dbus_daemon_execdir = $(DBUS_DAEMONDIR)

DBUS_BUS_LIBS = \
	$(CODE_COVERAGE_LDFLAGS) \
	$(XML_LIBS) \
	$(SELINUX_LIBS) \
	$(APPARMOR_LIBS) \
	$(THREAD_LIBS) \
	$(ADT_LIBS) \
	$(NETWORK_libs) \
	$(NULL)

DBUS_LAUNCHER_LIBS = \
	$(CODE_COVERAGE_LDFLAGS) \
	$(XML_LIBS) \
	$(THREAD_LIBS) \
	$(NETWORK_libs) \
	$(NULL)

AM_CPPFLAGS = \
	$(CODE_COVERAGE_CPPFLAGS) \
	-I$(top_srcdir) \
	$(DBUS_STATIC_BUILD_CPPFLAGS) \
	$(XML_CFLAGS) \
	$(APPARMOR_CFLAGS) \
	-DDBUS_SYSTEM_CONFIG_FILE=\""$(dbusdatadir)/system.conf"\" \
	-DDBUS_COMPILATION \
	$(NULL)

# if assertions are enabled, improve backtraces
AM_LDFLAGS = @R_DYNAMIC_LDFLAG@

AM_CFLAGS = \
	$(CODE_COVERAGE_CFLAGS) \
	$(NULL)

EFENCE=

CONFIG_IN_FILES=				\
	session.conf.in				\
	system.conf.in				\
	legacy-config/session.conf.in		\
	legacy-config/system.conf.in		\
	org.freedesktop.dbus-session.plist.in	\
	example-system-enable-stats.conf.in	\
	example-session-disable-stats.conf.in	\
	$(NULL)

dbusdata_DATA = session.conf
legacydbusdata_DATA = legacy-config/session.conf

if DBUS_UNIX
dbusdata_DATA += system.conf
legacydbusdata_DATA += legacy-config/system.conf
endif

examplesdir = ${docdir}/examples
examples_DATA = \
	example-system-enable-stats.conf	\
	example-session-disable-stats.conf	\
	$(NULL)

if DBUS_ENABLE_LAUNCHD
agentdir=$(LAUNCHD_AGENT_DIR)
agent_DATA=org.freedesktop.dbus-session.plist
endif

XML_SOURCES=config-loader-expat.c

if DBUS_BUS_ENABLE_KQUEUE
DIR_WATCH_SOURCE=dir-watch-kqueue.c
else
if DBUS_BUS_ENABLE_INOTIFY
DIR_WATCH_SOURCE=dir-watch-inotify.c
else
DIR_WATCH_SOURCE=dir-watch-default.c
endif
endif

BUS_SOURCES=					\
	activation.c				\
	activation.h				\
	activation-exit-codes.h			\
	apparmor.c				\
	apparmor.h				\
	audit.c					\
	audit.h					\
	bus.c					\
	bus.h					\
	config-parser.c				\
	config-parser.h				\
	config-parser-common.c			\
	config-parser-common.h			\
	connection.c				\
	connection.h				\
	desktop-file.c				\
	desktop-file.h				\
	$(DIR_WATCH_SOURCE)			\
	dir-watch.h				\
	dispatch.c				\
	dispatch.h				\
	driver.c				\
	driver.h				\
	expirelist.c				\
	expirelist.h				\
	policy.c				\
	policy.h				\
	selinux.h				\
	selinux.c				\
	services.c				\
	services.h				\
	signals.c				\
	signals.h				\
	stats.c					\
	stats.h					\
	test.c					\
	test.h					\
	utils.c					\
	utils.h					\
	$(XML_SOURCES)

dbus_daemon_SOURCES=				\
	$(BUS_SOURCES)				\
	main.c

dbus_daemon_LDADD=					\
	$(top_builddir)/dbus/libdbus-1.la	\
	$(top_builddir)/dbus/libdbus-internal.la	\
	$(EFENCE)					\
	$(DBUS_BUS_LIBS)

LAUNCH_HELPER_SOURCES=				\
	$(XML_SOURCES)				\
	config-parser-common.c			\
	config-parser-common.h			\
	config-parser-trivial.c			\
	config-parser-trivial.h			\
	desktop-file.c				\
	desktop-file.h				\
	utils.c					\
	utils.h					\
	activation-exit-codes.h			\
	activation-helper.h	  		\
	activation-helper.c

## This is the installed launch helper with the setuid checks
dbus_daemon_launch_helper_SOURCES=		\
	activation-helper-bin.c			\
	$(LAUNCH_HELPER_SOURCES)

dbus_daemon_launch_helper_LDADD=		\
	$(top_builddir)/dbus/libdbus-1.la \
	$(top_builddir)/dbus/libdbus-internal.la	\
	$(DBUS_LAUNCHER_LIBS)

## we build another binary so we can do the launch testing without root privs.
## DO NOT INSTALL THIS FILE
dbus_daemon_launch_helper_test_SOURCES=		\
	activation-helper-bin.c			\
	$(LAUNCH_HELPER_SOURCES)

dbus_daemon_launch_helper_test_LDADD=		\
	$(top_builddir)/dbus/libdbus-1.la \
	$(top_builddir)/dbus/libdbus-internal.la \
	$(DBUS_LAUNCHER_LIBS)

dbus_daemon_launch_helper_test_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	-DACTIVATION_LAUNCHER_TEST

## we build yet another binary so we can do the OOM tests
## DO NOT INSTALL THIS FILE
test_bus_launch_helper_SOURCES=		\
	test-launch-helper.c   		\
	$(LAUNCH_HELPER_SOURCES)

test_bus_launch_helper_LDADD=		\
	$(top_builddir)/dbus/libdbus-1.la \
	$(top_builddir)/dbus/libdbus-internal.la \
	$(DBUS_LAUNCHER_LIBS)
	$(NULL)

test_bus_launch_helper_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	-DACTIVATION_LAUNCHER_TEST	\
	-DACTIVATION_LAUNCHER_DO_OOM

noinst_PROGRAMS =
dbus_daemon_exec_PROGRAMS = dbus-daemon
if DBUS_UNIX
libexec_PROGRAMS = dbus-daemon-launch-helper
endif DBUS_UNIX

## Note that TESTS has special meaning (stuff to use in make check).
## We don't actually want to run any of these tests until test/ has been
## compiled, so we don't put them in TESTS here; we run them in test/
## instead.

if DBUS_ENABLE_EMBEDDED_TESTS
## we use noinst_PROGRAMS not check_PROGRAMS so that we build
## even when not doing "make check"

# run as a test by test/Makefile.am
noinst_PROGRAMS += test-bus

if DBUS_UNIX
# run as a test by test/Makefile.am
noinst_PROGRAMS += test-bus-launch-helper test-bus-system
# this is used by the tests but is not,itself, a test
noinst_PROGRAMS += dbus-daemon-launch-helper-test
endif DBUS_UNIX

endif DBUS_ENABLE_EMBEDDED_TESTS

test_bus_system_SOURCES=			\
	$(XML_SOURCES)				\
	config-parser-common.c			\
	config-parser-common.h			\
	config-parser-trivial.c			\
	config-parser-trivial.h			\
	utils.c					\
	utils.h					\
	test-system.c

test_bus_system_LDADD = \
	$(top_builddir)/dbus/libdbus-1.la \
	$(top_builddir)/dbus/libdbus-internal.la \
	$(DBUS_BUS_LIBS) \
	$(NULL)

test_bus_SOURCES=				\
	$(BUS_SOURCES)				\
	test-main.c

test_bus_LDADD = \
	$(top_builddir)/dbus/libdbus-1.la \
	$(top_builddir)/dbus/libdbus-internal.la \
	$(DBUS_BUS_LIBS) \
	$(NULL)

install-data-hook:
	$(mkinstalldirs) $(DESTDIR)$(dbusdatadir)/session.d
	$(mkinstalldirs) $(DESTDIR)$(dbusdatadir)/services
if DBUS_UNIX
	$(mkinstalldirs) $(DESTDIR)$(localstatedir)/run/dbus
	$(mkinstalldirs) $(DESTDIR)$(dbusdatadir)/system.d
	$(mkinstalldirs) $(DESTDIR)$(dbusdatadir)/system-services
endif
if HAVE_SYSTEMD
# Install dbus.socket as default implementation of a D-Bus stack.
# Deliberately not using $(LN_S) here: ln -fs is not universally portable,
# but neither is systemd, so it's OK to assume here that ln complies with SUS.
# Unconditionally enable D-Bus on systemd installations
	$(mkinstalldirs) $(DESTDIR)$(systemdsystemunitdir)/sockets.target.wants
	ln -fs ../dbus.socket $(DESTDIR)$(systemdsystemunitdir)/sockets.target.wants/dbus.socket
	$(mkinstalldirs) $(DESTDIR)$(systemdsystemunitdir)/multi-user.target.wants
	ln -fs ../dbus.service $(DESTDIR)$(systemdsystemunitdir)/multi-user.target.wants/dbus.service
endif
if DBUS_ENABLE_USER_SESSION
	$(mkinstalldirs) $(DESTDIR)$(systemduserunitdir)/sockets.target.wants
	ln -fs ../dbus.socket $(DESTDIR)$(systemduserunitdir)/sockets.target.wants/dbus.socket
endif

if DBUS_UNIX
install-exec-hook:
	if test `id -u` -eq 0; then \
		chown root:$(DBUS_USER) $(DESTDIR)$(libexecdir)/dbus-daemon-launch-helper$(EXEEXT); \
		chmod 4750 $(DESTDIR)$(libexecdir)/dbus-daemon-launch-helper$(EXEEXT); \
	else \
		echo "Not installing $(DESTDIR)$(libexecdir)/dbus-daemon-launch-helper binary setuid!"; \
		echo "You'll need to manually set permissions to root:$(DBUS_USER) and permissions 4750"; \
	fi
endif

#### Init scripts fun
SCRIPT_IN_FILES=messagebus.in \
		messagebus-config.in \
		rc.messagebus.in

## Red Hat start
if DBUS_INIT_SCRIPTS_RED_HAT

initddir=$(sysconfdir)/rc.d/init.d

initd_SCRIPTS= 	\
	messagebus

endif
 ## Red Hat end

## Slackware start
if DBUS_INIT_SCRIPTS_SLACKWARE

initddir=$(sysconfdir)/rc.d/

initd_SCRIPTS= 	\
	rc.messagebus

endif
## Slackware end

## Cygwin start
if DBUS_INIT_SCRIPTS_CYGWIN

bin_SCRIPTS= 	\
	messagebus-config

endif
## Cygwin end

if HAVE_SYSTEMD
SCRIPT_IN_FILES += \
	dbus.service.in \
	dbus.socket.in \
	systemd-user/dbus.service.in \
	systemd-user/dbus.socket.in \
	$(NULL)

systemdsystemunit_DATA = \
	dbus.service \
	dbus.socket
endif

if DBUS_ENABLE_USER_SESSION
systemduserunit_DATA = \
	systemd-user/dbus.service \
	systemd-user/dbus.socket \
	$(NULL)
endif

# Add rules for code-coverage testing, as defined by AX_CODE_COVERAGE
@CODE_COVERAGE_RULES@

#### Extra dist

EXTRA_DIST=$(CONFIG_IN_FILES) $(SCRIPT_IN_FILES)
