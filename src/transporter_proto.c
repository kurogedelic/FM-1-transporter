#include "transporter_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "tusb.h"

#include "jieli_uboot.h"
#include "log.h"
#include "pio_host.h"

#define DATA_ITF 1
#define TX_SIZE 16384           // power of two
#define LINE_MAX 64

// core 0 -> core 1: one request line at a time, plus the 4 KiB payload of a
// `wsec` request.
static char rx_line[LINE_MAX];
static unsigned rx_len;
static uint8_t __attribute__((aligned(4))) sector[JL_SECTOR_SIZE];
static unsigned sector_fill;
static bool rx_binary;      // collecting a wsec/memw payload
static unsigned rx_expect;  // payload size: 4096 for wsec, LEN for memw
static char req[LINE_MAX];
static volatile bool req_pending;

// core 1 -> core 0: output stream with backpressure.
static uint8_t tx[TX_SIZE];
static volatile uint32_t tx_head;   // core 1
static volatile uint32_t tx_tail;   // core 0

static uint8_t __attribute__((aligned(4))) io[JL_READ_MAX];

//--------------------------------------------------------------------+
// core 0
//--------------------------------------------------------------------+

void fm1_proto_rx(const char *buf, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
        if (rx_binary) {
            unsigned n = len - i;
            if (n > rx_expect - sector_fill) n = rx_expect - sector_fill;
            memcpy(&sector[sector_fill], &buf[i], n);
            sector_fill += n;
            i += n - 1;
            if (sector_fill == rx_expect) {
                rx_binary = false;
                __dmb();
                req_pending = true;     // req already holds the wsec line
            }
            continue;
        }
        char c = buf[i];
        if (c == '\r') continue;
        if (c != '\n') {
            if (rx_len < LINE_MAX - 1) rx_line[rx_len++] = c;
            continue;
        }
        rx_line[rx_len] = 0;
        rx_len = 0;
        if (!strcmp(rx_line, "rekey")) {
            extern void fm1_rekey(void);
            tud_cdc_n_write_str(DATA_ITF, "OK rekey\n");
            tud_cdc_n_write_flush(DATA_ITF);
            fm1_rekey();
            continue;
        }
        if (!strcmp(rx_line, "ping")) {
            // Answered here so fm1t can find the port while core 1 is still
            // busy with the USB_KEY recovery.
            tud_cdc_n_write_str(DATA_ITF, "PONG\n");
            tud_cdc_n_write_flush(DATA_ITF);
            continue;
        }
        if (req_pending) continue;      // one request at a time; drop extras
        memcpy(req, rx_line, sizeof(req));
        if (!strncmp(rx_line, "wsec ", 5)) {
            sector_fill = 0;
            rx_expect = JL_SECTOR_SIZE;
            rx_binary = true;           // the request fires once 4 KiB arrived
            continue;
        }
        if (!strncmp(rx_line, "ldr ", 4)) {
            // ldr <off> <len> <crc32> [<addr> <size>], then <len> raw bytes (1..4096)
            char *a = strchr(rx_line + 4, ' ');
            unsigned long n = a ? strtoul(a + 1, NULL, 0) : 0;
            if (n >= 1 && n <= JL_SECTOR_SIZE) {
                sector_fill = 0;
                rx_expect = (unsigned)n;
                rx_binary = true;
                continue;
            }
        }
        if (!strncmp(rx_line, "memw ", 5)) {
            // memw <addr> <len> <crc32>, then <len> raw bytes (1..4096)
            char *p = strchr(rx_line + 5, ' ');
            unsigned long n = p ? strtoul(p + 1, NULL, 0) : 0;
            if (n >= 1 && n <= JL_SECTOR_SIZE) {
                sector_fill = 0;
                rx_expect = (unsigned)n;
                rx_binary = true;
                continue;
            }
            // bad length: hand the line to core 1, which answers ERR
        }
        __dmb();
        req_pending = true;
    }
}

void fm1_proto_drain(void) {
    if (!tud_cdc_n_connected(DATA_ITF)) {
        tx_tail = tx_head;              // nobody listening; discard
        return;
    }
    for (;;) {
        uint32_t h = tx_head;
        __dmb();
        uint32_t t = tx_tail;
        if (h == t) break;
        uint32_t avail = tud_cdc_n_write_available(DATA_ITF);
        if (!avail) break;
        uint32_t idx = t & (TX_SIZE - 1);
        uint32_t n = h - t;
        if (n > TX_SIZE - idx) n = TX_SIZE - idx;
        if (n > avail) n = avail;
        n = tud_cdc_n_write(DATA_ITF, &tx[idx], n);
        __dmb();
        tx_tail = t + n;
    }
    tud_cdc_n_write_flush(DATA_ITF);
}

