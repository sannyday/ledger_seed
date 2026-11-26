# ledger_seed - deterministic seed table from the Ethereum app
#
# The DEFAULT build is FULLY STATIC: the static binary is the artifact that
# gets tested and released.  It needs a static toolchain:
#   nix:    nix-shell -p glibc.static --run 'make'
#   nix:    nix-shell -p pkgsStatic.stdenv.cc --run 'make'
#   fedora: sudo dnf install glibc-static   (or musl-gcc musl-devel)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
BIN      = ledger_seed

all: $(BIN)

$(BIN): ledger_seed.c
	@err=$$(mktemp); \
	if command -v musl-gcc >/dev/null 2>&1 && \
	   musl-gcc $(CFLAGS) -static -o $@ $< 2>"$$err"; then \
		echo "built $@ (static, musl-gcc)"; \
	elif $(CC) $(CFLAGS) -static -o $@ $< 2>"$$err"; then \
		echo "built $@ (static, $(CC))"; \
	else \
		echo "static link failed - last error:"; \
		sed 's/^/  /' "$$err" | head -6; \
		rm -f "$$err"; \
		echo "the default (and tested) build is static; provide a static toolchain:"; \
		echo "  nix-shell -p glibc.static --run 'make'"; \
		echo "  nix-shell -p pkgsStatic.stdenv.cc --run 'make'"; \
		echo "  fedora: sudo dnf install glibc-static (or musl-gcc musl-devel)"; \
		exit 1; \
	fi; rm -f "$$err"

# dynamic glibc build - local experiments only, NOT the tested artifact
dynamic: ledger_seed.c
	$(CC) $(CFLAGS) -o $(BIN) $<

clean:
	rm -f $(BIN)

.PHONY: all dynamic clean
