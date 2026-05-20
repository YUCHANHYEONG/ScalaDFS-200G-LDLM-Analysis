// SPDX-License-Identifier: GPL-2.0+
/*
 * Read-Copy Update module-based scalability-test facility
 *
 * Copyright (C) IBM Corporation, 2015
 *
 * Authors: Paul E. McKenney <paulmck@linux.ibm.com>
 */

#define pr_fmt(fmt) fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/pcupdate.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/freezer.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/stat.h>
#include <linux/spcu.h>
#include <linux/slab.h>
#include <asm/byteorder.h>
#include <linux/torture.h>
#include <linux/vmalloc.h>
#include <linux/pcupdate_trace.h>

#include "pcu.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paul E. McKenney <paulmck@linux.ibm.com>");

#define SCALE_FLAG "-scale:"
#define SCALEOUT_STRING(s) \
	pr_alert("%s" SCALE_FLAG " %s\n", scale_type, s)
#define VERBOSE_SCALEOUT_STRING(s) \
	do { if (verbose) pr_alert("%s" SCALE_FLAG " %s\n", scale_type, s); } while (0)
#define VERBOSE_SCALEOUT_ERRSTRING(s) \
	do { if (verbose) pr_alert("%s" SCALE_FLAG "!!! %s\n", scale_type, s); } while (0)

/*
 * The intended use cases for the nreaders and nwriters module parameters
 * are as follows:
 *
 * 1.	Specify only the nr_cpus kernel boot parameter.  This will
 *	set both nreaders and nwriters to the value specified by
 *	nr_cpus for a mixed reader/writer test.
 *
 * 2.	Specify the nr_cpus kernel boot parameter, but set
 *	pcuscale.nreaders to zero.  This will set nwriters to the
 *	value specified by nr_cpus for an update-only test.
 *
 * 3.	Specify the nr_cpus kernel boot parameter, but set
 *	pcuscale.nwriters to zero.  This will set nreaders to the
 *	value specified by nr_cpus for a read-only test.
 *
 * Various other use cases may of course be specified.
 *
 * Note that this test's readers are intended only as a test load for
 * the writers.  The reader scalability statistics will be overly
 * pessimistic due to the per-critical-section interrupt disabling,
 * test-end checks, and the pair of calls through pointers.
 */

#ifdef MODULE
# define PCUSCALE_SHUTDOWN 0
#else
# define PCUSCALE_SHUTDOWN 1
#endif

torture_param(bool, gp_async, false, "Use asynchronous GP wait primitives");
torture_param(int, gp_async_max, 1000, "Max # outstanding waits per reader");
torture_param(bool, gp_exp, false, "Use expedited GP wait primitives");
torture_param(int, holdoff, 10, "Holdoff time before test start (s)");
torture_param(int, nreaders, -1, "Number of PCU reader threads");
torture_param(int, nwriters, -1, "Number of PCU updater threads");
torture_param(bool, shutdown, PCUSCALE_SHUTDOWN,
	      "Shutdown at end of scalability tests.");
torture_param(int, verbose, 1, "Enable verbose debugging printk()s");
torture_param(int, writer_holdoff, 0, "Holdoff (us) between GPs, zero to disable");
torture_param(int, kfree_pcu_test, 0, "Do we run a kfree_pcu() scale test?");
torture_param(int, kfree_mult, 1, "Multiple of kfree_obj size to allocate.");

static char *scale_type = "pcu";
module_param(scale_type, charp, 0444);
MODULE_PARM_DESC(scale_type, "Type of PCU to scalability-test (pcu, spcu, ...)");

static int nrealreaders;
static int nrealwriters;
static struct task_struct **writer_tasks;
static struct task_struct **reader_tasks;
static struct task_struct *shutdown_task;

static u64 **writer_durations;
static int *writer_n_durations;
static atomic_t n_pcu_scale_reader_started;
static atomic_t n_pcu_scale_writer_started;
static atomic_t n_pcu_scale_writer_finished;
static wait_queue_head_t shutdown_wq;
static u64 t_pcu_scale_writer_started;
static u64 t_pcu_scale_writer_finished;
static unsigned long b_pcu_gp_test_started;
static unsigned long b_pcu_gp_test_finished;
static DEFINE_PER_CPU(atomic_t, n_async_inflight);

