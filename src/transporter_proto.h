#pragma once

// Mac-facing data channel on the second CDC interface (tools/fm1t.py).
//
// Requests are text lines; replies are text lines, and `read` replies carry
// a raw binary payload:
//
//   ping                  -> PONG
//   status                -> OK uboot=<0|1> v15=<0|1> loader=<0|1>
//   uboot                 -> OK uboot | OK softkey | ERR no-v15
//                            (soft key: stock V15 -> mask ROM UBOOT1.00)
//   runapp                -> OK | ERR no-loader  (loader RUN_APP: boot firmware)
//   rekey                 -> OK rekey, then the transporter reboots into
//                            USB_KEY mode (for a unit stuck with USB up)
//   info                  -> OK key=980F type=3 id=856014 | ERR <why>
//   read <addr> <len>     -> DATA <len>\n <len raw bytes> END <crc32>\n | ERR <why>
//   ldr <off> <len> <crc32> [<addr> <size>]\n <len raw bytes (<= 4096)>
//                         -> OK | ERR <why>   (upload the loader in order)
//                            wl82loader.bin by default. The first chunk may
//                            give another loader's RAM address and size,
//                            such as m-bootloader's flash loader (MIT) at
//                            0x1C20000, stored ciphered in 512-byte blocks.
//   wsec <addr> <crc32>\n <4096 raw bytes>
//                         -> OK | ERR <why>   (erase + write + read-back verify)
//
// Writes are accepted only for whole 4 KiB sectors in [0x4000, 0x93000);
// see jieli_uboot.h. No block/chip erase exists.

// core 0: feed bytes received on CDC 1; drain queued output to CDC 1.
void fm1_proto_rx(const char *buf, unsigned len);
void fm1_proto_drain(void);

// core 1: handle a pending request, if any.
void fm1_proto_poll(void);
