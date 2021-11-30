#include <inc/lib.h>

const int n = 5;

void go(void*f)
{
	int i;
	int tid = sys_getthdid();
	for(i=0;i<n;i++)
	{
		cprintf("hello world! from %d time = %d\n", tid, i);
		if (i == (tid & 1023))
			delete_thread(0);
	}
}

void umain(int argc, char **argv)
{
	int child[n], i;
	for(i=0;i<n;i++)
		child[i] = create_thread(go, 0);
	for(i=0;i<n;i++)
		wait_thread(child[i]);
	for(i=0;i<n;i++)
		child[i] = create_thread(go, 0);
	for(i=0;i<n;i++)
		wait_thread(child[i]);
}
