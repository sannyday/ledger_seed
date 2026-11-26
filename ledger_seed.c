/*
 * ledger_seed.c - deterministic seed table from a Ledger Nano.
 *
 * NO EXTERNAL LIBRARIES - the USB HID transport is implemented directly:
 * it scans /sys/bus/hid/devices for vendor 0x2c97, opens /dev/hidrawN and
 * speaks the Ledger channel protocol (64-byte reports) via plain write/poll.
 * The only linked library is libc.
 *
 * Usage:   ./ledger_seed [--debug] [YEAR]
 *          YEAR = upper bound, 2020..2050 (default: CURRENT YEAR).
 *          Prints seeds for N = 2020 .. YEAR, one line per N:
 *            YEAR-xxxxx-xxxxx-...  (SHA-256 of the compressed pubkey)
 *          The derivation path is 44'/60'/<N>'/0/0 on the Ethereum app
 *          (CLA E0 / INS 02; tested with Ethereum app 1.16.2 - the ETH app
 *          refuses non-standard paths with SW 0x6A15).
 *          Without --debug only the key lines are printed; --debug adds
 *          the app/version, path and driver diagnostic lines.
 *          App version < 1.16.2 -> error telling you to update it in
 *          Ledger Wallet (exit 2).
 *
 * Build:   cc -O2 -Wall -Wextra -o ledger_seed ledger_seed.c
 * Static:  musl-gcc -O2 -static -o ledger_seed ledger_seed.c
 *
 * Needs the udev rules for /dev/hidraw* (vendor 0x2c97, see README).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

#define LEDGER_VID_UEVENT   "00002C97"   /* HID_ID=...:00002C97:... in uevent */
#define TAG_APDU            0x05u
#define TIMEOUT_MS          3000

/* ------------------------------------------------------------------ */
/* SHA-256 (public domain style implementation)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i;

    for (i = 0; i < 16; i++)
        m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
    for (; i < 64; i++)
        m[i] = (ROTR(m[i - 2], 17) ^ ROTR(m[i - 2], 19) ^ (m[i - 2] >> 10)) +
               m[i - 7] +
               (ROTR(m[i - 15], 7) ^ ROTR(m[i - 15], 18) ^ (m[i - 15] >> 3)) +
               m[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) +
             ((e & f) ^ (~e & g)) + K256[i] + m[i];
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) +
             ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen  = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[32])
{
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    for (i = 0; i < 8; i++) ctx->data[63 - i] = (uint8_t)(ctx->bitlen >> (i * 8));
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; i++) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xff;
    }
}

/* ------------------------------------------------------------------ */
/* Built-in hidraw transport (no libusb / libhidapi / libudev)         */
/* ------------------------------------------------------------------ */

/* --debug: show all diagnostic lines; without it, print only the keys */
static int g_debug = 0;

/* scan /sys/bus/hid/devices/<dev>/uevent for vendor 2c97 and open ALL of
 * its /dev/hidrawN nodes (some models expose several interfaces - only one
 * carries the APDU channel). Fills fds[], returns the count. */
static int hidraw_open_ledgers(int *fds, int max_fds)
{
    DIR *dir = opendir("/sys/bus/hid/devices");
    struct dirent *e;
    char path[512];
    int count = 0;

    if (!dir)
        return 0;
    while ((e = readdir(dir)) != NULL && count < max_fds) {
        char uevent[2048];
        FILE *f;
        size_t n;
        DIR *hd;
        struct dirent *he;

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/bus/hid/devices/%s/uevent", e->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        n = fread(uevent, 1, sizeof(uevent) - 1, f);
        fclose(f);
        uevent[n] = '\0';
        if (!strstr(uevent, LEDGER_VID_UEVENT) && !strstr(uevent, "00002c97"))
            continue;

        /* this is a Ledger interface - find its hidraw device node */
        if (g_debug)
            fprintf(stderr, "hidraw: matched /sys/bus/hid/devices/%s\n", e->d_name);
        snprintf(path, sizeof(path), "/sys/bus/hid/devices/%s/hidraw", e->d_name);
        hd = opendir(path);
        if (!hd)
            continue;
        while ((he = readdir(hd)) != NULL && count < max_fds) {
            int fd;
            if (strncmp(he->d_name, "hidraw", 6) != 0)
                continue;
            snprintf(path, sizeof(path), "/dev/%s", he->d_name);
            fd = open(path, O_RDWR);
            if (fd >= 0) {
                if (g_debug)
                    fprintf(stderr, "hidraw: opened %s (candidate %d)\n",
                            path, count + 1);
                fds[count++] = fd;
            }
        }
        closedir(hd);
    }
    closedir(dir);
    return count;
}

