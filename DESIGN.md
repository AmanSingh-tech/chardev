# Design notes: synchronization

## One mutex for all shared state

`chardev_lock` protects the buffer (`msg_buf`, `buf_used`) and the counters
(`msg_count`, `open_count`, `bytes_written`). Every file operation, both
ioctls and the `/proc` show function take it.

**Why a mutex rather than a spinlock.** `read()` and `write()` call
`copy_to_user()` and `copy_from_user()` while holding the lock. These copies
can page-fault, for example when the user's page is swapped out or not yet
mapped, and handling a fault can sleep. Sleeping while holding a spinlock is
a bug: other CPUs spin uselessly, and with preemption disabled it can
deadlock (the kernel warns "BUG: sleeping function called from invalid
context"). A mutex puts waiters to sleep instead, so the holder is allowed to
sleep too. None of this state is touched from interrupt context, so we never
need a lock that works there.

**Why hold the lock across the copy at all.** The copy is what changes the
shared buffer. On write, the data has to land at `buf_used`, and another
writer must not take that offset at the same time. On read, the message must
stay put until it has been copied out. The alternative is to copy into a
temporary kernel buffer first and lock only for a `memcpy`. That shortens the
critical section, but costs an extra copy and a 256-byte stack or heap
buffer. At this scale the simpler version is easier to show correct.

**Why one lock and not several.** The counters change together with the
buffer (a write updates `buf_used`, `msg_count` and `bytes_written` together),
so separate locks would let `/proc` or `GET_COUNT` see a half-finished
update, and would add lock-ordering rules. This driver is never going to be
limited by lock contention.

**`mutex_lock_interruptible`.** If a process is waiting for the lock and gets
a signal (Ctrl-C), the call returns `-ERESTARTSYS` instead of leaving the
process stuck in uninterruptible sleep (state D). The kernel then either
restarts the system call or returns `EINTR`.

## Atomicity of each operation

- **Commit after copy.** `write()` copies into the free area past `buf_used`
  and only then advances `buf_used`. `read()` removes a message only after
  `copy_to_user()` succeeds. A faulting user pointer (`-EFAULT`) therefore
  never leaves a half-written or lost message.
- **Checks outside the lock.** The size checks in `write()` depend only on
  the arguments, so they run before taking the lock. The space check depends
  on shared state, so it runs under the lock in the same critical section as
  the copy; otherwise two writers could both see free space and overfill the
  buffer.
- **One message per call.** Because each `read`/`write` holds the lock for
  its whole message, concurrent users can never see interleaved or torn
  messages. The threaded test checks exactly this.

## Open and release

There is no per-open state (`file->private_data` is unused). Every open file
descriptor shares the same queue, so concurrent opens need nothing beyond the
locked `open_count++`. `.owner = THIS_MODULE` makes the VFS hold a reference
on the module while any file is open, so `rmmod` fails with "in use" instead
of unloading code that is still running.

## Init and exit ordering

`cdev_add()` makes the device usable immediately, so the buffer is allocated
*before* it, and the mutex is initialised statically (`DEFINE_MUTEX`).
`/proc` is created last and removed first. Exit undoes the steps in exactly
the reverse order, and each init error path undoes only the steps that had
succeeded.

## Deliberately not done

- **Blocking I/O.** A read on an empty queue returns 0, and a write to a full
  queue returns `-ENOSPC`, so a wait queue isn't needed. Blocking reads would
  need `wait_event_interruptible()` and a `wake_up_interruptible()` on each
  write; it would be a natural next step, but it isn't in the spec.
- **Lock-free counters.** `open_count` could be an `atomic64_t` so `open()`
  doesn't take the mutex. It isn't worth a second synchronization method
  here.
- **A ring buffer.** A read moves the remaining data down to the front of the
  buffer (`memmove`). That costs at most 4 KiB per read, and keeps the
  layout trivially correct with no wrap-around cases.
