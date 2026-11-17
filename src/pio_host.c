#include "pio_host.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pio_usb.h"
#include "tusb.h"
#include "class/msc/msc.h"

#include "log.h"
#include "jieli_uboot.h"
#include "recovery.h"
#include "transporter_proto.h"

#define HOST_RHPORT 1
#define FM1_VID 0x4C4A
#define FM1_PID 0x8057
#define FM1_V15_PID 0xC755      // stock FM-1 application (USB audio + MIDI)
#define XFER_TIMEOUT_MS 2000
#define FIRST_CBW_DELAY_MS 1500     // macOS had the device for seconds before its first CBW
#define INQUIRY_RETRY_MS 300
#define INQUIRY_TRIES 30
#define ATTACH_STATUS_MS 2000
// A target that pulls D+ up but never enumerates (V15 or a fresh UBOOT that
// booted under the running host, without the 1 ms pulses) is recovered by
// rebooting the transporter, which pulses first and then hosts again.
#define REATTACH_AFTER_MS 3000
#define REATTACH_MAX 3              // consecutive reboots without a mount
#define REATTACH_SCRATCH 1          // watchdog scratch register for the count

static volatile uint8_t mounted_addr;
static volatile bool probe_pending;
static uint64_t host_start_us;
static uint64_t last_status_us;

static uint8_t ep_in, ep_out, msc_itf;
static tusb_desc_endpoint_t other_bulk_out;   // e.g. V15's USB-MIDI OUT, for 'm'
static bool other_bulk_out_open;
static bool mounted_is_v15;
static uint32_t cbw_tag = 1;
static volatile char pending_cmd;
static uint64_t mount_us;
static uint64_t unmounted_since_us;
static int inquiry_tries;       // > 0 while the post-mount INQUIRY loop is active
static uint64_t next_inquiry_us;

static bool bot_cmd(uint8_t daddr, const uint8_t *cdb, uint8_t cdb_len, bool in, void *buf,
                    uint16_t len, uint32_t *got);

static CFG_TUSB_MEM_ALIGN uint8_t cfg_buf[512];
static CFG_TUSB_MEM_ALIGN uint8_t data_buf[512];
static CFG_TUSB_MEM_ALIGN msc_cbw_t cbw;
static CFG_TUSB_MEM_ALIGN msc_csw_t csw;

