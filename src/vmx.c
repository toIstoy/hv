#include <cpuid.h>	/* compiler header */

#include <compiler.h>
#include <gdt.h>
#include <kmalloc.h>
#include <page.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>

#include <vmx.h>

#ifndef __clang__
#define bit_VMX	0x20
#endif

/* TSS definition is not exported */
extern char tss[];

struct vmcs {
	u32	rev_id;
	u32	vmx_abort;
	u8	data[0];
} __packed;

int has_vmx_support(void)
{
	u32 eax, ebx, ecx, edx;

	/* CPUID support has already been tested in bootstrap */
	__cpuid(1, eax, ebx, ecx, edx);
	if (!(ecx & bit_VMX))
		return 0;

	u64 features = __readmsr(MSR_FEATURE_CONTROL);
	if (!(features & MSR_FEATURE_CONTROL_LOCK))
		return 0;
	if (!(features & MSR_FEATURE_CONTROL_VMXON_OUTSIDE_SMX))
		return 0;

	return 1;
}

static inline void vmm_read_vmx_msrs(struct vmm *vmm)
{
	for (u64 i = 0; i < NR_VMX_MSR; ++i)
		vmm->vmx_msr[i] = __readmsr(MSR_VMX_BASIC + i);
}

#define VMCS_NB_PAGES 2
static int alloc_vmcs(struct vmm *vmm)
{
	void *mem = alloc_pages(VMCS_NB_PAGES);
	if (mem == NULL)
		return 1;
	memset(mem, 0, VMCS_NB_PAGES * PAGE_SIZE);
	vmm->vmx_on = mem;
	vmm->vmcs = (vaddr_t)mem + PAGE_SIZE;
	return 0;
}

static inline void release_vmcs(struct vmm *vmm)
{
	release_pages(vmm->vmx_on, VMCS_NB_PAGES);
}

/* Write back caching */
#define EPT_MEMORY_TYPE_WB	0x6

static void setup_eptp(struct eptp *eptp, struct ept_pml4e *ept_pml4)
{
	eptp->quad_word = 0;
	eptp->type = EPT_MEMORY_TYPE_WB; /* imposed by KVM */
	eptp->page_walk_length = 3;	 /* same */
	eptp->enable_dirty_flag = 0;
	eptp->pml4_addr = virt_to_phys(ept_pml4) >> PAGE_SHIFT;
}

static inline void ept_set_pte_rwe(void *ept_pte)
{
	struct ept_pte *pte = (struct ept_pte *)ept_pte;
	pte->read = 1;
	pte->write = 1;
	pte->kern_exec = 1;
}

static inline void ept_init_default(void *ept_pte, paddr_t paddr)
{
	ept_set_pte_rwe(ept_pte);
	struct ept_pte *pte = ept_pte;
	pte->paddr = paddr >> PAGE_SHIFT;
}

static void ept_init_pt(struct ept_pte *ept_pt, paddr_t host, paddr_t guest)
{
	u16 pte_off = pte_offset(guest);
	for (; pte_off < EPT_PTRS_PER_TABLE; ++pte_off, host += PAGE_SIZE) {
		struct ept_pte *cur_pte = ept_pt + pte_off;
		ept_set_pte_rwe(cur_pte);
		cur_pte->memory_type = EPT_MEMORY_TYPE_WB;
		cur_pte->ignore_pat = 1;
		cur_pte->paddr = host >> PAGE_SHIFT;
	}
}

static u64 needed_paging_structs(const u64 mem_size, const u64 mapped_size)
{
	u64 n = mem_size / mapped_size;
	if (n == 0)
		return 1;
	if (mem_size % mapped_size != 0)
		n += 1;
	return n;
}

/*
 * Build EPT structures. Only RWE mappings atm.
 * XXX: atm, KVM only support nested EPT translations using 4 level structures
 * (not more not less), so we'll stick to that. 512G max supported here.
 */