#define MAX_MEAS 10000
#define MIN_MEAS 100

/*
 * Operations vector for selecting different types of tests.
 */

struct pcu_scale_ops {
	int ptype;
	void (*init)(void);
	void (*cleanup)(void);
	int (*readlock)(void);
	void (*readunlock)(int idx);
	unsigned long (*get_gp_seq)(void);
	unsigned long (*gp_diff)(unsigned long new, unsigned long old);
	unsigned long (*exp_completed)(void);
	void (*async)(struct pcu_head *head, pcu_callback_t func);
	void (*gp_barrier)(void);
	void (*sync)(void);
	void (*exp_sync)(void);
	const char *name;
};

static struct pcu_scale_ops *cur_ops;

/*
 * Definitions for pcu scalability testing.
 */

static int pcu_scale_read_lock(void) __acquires(PCU)
{
	pcu_read_lock();
	return 0;
}

static void pcu_scale_read_unlock(int idx) __releases(PCU)
{
	pcu_read_unlock();
}

static unsigned long __maybe_unused pcu_no_completed(void)
{
	return 0;
}

static void pcu_sync_scale_init(void)
{
}

static struct pcu_scale_ops pcu_ops = {
	.ptype		= PCU_FLAVOR,
	.init		= pcu_sync_scale_init,
	.readlock	= pcu_scale_read_lock,
	.readunlock	= pcu_scale_read_unlock,
	.get_gp_seq	= pcu_get_gp_seq,
	.gp_diff	= pcu_seq_diff,
	.exp_completed	= pcu_exp_batches_completed,
	.async		= call_pcu,
	.gp_barrier	= pcu_barrier,
	.sync		= synchronize_pcu,
	.exp_sync	= synchronize_pcu_expedited,
	.name		= "pcu"
};

/*
 * Definitions for spcu scalability testing.
 */

DEFINE_STATIC_SPCU(spcu_ctl_scale);
static struct spcu_struct *spcu_ctlp = &spcu_ctl_scale;

static int spcu_scale_read_lock(void) __acquires(spcu_ctlp)
{
	return spcu_read_lock(spcu_ctlp);
}

static void spcu_scale_read_unlock(int idx) __releases(spcu_ctlp)
{
	spcu_read_unlock(spcu_ctlp, idx);
}

static unsigned long spcu_scale_completed(void)
{
	return spcu_batches_completed(spcu_ctlp);
}

static void spcu_call_pcu(struct pcu_head *head, pcu_callback_t func)
{
	call_spcu(spcu_ctlp, head, func);
}

static void spcu_pcu_barrier(void)
{
	spcu_barrier(spcu_ctlp);
}

static void spcu_scale_synchronize(void)
{
	synchronize_spcu(spcu_ctlp);
}

static void spcu_scale_synchronize_expedited(void)
{
	synchronize_spcu_expedited(spcu_ctlp);
}

static struct pcu_scale_ops spcu_ops = {
	.ptype		= SPCU_FLAVOR,
	.init		= pcu_sync_scale_init,
	.readlock	= spcu_scale_read_lock,
	.readunlock	= spcu_scale_read_unlock,
	.get_gp_seq	= spcu_scale_completed,
	.gp_diff	= pcu_seq_diff,
	.exp_completed	= spcu_scale_completed,
	.async		= spcu_call_pcu,
	.gp_barrier	= spcu_pcu_barrier,
	.sync		= spcu_scale_synchronize,
	.exp_sync	= spcu_scale_synchronize_expedited,
	.name		= "spcu"
};

static struct spcu_struct spcud;

static void spcu_sync_scale_init(void)
{
	spcu_ctlp = &spcud;
	init_spcu_struct(spcu_ctlp);
}

static void spcu_sync_scale_cleanup(void)
{
	cleanup_spcu_struct(spcu_ctlp);
}

static struct pcu_scale_ops spcud_ops = {
	.ptype		= SPCU_FLAVOR,
	.init		= spcu_sync_scale_init,
	.cleanup	= spcu_sync_scale_cleanup,
	.readlock	= spcu_scale_read_lock,
	.readunlock	= spcu_scale_read_unlock,
	.get_gp_seq	= spcu_scale_completed,
	.gp_diff	= pcu_seq_diff,
	.exp_completed	= spcu_scale_completed,
	.async		= spcu_call_pcu,
	.gp_barrier	= spcu_pcu_barrier,
	.sync		= spcu_scale_synchronize,
	.exp_sync	= spcu_scale_synchronize_expedited,
	.name		= "spcud"
};

