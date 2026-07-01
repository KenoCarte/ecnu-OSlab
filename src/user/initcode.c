#include "sys.h"
#define NULL ((void*)0)

int main() {
	int pid = syscall(SYS_fork);
	if (pid == 0) {
		char* argv[] = { "test_4", "111", "222", "333", NULL };
		syscall(SYS_exec, "/test_4", argv);
		syscall(SYS_exit, -1);
	}
	else {
		syscall(SYS_wait, NULL);
	}
	while (1);
}