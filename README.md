# ledger_seed

One native C tool, **zero dependencies** beyond libc, that prints a table of
deterministic **seed** strings derived on a Ledger Nano through the
**Ethereum app** (tested with Ethereum app **1.16.2**).

- Walks **N = 2020 .. YEAR** (YEAR = 2020..2050, default: the current year)
  and prints `seed = SHA-256(compressed secp256k1 pubkey)` for the path
  `44'/60'/<N>'/0/0` - the standard Ethereum account path. The ETH app
  refuses non-standard paths with SW `0x6A15`, hence this mapping; N is the
  account index.
- Same seed + same BIP39 passphrase -> the same table every run. Changing
  the seed or the temporary passphrase on the device changes every value.
- If the Ethereum app is **older than 1.16.2**, the tool exits with an error
  telling you to update it in Ledger Wallet.

## Usage

```bash
make                         # build ./ledger_seed (needs only a C compiler)
./ledger_seed                # keys for 2020 .. current year
./ledger_seed 2030           # keys for 2020 .. 2030
./ledger_seed --debug 2030   # same, plus app/path/driver diagnostic lines
```

Simple mode (default) prints **only the keys**, one per line:

```
2020-7da79-fc7f0-d091b-475c9-83d9e-35e5f-4561a-51d3f-84d67-27050-019b0-7a56d-72f4
2021-eb697-59478-093b6-6af69-15a98-96671-303b2-18530-90324-00876-aaedd-b4878-08a8
...
```

`--debug` adds everything else:

```
hidraw: matched /sys/bus/hid/devices/0003:2C97:1011.00E5
hidraw: opened /dev/hidraw7 (candidate 1)
app  : Ethereum 1.16.2
path : 44'/60'/<N>'/0/0   (N = 2020 .. 2030)

2020-...
```

- Format: `YEAR-` then the 64-hex seed in groups of 5 separated by `-`
  (12 groups of 5 + a final group of 4).
- YEAR: upper bound, 2020..2050; without it the **current year** is used.
- `--help` prints usage. Invalid years exit with code 2.

## Device setup

- Open the **Ethereum** app on the device (it must be unlocked).
- The tool needs access to `/dev/hidraw*` for vendor `0x2c97`. As root:

```bash
sudo tee /etc/udev/rules.d/20-hw1.rules >/dev/null <<'RULE'
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2c97", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="usb", ATTRS{idVendor}=="2c97", MODE="0660", GROUP="plugdev"
RULE
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -aG plugdev "$USER"   # then log out / log in
```

## Build

```bash
make           # cc -O2 -Wall -Wextra -o ledger_seed ledger_seed.c
make static    # fully static musl binary (needs musl-gcc, e.g. from nixpkgs)
```

## Checking that a passphrase change alters the output

```bash
./ledger_seed                 # note any row
# on the device: Settings > Security > Passphrase -> enable it, then unlock
# with 'Set temporary passphrase' and enter a DIFFERENT passphrase (twice)
./ledger_seed                 # every row must now be different
```

The passphrase feature must be enabled and you must actually unlock with the
new temporary passphrase.

## What to save (recovery)

| Thing | Rebuildable from seed+passphrase alone? |
|---|---|
| the tool | yes - pure derivation, nothing else to save |
| the path | fixed: `44'/60'/<N>'/0/0` |

Secret backup: 24 words + passphrase - nothing else is secret.

## Files

| File | Purpose |
|---|---|
| `ledger_seed.c` | the whole tool (built-in SHA-256 + hidraw transport) |
| `Makefile` | build rules (`make`, `make static`, `make clean`) |

## Notes

- APDU framing verified against the
  [Ethereum app source](https://github.com/LedgerHQ/app-ethereum)
  (`CLA E0`, `INS 02`, response `[65][pubkey][40][address]`) and
  [ledgerblue's comm.py](https://github.com/LedgerHQ/blue-loader-python)
  (hidraw report-id framing: write `[0x00]+64B`, read 65B and strip).
- The tool links nothing but libc.
