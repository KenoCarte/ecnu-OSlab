#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

volatile static int started = 0;

volatile static int over_1 = 0, over_2 = 0;
int main() {
    int cpuid = r_tp();
    if (cpuid == 0) {
        print_init();
        pmem_init();
        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        started = 1;
        test_case_2();
    }
    else {
        while (started == 0);
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        //test_case_2();
    }
    while (1);
}