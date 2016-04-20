/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <synch.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 *
 * NOTE: it's been found over the years that students often begin on
 * the VM assignment by copying dumbvm.c and trying to improve it.
 * This is not recommended. dumbvm is (more or less intentionally) not
 * a good design reference. The first recommendation would be: do not
 * look at dumbvm at all. The second recommendation would be: if you
 * do, be sure to review it from the perspective of comparing it to
 * what a VM system is supposed to do, and understanding what corners
 * it's cutting (there are many) and why, and more importantly, how.
 */

void
vm_bootstrap(void)
{
	/* Do nothing. */
	booted = true;
	mem_lock = lock_create("mem_lock");
}

/*
 * Check if we're in a context that can sleep. While most of the
 * operations in dumbvm don't in fact sleep, in a real VM system many
 * of them would. In those, assert that sleeping is ok. This helps
 * avoid the situation where syscall-layer code that works ok with
 * dumbvm starts blowing up during the VM assignment.
static
void
dumbvm_can_sleep(void)
{

}
*/

static
paddr_t
getppages(unsigned long npages)
{
	unsigned long count = 0, page_counter = ram_getsize() / PAGE_SIZE, i;

	if (booted) {
		lock_acquire(mem_lock);
	}

	for(i = coremap_addr/PAGE_SIZE; i < page_counter && count != npages; i++) {
		if(coremap[i].state == FREE) {
			count++;
		} else {
			count = 0;
		}
	}

	if (count < npages) {
		lock_release(mem_lock);
		return 0;
	} else {
		i--;
	}

	while(count > 0) {
		coremap[i].state = FIXED;

		if (count == 1) {
			coremap[i].chunk_size = npages;
			break;
		}
		i--;
		count--;
	}

	if (booted) {
		lock_release(mem_lock);
	}

	return i * PAGE_SIZE;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(unsigned npages)
{
	(void) npages;
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void
free_kpages(vaddr_t addr)
{
	int index = (addr - MIPS_KSEG0) / PAGE_SIZE;

	if (booted) {
		lock_acquire(mem_lock);
	}

	int pages = coremap[index].chunk_size;

	coremap[index].chunk_size = 0;

	for (int i = 0; i < pages; i++) {
		coremap[index + i].state = FREE;
	}

	if (booted) {
		lock_release(mem_lock);
	}
}

unsigned
int
coremap_used_bytes() {

	int count = 0;

	if (booted) {
		lock_acquire(mem_lock);
	}

	for (unsigned int i = 0 ; i < ram_getsize() / PAGE_SIZE; i++) {
		if (coremap[i].state == FIXED) {
			count++;
		}
	}

	if (booted) {
		lock_release(mem_lock);
	}

	return count * PAGE_SIZE;
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	paddr_t paddr;
	//int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "mipsvm: fault: 0x%x\n", faultaddress);

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	bool found = false;

	struct page_table *temp = as->page_table_entry;
	struct page_table *last_page = as->page_table_entry;

	switch (faulttype) {
		case VM_FAULT_READONLY:
			/* We always create pages read-write, so we can't get this */
			panic("mipsvm: got VM_FAULT_READONLY\n");
		case VM_FAULT_READ:
		case VM_FAULT_WRITE: {

			while (temp != NULL) {
				if ((temp->vpn >> 12) == (faultaddress >> 12)) {
					found = true;
					break;
				}

				last_page = temp;
				temp = temp->next;
			}

			if (!found) {
				temp = (struct page_table *) kmalloc(sizeof(struct page_table));
				temp->vpn = faultaddress;
				temp->ppn = getppages(1);
				temp->next = NULL;
				found = true;

				if (as->page_table_entry == NULL) {
					as->page_table_entry = temp;
				} else {
					last_page->next = temp;
				}
			}

			break;
		}
		default:
			return EINVAL;
	}

	paddr = temp->ppn;
	//paddr = ((temp->ppn << 12) | (faultaddress & ((1 << 13) - 1)));
	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	tlb_random(ehi, elo);
	splx(spl);
	return 0;

}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));

	if (as==NULL) {
		return NULL;
	}

	as->addr_regions = NULL;
	//as->heap_start = 0;
	//as->heap_size = 0;
	as->page_table_entry = NULL;

	return as;
}

void
as_destroy(struct addrspace *as) {

	struct region *address_temp = as->addr_regions;
	struct region *addr_temp;

	while (address_temp != NULL) {
		addr_temp = address_temp->next;
		kfree(address_temp);
		address_temp = addr_temp;
	}

	struct page_table *temp = as->page_table_entry;
	struct page_table *page_temp;

	while (temp != NULL) {
		page_temp = temp->next;
		free_kpages(temp->vpn);
		kfree(temp);
		temp = page_temp;
	}

	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
				 int readable, int writeable, int executable)
{
	//size_t npages;

	(void)readable;
	(void)writeable;
	(void)executable;

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	//npages = sz / PAGE_SIZE;

	struct region *address_temp = as->addr_regions;
	struct region *address_last = NULL;

	while (address_temp != NULL) {
		address_last = address_temp;
		address_temp = address_temp->next;
	}

	address_temp = (struct region *) kmalloc(sizeof(struct region));
	address_temp->region_start = vaddr;
	address_temp->region_size = sz;
	//address_temp->permission = (readable + writeable + executable);

	if (address_last == NULL) {
		as->addr_regions = address_temp;
	} else {
		address_last->next = address_temp;
	}
	as->heap_start = address_temp->region_start + address_temp->region_size;

	return 0;
}

/*
static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	(void) paddr, (void) npages;
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}
*/

int
as_prepare_load(struct addrspace *as)
{
	(void) as;
	//panic("as_prepare_load");
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	//panic("as_complete_load");
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	(void) as;
	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	(void) old, (void) ret;
	//panic("as_copy");
	return 0;
}