static int ept_setup_range(struct vmm *vmm, paddr_t host_start,
			    paddr_t host_end, paddr_t guest_start)
{
	const u64 mmap_size = host_end - host_start;
	if (mmap_size > GB(512))
		return 1;

	struct ept_pml4e *ept_pml4 = alloc_page();
	if (ept_pml4 == NULL)
		return 1;
	struct ept_pdpte *ept_pdpt = alloc_page();
	if (ept_pdpt == NULL)
		goto free_pml4;


	/* Init pml4 with pdpt physical address */
	struct ept_pml4e *pml4e = ept_pml4 + pgd_offset(guest_start);
	ept_init_default(pml4e, virt_to_phys(ept_pdpt));

	/* Make ept_pdpt point to the first entry we fill */
	const u64 pdpt_off = pud_offset(guest_start);
	ept_pdpt += pdpt_off;

	const u64 needed_pd = needed_paging_structs(mmap_size, GB(1));
	u64 needed_pt = needed_paging_structs(mmap_size, MB(2));
	if (pdpt_off + needed_pd >= EPT_PTRS_PER_TABLE)
		goto free_pdpt;

	struct ept_pde *ept_pd = alloc_pages(needed_pd);
	if (ept_pd == NULL)
		goto free_pdpt;
	struct ept_pte *ept_pt = alloc_pages(needed_pt);
	if (ept_pt == NULL)
		goto free_pd;

	memset(ept_pd, 0, needed_pd * PAGE_SIZE);
	memset(ept_pt, 0, needed_pt * PAGE_SIZE);

	/* Init PDPT */
	for (u64 i = 0; i < needed_pd; ++i) {
		struct ept_pde *cur_pd = (vaddr_t)ept_pd + i * PAGE_SIZE;
		ept_init_default(ept_pdpt + i, virt_to_phys(cur_pd));

		/* Init page directory */
		for (u64 j = 0; j < EPT_PTRS_PER_TABLE && j < needed_pt; ++j) {
			struct ept_pde *pde = cur_pd + pmd_offset(guest_start);
			struct ept_pte *pt = (vaddr_t)ept_pt + j * PAGE_SIZE;
			ept_init_default(pde, virt_to_phys(pt));

			/* Init page table for current page dir entry */
			ept_init_pt(pt, host_start, guest_start);

			host_start += MB(2);
			guest_start += MB(2);
		}

		needed_pt -= EPT_PTRS_PER_TABLE;
	}

	setup_eptp(&vmm->eptp, ept_pml4);
	return 0;

free_pd:
	release_pages(ept_pd, needed_pd);
free_pdpt:
	release_page(ept_pdpt);
free_pml4:
	release_page(ept_pml4);
	return 1;
}

/* XXX: ATM allocates 200M of memory for the VM */
static int setup_ept(struct vmm *vmm)
{
	u8 nb_pages = 100;
	void *p = alloc_huge_pages(nb_pages);
	if (p == NULL)
		return 1;

	paddr_t start = virt_to_phys(p);
	paddr_t end = start + nb_pages * HUGE_PAGE_SIZE;
	if (ept_setup_range(vmm, start, end, 0))
		return 1;
	vmm->guest_mem.start = p;
	vmm->guest_mem.end = phys_to_virt(end);
	return 0;
}

