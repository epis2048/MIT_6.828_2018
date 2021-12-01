/* See COPYRIGHT for copyright information. */

#ifndef JOS_KERN_ENV_H
#define JOS_KERN_ENV_H

#include <inc/env.h>
#include <kern/cpu.h>

extern struct Env *envs;		// All environments
extern struct Thd *thds;
#define curthd (thiscpu->cpu_thd)		// Current environment
#define curenv (curthd ? curthd->thd_env : NULL)	// Current environment
extern struct Segdesc gdt[];

void	env_init(void);
void	env_init_percpu(void);
int		env_alloc(struct Env **e, envid_t parent_id);
void	env_free(struct Env *e);
void	env_create(uint8_t *binary, enum EnvType type);
void	env_destroy(struct Env *e);	// Does not return if e == curenv
int		thd_alloc(struct Thd **newthd_store, struct Env *env);
void	thd_free(struct Thd *t);
void	thd_destroy(struct Thd *t);
int thdid2thd(thdid_t thdid, struct Thd **thd_store, bool checkperm);

int		envid2env(envid_t envid, struct Env **env_store, bool checkperm);
// The following two functions do not return
void	thd_run(struct Thd *e) __attribute__((noreturn));
void	thd_pop_tf(struct Trapframe *tf) __attribute__((noreturn));

// Without this extra macro, we couldn't pass macros like TEST to
// ENV_CREATE because of the C pre-processor's argument prescan rule.
#define ENV_PASTE3(x, y, z) x ## y ## z

#define ENV_CREATE(x, type)						\
	do {								\
		extern uint8_t ENV_PASTE3(_binary_obj_, x, _start)[];	\
		env_create(ENV_PASTE3(_binary_obj_, x, _start),		\
			   type);					\
	} while (0)

#endif // !JOS_KERN_ENV_H
