CC = aarch64-linux-gnu-gcc
CFLAGS = -shared -fPIC -O2 -fno-builtin -nostdlib
LDFLAGS = -Wl,-soname,natartos_loader.so

all: natartos_loader.so

natartos_loader.so: natartos_loader.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "[natartos] Built $@ ($$(stat -c%s $@) bytes)"

clean:
	rm -f natartos_loader.so

.PHONY: all clean
