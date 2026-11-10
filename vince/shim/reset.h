/*
 * Getting the phone back without opening it. The Redmi 5 is a unibody phone: a loader or kernel that
 * simply stops leaves it running until the battery is unplugged, because after `fastboot boot` nothing
 * has told the PMIC to reset on a long press of Power.
 *
 * Three independent ways out, set up by the loader before anything else runs:
 *   1. board_reset_prepare() programs the PM8953 so that holding Power (about 7 s) is a hard reset.
 *      The PMIC does this by itself from then on, whatever the CPU is doing: it also covers xnu.
 *   2. board_halt(N) / board_reset_now(): the code that stops resets the phone itself, by dropping
 *      PS_HOLD (what Linux and LK do to reboot this SoC), then PSCI SYSTEM_RESET, then a watchdog bite.
 *   3. The PMIC's stage 3 reset (hardware default, not programmed here) stays as the last resort.
 *
 * A hard reset power-cycles the phone and it boots what is on the eMMC (Android): nothing of a
 * `fastboot boot` session survives, and nothing is written to storage here.
 */
#pragma once
#include <stdint.h>

#ifndef HOST
/* Program the Power-key and PS_HOLD resets; prints the registers before and after on the console. */
void board_reset_prepare(void);
/* Ask LK to stop in fastboot after the next reset, whatever causes it (volatile; LK clears the request). */
void board_reset_to_fastboot(void);
/* Count down `seconds` on the console, then reset (0: no countdown, stop for good). */
void board_halt(unsigned seconds) __attribute__((noreturn));
/* Reset the phone now. */
void board_reset_now(void) __attribute__((noreturn));
#else
static inline void board_reset_prepare(void) {}
static inline void board_reset_to_fastboot(void) {}
#endif

/* Seconds a final screen (test result, failure, exception) stays up before the automatic reset. */
#ifndef HALT_RESET_S
#define HALT_RESET_S 60
#endif
