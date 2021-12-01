/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>
#include <kern/time.h>
#include <kern/e1000.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.

	// LAB 3: Your code here.
	user_mem_assert(curenv, s, len, 0);

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	if (curenv != NULL) return curenv->env_id;
	else return -1;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
// 创建一个新进程，用户地址空间不做映射，寄存器状态copy父进程
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.

	// LAB 4: Your code here.
	struct Env * e;
	struct Thd * t;
	int ret = env_alloc(&e, curenv->env_id);
	if(ret < 0){
		return ret;
	}
	t = e->env_thd_head;
	t->thd_tf = curthd->thd_tf; // 复制寄存器
	t->thd_tf.tf_regs.reg_eax = 0; // 新的进程从sys_exofork()的返回值应该为0
	e->env_status = ENV_NOT_RUNNABLE; // 进程状态
	return e->env_id;
	// panic("sys_exofork not implemented");
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
// 设置一个特定进程的状态为ENV_RUNNABLE或ENV_NOT_RUNNABLE。
static int
sys_env_set_status(envid_t envid, int status)
{
	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

	// LAB 4: Your code here.
	if(status != ENV_NOT_RUNNABLE && status != ENV_RUNNABLE) return -E_INVAL;
	struct Env * e;
	int ret = envid2env(envid, &e, 1);
	if(ret < 0){
		return ret;
	}
	e->env_status = status;
	return 0;
	//panic("sys_env_set_status not implemented");
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3), interrupts enabled, and IOPL of 0.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
/*
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	// LAB 5: Your code here.
	// Remember to check whether the user has supplied us with a good
	// address!
	// panic("sys_env_set_trapframe not implemented");
	struct Env * e;
	if (envid2env(envid, &e, 1) < 0) {
		return -E_BAD_ENV;
	}
	e->env_tf = *tf;
	e->env_tf.tf_cs |= 0x3; // 修改一下提示要求的值
	e->env_tf.tf_eflags &=  (~FL_IOPL_MASK); // 普通进程不能有IO权限
	e->env_tf.tf_eflags |= FL_IF;
	return 0;
}
*/

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
// 指定的用户环境设置env_pgfault_upcall，缺页中断发生时，会执行env_pgfault_upcall指定位置的代码。
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	// LAB 4: Your code here.
	struct Env * e;
	int ret = envid2env(envid, &e, 1);
	if(ret < 0){
		return -E_BAD_ENV;
	}
	e->env_pgfault_upcall = func;
	return 0;
	// panic("sys_env_set_pgfault_upcall not implemented");
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
// 为特定进程分配一个物理页，映射指定线性地址va到该物理页。
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!

	// LAB 4: Your code here.
	struct Env * e;
	int ret = envid2env(envid, &e, 1);
	if(ret < 0){
		return ret;
	}
	// 判定va合法性
	if ((va >= (void*)UTOP) || PGOFF(va)) return -E_INVAL;
	// 判定权限
	int flag = PTE_U | PTE_P;
	if ((perm & ~(PTE_SYSCALL))!=0 || (perm & flag) != flag) return -E_INVAL;
	// 分配物理页
	struct PageInfo * pp = page_alloc(1);
	if(pp == NULL) return -E_NO_MEM;
	ret = page_insert(e->env_pgdir, pp, va, perm);
	if(ret < 0){ // page_insert错误
		page_free(pp);
		return ret;
	}
	return 0;
	// panic("sys_page_alloc not implemented");
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
// 拷贝页表，使指定进程共享当前进程相同的映射关系。本质上是修改特定进程的页目录和页表。
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.

	// LAB 4: Your code here.
	struct Env * se, * de;
	int ret = envid2env(srcenvid, &se, 1);
	if(ret) return ret; // 错误的Env
	ret = envid2env(dstenvid, &de, 1);
	if(ret) return ret; // 错误的Env

	// 如果两个地址越界或者不是页对齐的，则返回错误
	if (srcva >= (void*)UTOP || dstva >= (void*)UTOP || 
		ROUNDDOWN(srcva,PGSIZE) != srcva || ROUNDDOWN(dstva,PGSIZE) != dstva) 
		return -E_INVAL;

	// 如果源页没有映射给源进程，则返回错误
	pte_t *pte;
	struct PageInfo *pg = page_lookup(se->env_pgdir, srcva, &pte);
	if (pg == NULL) return -E_INVAL;

	// 如果页权限不允许，则返回错误
	int flag = PTE_U|PTE_P;
	if ((perm & ~(PTE_SYSCALL)) != 0 || (perm & flag) != flag) return -E_INVAL;

	// 如果权限为可写而源地址权限为只读，则返回错误
	if (((*pte&PTE_W) == 0) && (perm&PTE_W)) return -E_INVAL;

	// 如果内存不够生成页表也返回错误
	ret = page_insert(de->env_pgdir, pg, dstva, perm);
	return ret;
	// panic("sys_page_map not implemented");
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
// 解除页映射关系。本质上是修改指定用户环境的页目录和页表。
static int
sys_page_unmap(envid_t envid, void *va)
{
	// Hint: This function is a wrapper around page_remove().

	// LAB 4: Your code here.
	struct Env *env;
	int ret = envid2env(envid, &env, 1);
	if (ret) return ret;

	if ((va >= (void*)UTOP) || (ROUNDDOWN(va, PGSIZE) != va)) return -E_INVAL;
	page_remove(env->env_pgdir, va);
	return 0;
	// panic("sys_page_unmap not implemented");
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	// LAB 4: Your code here.
	// panic("sys_ipc_try_send not implemented");
	struct Env* env;
	if (envid2env(envid, &env, 0) < 0) return -E_BAD_ENV; // 错误ID
	if (env->env_ipc_recving == 0) return -E_IPC_NOT_RECV; // 进程不接收消息
	if ((uintptr_t) srcva < UTOP) { // 页地址小于UTOP
		pte_t *pte;
		struct PageInfo *pg = page_lookup(curenv->env_pgdir, srcva, &pte);

		//按照注释的顺序进行判定
		if (srcva != ROUNDDOWN(srcva, PGSIZE)) return -E_INVAL; //srcva没有页对齐
		if ((*pte & perm & 7) != (perm & 7)) return -E_INVAL; //perm应该是*pte的子集
		if (!pg) return -E_INVAL; //srcva还没有映射到物理页
		if ((perm & PTE_W) && !(*pte & PTE_W)) return -E_INVAL; //写权限
		
		if (env->env_ipc_dstva < (void*)UTOP) {
			int ret = page_insert(env->env_pgdir, pg, env->env_ipc_dstva, perm); //共享相同的映射关系
			if (ret) return ret;
			env->env_ipc_perm = perm;
		}
	}
	// 设置一些值
	env->env_ipc_recving = false;
	env->env_ipc_from = curenv->env_id;
	env->env_ipc_value = value;
	// env->env_ipc_perm = 0;
	env->env_ipc_thd->thd_status = THD_RUNNABLE;
	env->env_ipc_thd->thd_tf.tf_regs.reg_eax = 0;
	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
	// LAB 4: Your code here.
	// panic("sys_ipc_recv not implemented");
	if ((dstva < (void*)UTOP) && PGOFF(dstva)) return -E_INVAL; // 报错
	curenv->env_ipc_recving = true;
	curenv->env_ipc_dstva = dstva;
	curenv->env_ipc_thd = curthd;
	curthd->thd_status = THD_NOT_RUNNABLE;
	sys_yield();
	return 0;
}