void fm1_pio_host_start(void) {
    pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
    cfg.pin_dp = PIN_DP;
    // PIO0 holds the recovery programs; give the host all of PIO1.
    cfg.pio_tx_num = 1;
    cfg.pio_rx_num = 1;
    // pio_usb claims tx_ch itself and panics if it is taken.
    int ch = dma_claim_unused_channel(true);
    dma_channel_unclaim(ch);
    cfg.tx_ch = (uint8_t)ch;

    tuh_configure(HOST_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
    tuh_init(HOST_RHPORT);

    host_start_us = last_status_us = unmounted_since_us = time_us_64();
}

bool fm1_pio_host_mounted(void) {
    return mounted_addr != 0;
}

static bool uboot_ready;    // INQUIRY answered on the current mount

bool fm1_host_bot(const uint8_t *cdb, uint8_t cdb_len, bool in, void *buf, uint16_t len,
                  uint32_t *got) {
    uint8_t daddr = mounted_addr;
    if (!daddr || !uboot_ready) return false;
    return bot_cmd(daddr, cdb, cdb_len, in, buf, len, got);
}

bool fm1_host_uboot_ready(void) {
    return mounted_addr != 0 && uboot_ready;
}

bool fm1_host_v15_mounted(void) {
    return mounted_addr != 0 && mounted_is_v15;
}

static bool edpt_sync(uint8_t daddr, uint8_t ep, void *buf, uint16_t len, uint32_t *actual);

// Stock V15's USB-MIDI receive path compares every 8-byte transfer with
// 04 F0 22 24 07 35 xx F7; xx = 7D calls the mask ROM UBOOT1.00 and never
// returns. Nothing is written.
bool fm1_host_softkey(void) {
    static CFG_TUSB_MEM_ALIGN uint8_t pkt[8] = {0x04, 0xF0, 0x22, 0x24, 0x07, 0x35, 0x7D, 0xF7};
    uint8_t daddr = mounted_addr;
    if (!daddr || !mounted_is_v15 || !other_bulk_out.bLength) return false;
    if (!other_bulk_out_open) other_bulk_out_open = tuh_edpt_open(daddr, &other_bulk_out);

    dlog("SOFTKEY: sending F0 22 24 35 7D F7 to USB-MIDI OUT %02X",
         other_bulk_out.bEndpointAddress);
    bool sent = false;
    for (int i = 0; i < 3 && mounted_addr == daddr; i++) {
        // The unit leaves the bus once it takes the key, so later sends may fail.
        sent |= edpt_sync(daddr, other_bulk_out.bEndpointAddress, pkt, sizeof(pkt), NULL);
        sleep_ms(20);
    }
    return sent;
}

void tuh_mount_cb(uint8_t daddr) {
    dlog("HOST: device mounted, addr=%u (%.1f ms after host start)", daddr,
         (time_us_64() - host_start_us) / 1000.0);
    mounted_addr = daddr;
    mount_us = time_us_64();
    watchdog_hw->scratch[REATTACH_SCRATCH] = 0;
    uboot_ready = false;
    jl_reset();
    probe_pending = true;
}

void tuh_umount_cb(uint8_t daddr) {
    dlog("HOST: device addr=%u detached (%.1f ms after mount)", daddr,
         (time_us_64() - mount_us) / 1000.0);
    mounted_addr = 0;
    uboot_ready = false;
    mounted_is_v15 = false;
    unmounted_since_us = time_us_64();
    probe_pending = false;
    inquiry_tries = 0;
    rgb(false, false, true);
}

//--------------------------------------------------------------------+
// Blocking bulk transfer, run from the core 1 loop (never from a callback)
//--------------------------------------------------------------------+

static volatile xfer_result_t xfer_result;
static volatile uint32_t xfer_actual;

static void xfer_done(tuh_xfer_t *x) {
    xfer_actual = x->actual_len;
    xfer_result = x->result;
}

static bool edpt_sync(uint8_t daddr, uint8_t ep, void *buf, uint16_t len, uint32_t *actual) {
    xfer_result = XFER_RESULT_INVALID;
    xfer_actual = 0;

    tuh_xfer_t x = {
        .daddr = daddr,
        .ep_addr = ep,
        .buflen = len,
        .buffer = buf,
        .complete_cb = xfer_done,
    };
    if (!tuh_edpt_xfer(&x)) {
        dlog("HOST: EP %02X submit failed", ep);
        return false;
    }

    uint64_t deadline = time_us_64() + XFER_TIMEOUT_MS * 1000ull;
    while (xfer_result == XFER_RESULT_INVALID) {
        tuh_task();
        if (mounted_addr != daddr) {
            dlog("HOST: EP %02X abandoned, device gone", ep);
            return false;
        }
        if (time_us_64() > deadline) {
            tuh_edpt_abort_xfer(daddr, ep);
            dlog("HOST: EP %02X timeout", ep);
            return false;
        }
    }

    if (actual) *actual = xfer_actual;
    if (xfer_result != XFER_RESULT_SUCCESS) {
        dlog("HOST: EP %02X result %d", ep, xfer_result);
        return false;
    }
    return true;
}

static bool ctrl_sync(uint8_t daddr, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                      void *buf, uint16_t len) {
    const tusb_control_request_t setup = {
        .bmRequestType = type,
        .bRequest = req,
        .wValue = value,
        .wIndex = index,
        .wLength = len,
    };
    xfer_result_t result = XFER_RESULT_INVALID;
    tuh_xfer_t x = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &setup,
        .buffer = buf,
        .complete_cb = NULL,    // blocking
        .user_data = (uintptr_t)&result,
    };
    if (!tuh_control_xfer(&x)) {
        dlog("HOST: control %02X/%02X submit failed", type, req);
        return false;
    }
    if (result != XFER_RESULT_SUCCESS) {
        dlog("HOST: control %02X/%02X result %d%s", type, req, result,
             result == XFER_RESULT_STALLED ? " (STALL)" : "");
        return false;
    }
    return true;
}

// One Bulk-Only Transport command. len == 0 means no data stage. buf must be
// word-aligned RAM.
static bool bot_cmd(uint8_t daddr, const uint8_t *cdb, uint8_t cdb_len, bool in, void *buf,
                    uint16_t len, uint32_t *got) {
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = MSC_CBW_SIGNATURE;
    cbw.tag = cbw_tag++;
    cbw.total_bytes = len;
    cbw.dir = in ? TUSB_DIR_IN_MASK : 0;
    cbw.lun = 0;
    cbw.cmd_len = cdb_len;
    memcpy(cbw.command, cdb, cdb_len);

    if (!edpt_sync(daddr, ep_out, &cbw, sizeof(cbw), NULL)) {
        dlog("HOST: CBW not accepted");
        return false;
    }
    if (len && !edpt_sync(daddr, in ? ep_in : ep_out, buf, len, got)) return false;
    if (!edpt_sync(daddr, ep_in, &csw, sizeof(csw), NULL)) {
        dlog("HOST: no CSW");
        return false;
    }

    if (csw.signature != MSC_CSW_SIGNATURE || csw.tag != cbw.tag) {
        dlog("HOST: bad CSW sig=%08lx tag=%lu", (unsigned long)csw.signature,
             (unsigned long)csw.tag);
        return false;
    }
    if (csw.status != 0) {
        dlog("HOST: CSW status %u residue %lu", csw.status, (unsigned long)csw.data_residue);
        return false;
    }
    return true;
}

