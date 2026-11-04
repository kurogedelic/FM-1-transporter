// WebUSB PICOBOOT flasher for RP2040 boards in BOOTSEL mode (from Clockworker).
const RP2040_FLASH_BASE = 0x10000000;
const RP2040_FLASH_END = 0x10200000;
const FLASH_SECTOR_SIZE = 4096;
const UF2_BLOCK_SIZE = 512;
const UF2_MAGIC_START0 = 0x0a324655;
const UF2_MAGIC_START1 = 0x9e5d5157;
const UF2_MAGIC_END = 0x0ab16f30;
const UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000;
const RP2040_FAMILY_ID = 0xe48bff56;
const PICOBOOT_MAGIC = 0x431fd10b;
const PICOBOOT_IF_RESET = 0x41;
const PICOBOOT_IF_CMD_STATUS = 0x42;
const PC_EXCLUSIVE_ACCESS = 0x01;
const PC_REBOOT = 0x02;
const PC_FLASH_ERASE = 0x03;
const PC_WRITE = 0x05;
const PC_EXIT_XIP = 0x06;
const EXCLUSIVE_AND_EJECT = 2;
const RP2040_SRAM_END = 0x20042000;
const REBOOT_DELAY_MS = 500;
const DEVICE_WAIT_MS = 2000;
const DEVICE_POLL_MS = 200;
const COMMAND_NAMES = {
  [PC_EXCLUSIVE_ACCESS]: "EXCLUSIVE_ACCESS", [PC_FLASH_ERASE]: "FLASH_ERASE",
  [PC_WRITE]: "WRITE", [PC_EXIT_XIP]: "EXIT_XIP",
};
const STATUS_NAMES = [
  "OK", "UNKNOWN_CMD", "INVALID_CMD_LENGTH", "INVALID_TRANSFER_LENGTH",
  "INVALID_ADDRESS", "BAD_ALIGNMENT", "INTERLEAVED_WRITE", "REBOOTING", "UNKNOWN_ERROR",
];

function require(condition, message) {
  if (!condition) throw new Error(message);
}

function parseUf2(buffer) {
  require(buffer.byteLength >= UF2_BLOCK_SIZE && buffer.byteLength % UF2_BLOCK_SIZE === 0,
    "Select a valid UF2 file.");

  const blocks = new Map();
  for (let offset = 0; offset < buffer.byteLength; offset += UF2_BLOCK_SIZE) {
    const view = new DataView(buffer, offset, UF2_BLOCK_SIZE);
    require(view.getUint32(0, true) === UF2_MAGIC_START0 &&
      view.getUint32(4, true) === UF2_MAGIC_START1 &&
      view.getUint32(508, true) === UF2_MAGIC_END,
    "The selected file is not a UF2 image.");
    const address = view.getUint32(12, true);
    const payloadSize = view.getUint32(16, true);
    const flags = view.getUint32(8, true);
    if (flags & UF2_FLAG_FAMILY_ID_PRESENT) {
      require(view.getUint32(28, true) === RP2040_FAMILY_ID,
        "This UF2 is not for RP2040. For Pico 2, copy the UF2 to the RP2350 drive instead.");
    }
    require(payloadSize > 0 && payloadSize <= 476 && payloadSize % 256 === 0,
      "UF2 blocks must contain 256-byte flash pages.");
    require(address >= RP2040_FLASH_BASE && address + payloadSize <= RP2040_FLASH_END,
      "UF2 contains data outside RP2040 flash.");
    require(address % 256 === 0, "UF2 flash pages must be 256-byte aligned.");
    blocks.set(address, new Uint8Array(buffer.slice(offset + 32, offset + 32 + payloadSize)));
  }
  return [...blocks.entries()].sort(([a], [b]) => a - b);
}

function findPicobootInterface(device) {
  const configuration = device.configuration;
  require(configuration, "RP2040 has no active USB configuration.");
  for (const iface of configuration.interfaces) {
    for (const alternate of iface.alternates) {
      if (alternate.interfaceClass !== 0xff || alternate.interfaceSubclass !== 0 ||
          alternate.interfaceProtocol !== 0) continue;
      const out = alternate.endpoints.find((endpoint) => endpoint.type === "bulk" &&
        endpoint.direction === "out");
      const input = alternate.endpoints.find((endpoint) => endpoint.type === "bulk" &&
        endpoint.direction === "in");
      if (out && input) return { number: iface.interfaceNumber, alternate: alternate.alternateSetting, out, input };
    }
  }
  throw new Error("RP2040 PICOBOOT interface was not found. Hold BOOTSEL while plugging the board in, then try again.");
}

class PicobootConnection {
  constructor(device, iface) {
    this.device = device;
    this.iface = iface;
    this.token = 1;
  }