/* GPA -> HPA */
hpa_t ept_translate(struct vmm *vmm, gpa_t addr)
{
	struct eptp *eptp = &vmm->eptp;

	paddr_t pgd_addr = eptp->pml4_addr << PAGE_SHIFT;
	struct ept_pml4e *pgd = (void *)phys_to_virt(pgd_addr);
	u16 pgd_off = pgd_offset(addr);
	if (!pg_present(pgd[pgd_off].quad_word))
		return (paddr_t)-1;

	paddr_t pud_addr = (paddr_t)(pgd[pgd_off].quad_word & PAGE_MASK);
	struct ept_pdpte *pud = (void *)phys_to_virt(pud_addr);
	u16 pud_off = pud_offset(addr);
	if (!pg_present(pud[pud_off].quad_word))
		return (paddr_t)-1;

	paddr_t pmd_addr = (paddr_t)(pud[pud_off].quad_word & PAGE_MASK);
	struct ept_pde *pmd = (void *)phys_to_virt(pmd_addr);
	u16 pmd_off = pmd_offset(addr);
	if (!pg_present(pmd[pmd_off].quad_word))
		return (paddr_t)-1;

	paddr_t pt_addr = (paddr_t)(pmd[pmd_off].quad_word & PAGE_MASK);
	struct ept_pte *pte = (void *)phys_to_virt(pt_addr);
	u16 pte_off = pte_offset(addr);
	if (!pg_present(pte[pte_off].quad_word))
		return (paddr_t)-1;

	u16 page_off = addr & ~PAGE_MASK;
	return (hpa_t)((pte[pte_off].quad_word & PAGE_MASK) + page_off);
}

hva_t gpa_to_hva(struct vmm *vmm, gpa_t gpa)
{
	hpa_t hpa = ept_translate(vmm, gpa);
	return (hva_t)phys_to_virt(hpa);
}

/* XXX: Only works with 64 bit paging backed with EPT */
/* Guest virtual to guest physical */
gpa_t gva_to_gpa(struct vmm *vmm, gva_t gva)
{
	u64 guest_cr3 = vmm->guest_state.reg_state.control_regs.cr3 & PAGE_MASK;

	hva_t *guest_pgd = (hva_t *)gpa_to_hva(vmm, guest_cr3);
	gpa_t pml4e = guest_pgd[pgd_offset(gva)];
	if (!pg_present(pml4e))
		return (gpa_t)-1;

	hva_t *guest_pud = (hva_t *)gpa_to_hva(vmm, pml4e & PAGE_MASK);
	gpa_t pdpte = guest_pud[pud_offset(gva)];
	if (!pg_present(pdpte))
		return (gpa_t)-1;
	if (pg_huge_page(pdpte))
		return (pdpte & PAGE_MASK) + (gva & ~PUD_MASK);

	hva_t *guest_pmd = (hva_t *)gpa_to_hva(vmm, pdpte & PAGE_MASK);
	gpa_t pde = guest_pmd[pmd_offset(gva)];
	if (!pg_present(pde))
		return (gpa_t)-1;
	if (pg_huge_page(pde))
		return (pde & PAGE_MASK) + (gva & ~PMD_MASK);

	hva_t *guest_pt = (hva_t *)gpa_to_hva(vmm, pde & PAGE_MASK);
	gpa_t pte = guest_pt[pte_offset(gva)];
	if (!pg_present(pte))
		return (gpa_t)-1;

	return (pte & PAGE_MASK) + (gva & ~PAGE_MASK);
}

hva_t gva_to_hva(struct vmm *vmm, gva_t gva)
{
	gpa_t gpa = gva_to_gpa(vmm, gva);
	if (gpa == (gpa_t)-1)
		return (hva_t)-1;

	return gpa_to_hva(vmm, gpa);
}

static void vmcs_get_host_selectors(struct segment_selectors *sel)
{
	sel->cs = read_cs();
	sel->ds = read_ds();
	sel->es = read_es();
	sel->ss = read_ss();
	sel->fs = read_fs();
	sel->gs = read_gs();
	sel->tr = __str();
}

static void vmcs_get_control_regs(struct control_regs *regs)
{
	regs->cr0 = read_cr0();
	regs->cr3 = read_cr3();
	regs->cr4 = read_cr4();
}

static void vmcs_fill_msr_state(struct vmcs_state_msr *msr)
{
	msr->ia32_fs_base = __readmsr(MSR_FS_BASE);
	msr->ia32_gs_base = __readmsr(MSR_GS_BASE);
	msr->ia32_sysenter_cs = __readmsr(MSR_SYSENTER_CS);
	msr->ia32_sysenter_esp = __readmsr(MSR_SYSENTER_ESP);
	msr->ia32_sysenter_eip = __readmsr(MSR_SYSENTER_EIP);
	msr->ia32_perf_global_ctrl = __readmsr(MSR_PERF_GLOBAL_CTRL);
	msr->ia32_pat = __readmsr(MSR_PAT);
	msr->ia32_efer = __readmsr(MSR_EFER);
	msr->ia32_debugctl = __readmsr(MSR_DEBUGCTL);
#if 0
	msr->ia32_bndcfgs = __readmsr(MSR_BNDCFGS);
#endif
}

