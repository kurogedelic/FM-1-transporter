// FM-1 Transporter - JieLi USB_KEY entry and ROM keep-alive pulses.
//
// The key sender and ACK detector are the known-good recovery baseline,
// unchanged except that only polarity A is used. Hardware findings from
// fm-1-research-lab (2026-09-30 / 2026-10-01) that shape the rest:
//   - Polarity A (D+ = clock, D- = data) took the key every time; B never did.
//   - After the ACK the ROM pulls D+ up within ~2 ms and keeps it up
//     while 1 ms pulses arrive (6 s in earlier runs). On 2026-10-01 it
//     released D+ after 4 s instead and re-attached ~1 s later.
//   - If the pulses stop before a host takes over, the ROM gives up and boots
//     flash. So the pulses are left running until the PIO USB host is ready.

#include "recovery.h"

#include <stdio.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#include "log.h"
#include "usb_key.pio.h"

#if FM1T_BOARD_DEBUGPROBE
// Debug Probe: red, green and yellow LEDs, lit high (yellow stands in for blue).
#define LED_R 2
#define LED_G 15
#define LED_B 16
#define LED_ON 1
#else
#define LED_R 17
#define LED_G 16
#define LED_B 25
#define LED_ON 0
#endif

// The key program sets both bus pins' directions with one immediate.
#if PIN_DM == PIN_DP + 1
#define KEY_PROGRAM usb_key_pp_program
#define KEY_CONFIG usb_key_pp_program_get_default_config
#elif PIN_DM == PIN_DP + 2
#define KEY_PROGRAM usb_key_pp_gap_program
#define KEY_CONFIG usb_key_pp_gap_program_get_default_config
#else
#error "PIN_DM must be one or two pins above PIN_DP"
#endif

#define USB_KEY_WORD 0x16EF
#define PIO_HZ 1000000
#define KEY_PACKET_US 340
#define GAP_PROBES 4
#define GAP_PROBE_SPACING_US 35
#define ACK_PROBE_LOWS 2
#define DP_HIGH_TIMEOUT_MS 500
#define PULSE_SAMPLE_US 100
#define PULSE_LOW_SAMPLES 3         // 300 us low = not just one of our 4 us pulses
#define HANDOFF_PULSE_MS 6000       // matches every proven run; try lower once M0 works
#define CAL_DONE_MIN_MS 1000        // a D+ drop after this long under pulses = calibration done
#define FALSE_PULLUP_LOW_MS 1000    // D+ still low this long at the timeout = no ROM pull-up

static PIO pio = pio0;
static uint sm_key, sm_sof, off_key, off_sof;

void rgb(bool r, bool g, bool b) {
    gpio_put(LED_R, LED_ON ? r : !r);
    gpio_put(LED_G, LED_ON ? g : !g);
    gpio_put(LED_B, LED_ON ? b : !b);
}

static void pad_setup(uint pin, enum gpio_drive_strength ds) {
    gpio_set_drive_strength(pin, ds);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_SLOW);
    gpio_set_pulls(pin, false, true);
}

void recovery_release_bus(void) {
    pio_sm_set_enabled(pio, sm_key, false);
    pio_sm_set_enabled(pio, sm_sof, false);

    const uint pins[] = {PIN_DP, PIN_DM};
    for (uint i = 0; i < 2; i++) {
        uint pin = pins[i];
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_dir(pin, GPIO_IN);
        gpio_put(pin, 0);
        gpio_set_outover(pin, GPIO_OVERRIDE_NORMAL);
        gpio_set_oeover(pin, GPIO_OVERRIDE_NORMAL);
        gpio_set_inover(pin, GPIO_OVERRIDE_NORMAL);
    }
}

static bool dp(void) { return gpio_get(PIN_DP); }

// Confirms a D+ high seen by dp() before it is taken for the ROM's pull-up.
// D+ is driven low for 1 us and released: the ROM's 1.5 kOhm pull-up brings
// it back within the 2 us settle, a stale level does not. On an RP2350
// (erratum E9: a released pad can hold a high level), or with an unpowered
// FM-1 charged by the key clock, a plain read gave false handoffs. Called
// once per candidate, so the ROM sees at most one extra edge.
static bool dp_pulled_up(void) {
    gpio_set_function(PIN_DP, GPIO_FUNC_SIO);
    gpio_put(PIN_DP, 0);
    gpio_set_dir(PIN_DP, GPIO_OUT);
    busy_wait_us_32(1);
    gpio_set_dir(PIN_DP, GPIO_IN);
    busy_wait_us_32(2);
    return gpio_get(PIN_DP);
}
static bool dm(void) { return gpio_get(PIN_DM); }

