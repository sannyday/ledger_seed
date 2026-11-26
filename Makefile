CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra

# no external libraries - the hidraw transport is built into ledger_seed.c

ledger_seed: ledger_seed.c
	$(CC) $(CFLAGS) -o $@ $<

# fully static binary (musl), for maximum portability:
static: ledger_seed.c
	@if command -v musl-gcc >/dev/null 2>&1; then \
		musl-gcc -O2 -static -o ledger_seed-static $<; \
	else \
		echo "musl-gcc not found - run: nix-shell -p musl --run 'make static'"; \
	fi

clean:
	rm -f ledger_seed ledger_seed-static

.PHONY: clean static
