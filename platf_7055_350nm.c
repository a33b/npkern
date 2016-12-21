/* platform-specific code that implements the reflashing back-end commands
 * (see platf.h)
 */

/* (c) copyright fenugrec 2016
 * GPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* General notes
 *
 * The 7055 datasheet is written in a slightly confusing style, and some
 * elements are not perfectly clear.
 * Renesas FDT includes sample kernel code that *should* work, but
 * in some respects does not follow the DS, adding to the confusion. The
 * FDT code is also handwritten assembly, commented in mostly-english, so
 * the intent is not always clear.
 *
 * Other code I've seen (Nissan kernel) is yet another interpretation of
 * the DS, and disagrees with both FDT code and the DS on some points.
 *
 * In here, I've attempted to follow the DS to the letter, referring to
 * FDT code for correctness and Nissan code for sanity.
 *
 **** Questionable points
 *
 * Use of WDT peripheral : DS and FDT code use it, Nissan doesn't.
 * I currently have it in here, but I may remove it since I don't know
 * if Nissan ECUs have the WDTOVF CPU pin to anything problematic.
 *
 * Computation of "additional programming data" : DS is unclear, Nissan
 * seems wrong, and FDT agrees with my reading of the DS.
 *
 * Delay loops : the most critical timing values are the "write pulse"
 * delays; for these I disable interrupts around the pulse so our ECU_WDT
 * interrupt doesn't interfere.
 */


/* I did attempt to port the Renesas FDT code to GNU as. Little/nothing to be gained
sh-elf-size with no implems: 3392	1024	536 => 4952
sh-elf-size with Erase asm implem: 4024	1024	552 => 5600, delta = 648B
sh-elf-size with Erase C implem: 3924	1024	548	=> 5496, delta = 544B
sh-elf-size with write + erase C implems: 4292	1024	548 => 5864, delta = 912B
*/


#include "functions.h"
#include "extra_functions.h"
#include "reg_defines/7055_350nm.h"	//io peripheral regs etc

#include <string.h>	//memcpy
#include "stypes.h"
#include "platf.h"
#include "iso_cmds.h"

/*********  Reflashing defines
 *
 * This is for SH7055 (0.35um) and assumes this RAM map (see .ld file)
 *
 * - stack @ 0xFFFF BFFC (growing downwards)
 * - kernel @ 0xFFFF 8100, this leaves ~16k for both kernel + stack
 */


#ifndef SH7055_35
#error Wrong target specified !
#endif

#define FL_MAXROM	(512*1024UL - 1UL)



/********** Timing defs
*/

//Assume 40MHz clock. Some critical timing depends on this being true,
//WDT stuff in particular isn't macro-fied
#define CPUFREQ	(40)

#define WDT_RSTCSR_SETTING 0x5A5F;	//power-on reset if TCNT overflows
#define WDT_TCSR_ESTART (0xA578 | 0x06)	//write value to start with 1:4096 div (26.2 ms @ 40MHz), for erase runaway
#define WDT_TCSR_WSTART (0xA578 | 0x05)	//write value to start with 1:1024 div (6.6 ms @ 40MHz), for write runaway
#define WDT_TCSR_STOP 0xA558	//write value to stop WDT count

//TODO recheck calculation
#define WAITN_TCYCLE 4		/* clock cycles per loop, see asm */
#define WAITN_CALCN(usec) (((usec) * CPUFREQ / WAITN_TCYCLE) + 1)


/** Common timing constants */
#define TSSWE	WAITN_CALCN(1)
#define TCSWE	WAITN_CALCN(100)

/** Erase timing constants */
#define TSESU	WAITN_CALCN(100)
#define TSE	WAITN_CALCN(10000UL)
#define TCE	WAITN_CALCN(10)
#define TCESU	WAITN_CALCN(10)
#define TSEV	WAITN_CALCN(6)	/******** Renesas has 20 for this !?? */
#define TSEVR	WAITN_CALCN(2)
#define TCEV	WAITN_CALCN(4)


/** Write timing constants */
#define TSPSU	WAITN_CALCN(50)
#define TSP10	WAITN_CALCN(10)
#define TSP30	WAITN_CALCN(30)
#define TSP200	WAITN_CALCN(200)
#define TCP	WAITN_CALCN(5)
#define TCPSU	WAITN_CALCN(5)
#define TSPV	WAITN_CALCN(4)
#define TSPVR	WAITN_CALCN(2)
#define TCPV	WAITN_CALCN(2)


/** FLASH constants */
#define MAX_ET	100		// The number of times of the maximum erase
#define MAX_WT	1000		// The number of times of the maximum writing
#define OW_COUNT	6		// The number of times of additional writing
#define BLK_MAX	16		// EB0..EB15
#define FLMCR1_MAXBLOCK 7	//EB0..7 controlled by FLMCR1


/** FLMCRx bit defines */
#define FLMCR_FWE	0x80
#define FLMCR_FLER	0x80
#define FLMCR_SWE	0x40
#define FLMCR_ESU	0x20
#define FLMCR_PSU	0x10
#define FLMCR_EV	0x08
#define FLMCR_PV	0x04
#define FLMCR_E	0x02
#define FLMCR_P	0x01


