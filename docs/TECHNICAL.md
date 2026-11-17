# Technical notes

This document explains the crash, how it was investigated, and how `sekiro_crashfix` works. All addresses are for `sekiro.exe` 1.6.0.0 (Steam), which loads at `0x140000000`. Offsets are written as `sekiro+RVA`.

## 1. The game's allocator

The crashes all happen inside one of the game's own heap allocators.

| Function | Address | Role |
|---|---|---|
| alloc | `sekiro+1A3FAD0` (aligned variants `+1A3FF20`, `+1A40910`) | allocate a block |
| free | `sekiro+1A40DA0` | free a block, merging it with free neighbours |
| locked wrappers | `sekiro+1159E80` (alloc), `sekiro+1158840` (free), `sekiro+115A010` (realloc) | take the heap lock and call the above |
| tree erase / insert | `sekiro+1AA5610` / `sekiro+1AA54D0` | red-black tree of large free blocks |

The heap object that gets corrupted is at `sekiro+3B182D8`. Its fields used here:

- `+0x18`: the small-bin array. Free blocks up to 0x1F8 bytes go into doubly linked lists.
- `+0x20`: the root of a red-black tree holding the larger free blocks.
- `+0x28` / `+0x30`: the lowest and highest valid user pointers.
- `+0x38`: the lock object. Its `CRITICAL_SECTION` is at `+0x40`.

Each block `B` starts with a 16-byte header, a classic boundary-tag layout:

```
[B+0x00]  previous block address      only meaningful while the PREVIOUS block is free
[B+0x08]  next block address | flags  bit0 = this block in use, bit1 = previous block in use
[B+0x10]  user data (or free-list / tree node while free)
```

The user data of a block extends up to `next+8`, so **the last 8 bytes of an object overlap the `[next+0]` field of the following block**. While the object is alive, those bytes belong to it. Once it is freed, the allocator writes "previous block = this block" there, and that value is read when the following block is freed.

The allocator handles the flag bits with arithmetic rather than masks. When it frees a block, it clears the following block's "previous in use" bit with `nh - 2`. When it walks free blocks, it reads their next block as `h - 2`. And it uses `[B+0]` exactly as stored when merging with a free previous block. A single wrong bit or a slightly wrong pointer therefore turns into a misaligned block address, which then spreads.

The free function also checks that the block is marked in use and that the pointer is inside the heap (assertions at source lines `0x793` and `0x79A`). In the retail build, the assertion routine (`sekiro+19DC8A0`) does nothing, and the function then carries on freeing anyway.

## 2. What the crash dumps show

Every crash comes down to a block address that is **16-byte aligned minus one**:

- `sekiro+1AA564F`, `+1AA566D`, `+1AA573B` (tree erase): the node to remove, or a node reached from it, has an odd address, so every pointer read through it is shifted by one byte. That gives values like `0x007ff4a91052b000` and access violations at `0xffffffffffffffff`.
- In the free function, called from the free wrapper: the "previous block" pointer `[B+0]` of the block being freed is `P - 1`, where `P` is a valid free block. In every dump the true block `P` is exactly 0x260 bytes long (0x258 bytes of user data).
- `ntdll!RtlpWaitOnCriticalSection`, called from `sekiro+19FBF90`: the game enters a critical section whose address is odd. In one dump, the lock object was at `…EB48` but the game passed `…EB4F`, that is `(…EB48 - 1) + 8`. A pointer to that lock had been decremented by one.

A validation hook on the free function (§4) confirmed this at runtime. Over 2 hours on the affected PC, 22 frees found `[B+0] == P - 1`, always with `P` a free 0x260-byte block. That rate matches the crash rate observed without the fix.

## 3. Finding the culprit

To find out what writes that `P - 1`, the investigation used:

- a record of who frees each 0x260-byte block;
- CPU hardware watchpoints (debug registers DR0 to DR3, set on every game thread) on the last 8 bytes of freed 0x260-byte objects.

Results:

