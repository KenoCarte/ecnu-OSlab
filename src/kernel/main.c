#include "arch/mod.h"
#include "lib/mod.h"

volatile static int started = 0;

volatile static int sum = 0;

static spinlock_t add_lk;

int main() {
    int cpuid = r_tp();
    if (cpuid == 0) {
        spinlock_init(&add_lk, "add");
        print_init();
        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        started = 1;
        for (int i = 0; i < 1000000; i++) {
            spinlock_acquire(&add_lk);
            sum++;
            spinlock_release(&add_lk);
        }
        printf("cpu %d report: sum = %d\n", cpuid, sum);
    }
    else {
        while (started == 0);
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        for (int i = 0; i < 1000000; i++) {
            spinlock_acquire(&add_lk);
            sum++;
            spinlock_release(&add_lk);
        }
        printf("cpu %d report: sum = %d\n", cpuid, sum);
    }
    while (1);
}