//--------------------------------------------------------------------+
// core 1
//--------------------------------------------------------------------+

static void tx_bytes(const void *data, uint32_t len) {
    const uint8_t *p = data;
    while (len) {
        uint32_t h = tx_head;
        uint32_t space = TX_SIZE - (h - tx_tail);
        if (!space) {
            tight_loop_contents();      // core 0 is draining
            continue;
        }
        uint32_t n = len < space ? len : space;
        for (uint32_t i = 0; i < n; i++) tx[(h + i) & (TX_SIZE - 1)] = p[i];
        __dmb();
        tx_head = h + n;
        p += n;
        len -= n;
    }
}

static void tx_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tx_line(const char *fmt, ...) {
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    tx_bytes(line, (uint32_t)n);
}

// zlib-compatible CRC-32, so the Mac side can use zlib.crc32.
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// wsec <addr> <crc32>: write one 4 KiB sector (payload already received).
static void do_write_sector(uint32_t addr, uint32_t crc) {
    uint32_t got = crc32_update(0, sector, JL_SECTOR_SIZE);
    if (got != crc) {
        tx_line("ERR crc %08lX", (unsigned long)got);
        return;
    }
    if (addr < JL_WRITE_MIN || addr >= JL_WRITE_END || (addr % JL_SECTOR_SIZE)) {
        tx_line("ERR range");
        return;
    }
    dlog("PROTO: write sector %06lX", (unsigned long)addr);
    tx_line(jl_flash_write_sector(addr, sector) ? "OK" : "ERR write");
}

static void do_read(uint32_t addr, uint32_t len) {
    if (len == 0 || addr >= JL_FLASH_SIZE || len > JL_FLASH_SIZE - addr) {
        tx_line("ERR range");
        return;
    }
    if (!jl_ensure_loader()) {
        tx_line("ERR loader");
        return;
    }

    dlog("PROTO: read %06lX+%lu", (unsigned long)addr, (unsigned long)len);
    uint64_t t0 = time_us_64();
    tx_line("DATA %lu", (unsigned long)len);

    uint32_t crc = 0;
    uint32_t chunk = jl_read_chunk();
    for (uint32_t off = 0; off < len; off += chunk) {
        uint16_t n = (uint16_t)((len - off) < chunk ? (len - off) : chunk);
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            ok = jl_flash_read(addr + off, n, io);
        }
        if (!ok) {
            // The Mac side counts bytes, so the stream cannot be resynced.
            // Pad with zeros and report the failure in END.
            memset(io, 0, n);
            tx_bytes(io, n);
            for (off += n; off < len; off += chunk) {
                uint32_t m = (len - off) < chunk ? (len - off) : chunk;
                tx_bytes(io, m);
            }
            tx_line("END FAIL");
            return;
        }
        crc = crc32_update(crc, io, n);
        tx_bytes(io, n);
    }

    tx_line("END %08lX", (unsigned long)crc);
    dlog("PROTO: read done, %lu bytes in %.2f s, crc32 %08lX", (unsigned long)len,
         (time_us_64() - t0) / 1e6, (unsigned long)crc);
}

//--------------------------------------------------------------------+
// RAM-run (ROM UBOOT1.00 stage, before the loader)
//--------------------------------------------------------------------+

// memw <addr> <len> <crc32>: payload already in sector[0..len)
static void do_memw(uint32_t addr, uint32_t len, uint32_t crc) {
    if (len < 1 || len > JL_SECTOR_SIZE) {
        tx_line("ERR len");
        return;
    }
    uint32_t got = crc32_update(0, sector, len);
    if (got != crc) {
        tx_line("ERR crc %08lX", (unsigned long)got);
        return;
    }
    if (jl_loader_running()) {
        tx_line("ERR loader-running");
        return;
    }
    for (uint32_t off = 0; off < len; off += JL_IO_SIZE) {
        uint16_t n = (uint16_t)((len - off) < JL_IO_SIZE ? (len - off) : JL_IO_SIZE);
        if (!jl_rom_mem_write(addr + off, &sector[off], n)) {
            tx_line("ERR memw at %08lX", (unsigned long)(addr + off));
            return;
        }
    }
    tx_line("OK");
}

// memr <addr> <len>: DATA <len>, raw bytes, END <crc32>
static void do_memr(uint32_t addr, uint32_t len) {
    if (len < 1 || len > JL_READ_MAX) {
        tx_line("ERR len");
        return;
    }
    if (jl_loader_running()) {
        tx_line("ERR loader-running");
        return;
    }
    for (uint32_t off = 0; off < len; off += JL_IO_SIZE) {
        uint16_t n = (uint16_t)((len - off) < JL_IO_SIZE ? (len - off) : JL_IO_SIZE);
        if (!jl_rom_mem_read(addr + off, n, &io[off])) {
            tx_line("ERR memr at %08lX", (unsigned long)(addr + off));
            return;
        }
    }
    tx_line("DATA %lu", (unsigned long)len);
    tx_bytes(io, len);
    tx_line("END %08lX", (unsigned long)crc32_update(0, io, len));
}