- The victim is always freed by the same code: the scalar deleting destructor at `sekiro+1CBD460`, which calls the destructor `sekiro+1CBD370` and then `operator delete(object, 0x258)`. It is called from an owner's cleanup routine around `sekiro+097D950`, which destroys several sub-objects in turn.
- The damage appears between 0 and about 450 ms after the object was freed.
- During the object's life, its last field is written by the class's own code: `0` at `sekiro+1CBD2F6` and then `1` at `sekiro+1CBD7B6`. That field behaves like a **counter** that is set when some work on the object starts and decremented when that work completes.
- Decrementing the counter after the object is gone turns the allocator's `[next+0] = P` into `P - 1`, which is exactly the corruption seen. Decrementing the low 32 bits of `P` gives the same result. If the memory has already been handed to a new object, that object's field at the same place gets decremented instead, which is the lock-pointer crash above.

The exact instruction that performs the late decrement was not caught. It most likely runs on another thread, and the watchpoints could not always be armed there in time. The fix below doesn't depend on knowing it.

The bug looks timing-dependent. On the test PC (16 logical CPUs), the game crashed after about 5 minutes with all cores and after about 11 minutes when restricted to 8. This would explain why only some machines are affected, but it is a hypothesis, not a proof.

## 4. The fix

`sekiro_crashfix` is a `dinput8.dll` proxy. Every DirectInput export is forwarded to the system `dinput8.dll`. When it loads, the DLL starts a thread that waits until the game's code is in its final state. At DLL load time the bytes don't match yet, and they do about a second later. The thread then redirects three code locations, each one only if it contains exactly the expected bytes:

| Location | Original | Redirected to |
|---|---|---|
| `sekiro+1A40DA0`, 14 bytes | entry of free: `test rdx,rdx / je / mov [rsp+20h],rsi` | `free_hook`, which runs `validate_free`, replays those instructions and continues at `+1A40DAE` |
| `sekiro+1A40DD6`, 27 bytes | "block not in use" assertion call | `err_stub_a`: log, restore registers, return without freeing |
| `sekiro+1A40DFD`, 27 bytes | "pointer outside heap" assertion call | `err_stub_b`: same |

A watchdog checks every 5 seconds that the redirections are still in place, and puts them back if they aren't.

### 4.1 Quarantine (root cause)

When the main heap frees a block of exactly 0x260 bytes, which is the size of the victim class, `validate_free` doesn't let the game free it. It stores the pointer with a timestamp and returns as if the block had been freed. The object's destructor has already run. On later frees, blocks older than 10 seconds are released for real, through the same validation. That release calls `free_hook_nq`, the hook without the quarantine.

During those 10 seconds, the memory still belongs to the dead object: the allocator hasn't written anything into it, and no new object can be placed there. A late decrement only changes a field of the dead object. The largest delay observed was about 450 ms, so 10 seconds leaves a wide margin.

The cost is small: in practice a few to a few dozen objects of 0x258 bytes are held at once, about 40 KB at most, out of a heap of roughly 1.5 GB.

### 4.2 Validation (safety net)

Before any free reaches the game's code, `validate_free` checks:

- the pointer is aligned and inside the heap;
- the block is marked in use, otherwise it is a double free and the free is skipped;
- the next block is aligned, after the block, and knows that its previous block is in use, otherwise the free is skipped;
- if the next block is free, its own next block is aligned and points back to it, otherwise the free is skipped;
- if the previous block is free, `[B+0]` points to a free block whose next is `B`.

If that last pointer is wrong, the aligned addresses within ±32 bytes are tested. If exactly that property holds for one of them, the pointer is repaired; otherwise the free is skipped. Skipping a free leaks the block, which is always safe for the allocator. All memory reads stay within the heap bounds.

### 4.3 Diagnostics

The log records:

- every repair or skip, with the return addresses found on the stack;
- the first 3 damaged blocks, as hex dumps;
- a status line every 10 minutes;
- a crash report from a first-chance exception handler for access violations and heap corruption. It contains the registers, the memory around them (read only after `VirtualQuery` says it is readable), the call stack and the last 32 frees. The handler always lets the exception continue.

## 5. Limitations

- Only Sekiro 1.6.0.0 on Steam is supported. On any other executable the redirections are never applied.
- A decrement that arrived more than 10 seconds late would not be covered by the quarantine. If it hit the allocator's data, the safety net would repair it; if it hit a reused object, it would not.
- The faulty code itself is not modified. The late decrement still happens, but now on dead memory.
- Crashes with other causes are not addressed.

## 6. Building and testing

See the README. The heap logic is covered by `tests/test_heap.c`, which runs the same source against a simulated heap using the layout above. It tests the repair of `P - 1`, the skips (garbage pointer, double free, inconsistent flags) and the quarantine timing.
