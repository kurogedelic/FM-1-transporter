# JieLi WL82 UBOOT protocol

Verified on the FM-1.

## Entry

- USB_KEY `0x16EF`, MSB first, D+ = clock and D- = data. Only this polarity works.
- After the ACK the ROM pulls D+ high. Send 1 ms pulses until a host takes over.
- The ROM accepts the key at power-on and after a watchdog reset.
- Stock V15 also enters UBOOT on the USB-MIDI packet `04 F0 22 24 07 35 7D F7`.

## Device

UBOOT shows up as `4C4A:8057` "WL80UBOOT1.00": USB Mass Storage, Bulk-Only Transport.

Each command is a 16-byte CDB: `cmd` (u16, big-endian) followed by its arguments, padded with `0xFF`.

| cmd | Name | Stage |
|---|---|---|
| `FB06` | WRITE_MEMORY | ROM |
| `FD07` | READ_MEMORY | ROM |
| `FB08` | JUMP | ROM |
| `FC09` | READ_KEY (expect `980F`) | loader |
| `FC0A` | GET_ONLINE_DEVICE (expect `856014`) | loader |
| `FD05` | READ_FLASH | loader |
| `FB01` | ERASE_SECTOR (4 KiB) | loader |
| `FB04` | WRITE_FLASH | loader |

- Upload `wl82loader.bin` to `0x1C02000`, then send `JUMP(0x1C02000, 1)`.
- In the ROM stage, memory data is XORed with `jl_crc_cipher`. Flash data through the loader is not ciphered.
- The ROM runs a 4 s watchdog. The loader resets the chip after about 3 s without a command.

## Flash layout (1 MiB)

| Range | Contents | Writes |
|---|---|---|
| `0x0`–`0x4000` | header, SPL, isd_config | never |
| `0x4000`–`0x93000` | application | allowed |
| `0x93000`– | device data | never |