// ldr <off> <len> <crc32> [<addr> <size>]: payload already in sector[0..len)
static void do_ldr(uint32_t off, uint32_t len, uint32_t crc, uint32_t addr, uint32_t size) {
    uint32_t got = crc32_update(0, sector, len);
    if (got != crc) {
        tx_line("ERR crc %08lX", (unsigned long)got);
        return;
    }
    if (jl_loader_running()) {
        tx_line("ERR loader-running");
        return;
    }
    tx_line(jl_loader_load(off, sector, len, addr, size) ? "OK" : "ERR loader-range");
}

// jump <addr> <arg>: call RAM code through the ROM; it must return (< ~2 s)
static void do_jump(uint32_t addr, uint32_t arg) {
    if (jl_loader_running()) {
        tx_line("ERR loader-running");
        return;
    }
    dlog("PROTO: jump %08lX arg %04lX", (unsigned long)addr, (unsigned long)arg);
    uint64_t t0 = time_us_64();
    bool ok = jl_rom_jump(addr, (uint16_t)arg);
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);
    if (ok) tx_line("OK ms=%lu", (unsigned long)ms);
    else tx_line("ERR jump ms=%lu", (unsigned long)ms);
}

void fm1_proto_poll(void) {
    if (!req_pending) {
        jl_keepalive();
        return;
    }
    __dmb();
    char line[LINE_MAX];
    memcpy(line, req, sizeof(line));
    req_pending = false;

    char *argv[6] = {0};
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 6; tok = strtok(NULL, " \t")) {
        argv[argc++] = tok;
    }
    if (!argc) return;

    if (!strcmp(argv[0], "status")) {
        tx_line("OK uboot=%d v15=%d loader=%d loader_running=%d", fm1_host_uboot_ready(),
                fm1_host_v15_mounted(), jl_have_loader(), jl_loader_running());
    } else if (!strcmp(argv[0], "uboot")) {
        if (fm1_host_uboot_ready()) {
            tx_line("OK uboot");
        } else if (fm1_host_softkey()) {
            tx_line("OK softkey");      // poll `status` for uboot=1
        } else {
            tx_line("ERR no-v15");
        }
    } else if (!strcmp(argv[0], "info")) {
        jl_info_t info;
        if (!fm1_host_uboot_ready()) {
            tx_line("ERR no-uboot");
        } else if (!jl_info(&info)) {
            tx_line("ERR info");
        } else {
            dlog("PROTO: chip key %04X, device type %u, flash id %06lX", info.chip_key,
                 info.dev_type, (unsigned long)info.flash_id);
            tx_line("OK key=%04X type=%u id=%06lX", info.chip_key, info.dev_type,
                    (unsigned long)info.flash_id);
        }
    } else if (!strcmp(argv[0], "read") && argc == 3) {
        if (!fm1_host_uboot_ready()) {
            tx_line("ERR no-uboot");
        } else {
            do_read(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0));
        }
    } else if (!strcmp(argv[0], "runapp")) {
        tx_line(jl_run_app() ? "OK" : "ERR no-loader");
    } else if (!strcmp(argv[0], "wsec") && argc == 3) {
        if (!fm1_host_uboot_ready()) {
            tx_line("ERR no-uboot");
        } else {
            do_write_sector(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 16));
        }
    } else if (!strcmp(argv[0], "ldr") && (argc == 4 || argc == 6)) {
        do_ldr(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0),
               strtoul(argv[3], NULL, 16), argc == 6 ? strtoul(argv[4], NULL, 0) : 0,
               argc == 6 ? strtoul(argv[5], NULL, 0) : 0);
    } else if (!strcmp(argv[0], "memw") && argc == 4) {
        if (!fm1_host_uboot_ready()) {
            tx_line("ERR no-uboot");
        } else {
            do_memw(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0),
                    strtoul(argv[3], NULL, 16));
        }
    } else if (!strcmp(argv[0], "memr") && argc == 3) {
        if (!fm1_host_uboot_ready()) {
            tx_line("ERR no-uboot");
        } else {
            do_memr(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0));
        }
    } else if (!strcmp(argv[0], "jump") && argc == 3) {
        if (!fm1_host_uboot_ready()) {
            tx_line("ERR no-uboot");
        } else {
            do_jump(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0));
        }
    } else {
        tx_line("ERR unknown");
    }
}