/*
 * Definitions for PCU-tasks scalability testing.
 */

static int tasks_scale_read_lock(void)
{
	return 0;
}

static void tasks_scale_read_unlock(int idx)
{
}

static struct pcu_scale_ops tasks_ops = {
	.ptype		= PCU_TASKS_FLAVOR,
	.init		= pcu_sync_scale_init,
	.readlock	= tasks_scale_read_lock,
	.readunlock	= tasks_scale_read_unlock,
	.get_gp_seq	= pcu_no_completed,
	.gp_diff	= pcu_seq_diff,
	.async		= call_pcu_tasks,
	.gp_barrier	= pcu_barrier_tasks,
	.sync		= synchronize_pcu_tasks,
	.exp_sync	= synchronize_pcu_tasks,
	.name		= "tasks"
};

/*
 * Definitions for PCU-tasks-trace scalability testing.
 */

static int tasks_trace_scale_read_lock(void)
{
	pcu_read_lock_trace();
	return 0;
}

static void tasks_trace_scale_read_unlock(int idx)
{
	pcu_read_unlock_trace();
}

static struct pcu_scale_ops tasks_tracing_ops = {
	.ptype		= PCU_TASKS_FLAVOR,
	.init		= pcu_sync_scale_init,
	.readlock	= tasks_trace_scale_read_lock,
	.readunlock	= tasks_trace_scale_read_unlock,
	.get_gp_seq	= pcu_no_completed,
	.gp_diff	= pcu_seq_diff,
	.async		= call_pcu_tasks_trace,
	.gp_barrier	= pcu_barrier_tasks_trace,
	.sync		= synchronize_pcu_tasks_trace,
	.exp_sync	= synchronize_pcu_tasks_trace,
	.name		= "tasks-tracing"
};

static unsigned long pcuscale_seq_diff(unsigned long new, unsigned long old)
{
	if (!cur_ops->gp_diff)
		return new - old;
	return cur_ops->gp_diff(new, old);
}

/*
 * If scalability tests complete, wait for shutdown to commence.
 */
static void pcu_scale_wait_shutdown(void)
{
	cond_resched_tasks_pcu_qs();
	if (atomic_read(&n_pcu_scale_writer_finished) < nrealwriters)
		return;
	while (!torture_must_stop())
		schedule_timeout_uninterruptible(1);
}

/*
 * PCU scalability reader kthread.  Repeatedly does empty PCU read-side
 * critical section, minimizing update-side interference.  However, the
 * point of this test is not to evaluate reader scalability, but instead
 * to serve as a test load for update-side scalability testing.
 */
static int
pcu_scale_reader(void *arg)
{
	unsigned long flags;
	int idx;
	long me = (long)arg;

	VERBOSE_SCALEOUT_STRING("pcu_scale_reader task started");
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	set_user_nice(current, MAX_NICE);
	atomic_inc(&n_pcu_scale_reader_started);

	do {
		local_irq_save(flags);
		idx = cur_ops->readlock();
		cur_ops->readunlock(idx);
		local_irq_restore(flags);
		pcu_scale_wait_shutdown();
	} while (!torture_must_stop());
	torture_kthread_stopping("pcu_scale_reader");
	return 0;
}

/*
 * Callback function for asynchronous grace periods from pcu_scale_writer().
 */
static void pcu_scale_async_cb(struct pcu_head *rhp)
{
	atomic_dec(this_cpu_ptr(&n_async_inflight));
	kfree(rhp);
}

/*
 * PCU scale writer kthread.  Repeatedly does a grace period.
 */
