#include <inc/assert.h>
#include <inc/x86.h>
#include <kern/spinlock.h>
#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>

void sched_halt(void);

// Choose a user environment to run and run it.
void
sched_yield(void)
{
	struct Thd *idle;

	// Implement simple round-robin scheduling.
	//
	// Search through 'envs' for an ENV_RUNNABLE environment in
	// circular fashion starting just after the env this CPU was
	// last running.  Switch to the first such environment found.
	//
	// If no envs are runnable, but the environment previously
	// running on this CPU is still ENV_RUNNING, it's okay to
	// choose that environment. Make sure curenv is not null before
	// dereferencing it.
	//
	// Never choose an environment that's currently running on
	// another CPU (env_status == ENV_RUNNING). If there are
	// no runnable environments, simply drop through to the code
	// below to halt the cpu.

	// LAB 4: Your code here.
	// 这里的基本单位改成Thd，重写一遍吧
	
	struct Thd *cur = curthd;
	if (cur == NULL) {
		for(idle = thds; idle != thds + NTHD; idle++)
			if (idle->thd_status == THD_RUNNABLE && idle->thd_env->env_status == ENV_RUNNABLE)
				thd_run(idle);
	}
	else {
		// 从现在找到末尾
		for(idle = cur + 1; idle != thds + NTHD; idle++)
			if (idle->thd_status == THD_RUNNABLE && idle->thd_env->env_status == ENV_RUNNABLE)
				thd_run(idle);
		// 没找到就从头找到现在
		for(idle = thds; idle != cur; idle++)
			if (idle->thd_status == THD_RUNNABLE && idle->thd_env->env_status == ENV_RUNNABLE)
				thd_run(idle);
		// 都没有就看看现在的
		if (cur->thd_status == THD_RUNNING && cur->thd_env->env_status == ENV_RUNNABLE)
			thd_run(cur);
	}
	// 如果实在找不到，就停机吧
	// sched_halt never returns
	sched_halt();
}

// Halt this CPU when there is nothing to do. Wait until the
// timer interrupt wakes it up. This function never returns.
//
void
sched_halt(void)
{
	int i;

	// For debugging and testing purposes, if there are no runnable
	// environments in the system, then drop into the kernel monitor.
	for (i = 0; i < NTHD; i++) {
		if ((thds[i].thd_status == THD_RUNNABLE ||
		     thds[i].thd_status == THD_RUNNING ||
		     thds[i].thd_status == THD_DYING))
			break;
	}
	if (i == NTHD) {
		cprintf("No runnable environments in the system!\n");
		while (1)
			monitor(NULL);
	}

	// Mark that no environment is running on this CPU
	curthd = NULL;
	lcr3(PADDR(kern_pgdir));

	// Mark that this CPU is in the HALT state, so that when
	// timer interupts come in, we know we should re-acquire the
	// big kernel lock
	xchg(&thiscpu->cpu_status, CPU_HALTED);

	// Release the big kernel lock as if we were "leaving" the kernel
	unlock_kernel();

	// Reset stack pointer, enable interrupts and then halt.
	asm volatile (
		"movl $0, %%ebp\n"
		"movl %0, %%esp\n"
		"pushl $0\n"
		"pushl $0\n"
		// Uncomment the following line after completing exercise 13
		"sti\n"
		"1:\n"
		"hlt\n"
		"jmp 1b\n"
	: : "a" (thiscpu->cpu_ts.ts_esp0));
}

