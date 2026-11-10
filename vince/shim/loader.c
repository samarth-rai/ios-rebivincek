/*
 * vince-m3b: act as iBoot for xnu on the Redmi 5 Plus (MSM8953).
 *
 * LK starts this image like a Linux kernel (EL1, MMU and caches off, x0 = DTB) and hands it the
 * Android boot image's ramdisk whole. In the ramdisk sits the payload built by tools/mkpayload.py:
 * the xnu Mach-O with its chained fixups already resolved, the Apple Device Tree and the boot
 * arguments string. This loader copies the segments to phys_base + (vmaddr - virt_base), puts
 * the ADT and boot_args behind them and jumps to the kernel entry point with x0 = boot_args
 * (physical), exactly the contract of iBoot. Everything runs with the MMU off.
 *
 * Structure and payload format follow ios-topaz (MIT), see NOTICE.md.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "board.h"	/* FB_BASE, PANEL_*, RAM_LO/HI, LK_RAMDISK_FALLBACK */
#include "reset.h"	/* Power-key reset, automatic reset of a stopped loader */


#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define MAGENTA	0xffff00ffU
#define RED	0xffff3030U

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL
#define PAYLOAD_SCAN		0x40000UL	/* bytes searched for the magic at the start of the ramdisk */

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* ---- line builder ---- */
static char lb[CON_MAXCOLS + 2];
static int lbn;
static const char *stage;	/* assigned at run time: a static initializer would need an absolute relocation */

static void pc(char c)
{
	if (lbn < CON_MAXCOLS)
		lb[lbn++] = c;
}

static void ps(const char *s)
{
	while (*s)
		pc(*s++);
}

static void px(uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = digits - 1; i >= 0; i--)
		pc(hex[(v >> (4 * i)) & 0xf]);
}

static int pe(uint32_t color)
{
	lb[lbn] = 0;
	lbn = 0;
	return con_line(lb, color);
}

#ifdef HOST
/* PC test (host_test_loader.c): the same loader code against mmap()ed "physical" memory */
extern void host_halt(void) __attribute__((noreturn));
extern void host_jump(uint64_t entry_pa, uint64_t ba_pa);
#define rsr(name) 0ULL
#define cpu_halt() host_halt()
#else
#define rsr(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })
#define cpu_halt() board_halt(HALT_RESET_S)	/* never just stop: the phone has no reset button */
#endif

void exc_handler(uint64_t slot, uint64_t *regs)
{
	const char *k;

	switch (slot & 3) {
	case 0: k = "SYNC"; break;
	case 1: k = "IRQ"; break;
	case 2: k = "FIQ"; break;
	default: k = "SERROR"; break;
	}
	ps("!! EXCEPTION "); ps(k); ps(" slot="); px(slot, 2); pe(MAGENTA);
	ps("!! stage: "); ps(stage); pe(MAGENTA);
	ps("ESR="); px(rsr("esr_el1"), 8); ps(" FAR="); px(rsr("far_el1"), 16); pe(MAGENTA);
	ps("ELR="); px(rsr("elr_el1"), 16); ps(" SPSR="); px(rsr("spsr_el1"), 8); pe(MAGENTA);
	ps("X0="); px(regs[0], 16); ps(" X1="); px(regs[1], 16); pe(MAGENTA);
	for (;;)
		cpu_halt();
}

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(uintptr_t a)
{
	static const char want[] = "SWEETXNU";
	volatile const uint8_t *m = (volatile const uint8_t *)a;
	for (int i = 0; i < 8; i++)
		if (m[i] != (uint8_t)want[i])
			return 0;
	return 1;
}