static int
pcu_scale_writer(void *arg)
{
	int i = 0;
	int i_max;
	long me = (long)arg;
	struct pcu_head *rhp = NULL;
	bool started = false, done = false, alldone = false;
	u64 t;
	u64 *wdp;
	u64 *wdpp = writer_durations[me];

	VERBOSE_SCALEOUT_STRING("pcu_scale_writer task started");
	WARN_ON(!wdpp);
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	sched_set_fifo_low(current);

	if (holdoff)
		schedule_timeout_uninterruptible(holdoff * HZ);

	/*
	 * Wait until pcu_end_inkernel_boot() is called for normal GP tests
	 * so that PCU is not always expedited for normal GP tests.
	 * The system_state test is approximate, but works well in practice.
	 */
	while (!gp_exp && system_state != SYSTEM_RUNNING)
		schedule_timeout_uninterruptible(1);

	t = ktime_get_mono_fast_ns();
	if (atomic_inc_return(&n_pcu_scale_writer_started) >= nrealwriters) {
		t_pcu_scale_writer_started = t;
		if (gp_exp) {
			b_pcu_gp_test_started =
				cur_ops->exp_completed() / 2;
		} else {
			b_pcu_gp_test_started = cur_ops->get_gp_seq();
		}
	}

	do {
		if (writer_holdoff)
			udelay(writer_holdoff);
		wdp = &wdpp[i];
		*wdp = ktime_get_mono_fast_ns();
		if (gp_async) {
retry:
			if (!rhp)
				rhp = kmalloc(sizeof(*rhp), GFP_KERNEL);
			if (rhp && atomic_read(this_cpu_ptr(&n_async_inflight)) < gp_async_max) {
				atomic_inc(this_cpu_ptr(&n_async_inflight));
				cur_ops->async(rhp, pcu_scale_async_cb);
				rhp = NULL;
			} else if (!kthread_should_stop()) {
				cur_ops->gp_barrier();
				goto retry;
			} else {
				kfree(rhp); /* Because we are stopping. */
			}
		} else if (gp_exp) {
			cur_ops->exp_sync();
		} else {
			cur_ops->sync();
		}
		t = ktime_get_mono_fast_ns();
		*wdp = t - *wdp;
		i_max = i;
		if (!started &&
		    atomic_read(&n_pcu_scale_writer_started) >= nrealwriters)
			started = true;
		if (!done && i >= MIN_MEAS) {
			done = true;
			sched_set_normal(current, 0);
			pr_alert("%s%s pcu_scale_writer %ld has %d measurements\n",
				 scale_type, SCALE_FLAG, me, MIN_MEAS);
			if (atomic_inc_return(&n_pcu_scale_writer_finished) >=
			    nrealwriters) {
				schedule_timeout_interruptible(10);
				pcu_ftrace_dump(DUMP_ALL);
				SCALEOUT_STRING("Test complete");
				t_pcu_scale_writer_finished = t;
				if (gp_exp) {
					b_pcu_gp_test_finished =
						cur_ops->exp_completed() / 2;
				} else {
					b_pcu_gp_test_finished =
						cur_ops->get_gp_seq();
				}
				if (shutdown) {
					smp_mb(); /* Assign before wake. */
					wake_up(&shutdown_wq);
				}
			}
		}
		if (done && !alldone &&
		    atomic_read(&n_pcu_scale_writer_finished) >= nrealwriters)
			alldone = true;
		if (started && !alldone && i < MAX_MEAS - 1)
			i++;
		pcu_scale_wait_shutdown();
	} while (!torture_must_stop());
	if (gp_async) {
		cur_ops->gp_barrier();
	}
	writer_n_durations[me] = i_max;
	torture_kthread_stopping("pcu_scale_writer");
	return 0;
}

static void
pcu_scale_print_module_parms(struct pcu_scale_ops *cur_ops, const char *tag)
{
	pr_alert("%s" SCALE_FLAG
		 "--- %s: nreaders=%d nwriters=%d verbose=%d shutdown=%d\n",
		 scale_type, tag, nrealreaders, nrealwriters, verbose, shutdown);
}