//--------------------------------------------------------------------+
// M0 probe: descriptors + INQUIRY, all read-only
//--------------------------------------------------------------------+

static void log_string(uint8_t daddr, const char *what, uint8_t idx) {
    if (!idx) return;
    uint16_t s[64];
    if (tuh_descriptor_get_string_sync(daddr, idx, 0x0409, s, sizeof(s)) != XFER_RESULT_SUCCESS) {
        dlog("  %s: <read failed>", what);
        return;
    }
    char a[64];
    int n = ((s[0] & 0xff) - 2) / 2;
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    for (int i = 0; i < n; i++) a[i] = (s[1 + i] < 0x80) ? (char)s[1 + i] : '?';
    a[n] = 0;
    dlog("  %s: \"%s\"", what, a);
}

static bool probe_descriptors(uint8_t daddr) {
    tusb_desc_device_t dd;
    if (tuh_descriptor_get_device_sync(daddr, &dd, sizeof(dd)) != XFER_RESULT_SUCCESS) {
        dlog("HOST: device descriptor read failed");
        return false;
    }

    dlog("DEVICE DESCRIPTOR OK");
    dlog("  VID=%04X PID=%04X bcdDevice=%04X bcdUSB=%04X", dd.idVendor, dd.idProduct,
         dd.bcdDevice, dd.bcdUSB);
    dlog("  class=%02X/%02X/%02X ep0=%u configs=%u", dd.bDeviceClass, dd.bDeviceSubClass,
         dd.bDeviceProtocol, dd.bMaxPacketSize0, dd.bNumConfigurations);
    log_string(daddr, "manufacturer", dd.iManufacturer);
    log_string(daddr, "product", dd.iProduct);
    log_string(daddr, "serial", dd.iSerialNumber);

    mounted_is_v15 = dd.idVendor == FM1_VID && dd.idProduct == FM1_V15_PID;
    if (mounted_is_v15) {
        dlog("  stock FM-1 application; `uboot` (fm1t) or console k enters UBOOT via the soft key");
    } else if (dd.idVendor != FM1_VID || dd.idProduct != FM1_PID) {
        dlog("  not the JieLi UBOOT (%04X:%04X expected)", FM1_VID, FM1_PID);
    }

    if (tuh_descriptor_get_configuration_sync(daddr, 0, cfg_buf, 9) != XFER_RESULT_SUCCESS) {
        dlog("HOST: configuration descriptor read failed");
        return false;
    }
    uint16_t total = ((tusb_desc_configuration_t *)cfg_buf)->wTotalLength;
    if (total > sizeof(cfg_buf)) total = sizeof(cfg_buf);
    if (tuh_descriptor_get_configuration_sync(daddr, 0, cfg_buf, total) != XFER_RESULT_SUCCESS) {
        dlog("HOST: configuration descriptor (%u bytes) read failed", total);
        return false;
    }

    dlog("CONFIGURATION DESCRIPTOR (%u bytes), bConfigurationValue=%u", total,
         ((tusb_desc_configuration_t *)cfg_buf)->bConfigurationValue);
    ep_in = ep_out = 0;
    other_bulk_out.bLength = 0;
    other_bulk_out_open = false;
    const tusb_desc_endpoint_t *bulk_in = NULL, *bulk_out = NULL;
    bool in_msc = false;

    for (uint16_t off = 0; off + 2 <= total && cfg_buf[off] >= 2; off += cfg_buf[off]) {
        const uint8_t *d = &cfg_buf[off];
        if (d[1] == TUSB_DESC_INTERFACE) {
            const tusb_desc_interface_t *itf = (const void *)d;
            dlog("  interface %u alt %u: class=%02X/%02X/%02X endpoints=%u",
                 itf->bInterfaceNumber, itf->bAlternateSetting, itf->bInterfaceClass,
                 itf->bInterfaceSubClass, itf->bInterfaceProtocol, itf->bNumEndpoints);
            in_msc = itf->bInterfaceClass == TUSB_CLASS_MSC;
            if (in_msc) msc_itf = itf->bInterfaceNumber;
        } else if (d[1] == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *ep = (const void *)d;
            dlog("    endpoint %02X attr=%02X maxpacket=%u interval=%u", ep->bEndpointAddress,
                 ep->bmAttributes.xfer, tu_edpt_packet_size(ep), ep->bInterval);
            if (!in_msc && ep->bmAttributes.xfer == TUSB_XFER_BULK &&
                tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_OUT && !other_bulk_out.bLength) {
                other_bulk_out = *ep;
            }
            if (in_msc && ep->bmAttributes.xfer == TUSB_XFER_BULK) {
                if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) bulk_in = ep;
                else bulk_out = ep;
            }
        }
    }

    if (!bulk_in || !bulk_out) {
        dlog("HOST: no MSC bulk IN/OUT pair found");
        return false;
    }

    if (!tuh_edpt_open(daddr, bulk_out) || !tuh_edpt_open(daddr, bulk_in)) {
        dlog("HOST: opening bulk endpoints failed");
        return false;
    }
    ep_in = bulk_in->bEndpointAddress;
    ep_out = bulk_out->bEndpointAddress;
    dlog("HOST: MSC bulk OUT=%02X IN=%02X", ep_out, ep_in);
    return true;
}

