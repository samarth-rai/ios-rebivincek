/*
 * Getting the phone back without opening it (see reset.h). MMU and caches are off: every access is
 * Device memory, naturally aligned. Nothing here touches storage: the PMIC registers written are the
 * power-on block's volatile reset configuration, the same ones Linux and LK program on every boot.
 *
 * Addresses are the MSM8953 / SDM450 ones (stock device tree of rosy and of vince):
 *   restart@4ab000 "qcom,pshold"            PS_HOLD, the SoC's line to the PMIC
 *   qcom,spmi@200f000                       PMIC arbiter: core 0x200f000, cnfg 0x200a000,
 *                                           channels 0x2400000, observer 0x2c00000 (0x8000 per APID)
 *   qcom,pm8953@0 / qcom,power-on@800       PON peripheral, PPID 0x008
 *   qcom,wdt@b017000 "qcom,msm-watchdog"    apps watchdog
 * The arbiter protocol is the one of xnu/patches/0979 and 0981 (measured on vince, arbiter v2).
 */
#include "reset.h"
#include "console.h"

#ifndef HOST

#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define RED	0xffff3030U

#define PS_HOLD		0x004ab000UL
#define ARB_CORE	0x0200f000UL
#define ARB_CNFG	0x0200a000UL
#define ARB_CHNLS	0x02400000UL
#define ARB_OBSRVR	0x02c00000UL
#define ARB_CMD		0x00
#define ARB_STATUS	0x08
#define ARB_WDATA0	0x10
#define ARB_RDATA0	0x18
#define OP_EXT_WRITEL	0U
#define OP_EXT_READL	1U
#define WDT_BASE	0x0b017000UL
#define WDT_RST		0x04
#define WDT_EN		0x08
#define WDT_BITE_TIME	0x14

/* PM8953 power-on peripheral (SID 0, 0x800): register offsets inside the peripheral */
#define PON_PPID		0x008U
#define PON_KPDPWR_S1_TIMER	0x40	/* bits 3:0 */
#define PON_KPDPWR_S2_TIMER	0x41	/* bits 2:0 */
#define PON_KPDPWR_S2_CNTL	0x42	/* bits 3:0: reset type */
#define PON_KPDPWR_S2_CNTL2	0x43	/* bit 7: enable */
#define PON_PS_HOLD_RESET_CTL	0x5a	/* bits 3:0: reset type */
#define PON_PS_HOLD_RESET_CTL2	0x5b	/* bit 7: enable */
#define PON_SOFT_RB_SPARE	0x8f	/* bits 7:2: reason for the next boot, read and cleared by LK */
#define PON_REASON_BOOTLOADER	0x02U	/* PON_RESTART_REASON_BOOTLOADER (Linux), FASTBOOT_HARD_RESET_MODE (LK) */
/* qcom,msm-imem@8600000 / restart_reason@65c in the stock device tree: the same request for a warm reset */
#define IMEM_RESTART_REASON	0x0860065cUL
#define RESTART_BOOTLOADER	0x77665500U
#define PON_RESET_EN		0x80U
#define PON_TYPE_HARD_RESET	0x07U	/* PON_POWER_OFF_HARD_RESET: power cycle, then a normal boot */
/* rosy's own device tree (qcom,pon_1): s1-timer 4480 ms, s2-timer 2000 ms, s2-type 7 */
#define KPDPWR_S1_IDX		13U	/* 4480 ms in the S1 table */
#define KPDPWR_S2_IDX		7U	/* 2000 ms in the S2 table */

#define rsr(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ---- line builder ---- */
static char lb[CON_MAXCOLS + 2];
static int lbn;
static void pc(char c) { if (lbn < CON_MAXCOLS) lb[lbn++] = c; }
static void ps(const char *s) { while (*s) pc(*s++); }
static void px(uint64_t v, int d) { for (int i = d - 1; i >= 0; i--) pc("0123456789ABCDEF"[(v >> (4 * i)) & 0xf]); }
static void pd(unsigned v) { if (v >= 100) pc('0' + (v / 100) % 10); if (v >= 10) pc('0' + (v / 10) % 10); pc('0' + v % 10); }
static int pe(uint32_t c) { lb[lbn] = 0; lbn = 0; return con_line(lb, c); }
static void pset(int idx, uint32_t c) { lb[lbn] = 0; lbn = 0; con_set(idx, lb, c); }

/* ---- time: the generic timer when it runs, a counted loop when it does not ---- */
static int timer_runs = -1;

