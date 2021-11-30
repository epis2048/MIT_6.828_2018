// Concurrent version of prime sieve of Eratosthenes.
// Invented by Doug McIlroy, inventor of Unix pipes.
// See http://swtch.com/~rsc/thread/.
// The picture halfway down the page and the text surrounding it
// explain what's going on here.
//
// Since NENV is 1024, we can print 1022 primes before running out.
// The remaining two environments are the integer generator at the bottom
// of main and user/idle.

#include <inc/lib.h>

unsigned
primeproc(void)
{
	/*
	int i, id, p;
	envid_t envid = 0;
	//envid = sys_exofork();
	if (envid < 0)
		panic("sys_exofork: %e", envid);
	if (envid == 0) {
		// We're the child.
		// The copied value of the global variable 'thisenv'
		// is no longer valid (it refers to the parent!).
		// Fix it and return 0.
		//thisenv = &envs[ENVX(sys_getenvid())];
		//cprintf("child env_id: %d\n", sys_getenvid());
		return 0;
	}
	//cprintf("child env_id: %d\n", sys_getenvid());
	*/
	return 0;
}

void
umain(int argc, char **argv)
{
	cprintf("bbbbbb!!!!\n");

	//primeproc();

}

