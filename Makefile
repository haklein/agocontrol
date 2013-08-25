export CC = gcc
export CXX = g++
export LD = g++ 
export LDFLAGS = -L../../shared
export CFLAGS = -c -Wall -Wno-format -g -DDEBUG
export INSTALL = install
export INCLUDES = -I../../shared

export BINDIR = $(DESTDIR)/opt/agocontrol/bin
export ETCDIR = $(DESTDIR)/etc
export LIBDIR = $(DESTDIR)/usr/lib
export CONFDIR = $(ETCDIR)/opt/agocontrol
export INCDIR = $(DESTDIR)/usr/include/agocontrol

DIRS = shared core devices

BUILDDIRS = $(DIRS:%=build-%)
INSTALLDIRS = $(DIRS:%=install-%)
CLEANDIRS = $(DIRS:%=clean-%)

all: $(BUILDDIRS)
$(DIRS): $(BUILDDIRS)
$(BUILDDIRS):
	$(MAKE) -C $(@:build-%=%)

build-core: build-shared
build-devices: build-shared

clean: $(CLEANDIRS)
$(CLEANDIRS): 
	$(MAKE) -C $(@:clean-%=%) clean

install: $(INSTALLDIRS) all
	@echo Installing
	install -d $(ETCDIR)
	install -d $(BINDIR)
	install -d $(INCDIR)
	install -d $(LIBDIR)
	install -d $(DESTDIR)/var/opt/agocontrol
	install -d $(CONFDIR)/uuidmap
	install -d $(DESTDIR)/lib/systemd/system
	install -d $(ETCDIR)/sysctl.d
	install -d $(ETCDIR)/security/limits.d
	install -d $(DESTDIR)/var/crash
	install conf/security-limits.conf $(ETCDIR)/security/limits.d/agocontrol.conf
	install conf/sysctl.conf $(ETCDIR)/sysctl.d/agocontrol.conf
	install conf/config.ini.tpl $(CONFDIR)
	install conf/schema.yaml $(CONFDIR)
	install conf/rpc_cert.pem $(CONFDIR)
	install conf/*.service $(DESTDIR)/lib/systemd/system
	install data/inventory.sql $(CONFDIR)
	install data/datalogger.sql $(CONFDIR)
	install gateways/agomeloware.py $(BINDIR)
	install scripts/agososreport.sh $(BINDIR)
	install scripts/convert-zwave-uuid.py $(BINDIR)

$(INSTALLDIRS):
	$(MAKE) -C $(@:install-%=%) install

