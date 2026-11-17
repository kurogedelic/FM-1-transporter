// JieLi WL82 UBOOT1.00 / LoaderV2 protocol, read-only subset.
// Ported from jl-uboot-tool (jltech/uboot.py, cipher.py, crc.py) and the
// fm-1-research-lab flow in tools/fm1_uboot_restore.py.

#include "jieli_uboot.h"

#include <string.h>

#include "pico/stdlib.h"

#include "log.h"
#include "pio_host.h"

#define LOADER_ARG 0x0001

// ROM UBOOT1.00
#define CMD_WRITE_MEMORY 0xFB06
#define CMD_READ_MEMORY 0xFD07
#define CMD_JUMP 0xFB08
// LoaderV2
#define CMD_ERASE_SECTOR 0xFB01
#define CMD_WRITE_FLASH 0xFB04
#define CMD_READ_FLASH 0xFD05
#define CMD_READ_KEY 0xFC09
#define CMD_GET_ONLINE_DEVICE 0xFC0A
#define CMD_GET_USB_BUFF_SIZE 0xFC14
#define CMD_RUN_APP 0xFC0C

#define KEEPALIVE_MS 1000   // the loader resets the chip after ~3 s without a command

static uint8_t loader_buf[JL_LOADER_SIZE];   // uploaded by the host (fm1t.py)
static uint32_t loader_len;
static uint32_t loader_addr = JL_LOADER_ADDR;
static uint32_t loader_size = JL_LOADER_SIZE;
static bool loader_running;
static uint16_t read_chunk = JL_IO_SIZE;
static uint64_t last_cmd_us;

static uint8_t __attribute__((aligned(4))) resp[16];
static uint8_t __attribute__((aligned(4))) block[JL_IO_SIZE];

void jl_reset(void) {
    loader_running = false;
    read_chunk = JL_IO_SIZE;
}

bool jl_have_loader(void) {
    return loader_size && loader_len == loader_size;
}

bool jl_loader_load(uint32_t off, const uint8_t *data, uint32_t len, uint32_t addr,
                    uint32_t size) {
    if (off == 0) {
        if (!addr && !size) {
            addr = JL_LOADER_ADDR;
            size = JL_LOADER_SIZE;
        }
        loader_len = 0;
        loader_size = 0;    // nothing to run until the new loader is whole
        if (!size || size > JL_LOADER_SIZE || addr < JL_LOADER_RAM_MIN ||
            addr > JL_LOADER_RAM_END - size || (addr % JL_IO_SIZE)) {
            return false;
        }
        loader_addr = addr;
        loader_size = size;
    }
    if (!loader_size || off != loader_len || len > loader_size - off) return false;
    memcpy(&loader_buf[off], data, len);
    loader_len += len;
    return true;
}

