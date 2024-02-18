#include <compiler.h>
#include <io.h>
#include <interrupts.h>
#include <string.h>
#include <stdio.h>
#include <multiboot2.h>
#include <page.h>
#include <pci.h>
#include <panic.h>
#include <memory.h>
#include <kmalloc.h>
#include <vmx.h>
#include <vmx_guest.h>

static int multiboot2_valid(u32 magic, u32 info_addr)
{
	if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
		printf("Please use a multiboot2 compliant bootloader\n");
		return 0;
	}

	if (__align(info_addr, MULTIBOOT_INFO_ALIGN) != info_addr) {
		printf("Unaligned MBI: %#x\n", info_addr);
		return 0;
	}

	return 1;
}

static inline struct multiboot_tag *multiboot_tag_start(vaddr_t info_addr)
{
	return (struct multiboot_tag *)(info_addr + 8);
}

static inline struct multiboot_tag *multiboot_tag_next(struct multiboot_tag *tag)
{
	u32 size = __align_n(tag->size, MULTIBOOT_INFO_ALIGN);
	return (struct multiboot_tag *)((u8 *)tag + size);
}

static inline int multiboot_tag_end(struct multiboot_tag *tag)
{
	return tag->type == MULTIBOOT_TAG_TYPE_END;
}

static void *get_multiboot_infos(vaddr_t info_addr, u8 tag_type)
{
	struct multiboot_tag *tag = multiboot_tag_start(info_addr);
	for (; !multiboot_tag_end(tag); tag = multiboot_tag_next(tag))
		if (tag->type == tag_type)
			return tag;

	return NULL;
}

#ifdef DEBUG
static const char *multiboot_mmap_entry_types[] = {
	[1] = "AVAILABLE",
	[2] = "RESERVED",
	[3] = "ACPI_RECLAIMABLE",
	[4] = "NVS",
	[5] = "BADRAM",
};

static void dump_memory_map(struct multiboot_tag_mmap *mmap)
{
	multiboot_memory_map_t *m = mmap->entries;
	while ((u8 *)m < (u8 *)mmap + mmap->size) {
		printf("base_addr=%#lx, length=%#lx type=%s\n",
				m->addr,
				m->len,
				multiboot_mmap_entry_types[m->type]);

		m = (multiboot_memory_map_t *)((u8 *)m + mmap->entry_size);
	}
}
#endif

static struct multiboot_tag_module *multiboot_get_module(vaddr_t info_addr,
							 const char *name)
{
	struct multiboot_tag *tag = multiboot_tag_start(info_addr);
	for (; !multiboot_tag_end(tag); tag = multiboot_tag_next(tag)) {
		if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
			struct multiboot_tag_module *mod = (void *)tag;
			if (strstr(mod->cmdline, name))
				return mod;
		}
	}
	return NULL;
}

static inline
struct multiboot_tag_module *multiboot_get_linux_module(vaddr_t info_addr)
{
	return multiboot_get_module(info_addr, "linux");
}

static inline
struct multiboot_tag_module *multiboot_get_linux_initramfs(vaddr_t info_addr)
{
	return multiboot_get_module(info_addr, "initramfs");
}

void hyper_main(u32 magic, u32 info_addr)
{
	if (!multiboot2_valid(magic, info_addr))
		panic("Invalid multiboot informations\n");

	vaddr_t mbi_addr = va(info_addr);
	struct multiboot_tag_mmap *mmap = get_multiboot_infos(mbi_addr,
					  MULTIBOOT_TAG_TYPE_MMAP);
	if (!mmap)
		panic("Unable to retrieve multiboot memory map\n");

	struct multiboot_tag_module *mod = multiboot_get_linux_module(mbi_addr);
	struct multiboot_tag_module *init = multiboot_get_linux_initramfs(mbi_addr);

	if ((void *)mod == NULL || (void *)init == NULL)
		panic("Unable to retrieve bzImage or initramfs\n");

#ifdef DEBUG
	dump_memory_map(mmap);
#endif
	init_idt();
	load_tss();

	memory_init(mmap, va(mod->mod_end));
	init_kmalloc();

#if 0
	pci_register_drivers();
	struct pci_bus pci_bus = {
		.num = 0
	};
	init_pci_bus(&pci_bus);
#endif

#ifndef DEBUG
	if (!has_vmx_support())
		panic("VMX is not supported by this CPU.\n");
#endif

	struct vmm vmm = {
		.setup_guest = setup_linux_guest,
		.guest_img = {
			.start = phys_to_virt(mod->mod_start),
			.end   = phys_to_virt(mod->mod_end),
		},
		.guest_initrd = {
			.start = phys_to_virt(init->mod_start),
			.end   = phys_to_virt(init->mod_end),
		},
	};

	vmm_init(&vmm);

	panic("VMM initialization failed\n");

}