  async command(commandId, address = 0, size = 0, payload = null, argsSize = 8) {
    const command = new ArrayBuffer(32);
    const view = new DataView(command);
    view.setUint32(0, PICOBOOT_MAGIC, true);
    view.setUint32(4, this.token++, true);
    view.setUint8(8, commandId);
    view.setUint8(9, argsSize);
    view.setUint32(12, payload ? payload.byteLength : 0, true);
    view.setUint32(16, address, true);
    view.setUint32(20, size, true);
    const commandResult = await this.device.transferOut(this.iface.out.endpointNumber, command);
    require(commandResult.status === "ok" && commandResult.bytesWritten === 32,
      "PICOBOOT rejected a command.");
    if (payload) {
      const dataResult = await this.device.transferOut(this.iface.out.endpointNumber, payload);
      require(dataResult.status === "ok" && dataResult.bytesWritten === payload.byteLength,
        "PICOBOOT rejected flash data.");
    }
    const acknowledgement = await this.device.transferIn(this.iface.input.endpointNumber, 1);
    if (acknowledgement.status !== "ok" || acknowledgement.data.byteLength !== 0) {
      throw new Error(`PICOBOOT ${COMMAND_NAMES[commandId]} at 0x${address.toString(16)} ` +
        `failed (${await this.status()}).`);
    }
  }

  // The bootrom stalls the acknowledgement on failure; the reason is kept in
  // the command status.
  async status() {
    try {
      const result = await this.device.controlTransferIn({
        requestType: "vendor", recipient: "interface", request: PICOBOOT_IF_CMD_STATUS,
        value: 0, index: this.iface.number,
      }, 16);
      const code = result.data.getUint32(4, true);
      return STATUS_NAMES[code] ?? `status ${code}`;
    } catch (error) {
      return `status unavailable: ${error?.message ?? error}`;
    }
  }

  async reboot() {
    const command = new ArrayBuffer(32);
    const view = new DataView(command);
    view.setUint32(0, PICOBOOT_MAGIC, true);
    view.setUint32(4, this.token++, true);
    view.setUint8(8, PC_REBOOT);
    view.setUint8(9, 12);
    // As picotool does: boot normally (PC 0) after a short delay. The reboot
    // only proceeds once the acknowledgement has been read.
    view.setUint32(20, RP2040_SRAM_END, true);
    view.setUint32(24, REBOOT_DELAY_MS, true);
    const result = await this.device.transferOut(this.iface.out.endpointNumber, command);
    require(result.status === "ok" && result.bytesWritten === 32,
      "PICOBOOT could not restart the board.");
    const acknowledgement = await this.device.transferIn(this.iface.input.endpointNumber, 1);
    require(acknowledgement.status === "ok", "PICOBOOT could not restart the board.");
  }
}

export async function flashUf2(file, onProgress = () => {}) {
  require("usb" in navigator, "WebUSB requires a current Chrome or Edge browser.");
  require(file, "Choose the FM-1 Transporter UF2 file first.");
  const blocks = parseUf2(await file.arrayBuffer());
  // Reuse an RP2040 in update mode that this page may already use, so the
  // chooser only appears the first time. The RP2040 needs about a second to
  // come back in update mode, so wait briefly for it before asking.
  const filter = { vendorId: 0x2e8a, productId: 0x0003 };
  let known = [];
  for (let waited = 0; waited <= DEVICE_WAIT_MS; waited += DEVICE_POLL_MS) {
    known = (await navigator.usb.getDevices()).filter((candidate) =>
      candidate.vendorId === filter.vendorId && candidate.productId === filter.productId);
    if (known.length > 0) break;
    await new Promise((resolve) => setTimeout(resolve, DEVICE_POLL_MS));
  }
  const device = known.length === 1 ? known[0] :
    await navigator.usb.requestDevice({ filters: [filter] });
  try {
    await device.open();
    if (!device.configuration) await device.selectConfiguration(1);
    const iface = findPicobootInterface(device);
    await device.claimInterface(iface.number);
    if (iface.alternate !== 0) await device.selectAlternateInterface(iface.number, iface.alternate);
    await device.controlTransferOut({
      requestType: "vendor", recipient: "interface", request: PICOBOOT_IF_RESET,
      value: 0, index: iface.number,
    });
    const connection = new PicobootConnection(device, iface);
    // As picotool does: keep the host's mass-storage driver off the flash
    // while it is rewritten, and leave XIP mode before erasing.
    await connection.command(PC_EXCLUSIVE_ACCESS, EXCLUSIVE_AND_EJECT, 0, null, 1);
    await connection.command(PC_EXIT_XIP, 0, 0, null, 0);
    const sectors = [...new Set(blocks.map(([address]) => address & ~(FLASH_SECTOR_SIZE - 1)))];
    for (let index = 0; index < sectors.length; index++) {
      await connection.command(PC_FLASH_ERASE, sectors[index], FLASH_SECTOR_SIZE);
      onProgress(index + 1, sectors.length, "Erasing");
    }
    for (let index = 0; index < blocks.length; index++) {
      const [address, payload] = blocks[index];
      await connection.command(PC_WRITE, address, payload.byteLength, payload);
      onProgress(index + 1, blocks.length, "Writing");
    }
    await connection.reboot();
  } finally {
    if (device.opened) await device.close().catch(() => {});
  }
}
