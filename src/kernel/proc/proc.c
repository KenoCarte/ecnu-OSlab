#include "mod.h"
#include "../../user/initcode.h"

#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap/trap_user.c
extern void trap_user_return();

/* ------------本地变量----------- */

// 进程结构体数组 + 第一个用户进程的指针
static proc_t proc_list[N_PROC];
static proc_t* proczero;

// 全局pid + 保护它的锁
static int global_pid;
static spinlock_t pid_lk;

/* 获取一个pid */
static int alloc_pid() {
    int tmp = 0;
    spinlock_acquire(&pid_lk);
    assert(global_pid > 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&pid_lk);
    return tmp;
}

/* 释放进程锁 + trap_user_return */
static void proc_return() {
    proc_t* p = myproc();
    assert(p != NULL, "proc_return: p is NULL");
    spinlock_release(&p->lk);
    trap_user_return();
}

/* 进程模块初始化 */
void proc_init() {
    for (int i = 0;i < N_PROC;i++) {
        spinlock_init(&proc_list[i].lk, "proc");
        proc_list[i].state = UNUSED;
    }
    proczero = NULL;
    global_pid = 1;
    spinlock_init(&pid_lk, "pid");
}

/*
    申请一个UNUSED进程结构体(返回时带锁)
    并执行通用的初始化逻辑
*/
proc_t* proc_alloc() {
    for (int i = 0;i < N_PROC;i++) {
        proc_t* p = &proc_list[i];
        spinlock_acquire(&p->lk);
        if (p->state == UNUSED) {
            p->pid = alloc_pid();
            memset(p->name, 0, sizeof(p->name));
            p->state = RUNNABLE;
            p->parent = NULL;
            p->pgtbl = NULL;
            p->mmap = NULL;
            p->tf = NULL;
            p->heap_top = 0;
            p->ustack_npage = 0;
            p->exit_code = 0;
            p->sleep_space = NULL;
            p->ctx.ra = (uint64)proc_return;
            p->ctx.sp = 0;
            return p;
        }
        spinlock_release(&p->lk);
    }
    panic("proc_alloc: no UNUSED proc");
    return NULL;
}

/*
    回收一个进程结构体并释放它包含的资源
    tips: 调用者需要持有进程锁
*/
void proc_free(proc_t* p) {
    assert(p->state == ZOMBIE, "proc_free: p is not ZOMBIE");
    if (p->pgtbl != NULL) {
        uvm_destroy_pgtbl(p->pgtbl);
        p->pgtbl = NULL;
    }
    p->tf = NULL;
    while (p->mmap != NULL) {
        mmap_region_t* tmp = p->mmap;
        p->mmap = p->mmap->next;
        uvm_munmap(tmp->begin, tmp->npages);
    }
    p->pid = 0;
    p->state = UNUSED;
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->ctx.ra = 0;
    p->ctx.sp = 0;
    memset(p->name, 0, sizeof(p->name));
    spinlock_release(&p->lk);
}

/*
    获得一个初始化过的用户页表
    完成trapframe和trampoline的映射
*/
pgtbl_t proc_pgtbl_init(uint64 trapframe) {
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: pmem_alloc failed");
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)&trampoline, PGSIZE, PTE_R | PTE_X);
    vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);
    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问
