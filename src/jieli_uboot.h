#pragma once

#include <stdbool.h>
#include <stdint.h>

// JieLi WL82 UBOOT1.00 + wl82loader (LoaderV2), read-only subset.
// See docs/PROTOCOL.md. Core 1 only.
//
// Writes are limited to whole 4 KiB sectors inside the application area
// [JL_WRITE_MIN, JL_WRITE_END). Header/SPL/isd_config below 0x4000 and the
// device data from 0x93000 up are refused here, whatever the host asks.
// Deliberately absent: block/chip erase, chip-key write. RAM writes exist only
// for the loader upload and the ROM-stage RAM-run calls below (RAM, never flash).

#define JL_CHIP_KEY 0x980F
#define JL_FLASH_ID 0x856014
#define JL_FLASH_SIZE (1024u * 1024u)
#define JL_IO_SIZE 512          // loader upload and flash write chunk
#define JL_READ_MAX 4096        // largest READ_FLASH we issue (see jl_read_chunk)
#define JL_SECTOR_SIZE 0x1000
#define JL_WRITE_MIN 0x4000
#define JL_WRITE_END 0x93000

typedef struct {
    uint16_t chip_key;
    uint8_t dev_type;
    uint32_t flash_id;
} jl_info_t;

// Forget loader state (call on every new mount).
void jl_reset(void);

#define JL_LOADER_SIZE 24064    // wl82loader.bin, the default loader
#define JL_LOADER_ADDR 0x1C02000u
// Where another loader may go, such as m-bootloader's flash loader (MIT),
// built for 0x1C20000 or 0x1C28000.
#define JL_LOADER_RAM_MIN 0x1C00000u
#define JL_LOADER_RAM_END 0x1C80000u

// True once the host has uploaded the whole loader.
bool jl_have_loader(void);

// Stores a loader chunk; chunks must arrive in order, starting at off 0. The
// first chunk sets the loader's RAM address and size; 0 for both means
// wl82loader.bin at JL_LOADER_ADDR. The loader must be stored already ciphered
// in 512-byte blocks, as wl82loader.bin is.
bool jl_loader_load(uint32_t off, const uint8_t *data, uint32_t len, uint32_t addr,
                    uint32_t size);

// Uploads the loader to RAM and jumps to it, once per mount.
bool jl_ensure_loader(void);

bool jl_info(jl_info_t *info);

// READ_FLASH size to use: the loader's USB buffer size (GET_USB_BUFF_SIZE),
// clamped to [JL_IO_SIZE, JL_READ_MAX] in 512-byte steps. Each Bulk-Only
// command costs at least three 1 ms frames on the PIO host (CBW, data, CSW),
// so larger reads are what make dumps faster.
uint16_t jl_read_chunk(void);

// Reads len (<= JL_READ_MAX) bytes of SPI flash at addr into buf.
// buf must be word-aligned RAM.
bool jl_flash_read(uint32_t addr, uint16_t len, uint8_t *buf);

// Erases one 4 KiB sector, writes it and reads it back. Returns false (and
// touches nothing) if addr is outside the writable area or misaligned.
bool jl_flash_write_sector(uint32_t addr, const uint8_t *data);

// LoaderV2 RUN_APP: leave UBOOT and boot the installed firmware. No write.
bool jl_run_app(void);

// Sends a harmless GET_ONLINE_DEVICE if the loader has been idle for 1 s.
// The loader resets the chip after ~3 s without a command.
void jl_keepalive(void);

uint16_t jl_crc16(const uint8_t *data, uint32_t len, uint16_t crc);

// RAM-run, ROM UBOOT1.00 stage only (refused once the loader runs). Memory
// data is ciphered/deciphered here; len <= JL_IO_SIZE. jl_rom_jump calls the
// code like a function and fails if it does not return within the host's
// transfer timeout (the ROM resets the chip after ~2 s anyway).
bool jl_loader_running(void);
bool jl_rom_mem_write(uint32_t addr, const uint8_t *data, uint16_t len);
bool jl_rom_mem_read(uint32_t addr, uint16_t len, uint8_t *out);
bool jl_rom_jump(uint32_t addr, uint16_t arg);
