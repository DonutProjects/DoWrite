CROSS_COMPILE ?=
CC            ?= gcc
CFLAGS        ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS       ?=
LDLIBS        ?=

PREFIX        ?= /usr/local
BINDIR        ?= $(PREFIX)/bin

SRC            = dowrite.c
TARGET         = dowrite

all: $(TARGET)

$(TARGET): $(SRC)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) $(LDLIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

uninstall:
	rm -f $(BINDIR)/$(TARGET)

.PHONY: clean install uninstall
