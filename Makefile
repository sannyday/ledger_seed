CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra

# no external libraries - the hidraw transport is built into ledger_seed.c

ledger_seed: ledger_seed.c
	$(CC) $(CFLAGS) -o $@ $<

# fully static binary, for maximum portability.  Needs a static toolchain:
#   nix:   nix-shell -p pkgsStatic.stdenv.cc --run 'make static'
#   nix:   nix-shell -p glibc.static --run 'make static'
#   fedora: sudo dnf install musl-gcc musl-devel
static: ledger_seed.c
	@if command -v musl-gcc >/dev/null 2>&1; then \
		musl-gcc -O2 -static -o ledger_seed-static $<; \
	elif $(CC) -O2 -static -o ledger_seed-static $< 2>/dev/null; then \
		: ; \
	else \
		echo "static link failed - use one of:"; \
		echo "  nix-shell -p pkgsStatic.stdenv.cc --run 'make static'"; \
		echo "  nix-shell -p glibc.static --run 'make static'"; \
		exit 1; \
	fi

clean:
	rm -f ledger_seed ledger_seed-static

.PHONY: clean static