static bool probe_high(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_put(pin, 1);
    gpio_set_dir(pin, GPIO_OUT);
    busy_wait_us_32(2);
    bool v = gpio_get(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_put(pin, 0);
    return v;
}

// Polarity A: D+ is the clock (side-set), D- carries the data.
static void key_start(void) {
    pio_sm_set_enabled(pio, sm_key, false);
    gpio_set_drive_strength(PIN_DP, GPIO_DRIVE_STRENGTH_2MA);   // pulse_start raised it

    pio_sm_config c = KEY_CONFIG(off_key);
    sm_config_set_set_pins(&c, PIN_DP, PIN_DM - PIN_DP + 1);
    sm_config_set_out_pins(&c, PIN_DM, 1);
    sm_config_set_sideset_pins(&c, PIN_DP);
    sm_config_set_out_shift(&c, false, false, 32);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / PIO_HZ);

    pio_sm_init(pio, sm_key, off_key, &c);

    uint32_t mask = (1u << PIN_DP) | (1u << PIN_DM);
    pio_sm_set_pins_with_mask(pio, sm_key, 0, mask);
    pio_sm_set_pindirs_with_mask(pio, sm_key, 0, mask);
    pio_sm_clear_fifos(pio, sm_key);
    pio_sm_set_enabled(pio, sm_key, true);
}

static void key_send_packet(void) {
    pio_gpio_init(pio, PIN_DP);
    pio_gpio_init(pio, PIN_DM);
    pio_sm_put_blocking(pio, sm_key, (uint32_t)USB_KEY_WORD << 16);
    sleep_us(KEY_PACKET_US);
}

static int gap_sense(uint data_pin, bool *alive) {
    int lows = 0;
    int dp_highs = 0;

    for (int i = 0; i < GAP_PROBES; i++) {
        sleep_us(GAP_PROBE_SPACING_US);

        if (dp()) {
            if (++dp_highs >= 2) {
                if (dp_pulled_up()) return 2;
                dp_highs = 0;
            }
        } else {
            dp_highs = 0;
        }

        if (probe_high(data_pin)) {
            *alive = true;
            lows = 0;
        } else if (*alive && ++lows >= ACK_PROBE_LOWS) {
            return 1;
        }
    }

    return 0;
}

static void pulse_start(void) {
    gpio_set_drive_strength(PIN_DP, GPIO_DRIVE_STRENGTH_8MA);
    pio_gpio_init(pio, PIN_DP);

    pio_sm_config c = sof_pulse_program_get_default_config(off_sof);
    sm_config_set_set_pins(&c, PIN_DP, 1);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / PIO_HZ);

    pio_sm_init(pio, sm_sof, off_sof, &c);
    pio_sm_set_pins_with_mask(pio, sm_sof, 0, 1u << PIN_DP);
    pio_sm_set_pindirs_with_mask(pio, sm_sof, 0, 1u << PIN_DP);
    pio_sm_set_enabled(pio, sm_sof, true);
}

// Waits for the ROM's D+ pull-up after an ACK. A D- pull-up instead means the
// two data wires are swapped (seen on hardware after rewiring).
static bool wait_dp_high(uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
    int highs = 0;
    int dm_highs = 0;

    while (time_us_64() < deadline) {
        if (dp()) {
            if (++highs >= 5) {
                if (dp_pulled_up()) return true;
                highs = 0;
            }
        } else {
            highs = 0;
        }

        if (dm()) {
            if (++dm_highs == 5) {
                dlog("D- pulled up instead of D+ - are D+/D- wired swapped?");
            }
        } else {
            dm_highs = 0;
        }

        sleep_us(50);
    }

    return false;
}