const u32 fblocks[] = {
	0x00000000,
	0x00001000,
	0x00002000,
	0x00003000,
	0x00004000,
	0x00005000,
	0x00006000,
	0x00007000,
	0x00008000,
	0x00010000,
	0x00020000,
	0x00030000,
	0x00040000,
	0x00050000,
	0x00060000,
	0x00070000,
	0x00080000		//last one just for delimiting the last block
};




static bool reflash_enabled = 0;	//global flag to protect flash, see platf_flash_enable()


static volatile u8 *pFLMCR;	//will point to FLMCR1 or FLMCR2 as required
static volatile u8 *pEBR;		//similar idea, for EBR1 / EBR2


/** spin for <loops> .
 * Constants should be calculated at compile-time.
 */
#define WAITN_TCYCLE	4	//clock cycles per loop

static void waitn(unsigned loops) {
	u32 tmp;
	asm volatile ("0: dt %0":"=r"(tmp):"0"(loops):"cc");
	asm volatile ("bf 0b");
}



/** Check FWE and FLER bits
 * ret 1 if ok
 */
static bool fwecheck(void) {
	if (!FLASH.FLMCR1.BIT.FWE) return 0;
	if (FLASH.FLMCR2.BIT.FLER) return 0;
	return 1;
}

/** Set SWE bit and wait */
static void sweset(void) {
	*pFLMCR |= FLMCR_SWE;
	waitn(TSSWE);
	return;
}

/** Clear SWE bit and wait */
static void sweclear(void) {
	*pFLMCR &= ~FLMCR_SWE;
	waitn(TCSWE);
}


/*********** Erase ***********/

/** Erase verification
 * Assumes pFLMCR is set, of course
 * ret 1 if ok
 */
static bool ferasevf(unsigned blockno) {
	bool rv = 1;
	volatile u32 *cur, *end;

	cur = (volatile u32 *) fblocks[blockno];
	end = (volatile u32 *) fblocks[blockno + 1];

	for (; cur < end; cur++) {
		*pFLMCR |= FLMCR_EV;
		waitn(TSEV);
		*cur = 0xFFFFFFFF;
		waitn(TSEVR);
		if (*cur != 0xFFFFFFFF) {
			rv = 0;
			break;
		}
	}
	*pFLMCR &= ~FLMCR_EV;
	waitn(TCEV);

	return rv;
}



/* pFLMCR and pEBR must be set;
 * blockno validated <= 15 of course
 */
static void ferase(unsigned blockno) {
	unsigned bitsel;

	bitsel = 1;
	blockno &= 0x07;	//keep 3 bits for EB0..7 or EB8..15
	while (blockno) {
		bitsel <<= 1;
		blockno -= 1;
	}
	*pEBR = bitsel;

	WDT.WRITE.TCSR = WDT_TCSR_STOP;	//this also clears TCNT
	WDT.WRITE.TCSR = WDT_TCSR_ESTART;

	*pFLMCR |= FLMCR_ESU;
	waitn(TSESU);
	*pFLMCR |= FLMCR_E;	//start Erase pulse
	waitn(TSE);
	*pFLMCR &= ~FLMCR_E;	//stop pulse
	waitn(TCE);
	*pFLMCR &= ~FLMCR_ESU;
	waitn(TCESU);

	WDT.WRITE.TCSR = WDT_TCSR_STOP;

	*pEBR = 0;

}



//same error codes as 180nm, for convenience maybe
#define PFEB_BADBLOCK (0x84 | 0x00)
#define PFEB_VERIFAIL (0x84 | 0x01)
uint32_t platf_flash_eb(unsigned blockno) {
	unsigned count;

	if (blockno >= BLK_MAX) return PFEB_BADBLOCK;
	if (!reflash_enabled) return 0;

	if (blockno > FLMCR1_MAXBLOCK) {
		pFLMCR = &FLASH.FLMCR1.BYTE;
		pEBR = &FLASH.EBR1.BYTE;
	} else {
		pFLMCR = &FLASH.FLMCR2.BYTE;
		pEBR = &FLASH.EBR2.BYTE;
	}

	if (!fwecheck()) {
		return -1;
	}

	sweset();
	WDT.WRITE.TCSR = WDT_TCSR_STOP;
	WDT.WRITE.RSTCSR = WDT_RSTCSR_SETTING;

	/* XXX is this pre-erase verify really necessary ?? DS doesn't say so;
	 * FDT example has this, and Nissan kernel doesn't
	 */
#if 0
	if (ferasevf(blockno)) {
		//already blank
		sweclear();
		return 0;
	}
#endif


	for (count = 0; count < MAX_ET; count++) {
		ferase(blockno);
		if (ferasevf(blockno)) {
			sweclear();
			return 0;
		}
	}
	/* haven't managed to get a succesful ferasevf() : badexit */
	sweclear();
	return -1;

}



/*********** Write ***********/

/** write pulse for tps=10/30/200us as specified
 */
