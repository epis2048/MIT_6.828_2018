/* See COPYRIGHT for copyright information. */

#ifndef JOS_INC_ENV_H
#define JOS_INC_ENV_H

#include <inc/types.h>
#include <inc/trap.h>
#include <inc/memlayout.h>

typedef int32_t envid_t;
typedef int32_t thdid_t;

// An environment ID 'envid_t' has three parts:
//
// +1+---------------21-----------------+--------10--------+
// |0|          Uniqueifier             |   Environment    |
// | |                                  |      Index       |
// +------------------------------------+------------------+
//                                       \--- ENVX(eid) --/
//
// The environment index ENVX(eid) equals the environment's index in the
// 'envs[]' array.  The uniqueifier distinguishes environments that were
// created at different times, but share the same environment index.
//
// All real environments are greater than 0 (so the sign bit is zero).
// envid_ts less than 0 signify errors.  The envid_t == 0 is special, and
// stands for the current environment.

#define LOG2NENV		10
#define NENV			(1 << LOG2NENV)
#define ENVX(envid)		((envid) & (NENV - 1))
#define LOG2NTHD		10
#define NTHD			(1 << LOG2NTHD)
#define THDX(thdid)		((thdid) & (NTHD - 1))

// Values of env_status in struct Env
enum EnvStatus{
	ENV_FREE = 0,
	ENV_DYING,
	ENV_RUNNABLE,
	ENV_RUNNING,
	ENV_NOT_RUNNABLE
};

// 线程状态
enum ThdStatus {
	THD_FREE = 0,
	THD_DYING,
	THD_RUNNABLE,
	THD_RUNNING,
	THD_NOT_RUNNABLE
};

// Special environment types
enum EnvType {
	ENV_TYPE_USER = 0,
	ENV_TYPE_FS,		// File system server
	ENV_TYPE_NS,		// Network server
};

struct Env {
	// struct Trapframe env_tf;	// 寄存器改由线程保存
	struct Env *env_link;		// Next free Env
	envid_t env_id;			// Unique environment identifier
	envid_t env_parent_id;		// env_id of this env's parent
	enum EnvType env_type;		// Indicates special system environments
	enum EnvStatus env_status;		// Status of the environment
	// uint32_t env_runs;		// 数量统计也得改为由线程保存
	// int env_cpunum;			// CPU也改为由线程保存

	// 进程的线程列表
	struct Thd *env_thd_head;
	struct Thd *env_thd_tail;

	// Address space
	pde_t *env_pgdir;		// Kernel virtual address of page dir

	// Exception handling
	void *env_pgfault_upcall;	// Page fault upcall entry point

	// Lab 4 IPC
	bool env_ipc_recving;		// Env is blocked receiving
	struct Thd *env_ipc_thd;	// 接受的线程
	void *env_ipc_dstva;		// VA at which to map received page
	uint32_t env_ipc_value;		// Data value sent to us
	envid_t env_ipc_from;		// envid of the sender
	int env_ipc_perm;		// Perm of page mapping received
};

struct Thd { // 线程结构
    struct Trapframe thd_tf; // 来自Env，寄存器状态
	struct Thd *thd_link;	// Next free Thd
	thdid_t thd_id; // 线程ID
	struct Env *thd_env; // 所属Env
	struct Thd *thd_prev; // 双向链表前一节点
	struct Thd *thd_next;	// 双向链表后一节点
	enum ThdStatus thd_status;	// 状态
	uint32_t thd_runs;		// 来自Env，运行的数量
	int thd_cpunum;			// 来自Env，运行的CPU
	uintptr_t thd_uxstack;
};

#endif // !JOS_INC_ENV_H
