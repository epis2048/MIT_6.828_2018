/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

struct Env *envs = NULL;		// All environments
static struct Env *env_free_list;	// Free environment list
					// (linked by Env->env_link)

struct Thd *thds = NULL;
static struct Thd *thd_free_list;

#define ENVGENSHIFT	12		// >= LOGNENV
#define THDGENSHIFT 12

// Global descriptor table.
//
// Set up global descriptor table (GDT) with separate segments for
// kernel mode and user mode.  Segments serve many purposes on the x86.
// We don't use any of their memory-mapping capabilities, but we need
// them to switch privilege levels. 
//
// The kernel and user segments are identical except for the DPL.
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
// In particular, the last argument to the SEG macro used in the
// definition of gdt specifies the Descriptor Privilege Level (DPL)
// of that descriptor: 0 for kernel and 3 for user.
//
struct Segdesc gdt[NCPU + 5] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// Per-CPU TSS descriptors (starting from GD_TSS0) are initialized
	// in trap_init_percpu()
	[GD_TSS0 >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

//
// Converts an envid to an env pointer.
// If checkperm is set, the specified environment must be either the
// current environment or an immediate child of the current environment.
//
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
int
envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	// If envid is zero, return the current environment.
	if (envid == 0) {
		*env_store = curenv;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that struct Env
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}

// Mark all environments in 'envs' as free, set their env_ids to 0,
// and insert them into the env_free_list.
// Make sure the environments are in the free list in the same order
// they are in the envs array (i.e., so that the first call to
// env_alloc() returns envs[0]).
// 初始化envs数组，构建env_free_list链表，使用头插法
// 修改：还要初始化thds
void
env_init(void)
{
	// Set up envs array
	// LAB 3: Your code here.
	env_free_list = NULL;
	for(int i = NENV - 1; i >= 0; i--){
		envs[i].env_id = 0;
		envs[i].env_status = ENV_FREE;
		envs[i].env_link = env_free_list;
		env_free_list = &envs[i];
	}
	thd_free_list = NULL;
	for(int i = NTHD - 1; i >= 0; i--){
		thds[i].thd_link = thd_free_list;
		thds[i].thd_id = 0;
		thds[i].thd_status = THD_FREE;
		thd_free_list = &thds[i];
	}
	// Per-CPU part of the initialization
	env_init_percpu();
}

// Load GDT and segment descriptors.
void
env_init_percpu(void)
{
	lgdt(&gdt_pd);
	// The kernel never uses GS or FS, so we leave those set to
	// the user data segment.
	asm volatile("movw %%ax,%%gs" : : "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" : : "a" (GD_UD|3));
	// The kernel does use ES, DS, and SS.  We'll change between
	// the kernel and user data segments as needed.
	asm volatile("movw %%ax,%%es" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" : : "a" (GD_KD));
	// Load the kernel text segment into CS.
	asm volatile("ljmp %0,$1f\n 1:\n" : : "i" (GD_KT));
	// For good measure, clear the local descriptor table (LDT),
	// since we don't use it.
	lldt(0);
}

//
// Initialize the kernel virtual memory layout for environment e.
// Allocate a page directory, set e->env_pgdir accordingly,
// and initialize the kernel portion of the new environment's address space.
// Do NOT (yet) map anything into the user portion
// of the environment's virtual address space.
//
// Returns 0 on success, < 0 on error.  Errors include:
//	-E_NO_MEM if page directory or table could not be allocated.
//
static int
env_setup_vm(struct Env *e)
{
	int i;
	struct PageInfo *p = NULL;

	// Allocate a page for the page directory
	if (!(p = page_alloc(ALLOC_ZERO)))
		return -E_NO_MEM;

	// Now, set e->env_pgdir and initialize the page directory.
	//
	// Hint:
	//    - The VA space of all envs is identical above UTOP
	//	(except at UVPT, which we've set below).
	//	See inc/memlayout.h for permissions and layout.
	//	Can you use kern_pgdir as a template?  Hint: Yes.
	//	(Make sure you got the permissions right in Lab 2.)
	//    - The initial VA below UTOP is empty.
	//    - You do not need to make any more calls to page_alloc.
	//    - Note: In general, pp_ref is not maintained for
	//	physical pages mapped only above UTOP, but env_pgdir
	//	is an exception -- you need to increment env_pgdir's
	//	pp_ref for env_free to work correctly.
	//    - The functions in kern/pmap.h are handy.

	// 将刚分配的物理页用作页目录，并让其从内核页目录继承数据
	p->pp_ref++;
	e->env_pgdir = (pde_t *) page2kva(p);
	memcpy(e->env_pgdir, kern_pgdir, PGSIZE); 

	// UVPT maps the env's own page table read-only.
	// Permissions: kernel R, user R
	e->env_pgdir[PDX(UVPT)] = PADDR(e->env_pgdir) | PTE_P | PTE_U;

	return 0;
}

//
// Allocates and initializes a new environment.
// On success, the new environment is stored in *newenv_store.
//
// Returns 0 on success, < 0 on failure.  Errors include:
//	-E_NO_FREE_ENV if all NENV environments are allocated
//	-E_NO_MEM on memory exhaustion
//
int
env_alloc(struct Env **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	struct Env *e;
	struct Thd *t;

	if (!(e = env_free_list))
		return -E_NO_FREE_ENV;
	
	e->env_thd_head = NULL;
	e->env_thd_tail = NULL;

	if ((r = thd_alloc(&t, e)) < 0)
		return r;

	// Allocate and set up the page directory for this environment.
	if ((r = env_setup_vm(e)) < 0) {
		thd_free(t);
		return r;
	}

	// Generate an env_id for this environment.
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	if (generation <= 0)	// Don't create a negative env_id.
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs);

	// Set the basic status variables.
	e->env_parent_id = parent_id;
	e->env_type = ENV_TYPE_USER;
	e->env_status = ENV_RUNNABLE;

	// 注释掉Env中放弃的字段
	// e->env_runs = 0;

	// Clear out all the saved register state,
	// to prevent the register values
	// of a prior environment inhabiting this Env structure
	// from "leaking" into our new environment.
	// memset(&e->env_tf, 0, sizeof(e->env_tf));

	// Set up appropriate initial values for the segment registers.
	// GD_UD is the user data segment selector in the GDT, and
	// GD_UT is the user text segment selector (see inc/memlayout.h).
	// The low 2 bits of each segment register contains the
	// Requestor Privilege Level (RPL); 3 means user mode.  When
	// we switch privilege levels, the hardware does various
	// checks involving the RPL and the Descriptor Privilege Level
	// (DPL) stored in the descriptors themselves.
	// e->env_tf.tf_ds = GD_UD | 3;
	// e->env_tf.tf_es = GD_UD | 3;
	// e->env_tf.tf_ss = GD_UD | 3;
	// e->env_tf.tf_esp = USTACKTOP;
	// e->env_tf.tf_cs = GD_UT | 3;
	// You will set e->env_tf.tf_eip later.

	// Enable interrupts while in user mode.
	// LAB 4: Your code here.
	// e->env_tf.tf_eflags |= FL_IF;

	// Clear the page fault handler until user installs one.
	e->env_pgfault_upcall = 0;

	// Also clear the IPC receiving flag.
	e->env_ipc_recving = 0;

	// commit the allocation
	env_free_list = e->env_link;
	*newenv_store = e;

	// cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
	return 0;
}

//
// Allocate len bytes of physical memory for environment env,
// and map it at virtual address va in the environment's address space.
// Does not zero or otherwise initialize the mapped pages in any way.
// Pages should be writable by user and kernel.
// Panic if any allocation attempt fails.
// 在e指向的用户环境中，为va开头，长度为len的地址分配物理空间。
static void
region_alloc(struct Env *e, void *va, size_t len)
{
	// LAB 3: Your code here.
	// (But only if you need it for load_icode.)
	//
	// Hint: It is easier to use region_alloc if the caller can pass
	//   'va' and 'len' values that are not page-aligned.
	//   You should round va down, and round (va + len) up.
	//   (Watch out for corner-cases!)
	void *begin = ROUNDDOWN(va, PGSIZE), *end = ROUNDUP(va+len, PGSIZE); // 以PGSIZE取整
	while(begin < end){
		struct PageInfo * p = page_alloc(0);
		if(p == NULL){
			panic("region_alloc: Out of Memory!\n");
		}
		page_insert(e->env_pgdir, p, begin, PTE_W | PTE_U);
		begin += PGSIZE;
	}
}

//
// Set up the initial program binary, stack, and processor flags
// for a user process.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
//
// This function loads all loadable segments from the ELF binary image
// into the environment's user memory, starting at the appropriate
// virtual addresses indicated in the ELF program header.
// At the same time it clears to zero any portions of these segments
// that are marked in the program header as being mapped
// but not actually present in the ELF file - i.e., the program's bss section.
//
// All this is very similar to what our boot loader does, except the boot
// loader also needs to read the code from disk.  Take a look at
// boot/main.c to get ideas.
//
// Finally, this function maps one page for the program's initial stack.
//
// load_icode panics if it encounters problems.
//  - How might load_icode fail?  What might be wrong with the given input?
// 加载可执行文件到Env中
static void
load_icode(struct Env *e, uint8_t *binary)
{
	// Hints:
	//  Load each program segment into virtual memory
	//  at the address specified in the ELF segment header.
	//  You should only load segments with ph->p_type == ELF_PROG_LOAD.
	//  Each segment's virtual address can be found in ph->p_va
	//  and its size in memory can be found in ph->p_memsz.
	//  The ph->p_filesz bytes from the ELF binary, starting at
	//  'binary + ph->p_offset', should be copied to virtual address
	//  ph->p_va.  Any remaining memory bytes should be cleared to zero.
	//  (The ELF header should have ph->p_filesz <= ph->p_memsz.)
	//  Use functions from the previous lab to allocate and map pages.
	//
	//  All page protection bits should be user read/write for now.
	//  ELF segments are not necessarily page-aligned, but you can
	//  assume for this function that no two segments will touch
	//  the same virtual page.
	//
	//  You may find a function like region_alloc useful.
	//
	//  Loading the segments is much simpler if you can move data
	//  directly into the virtual addresses stored in the ELF binary.
	//  So which page directory should be in force during
	//  this function?
	//
	//  You must also do something with the program's entry point,
	//  to make sure that the environment starts executing there.
	//  What?  (See env_run() and env_pop_tf() below.)

	// LAB 3: Your code here.
	struct Elf * ELF = (struct Elf *) binary;
	struct Proghdr * ph;
	int ph_num;
	if(ELF->e_magic != ELF_MAGIC){ // 判断格式是否是ELF
		panic("Binary is not ELF format! \n");
	}

	ph = (struct Proghdr *) ((uint8_t *)ELF + ELF->e_phoff); // ph是程序头距离ELF的偏移
	ph_num = ELF->e_phnum;
	lcr3(PADDR(e->env_pgdir)); // 切换到当前用户环境的页目录表
	for(int i = 0; i < ph_num; i++){
		if(ph[i].p_type == ELF_PROG_LOAD){ // 只加载Load类型的Segment
			if (ph->p_filesz > ph->p_memsz) {
                panic("load_icode: file size is greater than memory size");
            }
			region_alloc(e, (void*)ph[i].p_va, ph[i].p_memsz); // 给每个Segment分配物理空间
			memset((void*)ph[i].p_va, 0, ph[i].p_memsz);
			memcpy((void*)ph[i].p_va, binary + ph[i].p_offset, ph[i].p_filesz);
		}
	}
	// e->env_tf.tf_eip = ELF->e_entry; // EIP指向程序入口
	e->env_thd_head->thd_tf.tf_eip = ELF->e_entry; // 进程中的首个线程

	// Now map one page for the program's initial stack
	// at virtual address USTACKTOP - PGSIZE.

	// LAB 3: Your code here.
	// 分配初始栈空间
	region_alloc(e, (void*)(USTACKTOP - PGSIZE), PGSIZE);
	lcr3(PADDR(kern_pgdir)); // 切换回系统的页目录表
}

//
// Allocates a new env with env_alloc, loads the named elf
// binary into it with load_icode, and sets its env_type.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
// The new env's parent ID is set to 0.
// 从env_free_list链表拿一个Env结构，加载从binary地址开始处的ELF可执行文件到该Env结构。
void
env_create(uint8_t *binary, enum EnvType type)
{
	// LAB 3: Your code here.

	// If this is the file server (type == ENV_TYPE_FS) give it I/O privileges.
	// LAB 5: Your code here.
	struct Env * e;
	int r = env_alloc(&e, 0);
	if(r < 0){
		panic("env_create: Create Env failed: %e", r);
	}
	if (type == ENV_TYPE_FS) {
		if (e->env_thd_head != NULL) {
			// cprintf("given io: %d\n", e->env_type);
			e->env_thd_head->thd_tf.tf_eflags |= FL_IOPL_MASK;
		}
	}
	e->env_type = type;
	load_icode(e, binary);
}

//
// Frees env e and all memory it uses.
//
void
env_free(struct Env *e)
{
	pte_t *pt;
	uint32_t pdeno, pteno;
	physaddr_t pa;

	// If freeing the current environment, switch to kern_pgdir
	// before freeing the page directory, just in case the page
	// gets reused.
	if (e == curenv)
		lcr3(PADDR(kern_pgdir));

	// Note the environment's demise.
	// cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, e->env_id);

	// Flush all mapped pages in the user portion of the address space
	static_assert(UTOP % PTSIZE == 0);
	for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

		// only look at mapped page tables
		if (!(e->env_pgdir[pdeno] & PTE_P))
			continue;

		// find the pa and va of the page table
		pa = PTE_ADDR(e->env_pgdir[pdeno]);
		pt = (pte_t*) KADDR(pa);

		// unmap all PTEs in this page table
		for (pteno = 0; pteno <= PTX(~0); pteno++) {
			if (pt[pteno] & PTE_P)
				page_remove(e->env_pgdir, PGADDR(pdeno, pteno, 0));
		}

		// free the page table itself
		e->env_pgdir[pdeno] = 0;
		page_decref(pa2page(pa));
	}

	// free the page directory
	pa = PADDR(e->env_pgdir);
	e->env_pgdir = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	e->env_status = ENV_FREE;
	e->env_link = env_free_list;
	env_free_list = e;
}