static void spin(uint64_t n)
{
	for (volatile uint64_t i = 0; i < n; i++)
		;
}

static void delay_us(uint64_t us)
{
	if (timer_runs < 0) {
		uint64_t a = rsr("cntvct_el0");
		spin(20000);
		timer_runs = rsr("cntvct_el0") != a && rsr("cntfrq_el0") >= 1000000;
	}
	if (timer_runs) {
		uint64_t f = rsr("cntfrq_el0");
		uint64_t end = rsr("cntvct_el0") + (f / 1000000) * us + 1;
		/* the cap ends the wait if the counter stops after all */
		for (volatile uint64_t n = 0; n < us * 2000 + 100000 && rsr("cntvct_el0") < end; n++)
			;
	} else {
		spin(us * 20);	/* rough: no clock to go by */
	}
}

/* ---- PMIC arbiter ---- */
static int arb_apid = -1;	/* APID of the PON peripheral, -1 unknown, -2 not usable */

static int pon_apid(void)
{
	if (arb_apid != -1)
		return arb_apid;
	arb_apid = -2;
	uint32_t ver = rd32(ARB_CORE);
	if ((ver >> 28) != 2) {		/* only the v2 layout is known to work */
		ps("PMIC arbiter version "); px(ver, 8); ps(": not touched"); pe(YELLOW);
		return arb_apid;
	}
	for (unsigned apid = 0; apid < 256; apid++) {
		uint32_t m = rd32(ARB_CORE + 0x800 + 4 * apid);
		if (m && ((m >> 8) & 0xfff) == PON_PPID) {
			uint32_t owner = rd32(ARB_CNFG + 0x700 + 4 * apid) & 7;
			if (owner != 0) {
				ps("PON owned by EE "); px(owner, 1); ps(": not touched"); pe(YELLOW);
				return arb_apid;
			}
			arb_apid = (int)apid;
			return arb_apid;
		}
	}
	ps("PON peripheral not found in the arbiter"); pe(YELLOW);
	return arb_apid;
}

static int arb_wait(uintptr_t chan)
{
	for (int i = 0; i < 20000; i++) {
		uint32_t st = rd32(chan + ARB_STATUS);
		if (st & 0xe)
			return -1;
		if (st & 1)
			return 0;
		delay_us(1);
	}
	return -1;
}

static int pon_read(unsigned reg, uint8_t *val)
{
	int apid = pon_apid();
	if (apid < 0)
		return -1;
	uintptr_t chan = ARB_OBSRVR + 0x8000UL * (unsigned)apid;
	wr32(chan + ARB_CMD, OP_EXT_READL << 27 | (reg & 0xff) << 4);
	if (arb_wait(chan))
		return -1;
	*val = (uint8_t)rd32(chan + ARB_RDATA0);
	return 0;
}

static int pon_write(unsigned reg, uint8_t val)
{
	int apid = pon_apid();
	if (apid < 0)
		return -1;
	uintptr_t chan = ARB_CHNLS + 0x8000UL * (unsigned)apid;
	wr32(chan + ARB_WDATA0, val);
	wr32(chan + ARB_CMD, OP_EXT_WRITEL << 27 | (reg & 0xff) << 4);
	return arb_wait(chan);
}

/* Change only `mask` of a register; -1 if the register cannot be read or written. */
static int pon_update(unsigned reg, uint8_t mask, uint8_t val)
{
	uint8_t old;
	if (pon_read(reg, &old))
		return -1;
	return pon_write(reg, (uint8_t)((old & ~mask) | (val & mask)));
}

/*
 * The order Linux (qpnp-power-on.c, qpnp_config_reset / qpnp_pon_system_pwr_off) and LK
 * (pm8x41_reset_configure) use: switch the reset off, wait, set it up, switch it on.
 */
static int pon_config(unsigned cntl, unsigned cntl2, uint8_t type, int s1, int s2)
{
	int r = pon_update(cntl2, PON_RESET_EN, 0);
	delay_us(500);
	if (s1 >= 0)
		r |= pon_update(PON_KPDPWR_S1_TIMER, 0x0f, (uint8_t)s1);
	if (s2 >= 0)
		r |= pon_update(PON_KPDPWR_S2_TIMER, 0x07, (uint8_t)s2);
	r |= pon_update(cntl, 0x0f, type);
	r |= pon_update(cntl2, PON_RESET_EN, PON_RESET_EN);
	return r;
}