// Return the current time.
static int
sys_time_msec(void)
{
	// LAB 6: Your code here.
	// panic("sys_time_msec not implemented");
	return time_msec();
}

static int
sys_packet_try_send(void *addr, uint32_t len) {
    return e1000_transmit(addr, len);
}

static int
sys_packet_receive(void *addr, uint32_t *len) {
    return e1000_receive(addr, len);
}

static thdid_t sys_getthdid(void) {
	return curthd->thd_id;
}

static thdid_t sys_thd_create() {
	struct Thd * t;
	int r;
	r = thd_alloc(&t,curenv);
	if(r < 0) 
		return r;
	t->thd_status = THD_NOT_RUNNABLE;
	return t->thd_id;
}

static int sys_thd_destroy(thdid_t tid) {
	int r;
	struct Thd * t;
	r = thdid2thd(tid, &t, true);
	if (r < 0) return r;
	thd_destroy(t);
	return 0;
}

static int sys_thd_set_status(thdid_t tid, int status) {
	struct Thd * t;
	int r;
	r = thdid2thd(tid, &t, true);
	if (r < 0) return r;
	if (status != THD_RUNNABLE && status != THD_NOT_RUNNABLE)
		return -E_INVAL;
	t->thd_status  = status;
	return 0;
}
static int sys_thd_set_trapframe(thdid_t tid, struct Trapframe *tf) {
	struct Thd * t;
	int r;
	r = thdid2thd(tid, &t, true);
	if (r < 0) return r;
	user_mem_assert(curenv, tf, sizeof(struct Trapframe), 0);
	t->thd_tf = *tf;
	t->thd_tf.tf_ds = GD_UD | 3;
	t->thd_tf.tf_es = GD_UD | 3;
	t->thd_tf.tf_ss = GD_UD | 3;
	t->thd_tf.tf_cs = GD_UT | 3;
    t->thd_tf.tf_eflags |= FL_IF;
	return 0;
}

