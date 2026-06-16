#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

volatile static int is_started = 0;

int main() {
    int cpuid = r_tp();
    if (cpuid == 0) {
        print_init();
        pmem_init();
        kvm_init();
        trap_kernel_init();
        trap_kernel_inithart();
        kvm_inithart();
        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        is_started = 1;
        uint64 last = 0;
        while(1) {
            uint64 now = timer_get_ticks();
            if (now != last) {
                printf("cpu %d:di da\n", cpuid);
                last = now;
            }
        }
    }
    else {
        while (is_started == 0);
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        uint64 last = 0;
        while(1) {
            uint64 now = timer_get_ticks();
            if (now != last) {
                printf("cpu %d:di da\n", cpuid);
                last = now;
            }
        }
    }
    while (1);
}