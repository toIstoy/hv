#ifndef _KMALLOC_H_
#define _KMALLOC_H_

void *kmalloc(u64 size);
void kfree(void *p);

int init_kmalloc(void);

#endif /* !_KMALLOC_H_ */
