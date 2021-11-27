// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
	if (!(
        (err & FEC_WR) && (uvpd[PDX(addr)] & PTE_P) &&
        (uvpt[PGNUM(addr)] & PTE_P) && (uvpt[PGNUM(addr)] & PTE_COW)
	)) panic("Neither the fault is a write nor copy-on-write page.\n");//如果不是因为这个原因 就panic

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.
	if((r = sys_page_alloc(0, PFTEMP, PTE_U | PTE_P | PTE_W)) < 0){
		 panic("sys_page_alloc: %e\n", r);//分配了一个页
	}
	addr = ROUNDDOWN(addr, PGSIZE);//页对齐
	memcpy((void *)PFTEMP, addr, PGSIZE);//把这个写时复制的页内容复制一遍
	if ((r = sys_page_map(0, (void *)PFTEMP, 0, addr, PTE_P | PTE_U | PTE_W)) < 0)
        panic("sys_page_map: %e\n", r);//把当前映射的 地址 指向PFTEMP 新分配的页
    if ((r = sys_page_unmap(0, (void *)PFTEMP)) < 0) //取消PFTEMP 的映射，这样就把虚拟地址指向了一个新的页。
        panic("sys_page_unmap: %e\n", r);
	//panic("pgfault not implemented");
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 4: Your code here.
	void* vaddr=(void*)(pn*PGSIZE);
	if(uvpt[pn]&PTE_SHARE){ // Lab5: 对于标识为PTE_SHARE的页，拷贝映射关系，并且两个进程都有读写权限
		if((r = sys_page_map(0, vaddr, envid, vaddr, uvpt[pn] & PTE_SYSCALL)) < 0)return r;
	}
	else if((uvpt[pn] & PTE_W) || (uvpt[pn] & PTE_COW)){
		if ((r = sys_page_map(0, vaddr, envid, vaddr, PTE_P | PTE_U | PTE_COW)) < 0)
            return r;//映射当前页为写时符合
        if ((r = sys_page_map(0, vaddr, 0, vaddr, PTE_P | PTE_U | PTE_COW)) < 0)
            return r;//把自己当前页页标记成写时复制。
	}
	else if((r = sys_page_map(0, vaddr, envid, vaddr, PTE_P | PTE_U)) < 0) {
		return r;//如果当前页已经是写时复制  就不需要更改了
	}
	//panic("duppage not implemented");
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	// LAB 4: Your code here.
	envid_t cenvid;
    unsigned pn;
    int r;
	set_pgfault_handler(pgfault); //设置 缺页处理
	if ((cenvid = sys_exofork()) < 0){ //创建了一个进程。
		panic("sys_exofork failed");
		return cenvid;
	}
	if(cenvid>0){//如果是 父亲进程
		for (pn=PGNUM(UTEXT); pn<PGNUM(USTACKTOP); pn++){ //复制UTEXT 到USTACKTOP的页
            if ((uvpd[pn >> 10] & PTE_P) && (uvpt[pn] & PTE_P))
                if ((r = duppage(cenvid, pn)) < 0)
                    return r;
		}
		if ((r = sys_page_alloc(cenvid, (void *)(UXSTACKTOP-PGSIZE), PTE_U | PTE_P | PTE_W)) < 0)  //分配一个新的页
            return r;
		extern void _pgfault_upcall(void); //缺页处理
		if ((r = sys_env_set_pgfault_upcall(cenvid, _pgfault_upcall)) < 0)
            return r; //为儿子设置一个缺页处理分支
		if ((r = sys_env_set_status(cenvid, ENV_RUNNABLE)) < 0)//设置成可运行
            return r;
        return cenvid;
	}
	else {
		thisenv = &envs[ENVX(sys_getenvid())];//如果是儿子就直接运行。
		return 0;
	}
	//panic("fork not implemented");
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