//
// Frees environment e.
// If e was the current env, then runs a new environment (and does not return
// to the caller).
//
void
env_destroy(struct Env *e)
{
	// If e is currently running on other CPUs, we change its state to
	// ENV_DYING. A zombie environment will be freed the next time
	// it traps to the kernel.
	struct Thd *t;
	
	for (t = e->env_thd_head; t != NULL; t = t->thd_next) {
		if (t->thd_status == THD_RUNNING && curthd != t) {
			t->thd_status = THD_DYING;
		}
		else {
			thd_free(t);
		}
	}

	if (e->env_thd_head != NULL) {
		e->env_status = ENV_DYING;
	}
	else {
		env_free(e);
	}

	//env_free(e);

	if (curthd->thd_env == e) {
		curthd = NULL;
		sched_yield();
	}
}


//
// Restores the register values in the Trapframe with the 'iret' instruction.
// This exits the kernel and starts executing some environment's code.
//
// This function does not return.
//
void
thd_pop_tf(struct Trapframe *tf)
{
	// Record the CPU we are running on for user-space debugging
	curthd->thd_cpunum = cpunum();

	asm volatile(
		"\tmovl %0,%%esp\n"
		"\tpopal\n"
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
		"\tiret\n"
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
}

//
// Context switch from curenv to env e.
// Note: if this is the first call to env_run, curenv is NULL.
//
// This function does not return.
// 开启一段thd
void
thd_run(struct Thd *t)
{
	// Step 1: If this is a context switch (a new environment is running):
	//	   1. Set the current environment (if any) back to
	//	      ENV_RUNNABLE if it is ENV_RUNNING (think about
	//	      what other states it can be in),
	//	   2. Set 'curenv' to the new environment,
	//	   3. Set its status to ENV_RUNNING,
	//	   4. Update its 'env_runs' counter,
	//	   5. Use lcr3() to switch to its address space.
	// Step 2: Use env_pop_tf() to restore the environment's
	//	   registers and drop into user mode in the
	//	   environment.

	// Hint: This function loads the new environment's state from
	//	e->env_tf.  Go back through the code you wrote above
	//	and make sure you have set the relevant parts of
	//	e->env_tf to sensible values.

	// LAB 3: Your code here.
	if(curthd != NULL && curthd->thd_status == THD_RUNNING){ //如果当前有运行的Thd，则挂起
		curthd->thd_status = THD_RUNNABLE;
	}
	curthd = t;
	t->thd_status = THD_RUNNING;
	if (curthd != t) t->thd_runs++;
	lcr3(PADDR(t->thd_env->env_pgdir)); // 加载当前Thd的线性地址到分页寄存器
	// Lab4: 释放内核
	unlock_kernel();
	thd_pop_tf(&t->thd_tf); // 从栈中取tf结构
}

int thd_alloc(struct Thd **newthd_store, struct Env *env) {
	// 模仿env_alloc
	int32_t generation;
	int r;
	struct Thd *t;
	
	if (!(t = thd_free_list))
		return -E_NO_FREE_ENV;

	generation = (t->thd_id + (1 << THDGENSHIFT)) & ~(NTHD - 1);
	if (generation <= 0)
		generation = 1 << THDGENSHIFT;
	t->thd_id = generation | (t - thds);

	t->thd_env = env;
	t->thd_status = THD_RUNNABLE;
	t->thd_runs = 0;
	t->thd_uxstack = UXSTACKTOP;

	// Clear out all the saved register state,
	// to prevent the register values
	// of a prior environment inhabiting this Env structure
	// from "leaking" into our new environment.
	memset(&t->thd_tf, 0, sizeof(t->thd_tf));
	
	// Set up appropriate initial values for the segment registers.
	// GD_UD is the user data segment selector in the GDT, and
	// GD_UT is the user text segment selector (see inc/memlayout.h).
	// The low 2 bits of each segment register contains the
	// Requestor Privilege Level (RPL); 3 means user mode.  When
	// we switch privilege levels, the hardware does various
	// checks involving the RPL and the Descriptor Privilege Level
	// (DPL) stored in the descriptors themselves.
	t->thd_tf.tf_ds = GD_UD | 3;
	t->thd_tf.tf_es = GD_UD | 3;
	t->thd_tf.tf_ss = GD_UD | 3;
	t->thd_tf.tf_esp = USTACKTOP;
	t->thd_tf.tf_cs = GD_UT | 3;
	// Enable interrupts while in user mode.
	t->thd_tf.tf_eflags |= FL_IF;

	// 插入双向链表
	if (env->env_thd_head) {
		t->thd_prev = env->env_thd_tail;
		t->thd_next = NULL;
		env->env_thd_tail->thd_next = t;
		env->env_thd_tail = t;
	} else {
		t->thd_prev = NULL;
		t->thd_next = NULL;
		env->env_thd_head = t;
		env->env_thd_tail = t;
	}

	thd_free_list = t->thd_link;
	*newthd_store = t;
	return 0;
}

void thd_free(struct Thd *t) {
	assert(t->thd_status != THD_FREE);
	
	t->thd_status = THD_FREE;

	if (t->thd_prev)
		t->thd_prev->thd_next = t->thd_next;
	else
		t->thd_env->env_thd_head = t->thd_next;
	if (t->thd_next)
		t->thd_next->thd_prev = t->thd_prev;
	else
		t->thd_env->env_thd_tail = t->thd_prev;

	t->thd_link = thd_free_list;
	thd_free_list = t;
}

void thd_destroy(struct Thd *t) {
	if (t->thd_status == THD_RUNNING && curthd != t) {
		t->thd_status = THD_DYING;
		return;
	}

	thd_free(t);

	if (t->thd_env->env_thd_head == NULL)
		env_free(t->thd_env);

	if (curthd == t) {
		curthd = NULL;
		sched_yield();
	}
}

int thdid2thd(thdid_t thdid, struct Thd **thd_store, bool checkperm) {
	struct Thd *t;

	if (thdid == 0) {
		*thd_store = curthd;
		return 0;
	}

	t = &thds[ENVX(thdid)];
	if (t->thd_status == THD_FREE || t->thd_id != thdid) {
		*thd_store = 0;
		return -E_BAD_ENV;
	}

	if (checkperm && t->thd_env != curenv) {
		*thd_store = 0;
		return -E_BAD_ENV;
	}

	*thd_store = t;
	return 0;
}