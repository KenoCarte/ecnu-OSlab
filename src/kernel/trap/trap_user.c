#include "mod.h"

// in trampoline.S
extern char trampoline[];  // 内核和用户切换的代码
extern char user_vector[]; // 用户触发陷阱进入内核
extern char user_return[]; // 内核处理完毕返回用户

// in trap.S
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口

// in trap_kernel.c
extern char* interrupt_info[16]; // 中断错误信息
extern char* exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler() {
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();
    uint64 stval = r_stval();
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from u-mode");
    w_stvec((uint64)kernel_vector);
    proc_t* p = myproc();
    p->tf->user_to_kern_epc = sepc;
    int trap_id = scause & 0xf;
    if (scause & 0x8000000000000000ul) {
        // 1-中断处理
        switch (trap_id) // 中断产生原因分类
        {
        case 1:case 5:
            timer_interrupt_handler();
            if (myproc() != NULL) proc_yield();
            break;
        case 9:
            external_interrupt_handler();
            if (myproc() != NULL) proc_yield();
            break;
        default: // 例外处理
            printf("\nunexpected interrupt: %s\n", interrupt_info[trap_id]);
            printf("trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, stval);
            panic("trap_user_handler");
        }
    }
    else {
        // 2-异常处理
        switch (trap_id) // 异常产生原因分类
        {
        case 8:
            syscall();
            p->tf->user_to_kern_epc += 4;
            break;
        case 13:
        case 15:
            // int32 old = (int32)(p->ustack_npage);
            int32 new_npage = (int32)(uvm_ustack_grow(p->pgtbl, p->ustack_npage, stval));
            if (new_npage < 0) {
                printf("\nuvm_ustack_grow: failed at stval = %p\n", stval);
                panic("trap_user_handler: stack grow failed");
            }
            p->ustack_npage = new_npage;
            // printf("page fault occured！trap id = 15\n");
            // printf("ustack_npage：%d -> %d\n", old, new_npage);
            break;
        default: // 例外处理
            printf("\nunexpected exception: %s\n", exception_info[trap_id]);
            printf("trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, stval);
            panic("trap_user_handler");
        }
    }
    trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
void trap_user_return() {
    proc_t* p = mycpu()->proc;
    assert(p != NULL, "trap_user_handler: p is NULL");
    intr_off();
    w_stvec(TRAMPOLINE + (user_vector - trampoline));
    p->tf->user_to_kern_hartid = mycpuid();
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_satp = r_satp();
    w_sepc(p->tf->user_to_kern_epc);
    uint64 status = r_sstatus();
    status &= ~SSTATUS_SPP;
    status |= SSTATUS_SPIE;
    w_sstatus(status);
    void (*jmp)(uint64, uint64) = (void (*)(uint64, uint64))TRAMPOLINE + (user_return - trampoline);
    jmp(TRAPFRAME, MAKE_SATP(p->pgtbl));
}