static int sys_thd_set_uxstack(thdid_t tid, uintptr_t uxstack) {
	struct Thd * t;
	int r;
	r = thdid2thd(tid, &t, true);
	if (r < 0) return r;
	if (PGOFF(uxstack)) return -E_INVAL;
	t->thd_uxstack = uxstack;
	return 0;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.
	// LAB 3: Your code here.

	int32_t ret = 0;

	// 根据中断号分别调用中断处理程序
	switch (syscallno) {
		case SYS_cputs:
			sys_cputs((char *)a1, (size_t)a2);
			break;
		case SYS_cgetc:
			ret = sys_cgetc();
			break;
		case SYS_getenvid:
			ret = sys_getenvid();
			break;
		case SYS_env_destroy:
			ret = sys_env_destroy((envid_t)a1);
			break;
		case SYS_yield:
			sys_yield();
			break;
		case SYS_exofork:
           	ret =  sys_exofork();
			break;
		case SYS_env_set_status:
           	ret = sys_env_set_status((envid_t)a1, (int)a2);
			break;
		case SYS_page_alloc:
           	ret = sys_page_alloc((envid_t)a1, (void *)a2, (int)a3);
			break;
       	case SYS_page_map:
           	ret = sys_page_map((envid_t)a1, (void *)a2, (envid_t)a3, (void *)a4, (int)a5);
			break;
       	case SYS_page_unmap:
           	ret = sys_page_unmap((envid_t)a1, (void *)a2);
			break;
		case SYS_env_set_pgfault_upcall:
			ret = sys_env_set_pgfault_upcall(a1, (void*) a2);
			break;
		case SYS_ipc_try_send:                                                                                          
			ret =  sys_ipc_try_send((envid_t)a1, (uint32_t)a2, (void *)a3, (unsigned)a4); 
			break;
		case SYS_ipc_recv:                                                                                              
			ret =  sys_ipc_recv((void *)a1);
			break;
		case SYS_time_msec:
			ret = sys_time_msec();
			break;
		case (SYS_packet_try_send):
        	ret = sys_packet_try_send((void *)a1,a2);
			break;
		case (SYS_packet_receive):
        	ret = sys_packet_receive((void *)a1,(size_t *)a2);
			break;
		case SYS_getthdid:
			ret = sys_getthdid();
			break;
		case SYS_thd_create:
			ret = sys_thd_create();
			break;
		case SYS_thd_destroy:
			ret = sys_thd_destroy((thdid_t) a1);
			break;
		case SYS_thd_set_status:
			ret = sys_thd_set_status((thdid_t)a1,(int) a2);
			break;
		case SYS_thd_set_trapframe:
			ret = sys_thd_set_trapframe((thdid_t)a1,(struct Trapframe *)a2);
			break;
		case SYS_thd_set_uxstack:
			ret = sys_thd_set_uxstack((thdid_t) a1, a2);
			break;
		default:
			ret = -E_INVAL;
	}
	return ret;
}