static void show4(const char *what, unsigned first)
{
	ps(what);
	for (unsigned i = 0; i < 4; i++) {
		uint8_t v = 0;
		pc(' ');
		if (pon_read(first + i, &v) == 0)
			px(v, 2);
		else
			ps("??");
	}
}

void board_reset_prepare(void)
{
	uint8_t a = 0, b = 0;

	if (pon_apid() < 0) {
		ps("no PMIC setup: Power key may not reset"); pe(YELLOW);
		return;
	}
	/* what LK left, then what we set: the four registers 0x840-0x843, then 0x85a/0x85b */
	show4("PON key was", PON_KPDPWR_S1_TIMER); pe(WHITE);
	int r1 = pon_config(PON_KPDPWR_S2_CNTL, PON_KPDPWR_S2_CNTL2, PON_TYPE_HARD_RESET, KPDPWR_S1_IDX, KPDPWR_S2_IDX);
	show4("PON key now", PON_KPDPWR_S1_TIMER); pe(r1 ? RED : WHITE);

	(void)pon_read(PON_PS_HOLD_RESET_CTL, &a);
	(void)pon_read(PON_PS_HOLD_RESET_CTL2, &b);
	ps("PS_HOLD was "); px(a, 2); pc(' '); px(b, 2);
	int r2 = pon_config(PON_PS_HOLD_RESET_CTL, PON_PS_HOLD_RESET_CTL2, PON_TYPE_HARD_RESET, -1, -1);
	a = b = 0;
	(void)pon_read(PON_PS_HOLD_RESET_CTL, &a);
	(void)pon_read(PON_PS_HOLD_RESET_CTL2, &b);
	ps(" now "); px(a, 2); pc(' '); px(b, 2); pe(r2 ? RED : WHITE);

	if (r1) {
		ps("!! Power-key reset NOT set up"); pe(RED);
	} else {
		ps("hold Power about 7 s = hard reset"); pe(GREEN);
	}
}

/*
 * Make the next reset end in fastboot instead of booting the eMMC: the reason code Android's
 * "reboot bootloader" leaves for LK, in the PMIC's spare register (kept across a hard reset) and in
 * the SoC's IMEM cookie (kept across a warm reset). Both are volatile and LK clears them when it has
 * read them; nothing is written to storage. Only on a phone whose PMIC arbiter was recognised.
 */
void board_reset_to_fastboot(void)
{
	uint8_t v = 0;

	if (pon_apid() < 0)
		return;
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	int r = pon_update(PON_SOFT_RB_SPARE, 0xfc, (uint8_t)(PON_REASON_BOOTLOADER << 2));
	(void)pon_read(PON_SOFT_RB_SPARE, &v);
	ps("next reset: fastboot (PON 8F="); px(v, 2); pc(')'); pe(r ? RED : GREEN);
}

static uint64_t smc(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "x4", "x5", "x6", "x7", "x8", "x9", "x10",
	    "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

void board_reset_now(void)
{
	ps("reset: PS_HOLD"); pe(YELLOW);
	wr32(PS_HOLD, 0);
	delay_us(3000000);

	ps("still here; reset: PSCI SYSTEM_RESET"); pe(YELLOW);
	uint64_t r = smc(0x84000009UL);
	delay_us(2000000);
	ps("PSCI returned "); px(r, 8); pe(YELLOW);

	ps("reset: watchdog bite"); pe(YELLOW);
	wr32(WDT_BASE + WDT_EN, 1);
	wr32(WDT_BASE + WDT_BITE_TIME, 1);
	wr32(WDT_BASE + WDT_RST, 1);
	delay_us(5000000);

	ps("!! no reset worked: hold Power 7 s,"); pe(RED);
	ps("!! then Power + Volume Down 2 minutes"); pe(RED);
	for (;;)
		__asm__ volatile("wfe");
}

void board_halt(unsigned seconds)
{
	if (seconds == 0) {
		ps("no automatic reset: hold Power about 7 s"); pe(YELLOW);
		for (;;)
			__asm__ volatile("wfe");
	}
	int idx = pe(YELLOW);		/* an empty row for the countdown */
	for (unsigned left = seconds; left > 0; left--) {
		ps("auto reset in "); pd(left); ps(" s (or hold Power 7 s)");
		pset(idx, YELLOW);
		delay_us(1000000);
	}
	board_reset_now();
}

#endif /* !HOST */