static bool probe_inquiry(uint8_t daddr) {
    static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    uint32_t got = 0;

    if (!bot_cmd(daddr, inquiry, sizeof(inquiry), true, data_buf, 36, &got) || got < 36) {
        dlog("INQUIRY failed (got %lu bytes)", (unsigned long)got);
        return false;
    }

    char vendor[9], product[17], rev[5];
    memcpy(vendor, &data_buf[8], 8);
    vendor[8] = 0;
    memcpy(product, &data_buf[16], 16);
    product[16] = 0;
    memcpy(rev, &data_buf[32], 4);
    rev[4] = 0;
    dlog("INQUIRY OK: vendor=\"%s\" product=\"%s\" rev=\"%s\"", vendor, product, rev);
    return true;
}

static void get_max_lun(uint8_t daddr) {
    static CFG_TUSB_MEM_ALIGN uint8_t lun;
    if (ctrl_sync(daddr, 0xA1, 0xFE, 0, msc_itf, &lun, 1)) dlog("GET MAX LUN: %u", lun);
}

//--------------------------------------------------------------------+
// Console commands (read-only diagnostics; no memory or flash access)
//--------------------------------------------------------------------+

void fm1_pio_host_command(char c) {
    pending_cmd = c;
}

static void run_command(uint8_t daddr, char c) {
    static const uint8_t tur[6] = {0};
    uint32_t got = 0;

    switch (c) {
    case 'l':
        get_max_lun(daddr);
        break;
    case 'i':
        probe_inquiry(daddr);
        break;
    case 'u':
        dlog("TEST UNIT READY: %s", bot_cmd(daddr, tur, sizeof(tur), true, NULL, 0, NULL) ? "OK" : "failed");
        break;
    case 'c':
        dlog("SET_CONFIGURATION 1: %s",
             ctrl_sync(daddr, 0x00, TUSB_REQ_SET_CONFIGURATION, 1, 0, NULL, 0) ? "OK" : "failed");
        break;
    case 'r':
        dlog("BOT reset: %s", ctrl_sync(daddr, 0x21, 0xFF, 0, msc_itf, NULL, 0) ? "OK" : "failed");
        dlog("CLEAR_HALT IN: %s",
             ctrl_sync(daddr, 0x02, TUSB_REQ_CLEAR_FEATURE, 0, ep_in, NULL, 0) ? "OK" : "failed");
        dlog("CLEAR_HALT OUT: %s",
             ctrl_sync(daddr, 0x02, TUSB_REQ_CLEAR_FEATURE, 0, ep_out, NULL, 0) ? "OK" : "failed");
        break;
    case 'n':
        if (edpt_sync(daddr, ep_in, data_buf, 64, &got)) {
            dlog("IN %02X: %lu bytes, first %02X %02X %02X %02X", ep_in, (unsigned long)got,
                 data_buf[0], data_buf[1], data_buf[2], data_buf[3]);
        }
        break;
    case 'd':
        probe_descriptors(daddr);
        break;
    case 'k':
        dlog("soft key: %s", fm1_host_softkey() ? "sent" : "not sent (no stock V15 mounted)");
        break;
    case 'm': {
        // Bulk OUT sanity check against a non-MSC bulk endpoint, e.g. the
        // stock V15 USB-MIDI OUT: one Active Sensing packet, harmless.
        static CFG_TUSB_MEM_ALIGN uint8_t pkt[4] = {0x0F, 0xFE, 0x00, 0x00};
        if (!other_bulk_out.bLength) {
            dlog("no non-MSC bulk OUT endpoint");
            break;
        }
        if (!other_bulk_out_open) {
            other_bulk_out_open = tuh_edpt_open(daddr, &other_bulk_out);
        }
        dlog("bulk OUT %02X, 4 bytes: %s", other_bulk_out.bEndpointAddress,
             edpt_sync(daddr, other_bulk_out.bEndpointAddress, pkt, sizeof(pkt), NULL) ? "ACKed" : "failed");
        break;
    }
    default:
        dlog("commands: l=GET MAX LUN  i=INQUIRY  u=TEST UNIT READY  c=SET_CONFIGURATION  "
             "r=BOT reset+clear halt  n=read IN once  d=descriptors  m=bulk OUT test (non-MSC)  "
             "k=soft key (stock V15 -> UBOOT)");
        break;
    }
}

