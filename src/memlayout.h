#ifndef MEMLAYOUT_H
#define MEMLAYOUT_H

/* The VM's whole address space is one flat byte array, split into four
 * regions. Pointers are plain 32-bit offsets into it (0 = NULL), so every
 * region must fit comfortably under INT32_MAX - 16 MiB total leaves huge
 * headroom while keeping every bytecode operand and Value payload a single
 * machine word. */
#define MEM_SIZE     (16 * 1024 * 1024)

#define GUARD_SIZE   64                          /* [0, GUARD_SIZE): never allocated, makes NULL (addr 0) safely detectable */
#define DATA_BASE    GUARD_SIZE
#define DATA_SIZE    (2 * 1024 * 1024)           /* globals + string literal bytes, laid out at compile time         */
#define HEAP_BASE    (DATA_BASE + DATA_SIZE)
#define HEAP_SIZE    (6 * 1024 * 1024)           /* malloc/free first-fit free-list arena                            */
#define STACK_BASE   (HEAP_BASE + HEAP_SIZE)
#define STACK_SIZE   (MEM_SIZE - STACK_BASE)     /* call-frame segment: each call gets a contiguous byte-sized frame */

#endif /* MEMLAYOUT_H */