static void writepulse(unsigned tsp) {
//	int prev_imask = get_imask();
//	set_imask(0x0F);
	unsigned uim = imask_savedisable();

	WDT.WRITE.TCSR = WDT_TCSR_STOP;
	WDT.WRITE.TCSR = WDT_TCSR_WSTART;

	*pFLMCR |= FLMCR_PSU;
	waitn(TSPSU);
	*pFLMCR |= FLMCR_P;
	waitn(tsp);
	*pFLMCR &= ~FLMCR_P;
	waitn(TCP);
	*pFLMCR &= !FLMCR_PSU;
	waitn(TCPSU);
	WDT.WRITE.TCSR = WDT_TCSR_STOP;

//	set_imask(prev_imask);
	imask_restore(uim);
}


/** ret 0 if ok,
 * assumes params are ok, and that block was already erased
 */
u32 flash_write128(u32 dest, u32 src) {
	u8 reprog[128] __attribute ((aligned (4)));	// retry / reprogram data
	u8 addit[128] __attribute ((aligned (4)));	// overwrite / additional data
	unsigned n;
	bool m;
	u32 rv;

	if (dest < fblocks[FLMCR1_MAXBLOCK + 1]) {
		pFLMCR = &FLASH.FLMCR1.BYTE;
	} else {
		pFLMCR = &FLASH.FLMCR2.BYTE;
	}

	if (!fwecheck()) {
		return -1;	//XXX
	}

	memcpy(reprog, (void *) src, 128);

	sweset();
	WDT.WRITE.TCSR = WDT_TCSR_STOP;
	WDT.WRITE.RSTCSR = WDT_RSTCSR_SETTING;

	n = 1;
	while (n < MAX_WT) {
		unsigned cur;

		m = 0;

		//1) write (latch) to flash, can't use memcpy because this must be u8 transfers
		for (cur = 0; cur < 128; cur++) {
			*(volatile u8 *) (dest + cur) = reprog[cur];
		}

		if (n <= OW_COUNT) {
			writepulse(TSP30);
		} else {
			writepulse(TSP200);
		}

		//2) Program verify
		*pFLMCR |= FLMCR_PV;
		waitn(TSPV);


		for (cur = 0; cur < 128; cur += 4) {
			u32 verifdata;
			u32 srcdata;

			*(volatile u32 *) (dest + cur) = (u32) -1;	//dummy write 0xFFFFFFFF
			waitn(TSPVR);

			verifdata = *(volatile u32 *) (dest + cur);
			srcdata = *(u32 *) (src + cur);

			if (verifdata != *(u32 *) (reprog + cur)) {
				//mismatch:
				m = 1;
			}

			if (n <= 6) {
				// compute "additional programming data"
				// The datasheet isn't very clear about this, and interpretations vary (Nissan kernel vs FDT example)
				// This follows what FDT does.
				* (u32 *) (addit + cur) = verifdata | (*(u32 *) (reprog + cur));

			}
			if (src & ~verifdata) {
				//wanted '1' bits, but somehow got '0's : serious error
				//XXXX
				rv = -1;
				goto badexit;
			}
			//compute reprogramming data. This fits with my reading of both the DS and the FDT code,
			//but Nissan proceeds differently
            * (u32 *) (reprog + cur) = srcdata | ~verifdata;
		}

		*pFLMCR &= ~FLMCR_PV;
		waitn(TCPV);

		if (n <= 6) {
			// write additional repro data; can't use memcpy because this must be u8 transfers
			for (cur = 0; cur < 128; cur++) {
				*(volatile u8 *) (dest + cur) = addit[cur];
			}

			writepulse(TSP10);
		}


	}	//while n < 1000

	if (!m) {
		//success
		sweclear();
		return 0;
	}

	//XXX failure (max retries or something)
	rv = -1;
badexit:
	sweclear();
	return rv;
}


//defined like 180nm code
#define PFWB_OOB (0x88 | 0x00)		//dest out of bounds
#define PFWB_MISALIGNED (0x88 | 0x01)	//dest not on 128B boundary
#define PFWB_LEN (0x88 | 0x02)		//len not multiple of 128
#define PFWB_VERIFAIL (0x88 | 0x03)	//post-write verify failed

uint32_t platf_flash_wb(uint32_t dest, uint32_t src, uint32_t len) {

	if (dest > FL_MAXROM) return PFWB_OOB;
	if (dest & 0x7F) return PFWB_MISALIGNED;	//dest not aligned on 128B boundary
	if (len & 0x7F) return PFWB_LEN;	//must be multiple of 128B too

	while (len) {
		uint32_t rv = 0;

		if (reflash_enabled) {
			rv = flash_write128(dest, src);
		}
		if (rv) {
			return rv;	//TODO : tweak into valid NRC
		}

		dest += 128;
		src += 128;
		len -= 128;
	}
	return 0;
}


/*********** init, unprotect ***********/

//TODO : give sane values to these & other errors
#define FL_ERROR 0x80

bool platf_flash_init(u8 *err) {
	reflash_enabled = 0;

	//Check FLER
	if (!fwecheck()) {
		*err = FL_ERROR;
		return 0;
	}

	/* suxxess ! */
	return 1;

}


void platf_flash_unprotect(void) {
	reflash_enabled = 1;
}
