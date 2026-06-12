CROSS_COMPILE ?=
CC            ?= gcc
CPPFLAGS      ?=
CFLAGS        ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS       ?=
LDLIBS        ?=

DESTDIR       ?=
PREFIX        ?= /usr/local
BINDIR        ?= $(PREFIX)/bin

SRC            = dowrite.c
TARGET         = dowrite

all: $(TARGET)

$(TARGET): $(SRC)
	$(CROSS_COMPILE)$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) $(LDFLAGS) $(LDLIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 $(TARGET) "$(DESTDIR)$(BINDIR)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"

.PHONY: all clean install uninstall