// CRC-16/XMODEM (poly 0x1021, no reflection), with caller-supplied init.
uint16_t jl_crc16(const uint8_t *data, uint32_t len, uint16_t crc) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// jl_crc_cipher with key 0xFFFFFFFF. The magic is a fixed 16-byte GB2312
// string from the JieLi tools.
static void jl_crc_cipher(uint8_t *buf, uint32_t len) {
    static const uint8_t magic[16] = {
        0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
        0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6,
    };
    static const uint8_t key_hi[2] = {0xFF, 0xFF};
    uint16_t crc = jl_crc16(key_hi, 2, 0xFFFF);
    for (uint32_t i = 0; i < len; i++) {
        crc = jl_crc16(&magic[i % 16], 1, crc);
        buf[i] ^= (uint8_t)crc;
    }
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// CDB = cmd (u16 BE) + args, padded with 0xFF to 16 bytes.
static void make_cdb(uint8_t cdb[16], uint16_t cmd, const uint8_t *args, uint8_t nargs) {
    memset(cdb, 0xFF, 16);
    cdb[0] = (uint8_t)(cmd >> 8);
    cdb[1] = (uint8_t)cmd;
    memcpy(&cdb[2], args, nargs);
}

// cmd_exec: 16-byte data-IN, resp[0:2] echoes the command.
static bool cmd_exec(uint16_t cmd, const uint8_t *args, uint8_t nargs) {
    uint8_t cdb[16];
    uint32_t got = 0;
    make_cdb(cdb, cmd, args, nargs);
    last_cmd_us = time_us_64();
    if (!fm1_host_bot(cdb, 16, true, resp, sizeof(resp), &got)) {
        dlog("JL: cmd %04X failed", cmd);
        return false;
    }
    uint16_t echo = (uint16_t)(resp[0] << 8 | resp[1]);
    if (echo != cmd) {
        dlog("JL: cmd %04X answered as %04X", cmd, echo);
        return false;
    }
    return true;
}

bool jl_ensure_loader(void) {
    if (loader_running) return true;
    if (!fm1_host_uboot_ready()) {
        dlog("JL: no UBOOT mounted");
        return false;
    }
    if (!jl_have_loader()) {
        dlog("JL: loader not uploaded (fm1t.py --loader)");
        return false;
    }
    dlog("JL: uploading the loader (%lu bytes) to %08lX", (unsigned long)loader_size,
         (unsigned long)loader_addr);

    // The blob is shipped already ciphered: send it raw, 512 bytes at a time.
    for (uint32_t off = 0; off < loader_size; off += JL_IO_SIZE) {
        uint16_t n = (uint16_t)((loader_size - off) < JL_IO_SIZE
                                    ? (loader_size - off) : JL_IO_SIZE);
        memcpy(block, &loader_buf[off], n);

        uint8_t args[9];
        put_be32(args, loader_addr + off);
        args[4] = (uint8_t)(n >> 8);
        args[5] = (uint8_t)n;
        args[6] = 0x00;
        uint16_t crc = jl_crc16(block, n, 0);
        args[7] = (uint8_t)crc;          // LE
        args[8] = (uint8_t)(crc >> 8);

        uint8_t cdb[16];
        make_cdb(cdb, CMD_WRITE_MEMORY, args, sizeof(args));
        if (!fm1_host_bot(cdb, 16, false, block, n, NULL)) {
            dlog("JL: loader block at +%lu failed", (unsigned long)off);
            return false;
        }
    }

    uint8_t args[6];
    put_be32(args, loader_addr);
    args[4] = (uint8_t)(LOADER_ARG >> 8);
    args[5] = (uint8_t)LOADER_ARG;
    if (!cmd_exec(CMD_JUMP, args, sizeof(args))) {
        dlog("JL: jump to loader failed");
        return false;
    }

    loader_running = true;
    last_cmd_us = time_us_64();
    dlog("JL: loader running");

    // Read-only query; falls back to 512-byte reads if unsupported.
    if (cmd_exec(CMD_GET_USB_BUFF_SIZE, NULL, 0)) {
        uint32_t size = (uint32_t)resp[2] << 24 | (uint32_t)resp[3] << 16 |
                        (uint32_t)resp[4] << 8 | resp[5];
        uint32_t chunk = size > JL_READ_MAX ? JL_READ_MAX : size;
        chunk -= chunk % JL_IO_SIZE;
        if (chunk >= JL_IO_SIZE) read_chunk = (uint16_t)chunk;
        dlog("JL: loader USB buffer %lu bytes, reading %u at a time", (unsigned long)size,
             read_chunk);
    }
    return true;
}

uint16_t jl_read_chunk(void) {
    return read_chunk;
}

bool jl_info(jl_info_t *info) {
    if (!jl_ensure_loader()) return false;

    uint8_t args[4];
    put_be32(args, 0x00AC6900);
    if (!cmd_exec(CMD_READ_KEY, args, sizeof(args))) return false;
    // payload = resp[2:]; key = cipher(reverse(payload[4:6])) as u16 LE
    uint8_t k[2] = {resp[2 + 5], resp[2 + 4]};
    jl_crc_cipher(k, 2);
    info->chip_key = (uint16_t)(k[0] | k[1] << 8);

    if (!cmd_exec(CMD_GET_ONLINE_DEVICE, NULL, 0)) return false;
    const uint8_t *p = &resp[2];
    info->dev_type = p[0];
    info->flash_id = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 |
                     (uint32_t)p[5] << 24;
    return true;
}

bool jl_flash_read(uint32_t addr, uint16_t len, uint8_t *buf) {
    if (len > JL_READ_MAX) return false;
    if (!jl_ensure_loader()) return false;

    uint8_t args[6];
    put_be32(args, addr);
    args[4] = (uint8_t)(len >> 8);
    args[5] = (uint8_t)len;

    uint8_t cdb[16];
    uint32_t got = 0;
    make_cdb(cdb, CMD_READ_FLASH, args, sizeof(args));
    last_cmd_us = time_us_64();
    if (!fm1_host_bot(cdb, 16, true, buf, len, &got) || got != len) {
        dlog("JL: flash read %06lX+%u failed (got %lu)", (unsigned long)addr, len,
             (unsigned long)got);
        return false;
    }
    return true;
}

bool jl_flash_write_sector(uint32_t addr, const uint8_t *data) {
    if (addr < JL_WRITE_MIN || addr >= JL_WRITE_END || (addr % JL_SECTOR_SIZE)) {
        dlog("JL: write to %06lX refused (allowed: %06X-%06X, 4 KiB aligned)",
             (unsigned long)addr, JL_WRITE_MIN, JL_WRITE_END);
        return false;
    }
    if (!jl_ensure_loader()) return false;

    uint8_t args[9];
    put_be32(args, addr);
    if (!cmd_exec(CMD_ERASE_SECTOR, args, 4)) {
        dlog("JL: erase %06lX failed", (unsigned long)addr);
        return false;
    }

    for (uint32_t off = 0; off < JL_SECTOR_SIZE; off += JL_IO_SIZE) {
        memcpy(block, &data[off], JL_IO_SIZE);
        put_be32(args, addr + off);
        args[4] = (uint8_t)(JL_IO_SIZE >> 8);
        args[5] = (uint8_t)JL_IO_SIZE;
        args[6] = 0x00;
        uint16_t crc = jl_crc16(block, JL_IO_SIZE, 0);
        args[7] = (uint8_t)crc;          // LE
        args[8] = (uint8_t)(crc >> 8);

        uint8_t cdb[16];
        make_cdb(cdb, CMD_WRITE_FLASH, args, sizeof(args));
        last_cmd_us = time_us_64();
        if (!fm1_host_bot(cdb, 16, false, block, JL_IO_SIZE, NULL)) {
            dlog("JL: write %06lX failed", (unsigned long)(addr + off));
            return false;
        }
    }

    for (uint32_t off = 0; off < JL_SECTOR_SIZE; off += JL_IO_SIZE) {
        if (!jl_flash_read(addr + off, JL_IO_SIZE, block)) return false;
        if (memcmp(block, &data[off], JL_IO_SIZE) != 0) {
            dlog("JL: verify %06lX failed", (unsigned long)(addr + off));
            return false;
        }
    }
    return true;
}

bool jl_run_app(void) {
    if (!loader_running) return false;
    uint8_t args[4];
    put_be32(args, 1);
    dlog("JL: RUN_APP - the FM-1 leaves UBOOT and boots its firmware");
    // The chip may reset before it answers; either way the loader is gone.
    cmd_exec(CMD_RUN_APP, args, sizeof(args));
    loader_running = false;
    return true;
}

void jl_keepalive(void) {
    if (!loader_running || !fm1_host_uboot_ready()) return;
    if (time_us_64() - last_cmd_us < KEEPALIVE_MS * 1000ull) return;
    if (!cmd_exec(CMD_GET_ONLINE_DEVICE, NULL, 0)) dlog("JL: keepalive failed");
}

//--------------------------------------------------------------------+
// RAM-run (ROM UBOOT1.00 stage only; requested by fm-1-research-lab)
//--------------------------------------------------------------------+
// WL82 UBOOT1.00 runs every memory write/read through jl_crc_cipher, per
// transfer. These refuse to run once the loader is up: it lives at
// JL_LOADER_ADDR, the same address RAM-run images use, and would be clobbered.

bool jl_loader_running(void) {
    return loader_running;
}

bool jl_rom_mem_write(uint32_t addr, const uint8_t *data, uint16_t len) {
    if (loader_running || !len || len > JL_IO_SIZE) return false;
    memcpy(block, data, len);
    jl_crc_cipher(block, len);
    uint8_t args[9];
    put_be32(args, addr);
    args[4] = (uint8_t)(len >> 8);
    args[5] = (uint8_t)len;
    args[6] = 0x00;
    uint16_t crc = jl_crc16(block, len, 0);
    args[7] = (uint8_t)crc;          // LE
    args[8] = (uint8_t)(crc >> 8);
    uint8_t cdb[16];
    make_cdb(cdb, CMD_WRITE_MEMORY, args, sizeof(args));
    last_cmd_us = time_us_64();
    return fm1_host_bot(cdb, 16, false, block, len, NULL);
}

bool jl_rom_mem_read(uint32_t addr, uint16_t len, uint8_t *out) {
    if (loader_running || !len || len > JL_IO_SIZE) return false;
    uint8_t args[6];
    put_be32(args, addr);
    args[4] = (uint8_t)(len >> 8);
    args[5] = (uint8_t)len;
    uint8_t cdb[16];
    make_cdb(cdb, CMD_READ_MEMORY, args, sizeof(args));
    uint32_t got = 0;
    last_cmd_us = time_us_64();
    if (!fm1_host_bot(cdb, 16, true, block, len, &got) || got != len) return false;
    jl_crc_cipher(block, len);
    memcpy(out, block, len);
    return true;
}

bool jl_rom_jump(uint32_t addr, uint16_t arg) {
    if (loader_running) return false;
    uint8_t args[6];
    put_be32(args, addr);
    args[4] = (uint8_t)(arg >> 8);
    args[5] = (uint8_t)arg;
    return cmd_exec(CMD_JUMP, args, sizeof(args));
}
