/*
 * ita2_btb.c - example of how use the BTB with the Itanium 2 PMU
 *
 * Copyright (c) 2003-2006 Hewlett-Packard Development Company, L.P.
 * Contributed by Stephane Eranian <eranian@hpl.hp.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of libpfm, a performance monitoring support library for
 * applications on Linux.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <perfmon/perfmon.h>
#include <perfmon/perfmon_dfl_smpl.h>
#include <perfmon/pfmlib_itanium2.h>

typedef pfm_dfl_smpl_hdr_t	btb_hdr_t;
typedef pfm_dfl_smpl_entry_t	btb_entry_t;
typedef pfm_dfl_smpl_arg_t	smpl_arg_t;

#define NUM_PMCS PFMLIB_MAX_PMCS
#define NUM_PMDS PFMLIB_MAX_PMDS

#define MAX_EVT_NAME_LEN	128
#define MAX_PMU_NAME_LEN	32

/*
 * The BRANCH_EVENT is increment by 1 for each branch event. Such event is composed of
 * two entries in the BTB: a source and a target entry. The BTB is full after 4 branch
 * events.
 */
#define SMPL_PERIOD	(4UL*256)

static void *smpl_vaddr;
static unsigned int entry_size;
static int id;

#define BPL (sizeof(uint64_t)<<3)
#define LBPL	6

static inline void pfm_bv_set(uint64_t *bv, uint16_t rnum)
{
	bv[rnum>>LBPL] |= 1UL << (rnum&(BPL-1));
}

/*
 * we don't use static to make sure the compiler does not inline the function
 */
long func1(void) { return 0;}

long
do_test(unsigned long loop)
{
	long sum  = 0;

	while(loop--) {
		if (loop & 0x1)
			sum += func1();
		else
			sum += loop;
	}
	return sum;
}


static void fatal_error(char *fmt,...) __attribute__((noreturn));

static void
fatal_error(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(1);
}

/*
 * print content of sampling buffer
 *
 * XXX: using stdio to print from a signal handler is not safe with multi-threaded
 * applications
 */
#define safe_printf	printf
static void
show_btb_reg(int j, pfm_ita2_pmd_reg_t reg, pfm_ita2_pmd_reg_t pmd16)
{
	unsigned long bruflush, b1;
	int is_valid = reg.pmd8_15_ita2_reg.btb_b == 0 && reg.pmd8_15_ita2_reg.btb_mp == 0 ? 0 :1;

	b1       = (pmd16.pmd_val >> (4 + 4*(j-8))) & 0x1;
	bruflush = (pmd16.pmd_val >> (5 + 4*(j-8))) & 0x1;

	safe_printf("\tPMD%-2d: 0x%016lx b=%d mp=%d bru=%ld b1=%ld valid=%c\n",
		j,
		reg.pmd_val,
		 reg.pmd8_15_ita2_reg.btb_b,
		 reg.pmd8_15_ita2_reg.btb_mp,
		 bruflush, b1,
		is_valid ? 'Y' : 'N');

	if (!is_valid) return;

	if (reg.pmd8_15_ita2_reg.btb_b) {
		unsigned long addr;

		
		addr = (reg.pmd8_15_ita2_reg.btb_addr+b1)<<4;

		addr |= reg.pmd8_15_ita2_reg.btb_slot < 3 ?  reg.pmd8_15_ita2_reg.btb_slot : 0;

		safe_printf("\t       Source Address: 0x%016lx\n"
			    "\t       Taken=%c Prediction: %s\n\n",
			 addr,
			 reg.pmd8_15_ita2_reg.btb_slot < 3 ? 'Y' : 'N',
			 reg.pmd8_15_ita2_reg.btb_mp ? "FE Failure" :
			 bruflush ? "BE Failure" : "Success");
	} else {
		safe_printf("\t       Target Address: 0x%016lx\n\n",
			 (unsigned long)(reg.pmd8_15_ita2_reg.btb_addr<<4));
	}
}