#define VM_EXIT_STACK_SIZE	(PAGE_SIZE - 32)
extern void vm_exit_stub(void);
static int vmcs_get_host_state(struct vmcs_host_state *state)
{
	vmcs_get_control_regs(&state->control_regs);
	vmcs_get_host_selectors(&state->selectors);

	struct gdtr gdtr;
	__sgdt(&gdtr);
	state->gdtr_base = gdtr.base;

	__sidt(&gdtr); /* gdtr has the same memory layout */
	state->idtr_base = gdtr.base;
	state->tr_base = (u64)tss;

	vmcs_fill_msr_state(&state->msr);

	void *ptr = kmalloc(PAGE_SIZE);
	if (ptr == NULL)
		return 1;
	state->rsp = (u64)ptr + VM_EXIT_STACK_SIZE;
	state->rip = (u64)vm_exit_stub;
	return 0;
}

static inline void host_set_stack_ctx(struct vmm *vmm)
{
	*((void **)vmm->host_state.rsp) = (void *)vmm;
}

#define VMM_IDX(idx) 		((idx) - MSR_VMX_BASIC)
#define VMM_MSR_VMX_BASIC	VMM_IDX(MSR_VMX_BASIC)
#define VMM_MSR_VMX_CR0_FIXED0	VMM_IDX(MSR_VMX_CR0_FIXED0)
#define VMM_MSR_VMX_CR0_FIXED1	VMM_IDX(MSR_VMX_CR0_FIXED1)
#define VMM_MSR_VMX_CR4_FIXED0	VMM_IDX(MSR_VMX_CR4_FIXED0)
#define VMM_MSR_VMX_CR4_FIXED1	VMM_IDX(MSR_VMX_CR4_FIXED1)

static inline void vmcs_write_control(struct vmm *vmm, enum vmcs_field field,
				      u64 ctl, u64 ctl_msr)
{
	u64 ctl_mask = vmm->vmx_msr[VMM_IDX(ctl_msr)];
	__vmwrite(field, adjust_vm_control(ctl, ctl_mask));
}

static inline void vmcs_write_pin_based_ctrls(struct vmm *vmm, u64 ctl)
{
	vmcs_write_control(vmm, PIN_BASED_VM_EXEC_CONTROL, ctl,
			   MSR_VMX_TRUE_PIN_CTLS);
}

static inline void vmcs_write_proc_based_ctrls(struct vmm *vmm, u64 ctl)
{
	vmcs_write_control(vmm, CPU_BASED_VM_EXEC_CONTROL, ctl,
			   MSR_VMX_TRUE_PROC_CTLS);
}

static inline void vmcs_write_proc_based_ctrls2(struct vmm *vmm, u64 ctl)
{

	vmcs_write_control(vmm, SECONDARY_VM_EXEC_CONTROL, ctl,
			   MSR_VMX_PROC_CTLS2);
}