static int hidraw_write(int fd, const uint8_t *buf, size_t len)
{
    return (int)write(fd, buf, len);
}

/* returns number of payload bytes read, 0 on timeout, -1 on error.
 * hidraw reads deliver the report WITH the leading 0x00 report-id byte -
 * it is stripped here (same as ledgerblue's read(65) + slicing). */
static int hidraw_read_timeout(int fd, uint8_t *buf, size_t len, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
    uint8_t tmp[65];
    int r, n;

    do {
        r = poll(&p, 1, ms);
    } while (r < 0 && errno == EINTR);
    if (r == 0)
        return 0;
    if (r < 0)
        return -1;

    n = (int)read(fd, tmp, sizeof(tmp));
    if (n <= 0)
        return -1;
    if (n > 1 && tmp[0] == 0x00 && (size_t)(n - 1) <= len) {
        memcpy(buf, tmp + 1, (size_t)n - 1);   /* strip report id */
        return n - 1;
    }
    if ((size_t)n <= len) {
        memcpy(buf, tmp, (size_t)n);           /* no report id present */
        return n;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Ledger channel protocol (channel 0x0101, TAG_APDU, 64-byte chunks)  */
/* ------------------------------------------------------------------ */

static int ledger_exchange(int fd, int timeout_ms, const uint8_t *apdu, size_t apdu_len,
                           uint8_t *resp, size_t *resp_len)
{
    uint8_t buf[64];
    uint8_t out[65];
    size_t total = 0, received = 0;
    uint16_t declared = 0, seq = 0;

    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01; buf[1] = 0x01;             /* channel id */
    buf[2] = TAG_APDU;
    buf[3] = 0x00; buf[4] = 0x00;             /* sequence 0 */
    buf[5] = (uint8_t)(apdu_len >> 8);        /* APDU length, big endian */
    buf[6] = (uint8_t)(apdu_len & 0xff);
    if (apdu_len > sizeof(buf) - 7)
        return -1;
    memcpy(buf + 7, apdu, apdu_len);

    memset(out, 0, sizeof(out));
    memcpy(out + 1, buf, sizeof(buf));
    if (hidraw_write(fd, out, sizeof(out)) < 0)
        return -1;

    /* Response framing: the first report is
     *   [01 01][05][seq16] [total16] [payload ...]
     * where total = payload bytes + 2 (the status word).  Continuation
     * reports drop the length field:
     *   [01 01][05][seq16] [payload ...]
     * The raw payload is NOT length-prefixed by the app; earlier code
     * mistook the response's first two bytes for a length. */
    for (;;) {
        int n = hidraw_read_timeout(fd, buf, sizeof(buf), timeout_ms);
        const uint8_t *payload;
        size_t hdr, plen;

        if (n < 5)
            return -1;
        if (((buf[0] << 8) | buf[1]) != 0x0101 || buf[2] != TAG_APDU)
            return -1;
        if (((buf[3] << 8) | buf[4]) != seq)
            return -1;

        if (seq == 0) {
            if (n < 7)
                return -1;
            declared = (uint16_t)((buf[5] << 8) | buf[6]);
            hdr = 7;
        } else {
            hdr = 5;
        }
        payload = buf + hdr;
        plen = (size_t)(n - hdr);
        if (received + plen > declared)
            plen = (size_t)(declared - received);
        if (total + plen > *resp_len)
            return -1;
        memcpy(resp + total, payload, plen);
        total += plen;
        received += plen;
        seq++;

        if (received >= declared)
            break;
    }

    if (total < 2)
        return -1;
    /* hand the callers [len16][data][SW] */
    if (total + 2 > *resp_len)
        return -1;
    memmove(resp + 2, resp, total);
    resp[0] = (uint8_t)(total >> 8);
    resp[1] = (uint8_t)(total & 0xff);
    *resp_len = total + 2;
    return 0;
}

/* status word at the end of an assembled response */
static uint16_t resp_sw(const uint8_t *resp, size_t resp_len)
{
    uint16_t rlen;
    if (resp_len < 4)
        return 0xffff;
    rlen = (uint16_t)((resp[0] << 8) | resp[1]);
    if ((size_t)rlen + 2u > resp_len)
        return 0xffff;
    return (uint16_t)((resp[2 + rlen - 2] << 8) | resp[2 + rlen - 1]);
}

/* GET PUBLIC KEY - Ethereum app. Compressed key from the 65-byte point,
 * plus the EIP-55 address (40 hex chars). The ETH app refuses non-standard
 * paths (single 2026' => SW 0x6A15), so the number is mapped into the
 * standard account path 44'/60'/<N>'/0/0. */
static int try_eth(int fd, int timeout_ms, uint32_t number,
                   uint8_t compressed[33], char *addr_out, size_t addr_size)
{
    uint8_t apdu[5 + 1 + 5 * 4];
    uint8_t resp[512];
    size_t resp_len = sizeof(resp);
    const uint32_t path[5] = { 0x8000002Cu, 0x8000003Cu,
                               0x80000000u | number, 0u, 0u };
    int i;

    apdu[0] = 0xE0; apdu[1] = 0x02; apdu[2] = 0x00; apdu[3] = 0x00;
    apdu[4] = 1 + 5 * 4;                    /* Lc = count + 5 levels */
    apdu[5] = 5;
    for (i = 0; i < 5; i++) {
        apdu[6 + i * 4]     = (uint8_t)(path[i] >> 24);
        apdu[7 + i * 4]     = (uint8_t)(path[i] >> 16);
        apdu[8 + i * 4]     = (uint8_t)(path[i] >> 8);
        apdu[9 + i * 4]     = (uint8_t)(path[i]);
    }

    if (ledger_exchange(fd, timeout_ms, apdu, sizeof(apdu), resp, &resp_len) != 0) {
        fprintf(stderr, "ethereum app: no response (timeout?)\n");
        return 0;
    }
    if (resp_sw(resp, resp_len) != 0x9000) {
        fprintf(stderr, "ethereum app: SW 0x%04x\n", resp_sw(resp, resp_len));
        return 0;
    }
    /* data = [len 65][0x04][X 32][Y 32][addr_len][address 40] */
    if (resp[2] != 65 || resp[3] != 0x04 || resp_len < 111 || resp[68] != 40) {
        fprintf(stderr, "ethereum app: unexpected response format\n");
        return 0;
    }
    compressed[0] = (uint8_t)(0x02u | (resp[67] & 1u));  /* 02/03 by Y parity */
    memcpy(compressed + 1, resp + 4, 32);                 /* X */
    if (addr_size > 40) {
        memcpy(addr_out, resp + 69, 40);
        addr_out[40] = '\0';
    }
    return 1;
}

/* BOLOS GET VERSION (CLA B0, INS 01) -> app name and version strings */
static int get_app_version(int fd, char *name, size_t name_size,
                           char *ver, size_t ver_size)
{
    uint8_t apdu[4] = { 0xB0, 0x01, 0x00, 0x00 };
    uint8_t resp[128];
    size_t resp_len = sizeof(resp);
    uint8_t n, v;

    if (ledger_exchange(fd, 1500, apdu, sizeof(apdu), resp, &resp_len) != 0)
        return 0;
    if (resp_sw(resp, resp_len) != 0x9000 || resp_len < 8)
        return 0;
    /* data = [format][name_len][name][version_len][version][flags] */
    n = resp[3];
    v = resp[4 + n];
    if ((size_t)(5 + n + v) > resp_len - 2)
        return 0;
    if ((size_t)n >= name_size || (size_t)v >= ver_size)
        return 0;
    memcpy(name, resp + 4, n);
    name[n] = '\0';
    memcpy(ver, resp + 5 + n, v);
    ver[v] = '\0';
    return 1;
}

static void usage(const char *prog)
{
    printf("usage: %s [--debug] [YEAR]\n"
           "  prints seeds for N = 2020 .. YEAR  (path 44'/60'/<N>'/0/0)\n"
           "  YEAR       upper bound, 2020..2050 (default: current year)\n"
           "  --debug    show app/path/driver lines too (otherwise only keys)\n",
           prog);
}

static void die(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

int main(int argc, char **argv)
{
    uint32_t n, max_year = 0;
    int fds[8], count, i, fd = -1;
    uint8_t compressed[33];
    char addr_str[41] = "";
    char app_name_buf[40] = "", app_ver_buf[16] = "";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            g_debug = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            char *end = NULL;
            unsigned long v = strtoul(argv[i], &end, 10);
            if (argv[i][0] == '\0' || end == argv[i] || *end != '\0' ||
                v < 2020 || v > 2050) {
                fprintf(stderr, "error: bad year '%s' (use 2020..2050)\n", argv[i]);
                usage(argv[0]);
                return 2;
            }
            max_year = (uint32_t)v;
        }
    }
    if (max_year == 0) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        max_year = (uint32_t)(tm.tm_year + 1900);   /* current year */
        if (max_year < 2020)
            max_year = 2020;
        if (max_year > 2050)
            max_year = 2050;
    }

    count = hidraw_open_ledgers(fds, 8);
    if (count <= 0)
        die("no Ledger found over /dev/hidraw* (cable? udev rules?)");

    /* find the interface that carries the APDU channel and verify the app */
    for (i = 0; i < count && fd < 0; i++) {
        char vn[40] = "", vv[16] = "";
        int mj = 0, mn = 0, pt = 0;

        if (!get_app_version(fds[i], vn, sizeof(vn), vv, sizeof(vv)))
            continue;
        if (strcmp(vn, "Ethereum") != 0)
            continue;                      /* dashboard / other app */
        if (sscanf(vv, "%d.%d.%d", &mj, &mn, &pt) == 3 &&
            (mj < 1 || (mj == 1 && (mn < 16 || (mn == 16 && pt < 2))))) {
            fprintf(stderr, "error: Ethereum app version %s is too old - "
                            "update it in Ledger Wallet (minimum 1.16.2)\n", vv);
            for (i = 0; i < count; i++)
                close(fds[i]);
            return 2;
        }
        fd = fds[i];
    }
    for (i = 0; i < count; i++)
        if (fds[i] != fd)
            close(fds[i]);
    if (fd < 0)
        die("the Ethereum app is not open (open it on the device, then re-run)");

    if (g_debug) {
        get_app_version(fd, app_name_buf, sizeof(app_name_buf),
                        app_ver_buf, sizeof(app_ver_buf));
        printf("app  : %s%s%s\n", app_name_buf[0] ? app_name_buf : "Ethereum",
               app_ver_buf[0] ? " " : "", app_ver_buf);
        printf("path : 44'/60'/<N>'/0/0   (N = 2020 .. %u)\n\n", max_year);
    }

    for (n = 2020; n <= max_year; n++) {
        uint8_t hash[32];
        char hex[65];
        SHA256_CTX ctx;

        if (!try_eth(fd, 3000, n, compressed, addr_str, sizeof(addr_str))) {
            fprintf(stderr, "error: derivation failed for %u\n", n);
            close(fd);
            return 1;
        }
        sha256_init(&ctx);
        sha256_update(&ctx, compressed, sizeof(compressed));
        sha256_final(&ctx, hash);

        for (i = 0; i < 32; i++)
            sprintf(hex + i * 2, "%02x", hash[i]);
        hex[64] = '\0';

        /* 2020-xxxxx-xxxxx-... (groups of 5, last group 4) */
        printf("%u", n);
        for (i = 0; i < 64; i++) {
            if (i % 5 == 0)
                putchar('-');
            putchar(hex[i]);
        }
        putchar('\n');
    }

    close(fd);
    return 0;
}