*/
void proc_make_first() {
    proczero = proc_alloc();
    memmove(proczero->name, "proczero", sizeof("proczero"));
    proczero->name[sizeof("proczero") - 1] = '\0';
    void* trapframe = pmem_alloc(false);
    assert(trapframe != NULL, "proc_make_first: trapframe alloc failed");
    proczero->pgtbl = proc_pgtbl_init((uint64)trapframe);
    proczero->tf = (trapframe_t*)trapframe;
    void* ustack = pmem_alloc(false);
    assert(ustack != NULL, "proc_make_first: ustack alloc failed");
    proczero->ustack_npage = 1;
    vm_mappages(proczero->pgtbl, TRAPFRAME - PGSIZE, (uint64)ustack, PGSIZE, PTE_R | PTE_W | PTE_U);
    proczero->heap_top = USER_BASE;
    void* elf = pmem_alloc(false);
    assert(elf != NULL, "proc_make_first: elf alloc failed");
    memmove(elf, initcode, initcode_len);
    vm_mappages(proczero->pgtbl, proczero->heap_top, (uint64)elf, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    proczero->heap_top += PGSIZE;
    proczero->mmap = NULL;
    proczero->kstack = KSTACK(proczero->pid);
    proczero->tf->user_to_kern_sp = proczero->kstack + PGSIZE;
    proczero->tf->user_to_kern_epc = USER_BASE;
    proczero->tf->sp = TRAPFRAME;
    proczero->ctx.sp = proczero->kstack + PGSIZE;
    mycpu()->proc = proczero;
    spinlock_release(&proczero->lk);
}

/*
    父进程产生子进程
    UNUSED -> RUNNABLE
*/
int proc_fork() {
    proc_t* p = myproc();
    proc_t* c = proc_alloc();
    assert(c != NULL, "proc_fork: c is NULL");
    memmove(c->name, p->name, sizeof(p->name));
    c->name[sizeof(p->name) - 1] = '\0';
    c->state = RUNNABLE;
    c->parent = p;
    void* trapframe = pmem_alloc(false);
    assert(trapframe != NULL, "proc_fork: trapframe alloc failed");
    memmove(trapframe, p->tf, sizeof(trapframe_t));
    c->pgtbl = proc_pgtbl_init((uint64)trapframe);
    c->tf = (trapframe_t*)trapframe;
    c->heap_top = p->heap_top;
    c->ustack_npage = p->ustack_npage;
    c->mmap = NULL;
    mmap_region_t* tmp = p->mmap, * cur = NULL;
    while (tmp) {
        mmap_region_t* new_node = mmap_region_alloc();
        new_node->begin = tmp->begin;
        new_node->npages = tmp->npages;
        if (c->mmap == NULL) c->mmap = new_node, cur = new_node;
        else {
            cur->next = new_node;
            cur = new_node;
            cur->next = NULL;
        }
        tmp = tmp->next;
    }
    c->kstack = KSTACK(c->pid);
    c->tf->user_to_kern_sp = c->kstack + PGSIZE;
    c->tf->user_to_kern_epc = p->tf->user_to_kern_epc;
    c->tf->sp = p->tf->sp;
    c->ctx.sp = c->kstack + PGSIZE;
    c->tf->a0 = 0;
    c->ctx.ra = (uint64)proc_return;
    uvm_copy_pgtbl(p->pgtbl, c->pgtbl, c->heap_top, c->ustack_npage, c->mmap);
    int cid = c->pid;
    spinlock_release(&c->lk);
    return cid;
}

/*
    进程主动放弃CPU控制权
    RUNNING->RUNNABLE
*/
void proc_yield() {
    proc_t* p = myproc();
    assert(p != NULL, "proc_yield: p is NULL");
    spinlock_acquire(&p->lk);
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

/*
    当父进程退出时, 让它的所有子进程认proczero为父
    因为proczero永不退出, 可以回收子进程的资源
*/
static void proc_reparent(proc_t* parent) {
    for (int i = 0;i < N_PROC;i++) {
        proc_t* p = &proc_list[i];
        spinlock_acquire(&p->lk);
        if (p->parent == parent) p->parent = proczero;
        spinlock_release(&p->lk);
    }
}

/*
    唤醒等待呼叫的进程
    由proc_exit调用
    tips: 调用者需要持有p的进程锁
*/
static void proc_try_wakeup(proc_t* p) {
    if (p->state == SLEEPING && (p->sleep_space == NULL || p->sleep_space == p)) {
        p->state = RUNNABLE;
        p->sleep_space = NULL;
    }
}

/*
    进程退出
    RUNNING -> ZOMBIE
*/
void proc_exit(int exit_code) {
    proc_t* p = myproc();
    assert(p != NULL, "proc_exit: p is NULL");
    spinlock_acquire(&p->lk);
    p->exit_code = exit_code;
    p->state = ZOMBIE;
    proc_reparent(p);
    spinlock_acquire(&p->parent->lk);
    proc_try_wakeup(p->parent);
    spinlock_release(&p->parent->lk);
    proc_sched();
}

/*
    父进程等待一个子进程进入ZOMBIE状态
    1. 如果等到: 释放子进程, 返回子进程的pid, 将子进程的退出状态传出到user_addr
    2. 如果发现没孩子: 返回-1
    3. 如果没等到: 父进程进入睡眠状态
*/
int proc_wait(uint64 user_addr) {
    proc_t* p = myproc();
    assert(p != NULL, "proc_wait: p is NULL");
    int have_kid = 0;
    spinlock_acquire(&p->lk);
    while (1) {
        for (int i = 0;i < N_PROC;i++) {
            proc_t* c = &proc_list[i];
            if (c->parent == p) {
                have_kid = 1;
                if (c->state == ZOMBIE) {
                    int pid = c->pid;
                    int exit_code = c->exit_code;
                    proc_free(c);
                    if (user_addr != 0)
                        uvm_copyout(p->pgtbl, user_addr, (uint64)&exit_code, sizeof(exit_code));
                    spinlock_release(&p->lk);
                    return pid;
                }
            }
            spinlock_release(&c->lk);
        }
        if (!have_kid) return -1;
        proc_sleep(p, &p->lk);
    }
}

/*
    进程等待sleep_space对应的资源, 进入睡眠状态
    RUNNING -> SLEEPING
*/
void proc_sleep(void* sleep_space, spinlock_t* lock) {
    proc_t* p = myproc();
    assert(p != NULL, "proc_sleep: p is NULL");
    if (lock && lock != &p->lk) spinlock_release(lock);
    spinlock_acquire(&p->lk);
    p->sleep_space = sleep_space;
    p->state = SLEEPING;
    proc_sched();
    p->sleep_space = NULL;
    spinlock_release(&p->lk);
    if (lock && lock != &p->lk) spinlock_acquire(lock);
}

/*
    唤醒所有等待sleep_space的进程
    SLEEPING -> RUNNABLE
*/
void proc_wakeup(void* sleep_space) {
    for (int i = 0;i < N_PROC;i++) {
        proc_t* p = &proc_list[i];
        spinlock_acquire(&p->lk);
        if (p->state == SLEEPING && p->sleep_space == sleep_space) {
            p->state = RUNNABLE;
            p->sleep_space = NULL;
        }
        spinlock_release(&p->lk);
    }
}

/*
    用户进程切换到调度器
    tips: 调用者保证持有当前进程的锁
*/
void proc_sched() {
    proc_t* p = myproc();
    assert(p != NULL, "proc_sched: p is NULL");
    swtch(&p->ctx, &mycpu()->ctx);
}

/*
    调度器
    RUNNABLE->RUNNING
*/
void proc_scheduler() {
    cpu_t* c = mycpu();
    assert(c != NULL, "proc_scheduler: c is NULL");
    c->proc = NULL;
    while (1) {
        intr_on();
        for (int i = 0;i < N_PROC;i++) {
            proc_t* p = &proc_list[i];
            spinlock_acquire(&p->lk);
            if (p->state == RUNNABLE) {
                p->state = RUNNING;
                c->proc = p;
                swtch(&c->ctx, &p->ctx);
                c->proc = NULL;
            }
            spinlock_release(&p->lk);
        }
    }
}