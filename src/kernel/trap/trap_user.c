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

}

// 调用user_return()
// 内核态返回用户态
void trap_user_return() {
    proc_t* p = mycpu()->proc;
    assert(p != NULL, "trap_user_handler: p is NULL");
    p->tf->user_to_kern_hartid = mycpuid();
    p->tf->user_to_kern_sp = (uint64)p->kstack + PGSIZE;
    p->tf->user_to_kern_trapvector = (uint64)user_vector;
    w_sepc(p->tf->user_to_kern_epc);
    uint64 status = r_sstatus();
    status &= ~SSTATUS_SPP;
    status |= SSTATUS_SPIE;
    w_sstatus(status);
    user_return(p->tf, MAKE_SATP(p->pagetable));
}