static void
show_btb(pfm_ita2_pmd_reg_t *btb, pfm_ita2_pmd_reg_t *pmd16)
{
	int i, last;


	i    = (pmd16->pmd16_ita2_reg.btbi_full) ? pmd16->pmd16_ita2_reg.btbi_bbi : 0;
	last = pmd16->pmd16_ita2_reg.btbi_bbi;

	safe_printf("btb_trace: i=%d last=%d bbi=%d full=%d\n", i, last,pmd16->pmd16_ita2_reg.btbi_bbi, pmd16->pmd16_ita2_reg.btbi_full);
	do {
		show_btb_reg(i+8, btb[i], *pmd16);
		i = (i+1) % 8;
	} while (i != last);
}


void
process_smpl_buffer(void)
{
	btb_hdr_t	*hdr;
	btb_entry_t	*ent;
	unsigned long pos;
	unsigned long smpl_entry = 0;
	pfm_ita2_pmd_reg_t *reg, *pmd16;
	unsigned long i;
	int ret;
	static unsigned long last_ovfl = ~0UL;


	hdr = (btb_hdr_t *)smpl_vaddr;

	/*
	 * check that we are not diplaying the previous set of samples again.
	 * Required to take care of the last batch of samples.
	 */
	if (hdr->hdr_overflows <= last_ovfl && last_ovfl != ~0UL) {
		printf("skipping identical set of samples %lu <= %lu\n", hdr->hdr_overflows, last_ovfl);
		return;
	}

	pos = (unsigned long)(hdr+1);
	/*
	 * walk through all the entries recored in the buffer
	 */
	for(i=0; i < hdr->hdr_count; i++) {

		ret = 0;

		ent = (btb_entry_t *)pos;
		/*
		 * print entry header
		 */
		safe_printf("Entry %ld PID:%d TID:%d CPU:%d STAMP:0x%lx IIP:0x%016lx\n",
			smpl_entry++,
			ent->tgid,
			ent->pid,
			ent->cpu,
			ent->tstamp,
			ent->ip);

		/*
		 * point to first recorded register (always contiguous with entry header)
		 */
		reg = (pfm_ita2_pmd_reg_t*)(ent+1);

		/*
		 * in this particular example, we have pmd8-pmd15 has the BTB. We have also
		 * included pmd16 (BTB index) has part of the registers to record. This trick
		 * allows us to get the index to decode the sequential order of the BTB.
		 *
		 * Recorded registers are always recorded in increasing order. So we know
		 * that pmd16 is at a fixed offset (+8*sizeof(unsigned long)) from pmd8.
		 */
		pmd16 = reg+8;
		show_btb(reg, pmd16);

		/*
		 * move to next entry
		 */
		pos += entry_size;
	}
}

static void
overflow_handler(int n, struct siginfo *info, struct sigcontext *sc)
{
	/* dangerous */
	printf("Notification received\n");

	process_smpl_buffer();

	/*
	 * And resume monitoring
	 */
	if (pfm_set_state(id, 0, PFM_ST_RESTART) == -1) {
		perror("pfm_set_state_session(restart)");
		exit(1);
	}
}