static int fdt_chosen_addr(const uint8_t *dtb, const char *prop, uint64_t *out)
{
	uint32_t len = 0;
	const uint8_t *p = fdt_prop(dtb, "/chosen", prop, &len);
	if (!p)
		return 0;
	if (len == 4) {
		*out = fdt_be32(p);
		return 1;
	}
	if (len == 8) {
		*out = ((uint64_t)fdt_be32(p) << 32) | fdt_be32(p + 4);
		return 1;
	}
	return 0;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

static void fail(const char *why)
{
	ps("!! "); ps(why); pe(RED);
	ps("   nothing was started; the phone is untouched"); pe(WHITE);
	for (;;)
		cpu_halt();
}

void loader_main(uint64_t dtb, uint64_t img)
{
	uint64_t rd_start = 0, rd_end = 0;

	stage = "start";
	con_init();
	con_add_target(FB_BASE, PANEL_W, PANEL_H, PANEL_STRIDE, PANEL_BPP);
	ps(BOARD_NAME " M3b: XNU loader"); pe(GREEN);
	ps("img="); px(img, 9); ps(" dtb="); px(dtb, 9);
	ps(" EL="); px((rsr("currentel") >> 2) & 3, 1);
	ps(" MPIDR="); px(rsr("mpidr_el1") & 0xffffff, 6); pe(WHITE);

	/* First of all make a long press of Power a hard reset; from here on the PMIC does that by itself. */
	stage = "reset setup";
	board_reset_prepare();

	stage = "find ramdisk";
	if (!fdt_chosen_addr((const uint8_t *)dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_addr((const uint8_t *)dtb, "linux,initrd-end", &rd_end)) {
		rd_start = LK_RAMDISK_FALLBACK;
		rd_end = rd_start + PAYLOAD_SCAN;
		ps("no initrd in /chosen, trying " LK_RAMDISK_TEXT); pe(YELLOW);
	}
	ps("ramdisk "); px(rd_start, 9); ps(" - "); px(rd_end, 9);
	ps(" ("); px((rd_end - rd_start) >> 10, 6); ps(" KiB)"); pe(WHITE);

	/* LK passes our ramdisk whole, so the payload starts at its first byte. Look a little further in case a header precedes it. */
	stage = "find payload";
	uintptr_t hit = 0;
	for (uintptr_t a = rd_start; a + 8 <= rd_end && a < rd_start + PAYLOAD_SCAN; a += 8)
		if (magic_ok(a)) {
			hit = a;
			break;
		}
	if (!hit)
		fail("no SWEETXNU payload at the start of the ramdisk");
	rd_start = hit;

	const struct payload *p = (const struct payload *)rd_start;
	if (p->version != 1 || (p->adt_off & 7))
		fail("bad payload header");

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	ps("phys "); px(p->phys_base, 9); ps(" size "); px(p->mem_size, 9); pe(WHITE);
	ps("virt "); px(p->virt_base, 16); pe(WHITE);
	ps("entry va "); px(p->entry_va, 16); ps(" segs "); px(p->nsegs, 2); pe(WHITE);

	stage = "check layout";
	if (p->phys_base < RAM_LO || p->phys_base + p->mem_size > RAM_HI || (p->phys_base & 0x1fffff))
		fail("kernel RAM outside " RAM_RANGE_TEXT " or not 2 MiB aligned");
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		fail("ramdisk overlaps kernel memory");
	if ((img + 0x10000) > p->phys_base && img < p->phys_base + p->mem_size)
		fail("loader image overlaps kernel memory");

	stage = "copy segments";
	ps("copying kernel..."); pe(YELLOW);
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		if (dst < p->phys_base || dst + s->vmsize > p->phys_base + p->mem_size)
			fail("segment outside kernel RAM");
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	ps("segments done, end pa "); px(p->kernel_end + pa_of_va_base, 9); pe(WHITE);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	stage = "boot_args";
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	if (adt_pa + p->adt_size + 2 * PAGE_16K > p->phys_base + p->mem_size)
		fail("no room for ADT and boot_args");
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	/*
	 * No boot video: xnu then uses its serial console path, which draws on the ADT node
	 * "sweet-fbcon" (24 bpp). Its early console (patch 0962) and the stage-0 marker use
	 * the fixed buffer 0x90001000.
	 */
	ba->Video.v_baseAddr = 0;
	ba->Video.v_display = 0;
	ba->Video.v_rowBytes = PANEL_STRIDE;
	ba->Video.v_width = PANEL_W;
	ba->Video.v_height = PANEL_H;
	ba->Video.v_depth = 0;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	ps("boot_args "); px(ba_pa, 9); ps(" top "); px(top, 9); pe(WHITE);
	ps("args: ");
	for (int i = 0; i < 52 && p->cmdline[i]; i++)
		pc(p->cmdline[i]);
	pe(WHITE);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	ps("JUMP "); px(entry_pa, 9); ps(" (xnu clears the screen)"); pe(GREEN);
#if defined(LOADER_PAUSE_S) && !defined(HOST)
	/*
	 * Diagnostic build only: keep the loader's lines on screen for LOADER_PAUSE_S seconds (generic
	 * timer; the iteration cap ends the wait if the counter does not run).
	 */
	{
		uint64_t f = rsr("cntfrq_el0");
		uint64_t end = rsr("cntvct_el0") + (f ? f : 19200000ULL) * LOADER_PAUSE_S;
		ps("(pause before the jump)"); pe(YELLOW);
		for (volatile uint64_t n = 0; n < 4000000000ULL && rsr("cntvct_el0") < end; n++)
			;
	}
#endif

	/* Quiet the timers LK may have left running, then drop stale I-cache lines (everything was written MMU/caches off). */
#ifdef HOST
	host_jump(entry_pa, ba_pa);
#else
	__asm__ volatile("msr cntv_ctl_el0, xzr; msr cntp_ctl_el0, xzr; isb" ::: "memory");
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
#endif
	fail("xnu returned");
}