static void
pcu_scale_cleanup(void)
{
	int i;
	int j;
	int ngps = 0;
	u64 *wdp;
	u64 *wdpp;

	/*
	 * Would like warning at start, but everything is expedited
	 * during the mid-boot phase, so have to wait till the end.
	 */
	if (pcu_gp_is_expedited() && !pcu_gp_is_normal() && !gp_exp)
		VERBOSE_SCALEOUT_ERRSTRING("All grace periods expedited, no normal ones to measure!");
	if (pcu_gp_is_normal() && gp_exp)
		VERBOSE_SCALEOUT_ERRSTRING("All grace periods normal, no expedited ones to measure!");
	if (gp_exp && gp_async)
		VERBOSE_SCALEOUT_ERRSTRING("No expedited async GPs, so went with async!");

	if (torture_cleanup_begin())
		return;
	if (!cur_ops) {
		torture_cleanup_end();
		return;
	}

	if (reader_tasks) {
		for (i = 0; i < nrealreaders; i++)
			torture_stop_kthread(pcu_scale_reader,
					     reader_tasks[i]);
		kfree(reader_tasks);
	}

	if (writer_tasks) {
		for (i = 0; i < nrealwriters; i++) {
			torture_stop_kthread(pcu_scale_writer,
					     writer_tasks[i]);
			if (!writer_n_durations)
				continue;
			j = writer_n_durations[i];
			pr_alert("%s%s writer %d gps: %d\n",
				 scale_type, SCALE_FLAG, i, j);
			ngps += j;
		}
		pr_alert("%s%s start: %llu end: %llu duration: %llu gps: %d batches: %ld\n",
			 scale_type, SCALE_FLAG,
			 t_pcu_scale_writer_started, t_pcu_scale_writer_finished,
			 t_pcu_scale_writer_finished -
			 t_pcu_scale_writer_started,
			 ngps,
			 pcuscale_seq_diff(b_pcu_gp_test_finished,
					   b_pcu_gp_test_started));
		for (i = 0; i < nrealwriters; i++) {
			if (!writer_durations)
				break;
			if (!writer_n_durations)
				continue;
			wdpp = writer_durations[i];
			if (!wdpp)
				continue;
			for (j = 0; j <= writer_n_durations[i]; j++) {
				wdp = &wdpp[j];
				pr_alert("%s%s %4d writer-duration: %5d %llu\n",
					scale_type, SCALE_FLAG,
					i, j, *wdp);
				if (j % 100 == 0)
					schedule_timeout_uninterruptible(1);
			}
			kfree(writer_durations[i]);
		}
		kfree(writer_tasks);
		kfree(writer_durations);
		kfree(writer_n_durations);
	}

	/* Do torture-type-specific cleanup operations.  */
	if (cur_ops->cleanup != NULL)
		cur_ops->cleanup();

	torture_cleanup_end();
}

/*
 * Return the number if non-negative.  If -1, the number of CPUs.
 * If less than -1, that much less than the number of CPUs, but
 * at least one.
 */
static int compute_real(int n)
{
	int nr;

	if (n >= 0) {
		nr = n;
	} else {
		nr = num_online_cpus() + 1 + n;
		if (nr <= 0)
			nr = 1;
	}
	return nr;
}

/*
 * PCU scalability shutdown kthread.  Just waits to be awakened, then shuts
 * down system.
 */
static int
pcu_scale_shutdown(void *arg)
{
	wait_event(shutdown_wq,
		   atomic_read(&n_pcu_scale_writer_finished) >= nrealwriters);
	smp_mb(); /* Wake before output. */
	pcu_scale_cleanup();
	kernel_power_off();
	return -EINVAL;
}

/*
 * kfree_pcu() scalability tests: Start a kfree_pcu() loop on all CPUs for number
 * of iterations and measure total time and number of GP for all iterations to complete.
 */

torture_param(int, kfree_nthreads, -1, "Number of threads running loops of kfree_pcu().");
torture_param(int, kfree_alloc_num, 8000, "Number of allocations and frees done in an iteration.");
torture_param(int, kfree_loops, 10, "Number of loops doing kfree_alloc_num allocations and frees.");
torture_param(bool, kfree_pcu_test_double, false, "Do we run a kfree_pcu() double-argument scale test?");
torture_param(bool, kfree_pcu_test_single, false, "Do we run a kfree_pcu() single-argument scale test?");

static struct task_struct **kfree_reader_tasks;
static int kfree_nrealthreads;
static atomic_t n_kfree_scale_thread_started;
static atomic_t n_kfree_scale_thread_ended;

struct kfree_obj {
	char kfree_obj[8];
	struct pcu_head rh;
};

