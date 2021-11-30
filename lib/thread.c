#include <inc/lib.h>

#define THREAD_MAX (PTSIZE / 4)

volatile int thread_used_stack[THREAD_MAX];
spinlock_t thread_lock = 0;

static int get_empty_thread_position();

void wait_thread(thdid_t tar) {
	int p = THDX(tar);
	while(thds[p].thd_status != THD_FREE && thds[p].thd_id == tar)
		sys_yield();
}

static void thread_start(void(*func)(void*), void *para) {
	func(para);
	delete_thread(0);
}

thdid_t create_thread(void(*func)(void*), void*para) {
	thdid_t child = sys_thd_create();
	if (child < 0)
		return child;
    spin_lock(&thread_lock);
	int p = get_empty_thread_position();
	if (p < 0)
		panic("create_thread error 0: Do not have thread space for new thread!");
	thread_used_stack[p] = child;
    spin_unlock(&thread_lock);
	int r;
	if (!((uvpd[PDX(UTXSTACKTOP(p) - PGSIZE)] & PTE_P) && (uvpt[PGNUM(UTXSTACKTOP(p) - PGSIZE)] & PTE_P))) {
		r = sys_page_alloc(0, (void*)(UTXSTACKTOP(p) - PGSIZE), PTE_P | PTE_U | PTE_W);
		if (r < 0)
			panic(" create_thread error 1: %e", r);
	}
	if (!((uvpd[PDX(UTSTACKTOP(p) - PGSIZE)] & PTE_P) && (uvpt[PGNUM(UTSTACKTOP(p) - PGSIZE)] & PTE_P))) {
		r = sys_page_alloc(0, (void*)(UTSTACKTOP(p) - PGSIZE), PTE_P | PTE_U | PTE_W);
		if (r < 0)
			panic(" create_thread error 2: %e", r);
	}
	// set stack for new thread
	uintptr_t *tmp = (uintptr_t*)UTSTACKTOP(p);
	tmp -= 2;
	*(--tmp) = (unsigned)para;
	*(--tmp) = (unsigned)func;

	struct Trapframe tf;
	memset(&tf, 0, sizeof tf);
	tf.tf_eflags = 0;
	tf.tf_eip = (uintptr_t)thread_start;
	tf.tf_esp = (uintptr_t)(tmp - 1);
	r = sys_thd_set_trapframe(child, &tf);
	if (r < 0)
		panic("create_thread error 3: %e", r);
	r = sys_thd_set_uxstack(child, UTXSTACKTOP(p));
	if (r < 0)
		panic("create_thread error 4: %e", r);
	r = sys_thd_set_status(child, THD_RUNNABLE);
	if (r < 0)
		panic("create_thread error 5: %e", r);
	return child;
}

int delete_thread(thdid_t tar) {
	thdid_t cur = sys_getthdid();
	if (tar == 0)
		tar = cur;
	if (tar == main_thdid)
		return -E_INVAL;
	int stk;
	for(stk = 1; stk < THREAD_MAX; stk++)
		if (thread_used_stack[stk] == tar)
			break;
	if (cur == tar) {
		if (stk == THREAD_MAX)
			panic("delete_thread error 0: target thread does not have stack");
        
        thread_used_stack[stk] = 0;

		asm volatile("int %0\n"
					:
					  : "i" (T_SYSCALL),
					    "a" (SYS_thd_destroy),
					    "d" (0),
					    "c" (0),
					    "b" (0),
					    "D" (0),
					    "S" (0)
				: "cc", "memory");

		assert(0);
	}
    else {
		int r = sys_thd_destroy(tar);
		if (r < 0)
			return r;
		if (stk == THREAD_MAX)
			panic("delete_thread error 2: target thread does not have stack");
		wait_thread(tar);
		thread_used_stack[stk] = 0;
	}
	return 0;
}

static int get_empty_thread_position()
{
	int i;
	for(i = 1; i < THREAD_MAX; i++)
		if (thread_used_stack[i] == 0)
			return i;
	return -1;
}