#define EXCEPTION_UD	(1ull << 6)
#define EXCEPTION_PF	(1ull << 14)
#define EXCEPTION_BITMAP_MASK	~(EXCEPTION_PF|EXCEPTION_UD)
static void vmcs_write_vm_exec_controls(struct vmm *vmm)
{
	vmcs_write_pin_based_ctrls(vmm, 0);

	u64 proc_flags1 = VM_EXEC_USE_MSR_BITMAPS|VM_EXEC_ENABLE_PROC_CTLS2|
			  VM_EXEC_CR3_LOAD_EXIT|VM_EXEC_UNCONDITIONAL_IO_EXIT;
	u64 proc_flags2 = VM_EXEC_UNRESTRICTED_GUEST|VM_EXEC_ENABLE_EPT;
	vmcs_write_proc_based_ctrls(vmm, proc_flags1);
	vmcs_write_proc_based_ctrls2(vmm, proc_flags2);

	__vmwrite(EXCEPTION_BITMAP, EXCEPTION_BITMAP_MASK);
	__vmwrite(MSR_BITMAP, virt_to_phys((vaddr_t)vmm->msr_bitmap));

	u64 guest_cr0 = vmm->guest_state.reg_state.control_regs.cr0;
	__vmwrite(CR0_READ_SHADOW, guest_cr0);
	__vmwrite(CR0_GUEST_HOST_MASK, guest_cr0);

	u64 guest_cr4 = vmm->guest_state.reg_state.control_regs.cr4;
	__vmwrite(CR4_READ_SHADOW, guest_cr4);
	__vmwrite(CR4_GUEST_HOST_MASK, guest_cr4);

	__vmwrite(EPT_POINTER, vmm->eptp.quad_word);
}

static void vmcs_write_vm_exit_controls(struct vmm *vmm)
{
	vmcs_write_control(vmm, VM_EXIT_CONTROLS,
			   VM_EXIT_LONG_MODE|VM_EXIT_SAVE_MSR_EFER,
			   MSR_VMX_TRUE_EXIT_CTLS);
}

/* TODO check if guest is in LM or PM */
static void vmcs_write_vm_entry_controls(struct vmm *vmm)
{
	vmcs_write_control(vmm, VM_ENTRY_CONTROLS, VM_ENTRY_LOAD_MSR_EFER,
			   MSR_VMX_TRUE_ENTRY_CTLS);
}

static void vmcs_write_control_regs(struct control_regs *regs, int host)
{
	enum vmcs_field base_field;
	if (host)
		base_field = HOST_CR0;
	else
		base_field = GUEST_CR0;

	__vmwrite(base_field, regs->cr0);
	__vmwrite(base_field + 2, regs->cr3);
	__vmwrite(base_field + 4, regs->cr4);
}

static void vmcs_write_vm_host_state(struct vmm *vmm)
{
	vmcs_write_control_regs(&vmm->host_state.control_regs, 1);

	__vmwrite(HOST_CS_SELECTOR, vmm->host_state.selectors.cs);
	__vmwrite(HOST_DS_SELECTOR, vmm->host_state.selectors.ds);
	__vmwrite(HOST_ES_SELECTOR, vmm->host_state.selectors.es);
	__vmwrite(HOST_SS_SELECTOR, vmm->host_state.selectors.ss);
	__vmwrite(HOST_FS_SELECTOR, vmm->host_state.selectors.fs);
	__vmwrite(HOST_GS_SELECTOR, vmm->host_state.selectors.gs);
	__vmwrite(HOST_TR_SELECTOR, vmm->host_state.selectors.tr);

	__vmwrite(HOST_TR_BASE, vmm->host_state.tr_base);
	__vmwrite(HOST_GDTR_BASE, vmm->host_state.gdtr_base);
	__vmwrite(HOST_IDTR_BASE, vmm->host_state.idtr_base);
	__vmwrite(HOST_FS_BASE, vmm->host_state.msr.ia32_fs_base);
	__vmwrite(HOST_GS_BASE, vmm->host_state.msr.ia32_gs_base);

	__vmwrite(HOST_SYSENTER_CS, vmm->host_state.msr.ia32_sysenter_cs);
	__vmwrite(HOST_SYSENTER_ESP, vmm->host_state.msr.ia32_sysenter_esp);
	__vmwrite(HOST_SYSENTER_EIP, vmm->host_state.msr.ia32_sysenter_eip);

	__vmwrite(HOST_PERF_GLOBAL_CTRL, vmm->host_state.msr.ia32_perf_global_ctrl);
	__vmwrite(HOST_PAT, vmm->host_state.msr.ia32_pat);
	__vmwrite(HOST_EFER, vmm->host_state.msr.ia32_efer);

	__vmwrite(HOST_RSP, vmm->host_state.rsp);
	__vmwrite(HOST_RIP, vmm->host_state.rip);
}