int
main(void)
{
	int ret;
	int type = 0;
	pfarg_pmr_t pc[NUM_PMCS];
	pfarg_pmd_attr_t pd[NUM_PMDS];
	pfmlib_input_param_t inp;
	pfmlib_output_param_t outp;
	pfmlib_ita2_input_param_t ita2_inp;
	smpl_arg_t buf_arg;
	pfmlib_options_t pfmlib_options;
	struct sigaction act;
	unsigned int i;

	/*
	 * Initialize pfm library (required before we can use it)
	 */
	ret = pfm_initialize();
	if (ret != PFMLIB_SUCCESS)
		fatal_error("Cannot initialize library: %s\n", pfm_strerror(ret));

	/*
	 * Let's make sure we run this on the right CPU
	 */
	pfm_get_pmu_type(&type);
	if (type != PFMLIB_ITANIUM2_PMU) {
		char model[MAX_PMU_NAME_LEN];
		pfm_get_pmu_name(model, MAX_PMU_NAME_LEN);
		fatal_error("this program does not work with %s PMU\n", model);
	}

	/*
	 * Install the overflow handler (SIGIO)
	 */
	memset(&act, 0, sizeof(act));
	act.sa_handler = (sig_t)overflow_handler;
	sigaction (SIGIO, &act, 0);


	/*
	 * pass options to library (optional)
	 */
	memset(&pfmlib_options, 0, sizeof(pfmlib_options));
	pfmlib_options.pfm_debug = 0; /* set to 1 for debug */
	pfmlib_options.pfm_verbose = 1; /* set to 1 for debug */
	pfm_set_options(&pfmlib_options);

	memset(pd, 0, sizeof(pd));
	memset(pc, 0, sizeof(pc));
	memset(&buf_arg, 0, sizeof(buf_arg));

	/*
	 * prepare parameters to library. we don't use any Itanium
	 * specific features here. so the pfp_model is NULL.
	 */
	memset(&inp,0, sizeof(inp));
	memset(&outp,0, sizeof(outp));
	memset(&ita2_inp,0, sizeof(ita2_inp));

	/*
	 * Before calling pfm_find_dispatch(), we must specify what kind
	 * of branches we want to capture. We are interested in all the
	 * mispredicted (target, taken/not taken) branches, therefore we
	 * program the various fields of the BTB config to:
	 */
	ita2_inp.pfp_ita2_btb.btb_used = 1;

	ita2_inp.pfp_ita2_btb.btb_ds  = 0;   /* capture target */
	ita2_inp.pfp_ita2_btb.btb_tm  = 0x3; /* all branches */
	ita2_inp.pfp_ita2_btb.btb_ptm = 0x1; /* target mispredicted */
	ita2_inp.pfp_ita2_btb.btb_ppm = 0x1; /* mispredicted path */
	ita2_inp.pfp_ita2_btb.btb_brt = 0x0; /* all types captured */
	ita2_inp.pfp_ita2_btb.btb_plm = PFM_PLM3;

	if (pfm_find_full_event("BRANCH_EVENT", &inp.pfp_events[0]) != PFMLIB_SUCCESS)
		fatal_error("cannot find event BRANCH_EVENT\n");
	/*
	 * set the (global) privilege mode:
	 * 	PFM_PLM3 : user level only
	 */
	inp.pfp_dfl_plm   = PFM_PLM3;
	/*
	 * how many counters we use
	 */
	inp.pfp_event_count = 1;

	/*
	 * let the library figure out the values for the PMCS
	 */
	if ((ret=pfm_dispatch_events(&inp, &ita2_inp, &outp, NULL)) != PFMLIB_SUCCESS)
		fatal_error("cannot configure events: %s\n", pfm_strerror(ret));

	/*
	 * the size of the buffer is indicated in bytes (not entries).
	 *
	 * The kernel will record into the buffer up to a certain point.
	 * No partial samples are ever recorded.
	 */
	buf_arg.buf_size = getpagesize();


	/*
	 * now create the session
	 */
	id = pfm_create(PFM_FL_SMPL_FMT, NULL, "default", &buf_arg, sizeof(buf_arg));
	if (id == -1) {
		if (errno == ENOSYS) {
			fatal_error("Your kernel does not have performance monitoring support!\n");
		}
		fatal_error("cannot create session %s\n", strerror(errno));
	}

	/*
	 * retrieve the virtual address at which the sampling
	 * buffer has been mapped
	 */
	smpl_vaddr = mmap(NULL, (size_t)buf_arg.buf_size, PROT_READ, MAP_PRIVATE, id, 0);
	if (smpl_vaddr == MAP_FAILED)
		fatal_error("cannot mmap sampling buffer errno %d\n", errno);

	printf("Sampling buffer mapped at %p\n", smpl_vaddr);

	/*
	 * Now prepare the argument to initialize the PMDs and PMCS.
	 * We must pfp_pmc_count to determine the number of PMC to intialize.
	 * We must use pfp_event_count to determine the number of PMD to initialize.
	 * Some events cause extra PMCs to be used, so  pfp_pmc_count may be >= pfp_event_count.
	 *
	 * This step is new compared to libpfm-2.x. It is necessary because the library no
	 * longer knows about the kernel data structures.
	 */

	for (i=0; i < outp.pfp_pmc_count; i++) {
		pc[i].reg_num   = outp.pfp_pmcs[i].reg_num;
		pc[i].reg_value = outp.pfp_pmcs[i].reg_value;
	}

	/*
	 * figure out pmd mapping from output pmc
	 * PMD16 is part of the set of used PMD returned by libpfm.
	 * It will be reset automatically
	 */
	for (i=0; i < outp.pfp_pmd_count; i++)
		pd[i].reg_num   = outp.pfp_pmds[i].reg_num;

	/*
	 * indicate we want notification when buffer is full
	 */
	pd[0].reg_flags |= PFM_REGFL_OVFL_NOTIFY;

	/*
	 * Now prepare the argument to initialize the PMD and the sampling period
	 * We know we use only one PMD in this case, therefore pmd[0] corresponds
	 * to our first event which is our sampling period.
	 */
	pd[0].reg_value       = - SMPL_PERIOD;

	pd[0].reg_long_reset  = - SMPL_PERIOD;
	pd[0].reg_short_reset = - SMPL_PERIOD;

	pfm_bv_set(pd[0].reg_smpl_pmds, 16);

	entry_size = sizeof(btb_entry_t) + 1 * 8;

	for(i=8; i < 16; i++) {
		pfm_bv_set(pd[0].reg_smpl_pmds, i);
		entry_size += 8;
	}

	/*
	 * When our counter overflows, we want to BTB index to be reset, so that we keep
	 * in sync. This is required to make it possible to interpret pmd16 on overflow
	 * to avoid repeating the same branch several times.
	 */
	pfm_bv_set(pd[0].reg_reset_pmds, 16);

		/*
	 * Now program the registers
	 */
	if (pfm_write(id, 0, PFM_RW_PMC, pc, outp.pfp_pmc_count * sizeof(*pc)) == -1)
		fatal_error("pfm_write error errno %d\n",errno);

	if (pfm_write(id, 0, PFM_RW_PMD_ATTR, pd, outp.pfp_pmd_count * sizeof(*pd)) == -1)
		fatal_error("pfm_write(PMD) error errno %d\n",errno);

	/*
	 * now attach session
	 */
	if (pfm_attach(id, 0, getpid()) == -1)
		fatal_error("pfm_attach error errno %d\n",errno);

	/*
	 * setup asynchronous notification on the file descriptor
	 */
	ret = fcntl(id, F_SETFL, fcntl(id, F_GETFL, 0) | O_ASYNC);
	if (ret == -1)
		fatal_error("cannot set ASYNC: %s\n", strerror(errno));

	/*
	 * get ownership of the descriptor
	 */
	ret = fcntl(id, F_SETOWN, getpid());
	if (ret == -1)
		fatal_error("cannot setown: %s\n", strerror(errno));

	/*
	 * Let's roll now.
	 */
	if (pfm_set_state(id, 0, PFM_ST_START))
		fatal_error("pfm_set_state error errno %d\n",errno);

	do_test(100000);

	if (pfm_set_state(id, 0, PFM_ST_STOP))
		fatal_error("pfm_set_state error errno %d\n",errno);

	/*
	 * We must call the processing routine to cover the last entries recorded
	 * in the sampling buffer. Note that the buffer may not be full at this point.
	 *
	 */

	process_smpl_buffer();

	/*
	 * let's stop this now
	 */
	munmap(smpl_vaddr, (size_t)buf_arg.buf_size);
	close(id);

	return 0;
}