// Feeds 1 ms pulses and returns, with the pulse state machine still running,
// as soon as the host should take over:
//   - D+ drops after CAL_DONE_MIN_MS: the ROM finished calibrating and is
//     about to re-attach as UBOOT. Seen on hardware 2026-10-01: drop at 4.0 s,
//     re-attach at 5.1 s, gave up at 7.2 s when no host answered. The host
//     must already be running when the re-attach comes.
//   - D+ held high for HANDOFF_PULSE_MS (the 6 s pattern from earlier runs).
// Returns false, with the bus released, when D+ has been low for
// FALSE_PULLUP_LOW_MS at the timeout: no ROM pull-up. The ROM's own early drop
// (7.3 ms into a proven run) and its release-and-re-attach (~1.1 s) are shorter.
static bool pulse_phase(void) {
    dlog("PULSE: D+ high (ROM pull-up), sending 4 us pulses every 1 ms");
    pulse_start();

    uint64_t start = time_us_64();
    uint32_t low_samples = 0;

    for (;;) {
        sleep_us(PULSE_SAMPLE_US);
        uint64_t now = time_us_64();
        uint64_t elapsed_us = now - start;

        if (!dp()) {
            if (++low_samples == PULSE_LOW_SAMPLES) {
                if (elapsed_us >= (uint64_t)CAL_DONE_MIN_MS * 1000) {
                    dlog("PULSE: D+ released after %.1f ms - calibration done, host takes over",
                         elapsed_us / 1000.0);
                    return true;
                }
                dlog("PULSE: early D+ blip at %.1f ms, ignored", elapsed_us / 1000.0);
            }
        } else {
            low_samples = 0;
        }

        if (elapsed_us >= (uint64_t)HANDOFF_PULSE_MS * 1000) {
            if (low_samples >= FALSE_PULLUP_LOW_MS * 1000 / PULSE_SAMPLE_US) {
                recovery_release_bus();
                dlog("PULSE: %d ms of pulses, D+ low for %.1f ms - no ROM pull-up, false ACK",
                     HANDOFF_PULSE_MS, low_samples * PULSE_SAMPLE_US / 1000.0);
                return false;
            }
            dlog("PULSE: %d ms of pulses, D+=%d - handing off", HANDOFF_PULSE_MS, dp());
            return true;
        }
    }
}

void recovery_run(void) {
    uint32_t packets = 0;
    bool alive = false;
    bool was_alive = false;
    uint64_t start = time_us_64();
    uint64_t last_status = start;
    uint64_t last_blink = start;
    bool blink = false;

    key_start();
    dlog("KEY: sending 0x%04X, polarity A (D+ clock, D- data). Switch the FM-1 ON now.",
         USB_KEY_WORD);

    for (;;) {
        key_send_packet();
        packets++;

        int s = gap_sense(PIN_DM, &alive);

        if (alive && !was_alive) {
            dlog("data line probe responds (not proof of a target)");
            was_alive = true;
        }

        if (s) {
            recovery_release_bus();
            dlog("%s after %lu packets",
                 s == 1 ? "ACK (data line held low)" : "D+ high in gap (ACK missed?)",
                 (unsigned long)packets);
            rgb(true, true, false);

            if (s == 2 || wait_dp_high(DP_HIGH_TIMEOUT_MS)) {
                if (pulse_phase()) return;
                dlog("resuming key");
            } else {
                dlog("no D+ pull-up within %d ms (D+=%d D-=%d) - false ACK, resuming key",
                     DP_HIGH_TIMEOUT_MS, dp(), dm());
            }
            alive = false;
            was_alive = false;
            key_start();
            continue;
        }

        uint64_t now = time_us_64();

        if (now - last_blink > 250000) {
            blink = !blink;
            rgb(blink, false, false);
            last_blink = now;
        }

        if (now - last_status > 2000000) {
            dlog("KEY: %lu packets", (unsigned long)packets);
            last_status = now;
        }
    }
}

bool recovery_wait_detached(uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
    int lows = 0;
    while (time_us_64() < deadline) {
        if (!dp()) {
            if (++lows >= 20) return true;     // 2 ms steadily low
        } else {
            lows = 0;
        }
        sleep_us(100);
    }
    return false;
}

bool recovery_target_attached(void) {
    for (int i = 0; i < 50; i++) {
        if (!dp()) return false;
        sleep_us(1000);
    }
    return true;
}

void recovery_pulses_only(uint32_t ms) {
    dlog("PULSE: %lu ms of 1 ms pulses for an already attached target", (unsigned long)ms);
    pulse_start();
    sleep_ms(ms);
}

void recovery_init(void) {
    gpio_init(LED_R);
    gpio_set_dir(LED_R, GPIO_OUT);
    gpio_init(LED_G);
    gpio_set_dir(LED_G, GPIO_OUT);
    gpio_init(LED_B);
    gpio_set_dir(LED_B, GPIO_OUT);
    rgb(false, false, false);

    gpio_init(PIN_DP);
    gpio_init(PIN_DM);
    pad_setup(PIN_DP, GPIO_DRIVE_STRENGTH_2MA);
    pad_setup(PIN_DM, GPIO_DRIVE_STRENGTH_2MA);

    off_key = pio_add_program(pio, &KEY_PROGRAM);
    off_sof = pio_add_program(pio, &sof_pulse_program);
    sm_key = pio_claim_unused_sm(pio, true);
    sm_sof = pio_claim_unused_sm(pio, true);
    recovery_release_bus();
}