static inline enum vmcs_field sel_offset(enum vmcs_field field)
{
	return field - GUEST_ES_SELECTOR;
}

static inline enum vmcs_field sel_limit(enum vmcs_field field)
{
	return sel_offset(GUEST_ES_LIMIT) + field;
}

static inline enum vmcs_field sel_access(enum vmcs_field field)
{
	return sel_offset(GUEST_ES_AR_BYTES) + field;
}

static inline enum vmcs_field sel_base(enum vmcs_field field)
{
	return sel_offset(GUEST_ES_BASE) + field;
}

static void vmcs_write_guest_selector(struct segment_descriptor *desc)
{
	enum vmcs_field field = desc->base_field;
	__vmwrite(field, desc->selector);
	__vmwrite(sel_limit(field), desc->limit);
	__vmwrite(sel_access(field), desc->access);
	__vmwrite(sel_base(field), desc->base);
}

static void vmcs_write_guest_selectors(struct segment_descriptors *desc)
{
	vmcs_write_guest_selector(&desc->cs);
	vmcs_write_guest_selector(&desc->ds);
	vmcs_write_guest_selector(&desc->es);
	vmcs_write_guest_selector(&desc->ss);
	vmcs_write_guest_selector(&desc->fs);
	vmcs_write_guest_selector(&desc->gs);
	vmcs_write_guest_selector(&desc->tr);
	vmcs_write_guest_selector(&desc->ldtr);
}

static void vmcs_write_guest_reg_state(struct vmcs_guest_register_state *state)
{
	vmcs_write_control_regs(&state->control_regs, 0);
	vmcs_write_guest_selectors(&state->seg_descs);

	__vmwrite(GUEST_GDTR_BASE, state->gdtr.base);
	__vmwrite(GUEST_IDTR_BASE, state->idtr.base);
	__vmwrite(GUEST_GDTR_LIMIT, state->gdtr.limit);
	__vmwrite(GUEST_IDTR_LIMIT, state->idtr.limit);

	__vmwrite(GUEST_SYSENTER_CS, state->msr.ia32_sysenter_cs);
	__vmwrite(GUEST_SYSENTER_ESP, state->msr.ia32_sysenter_esp);
	__vmwrite(GUEST_SYSENTER_EIP, state->msr.ia32_sysenter_eip);

	__vmwrite(GUEST_PAT, state->msr.ia32_pat);
	__vmwrite(GUEST_EFER, state->msr.ia32_efer);
	__vmwrite(GUEST_BNDCFGS, state->msr.ia32_bndcfgs);
	__vmwrite(GUEST_DEBUGCTL, state->msr.ia32_debugctl);
	__vmwrite(GUEST_PERF_GLOBAL_CTRL, state->msr.ia32_perf_global_ctrl);
	__vmwrite(GUEST_DR7, state->dr7);

	__vmwrite(GUEST_RFLAGS, state->regs.rflags);
	__vmwrite(GUEST_RSP, state->regs.rsp);
	__vmwrite(GUEST_RIP, state->regs.rip);

	__vmwrite(GUEST_ACTIVITY_STATE, 0);
	__vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
}

static void vmcs_write_guest_state(struct vmcs_guest_state *state)
{
	vmcs_write_guest_reg_state(&state->reg_state);

	__vmwrite(VMCS_LINK_POINTER, state->vmcs_link);
}

static inline void vmcs_write_vm_guest_state(struct vmm *vmm)
{
	vmcs_write_guest_state(&vmm->guest_state);
}

#define MSR_BITMAP_SZ		1024
#define MSR_ALL_BITMAP_SZ	(4 * MSR_BITMAP_SZ)
#define MSR_BITMAP_READ_LO	0
#define MSR_BITMAP_READ_HI	(MSR_BITMAP_READ_LO + MSR_BITMAP_SZ)
#define MSR_BITMAP_WRITE_LO	(MSR_BITMAP_READ_HI + MSR_BITMAP_SZ)
#define MSR_BITMAP_WRITE_HI	(MSR_BITMAP_WRITE_LO + MSR_BITMAP_SZ)

