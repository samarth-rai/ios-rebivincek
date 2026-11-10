/*
 * "FAKE XNU": a stand-in kernel that checks everything the loader promises xnu, on the phone.
 * It prints what it finds on the splash buffer: the boot_args fields, the ADT, the command line,
 * the zero-filled tail of its own segment and a pattern the loader had to copy.
 */
#include <stdint.h>
#include "console.h"
#include "../shim/board.h"	/* FB_BASE, PANEL_* */

#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define RED	0xffff3030U
#define MAGENTA	0xffff00ffU
#define BOOT_LINE_LENGTH 608

struct boot_video { uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; };
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

/* data the Mach-O carries (copied by the loader) and a zero-filled tail (zeroed by the loader) */
volatile const uint64_t pattern[4] = { 0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0xa5a5a5a5a5a5a5a5ULL, 0x5a5a5a5a5a5a5a5aULL };
volatile uint64_t zero_probe[512];	/* .bss: part of vmsize, not of filesize */

static char lb[CON_MAXCOLS + 2];
static int lbn;
static void pc(char c) { if (lbn < CON_MAXCOLS) lb[lbn++] = c; }
static void ps(const char *s) { while (*s) pc(*s++); }
static void px(uint64_t v, int d) { static const char h[] = "0123456789ABCDEF"; for (int i = d - 1; i >= 0; i--) pc(h[(v >> (4 * i)) & 0xf]); }
static void pe(uint32_t c) { lb[lbn] = 0; lbn = 0; con_line(lb, c); }
#define rsr(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })

void exc_handler(uint64_t slot, uint64_t *regs)
{
	ps("FAKE XNU EXCEPTION slot="); px(slot, 2); pe(MAGENTA);
	ps("ESR="); px(rsr("esr_el1"), 8); ps(" FAR="); px(rsr("far_el1"), 16); pe(MAGENTA);
	ps("ELR="); px(rsr("elr_el1"), 16); ps(" X0="); px(regs[0], 16); pe(MAGENTA);
	for (;;)
		__asm__ volatile("wfe");
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void fake_main(uint64_t ba_pa, uint64_t head)
{
	const volatile struct boot_args *ba = (const volatile struct boot_args *)ba_pa;
	int bad = 0;

	con_init();
	con_add_target(FB_BASE, PANEL_W, PANEL_H, PANEL_STRIDE, PANEL_BPP);
	ps("FAKE XNU reached (loader test)"); pe(GREEN);
	ps("pc "); px(head, 9); ps(" EL "); px((rsr("currentel") >> 2) & 3, 1);
	ps(" SCTLR "); px(rsr("sctlr_el1"), 8); pe(WHITE);
	ps("MPIDR "); px(rsr("mpidr_el1") & 0xffffff, 6); ps(" CNTFRQ "); px(rsr("cntfrq_el0"), 8); pe(WHITE);
	ps("boot_args "); px(ba_pa, 9); ps(" rev "); px(ba->Revision, 1); ps("/"); px(ba->Version, 1); pe(WHITE);
	ps("virtBase "); px(ba->virtBase, 16); pe(WHITE);
	ps("physBase "); px(ba->physBase, 9); ps(" mem "); px(ba->memSize, 9); pe(WHITE);
	ps("topOfKernelData "); px(ba->topOfKernelData, 9); pe(WHITE);
	ps("video base "); px(ba->Video.v_baseAddr, 1); ps(" display "); px(ba->Video.v_display, 1); pe(WHITE);
	ps("args: ");
	for (int i = 0; i < 56 && ba->CommandLine[i]; i++)
		pc(ba->CommandLine[i]);
	pe(WHITE);

	/* our own place in memory must be inside what the ADT says is kernel RAM */
	if (head < ba->physBase || head >= ba->physBase + ba->memSize) { ps("!! pc outside kernel RAM"); pe(RED); bad++; }
	if (ba->topOfKernelData > ba->physBase + ba->memSize) { ps("!! top beyond RAM"); pe(RED); bad++; }

	/* copied data */
	if (pattern[0] != 0x0123456789abcdefULL || pattern[3] != 0x5a5a5a5a5a5a5a5aULL) { ps("!! copied pattern wrong"); pe(RED); bad++; }
	else { ps("copied data ok"); pe(GREEN); }
	/* zero-filled tail */
	uint64_t nz = 0;
	for (int i = 0; i < 512; i++)
		nz |= zero_probe[i];
	if (nz) { ps("!! zero-fill tail not zero"); pe(RED); bad++; }
	else { ps("zero-filled tail ok"); pe(GREEN); }

	/* ADT: deviceTreeP is a kernel virtual address of the real xnu; here pa = va - virtBase + physBase */
	uint64_t adt_pa = ba->deviceTreeP - ba->virtBase + ba->physBase;
	const uint8_t *adt = (const uint8_t *)adt_pa;
	ps("ADT "); px(adt_pa, 9); ps(" len "); px(ba->deviceTreeLength, 6);
	ps(" props "); px(le32(adt), 2); ps(" kids "); px(le32(adt + 4), 2); pe(WHITE);
	if (adt_pa < ba->physBase || adt_pa + ba->deviceTreeLength > ba->topOfKernelData || le32(adt) < 4 || le32(adt + 4) < 3) {
		ps("!! ADT looks wrong"); pe(RED); bad++;
	}
	/* first property name of the root node must be AAPL,phandle */
	{
		const char *n = (const char *)(adt + 8);
		ps("first prop: ");
		for (int i = 0; i < 32 && n[i]; i++)
			pc(n[i]);
		pe(WHITE);
	}
	if (bad) { ps("LOADER TEST FAILED"); pe(RED); }
	else { ps("LOADER TEST PASSED"); pe(GREEN); }
	for (;;)
		__asm__ volatile("wfe");
}