static int
kfree_scale_thread(void *arg)
{
	int i, loop = 0;
	long me = (long)arg;
	struct kfree_obj *alloc_ptr;
	u64 start_time, end_time;
	long long mem_begin, mem_during = 0;
	bool kfree_pcu_test_both;
	DEFINE_TORTURE_RANDOM(tr);

	VERBOSE_SCALEOUT_STRING("kfree_scale_thread task started");
	set_cpus_allowed_ptr(current, cpumask_of(me % nr_cpu_ids));
	set_user_nice(current, MAX_NICE);
	kfree_pcu_test_both = (kfree_pcu_test_single == kfree_pcu_test_double);

	start_time = ktime_get_mono_fast_ns();

	if (atomic_inc_return(&n_kfree_scale_thread_started) >= kfree_nrealthreads) {
		if (gp_exp)
			b_pcu_gp_test_started = cur_ops->exp_completed() / 2;
		else
			b_pcu_gp_test_started = cur_ops->get_gp_seq();
	}

	do {
		if (!mem_during) {
			mem_during = mem_begin = si_mem_available();
		} else if (loop % (kfree_loops / 4) == 0) {
			mem_during = (mem_during + si_mem_available()) / 2;
		}

		for (i = 0; i < kfree_alloc_num; i++) {
			alloc_ptr = kmalloc(kfree_mult * sizeof(struct kfree_obj), GFP_KERNEL);
			if (!alloc_ptr)
				return -ENOMEM;

			// By default kfree_pcu_test_single and kfree_pcu_test_double are
			// initialized to false. If both have the same value (false or true)
			// both are randomly tested, otherwise only the one with value true
			// is tested.
			if ((kfree_pcu_test_single && !kfree_pcu_test_double) ||
					(kfree_pcu_test_both && torture_random(&tr) & 0x800))
				kfree_pcu(alloc_ptr);
			else
				kfree_pcu(alloc_ptr, rh);
		}

		cond_resched();
	} while (!torture_must_stop() && ++loop < kfree_loops);

	if (atomic_inc_return(&n_kfree_scale_thread_ended) >= kfree_nrealthreads) {
		end_time = ktime_get_mono_fast_ns();

		if (gp_exp)
			b_pcu_gp_test_finished = cur_ops->exp_completed() / 2;
		else
			b_pcu_gp_test_finished = cur_ops->get_gp_seq();

		pr_alert("Total time taken by all kfree'ers: %llu ns, loops: %d, batches: %ld, memory footprint: %lldMB\n",
		       (unsigned long long)(end_time - start_time), kfree_loops,
		       pcuscale_seq_diff(b_pcu_gp_test_finished, b_pcu_gp_test_started),
		       (mem_begin - mem_during) >> (20 - PAGE_SHIFT));

		if (shutdown) {
			smp_mb(); /* Assign before wake. */
			wake_up(&shutdown_wq);
		}
	}

	torture_kthread_stopping("kfree_scale_thread");
	return 0;
}

static void
kfree_scale_cleanup(void)
{
	int i;

	if (torture_cleanup_begin())
		return;

	if (kfree_reader_tasks) {
		for (i = 0; i < kfree_nrealthreads; i++)
			torture_stop_kthread(kfree_scale_thread,
					     kfree_reader_tasks[i]);
		kfree(kfree_reader_tasks);
	}

	torture_cleanup_end();
}

/*
 * shutdown kthread.  Just waits to be awakened, then shuts down system.
 */
static int
kfree_scale_shutdown(void *arg)
{
	wait_event(shutdown_wq,
		   atomic_read(&n_kfree_scale_thread_ended) >= kfree_nrealthreads);

	smp_mb(); /* Wake before output. */

	kfree_scale_cleanup();
	kernel_power_off();
	return -EINVAL;
}

static int 
kfree_scale_init(void)
{
	long i;
	int firsterr = 0;

	kfree_nrealthreads = compute_real(kfree_nthreads);
	/* Start up the kthreads. */
	if (shutdown) {
		init_waitqueue_head(&shutdown_wq);
		firsterr = torture_create_kthread(kfree_scale_shutdown, NULL,
						  shutdown_task);
		if (firsterr)
			goto unwind;
		schedule_timeout_uninterruptible(1);
	}

	pr_alert("kfree object size=%zu\n", kfree_mult * sizeof(struct kfree_obj));

	kfree_reader_tasks = kcalloc(kfree_nrealthreads, sizeof(kfree_reader_tasks[0]),
			       GFP_KERNEL);
	if (kfree_reader_tasks == NULL) {
		firsterr = -ENOMEM;
		goto unwind;
	}

	for (i = 0; i < kfree_nrealthreads; i++) {
		firsterr = torture_create_kthread(kfree_scale_thread, (void *)i,
						  kfree_reader_tasks[i]);
		if (firsterr)
			goto unwind;
	}

	while (atomic_read(&n_kfree_scale_thread_started) < kfree_nrealthreads)
		schedule_timeout_uninterruptible(1);

	torture_init_end();
	return 0;

unwind:
	torture_init_end();
	kfree_scale_cleanup();
	return firsterr;
}