void fm1_pio_host_task(void) {
    tuh_task();

    uint8_t daddr = mounted_addr;

    uint64_t now = time_us_64();

    if (probe_pending && daddr) {
        probe_pending = false;
        if (probe_descriptors(daddr)) {
            inquiry_tries = INQUIRY_TRIES;
            next_inquiry_us = mount_us + FIRST_CBW_DELAY_MS * 1000ull;
            dlog("HOST: first CBW in %d ms", FIRST_CBW_DELAY_MS);
        } else {
            rgb(true, false, false);
        }
    }

    if (inquiry_tries > 0 && daddr && now >= next_inquiry_us) {
        if (inquiry_tries == INQUIRY_TRIES) {
            // macOS asks for the LUN count before its first CBW; do the same.
            get_max_lun(daddr);
        }
        inquiry_tries--;
        dlog("INQUIRY attempt %d (%.1f ms after mount)", INQUIRY_TRIES - inquiry_tries,
             (time_us_64() - mount_us) / 1000.0);
        if (probe_inquiry(daddr)) {
            inquiry_tries = 0;
            uboot_ready = true;
            rgb(false, true, false);
            dlog("UBOOT READY: enumerated and answered INQUIRY via the PIO host.");
        } else if (!mounted_addr) {
            // detached mid-attempt; tuh_umount_cb already stopped the loop
        } else if (inquiry_tries == 0) {
            rgb(true, false, false);
            dlog("UBOOT: INQUIRY never accepted. Type h on the console for diagnostics.");
        } else {
            next_inquiry_us = time_us_64() + INQUIRY_RETRY_MS * 1000ull;
        }
    }

    fm1_proto_poll();

    char c = pending_cmd;
    if (c) {
        pending_cmd = 0;
        if (daddr && ((ep_in && ep_out) || c == 'm' || c == 'k')) {
            dlog("> %c", c);
            run_command(daddr, c);
        } else {
            dlog("> %c ignored: no UBOOT mounted", c);
        }
    }

    if (!daddr && now - unmounted_since_us > REATTACH_AFTER_MS * 1000ull) {
        // Pico-PIO-USB inverts the pad input (GPIO_OVERRIDE_INVERT), so a
        // pulled-up D+ reads as 0 here. With nothing mounted there is no bus
        // traffic, so the level is steady.
        bool pulled_up = true;
        for (int i = 0; i < 20 && pulled_up; i++) {
            pulled_up = !gpio_get(PIN_DP);
            busy_wait_us_32(100);
        }
        uint32_t n = watchdog_hw->scratch[REATTACH_SCRATCH];
        if (pulled_up && n < REATTACH_MAX) {
            watchdog_hw->scratch[REATTACH_SCRATCH] = n + 1;
            dlog("HOST: D+ pulled up but nothing enumerated for %d ms - rebooting the "
                 "transporter to pulse and re-host (%lu/%d)", REATTACH_AFTER_MS,
                 (unsigned long)(n + 1), REATTACH_MAX);
            sleep_ms(100);      // let core 0 flush the log
            watchdog_reboot(0, 0, 10);
            for (;;) tight_loop_contents();
        }
        unmounted_since_us = now;   // re-check after another interval
    }

    if (!daddr && now - last_status_us > ATTACH_STATUS_MS * 1000ull) {
        dlog("HOST: no device mounted yet (%.1f s after host start)",
             (now - host_start_us) / 1e6);
        last_status_us = now;
    }
}
