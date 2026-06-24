#include "mod.h"

// 这个文件通过make build生成, 是proczero对应的ELF文件
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t* old, context_t* new);

// in trap/trap_user.c
extern void trap_user_return();

// 第一个用户进程
static proc_t proczero;

// 获得一个初始化过的用户页表
// 完成trapframe和trampoline的映射
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

注意: 用用户空间的地址映射需要标记 PTE_U
*/
void proc_make_first() {
    proczero.pid = 0;
    void* trapframe = pmem_alloc(true);
    assert(trapframe != NULL, "proc_make_first: trapframe alloc failed");
    proczero.pgtbl = proc_pgtbl_init((uint64)trapframe);
    proczero.tf = (trapframe_t*)trapframe;
    void* ustack = pmem_alloc(false);
    assert(ustack != NULL, "proc_make_first: ustack alloc failed");
    proczero.ustack_npage = 1;
    vm_mappages(proczero.pgtbl, TRAPFRAME - PGSIZE, (uint64)ustack, PGSIZE, PTE_R | PTE_W | PTE_U);
    proczero.heap_top = USER_BASE;
    void* elf = pmem_alloc(false);
    assert(elf != NULL, "proc_make_first: elf alloc failed");
    memmove(elf, initcode, initcode_len);
    vm_mappages(proczero.pgtbl, proczero.heap_top, (uint64)elf, PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U);
    proczero.heap_top += PGSIZE;
    proczero.kstack = KSTACK(0);
    proczero.tf->user_to_kern_sp = proczero.kstack + PGSIZE;
    proczero.tf->user_to_kern_epc = USER_BASE;
    proczero.tf->sp = TRAPFRAME;
    proczero.ctx.ra = (uint64)trap_user_return;
    proczero.ctx.sp = proczero.kstack + PGSIZE;
    mycpu()->proc = &proczero;
    swtch(&mycpu()->ctx, &proczero.ctx);
}