static int 
pcu_scale_init(void)
{
	long i;
	int firsterr = 0;
	static struct pcu_scale_ops *scale_ops[] = {
		&pcu_ops, &spcu_ops, &spcud_ops, &tasks_ops, &tasks_tracing_ops
	};

	if (!torture_init_begin(scale_type, verbose))
		return -EBUSY;

	/* Process args and announce that the scalability'er is on the job. */
	for (i = 0; i < ARRAY_SIZE(scale_ops); i++) {
		cur_ops = scale_ops[i];
		if (strcmp(scale_type, cur_ops->name) == 0)
			break;
	}
	if (i == ARRAY_SIZE(scale_ops)) {
		pr_alert("pcu-scale: invalid scale type: \"%s\"\n", scale_type);
		pr_alert("pcu-scale types:");
		for (i = 0; i < ARRAY_SIZE(scale_ops); i++)
			pr_cont(" %s", scale_ops[i]->name);
		pr_cont("\n");
		firsterr = -EINVAL;
		cur_ops = NULL;
		goto unwind;
	}
	if (cur_ops->init)
		cur_ops->init();

	if (kfree_pcu_test)
		return kfree_scale_init();

	nrealwriters = compute_real(nwriters);
	nrealreaders = compute_real(nreaders);
	atomic_set(&n_pcu_scale_reader_started, 0);
	atomic_set(&n_pcu_scale_writer_started, 0);
	atomic_set(&n_pcu_scale_writer_finished, 0);
	pcu_scale_print_module_parms(cur_ops, "Start of test");

	/* Start up the kthreads. */

	if (shutdown) {
		init_waitqueue_head(&shutdown_wq);
		firsterr = torture_create_kthread(pcu_scale_shutdown, NULL,
						  shutdown_task);
		if (firsterr)
			goto unwind;
		schedule_timeout_uninterruptible(1);
	}
	reader_tasks = kcalloc(nrealreaders, sizeof(reader_tasks[0]),
			       GFP_KERNEL);
	if (reader_tasks == NULL) {
		VERBOSE_SCALEOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealreaders; i++) {
		firsterr = torture_create_kthread(pcu_scale_reader, (void *)i,
						  reader_tasks[i]);
		if (firsterr)
			goto unwind;
	}
	while (atomic_read(&n_pcu_scale_reader_started) < nrealreaders)
		schedule_timeout_uninterruptible(1);
	writer_tasks = kcalloc(nrealwriters, sizeof(reader_tasks[0]),
			       GFP_KERNEL);
	writer_durations = kcalloc(nrealwriters, sizeof(*writer_durations),
				   GFP_KERNEL);
	writer_n_durations =
		kcalloc(nrealwriters, sizeof(*writer_n_durations),
			GFP_KERNEL);
	if (!writer_tasks || !writer_durations || !writer_n_durations) {
		VERBOSE_SCALEOUT_ERRSTRING("out of memory");
		firsterr = -ENOMEM;
		goto unwind;
	}
	for (i = 0; i < nrealwriters; i++) {
		writer_durations[i] =
			kcalloc(MAX_MEAS, sizeof(*writer_durations[i]),
				GFP_KERNEL);
		if (!writer_durations[i]) {
			firsterr = -ENOMEM;
			goto unwind;
		}
		firsterr = torture_create_kthread(pcu_scale_writer, (void *)i,
						  writer_tasks[i]);
		if (firsterr)
			goto unwind;
	}
	torture_init_end();
	return 0;

unwind:
	torture_init_end();
	pcu_scale_cleanup();
	if (shutdown) {
		WARN_ON(!IS_MODULE(CONFIG_PCU_SCALE_TEST));
		kernel_power_off();
	}
	return firsterr;
}

//module_init(pcu_scale_init);
//module_exit(pcu_scale_cleanup);
