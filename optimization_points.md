## 10 Areas for Kernel Optimization

1. **Memory - Replace Bump Allocator in `heap.c`:**
   The kernel currently uses a simple bump allocator for `kmalloc` (`kernel/mem/heap.c`). The `free` function is an empty stub (no-op). This means the kernel heap can only grow and will eventually exhaust memory. Implementing a proper memory allocator (like a slab allocator or a free-list based allocator) will significantly improve long-term memory utilization and stability.

2. **Memory - Optimize PMM Search in `pmm.c`:**
   The physical memory manager (`kernel/mem/pmm.c`) uses a simple linear search (`for (uint32_t i = 0; i < total_pages; ++i)`) starting from page 0 every time a physical page is allocated (`PMM_AllocatePhysicalPage`). This is O(N) where N is the total number of pages. Storing the index of the last allocated/freed page and starting the search from there, or maintaining a stack/list of free pages, would reduce allocation overhead to O(1) in the average case.

3. **Memory - Optimize bitmap updates in `pmm.c`:**
   `bitmap_set`, `bitmap_clear`, and `bitmap_is_set` in `kernel/mem/pmm.c` operate on a bit-by-bit basis inside loops. Operations could be optimized by checking or setting entire bytes (or 32-bit words) at once to quickly skip fully allocated sections of physical memory.

4. **Scheduling - Improve O(N) Scheduler Search in `scheduler.c`:**
   The scheduler (`kernel/cpu/scheduler.c`) uses an O(N) array-based linear search to find the next runnable process (`Scheduler_GetNextRunnableProcess`). It iterates over all registered processes. Transitioning to linked lists (or priority queues) for ready, blocked, and sleeping processes would make finding the next runnable task O(1).

5. **Filesystem - FAT Directory Traversal Limits:**
   In `kernel/fs/fat/fat.c`, there are multiple instances where directory traversal or cluster chain following uses arbitrary loop limits (e.g., `if (++loop_counter > 10000)` or `sectorsScanned < maxSectorsToScan (4096)`). While this prevents infinite loops, it represents a hardcoded limitation that could break with large files or directories, and adds overhead. A more robust state-tracking approach or caching mechanism would be safer and potentially more performant.

6. **Filesystem - VFS Path Parsing and Allocations:**
   The VFS and FAT drivers make frequent use of `kmalloc` and `free` during path parsing (e.g., in `VFS_Open`, `FAT_Open`). Given that `free` is a no-op currently, this causes rapid heap exhaustion. Even with a working `free`, repeatedly allocating small strings on the heap during path resolution is slow. Passing pre-allocated buffers or keeping parsing inline on the stack could reduce this overhead.

7. **Storage - ATA Driver PIO Polling and Delays:**
   The ATA driver (`kernel/drivers/ata/ata.c`) heavily relies on PIO mode with busy-waiting loops (e.g., `for (volatile int i = 0; i < 100; i++)`). It reads words using `inw` in tight loops instead of string I/O instructions like `insw`/`outsw`. Replacing explicit loops with block I/O instructions or transitioning to DMA (Direct Memory Access) and interrupt-driven I/O would significantly reduce CPU utilization during disk operations.

8. **Standard Library - Naive String Implementations:**
   Functions in `kernel/std/string.c` such as `strlen`, `strcpy`, and `strcmp` process strings byte-by-byte. These could be optimized by aligning pointers and processing multiple bytes at once (e.g., 32-bit or 64-bit word chunks), similar to how `memcpy` and `memset` are optimized in assembly (`kernel/arch/i686/mem/memory_asm.S`).

9. **Device Drivers - Floppy Disk Controller Busy-Waiting:**
   The FDC driver (`kernel/drivers/fdc/fdc.c`) contains extreme busy-waiting loops (e.g., `for (volatile int i = 0; i < 500000; i++);`) to simulate delays for the floppy motor to spin up. These volatile loops waste CPU cycles and do not represent a predictable real-time delay. They should be replaced with timer-based sleeps (using the scheduler to yield the task or wait for a timer interrupt).

10. **Hardware Abstraction - Inline I/O Ports:**
    The system makes widespread use of function calls to read/write hardware ports (`g_HalIoOperations->inb()`). In architecture-specific drivers like ATA, using inline assembly for I/O ports would eliminate the overhead of function pointers and indirect jumps, which can stall the pipeline and reduce overall driver throughput.