static int init_msr_bitmap(struct vmm *vmm)
{
	vmm->msr_bitmap = alloc_page();
	if (vmm->msr_bitmap != NULL)
		memset(vmm->msr_bitmap, 0, MSR_ALL_BITMAP_SZ);
	return vmm->msr_bitmap == NULL;
}

/* Hack to launch linux with correct reg state, this is really ugly. */
static int launch_vm(struct vmm *vmm)
{
	struct vmcs_guest_register_state *state = &vmm->guest_state.reg_state;
	asm volatile goto ("movq %3, %%rbp\n\t"
			   "vmlaunch\n\t"
			   "jbe %l4"
			   : /* No output */
			   : "S"(state->regs.rsi), "D"(state->regs.rdi),
			     "r"(state->regs.rbp), "b"(state->regs.rbx)
			   : "memory"
			   : fail
			  );
	return 0;
fail:
	return 1;
}

int vmm_init(struct vmm *vmm)
{
	vmm_read_vmx_msrs(vmm);
	if (alloc_vmcs(vmm))
		return 1;

	u32 rev_id = vmm->vmx_msr[VMM_MSR_VMX_BASIC] & 0x7fffffff;
	vmm->vmcs->rev_id = rev_id;
	vmm->vmx_on->rev_id = rev_id;

	u64 cr0 = read_cr0();
	cr0 |= vmm->vmx_msr[VMM_MSR_VMX_CR0_FIXED0];
	cr0 &= vmm->vmx_msr[VMM_MSR_VMX_CR0_FIXED1];
	write_cr0(cr0);

	u64 cr4 = read_cr4();
	cr4 |= CR4_VMXE;
	cr4 |= vmm->vmx_msr[VMM_MSR_VMX_CR4_FIXED0];
	cr4 &= vmm->vmx_msr[VMM_MSR_VMX_CR4_FIXED1];
	write_cr4(cr4);

	if (vmcs_get_host_state(&vmm->host_state)) {
		printf("Failed to setup host state\n");
		goto free_vmcs;
	}

	host_set_stack_ctx(vmm);

	if (setup_ept(vmm)) {
		printf("Failed to setup EPT\n");
		goto free_host;
	}

	if (init_msr_bitmap(vmm))
		goto free_host;

	vmm->setup_guest(vmm);
	init_vm_exit_handlers(vmm);

	if (__vmxon(virt_to_phys(vmm->vmx_on))) {
		printf("VMXON failed\n");
		goto free_msr;
	}

	paddr_t vmcs_paddr = virt_to_phys(vmm->vmcs);
	if (__vmclear(vmcs_paddr)) {
		printf("VMCLEAR failed\n");
		goto free_vmxoff;
	}

	if (__vmptrld(vmcs_paddr)) {
		printf("VMPTRLD failed\n");
		goto free_vmxoff;
	}

	vmcs_write_vm_exec_controls(vmm);
	vmcs_write_vm_exit_controls(vmm);
	vmcs_write_vm_entry_controls(vmm);
	vmcs_write_vm_host_state(vmm);

#ifdef DEBUG
	dump_guest_state(&vmm->guest_state);
#endif

	vmcs_write_vm_guest_state(vmm);

	printf("Hello from VMX ROOT\n");
	printf("Entering guest ...\n");

	if (launch_vm(vmm)) {
		printf("VMLAUNCH failed\n");
		goto free_vmxoff;
	}

	return 0;

free_vmxoff:
	__vmxoff();
free_msr:
	release_page(vmm->msr_bitmap);
free_host:
	kfree((void *)(vmm->host_state.rsp - VM_EXIT_STACK_SIZE));
free_vmcs:
	release_vmcs(vmm);
	return 1;
}
