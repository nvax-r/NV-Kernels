# Proxy Exec + sched_ext Session Notes

## Goal

Make proxy execution work correctly with `sched_ext`.

The end state is not just "the config builds" or "SCX uses `rq->donor` in a few
places". The target is that proxy execution can run together with a BPF
scheduler without corrupting scheduler, PSI, SCX, BPF callback, or task
ownership state.

## Manual Test Method

Do not run this test automatically from an agent. The test is destructive and
may crash the machine. It should be run manually by the human operator while
watching kernel logs.

Scheduler:

```sh
sudo ~/scx/target/release/scx_flash
```

Workload:

```sh
nice -n 20 stress-ng --vm 0 --io 0
```

Expected final pass condition:

- `scx_flash` stays enabled.
- The workload runs.
- No kernel crash, Oops, WARN, BUG, lockdep splat, PSI warning, SCX error, or
  dmesg warning appears.

Observed failure from the crash test:

```text
sched_ext: BPF scheduler "flash_1.1.0_aarch64_unknown_linux_gnu" enabled
psi: inconsistent task state! task=4492:stress-ng-vm cpu=21 psi_flags=0 clear=10 set=0
Internal error: Oops - FPAC: 0000000072000000 [#1]  SMP
```

The previous boot journal preserved only those lines. There was no vmcore:
`/sys/kernel/kexec_crash_loaded` was `0`, kdump services were inactive, and
`/sys/fs/pstore` was empty. `CONFIG_PSTORE_CONSOLE` was not enabled.

`clear=10` is hexadecimal `0x10`, which is `TSK_ONCPU` from
`include/linux/psi_types.h`. PSI attempted to clear the on-CPU bit from the
task, but `psi_flags` was already zero.

## Current Local Branch Context

The investigation branch imported these two changes:

- `f2a001d4d39c sched/ext: Split curr|donor references properly`
- `f9588a5ecd67 sched: Allow to enable proxy exec with sched_ext`

The first patch changes selected SCX internals from `rq->curr` to `rq->donor`.
The second patch removes the Kconfig exclusion:

```c
depends on !SCHED_CLASS_EXT
```

This opens the config combination, but it does not make proxy execution
semantically correct for sched_ext.

Andrea later added:

- `17e86247ab45 sched/core: Disable proxy-exec context donation under sched_ext`

That commit explicitly identifies the mismatch: proxy execution can run a task
that the BPF scheduler did not dispatch. The local `andrea-scx-proxy` branch had
that commit reverted, reopening the broken path.

## Proxy Exec Model

With `CONFIG_SCHED_PROXY_EXEC=y`, `struct rq` has two task pointers:

```c
struct task_struct __rcu *donor;  /* Scheduling context */
struct task_struct __rcu *curr;   /* Execution context */
```

The intended model is:

- `rq->donor`: the task selected by the scheduler and charged as scheduling
  context.
- `rq->curr`: the task that actually executes on the CPU.

For mutex proxy execution, the selected task can be blocked on a mutex. The
scheduler keeps that blocked waiter as the donor, follows the mutex owner chain,
and runs the owner as the execution context.

The relevant core flow is in `kernel/sched/core.c`:

```c
next = pick_next_task(rq, rq->donor, &rf);
rq_set_donor(rq, next);

if (unlikely(next->blocked_on))
	next = find_proxy_task(rq, next, &rf);

rq->curr = next;
psi_sched_switch(prev, next, ...);
context_switch(..., prev, next, ...);
```

So after `find_proxy_task()`, `next` may no longer be the selected donor. It may
be the mutex owner.

## How EEVDF / fair Handles Proxy Exec

EEVDF/fair has been wired for the donor/execution split.

Important points:

- Fair's `cfs_rq->curr` tracks the picked scheduling entity, i.e. the donor.
- The actual execution context can be different and is available as `rq->curr`.
- Fair code contains explicit donor-aware checks such as
  `task_current_donor()`.

The comment in `kernel/sched/fair.c` is the key invariant:

```c
/*
 * Note: cfs_rq->curr corresponds to the task picked to
 * run (ie: rq->donor.se) which due to proxy-exec may
 * not necessarily be the actual task running
 * (rq->curr.se). This is easy to confuse!
 */
```

Fair also accounts runtime carefully:

- execution runtime is charged to the actual running task (`rq->curr`);
- cgroup CPU time is charged to the donor;
- scheduling decisions, hrtick, throttling, and migration checks use donor-aware
  helpers where needed.

Because fair owns the scheduler state internally, it can keep this split
consistent inside the kernel.

## How sched_ext Currently Handles Proxy Exec

sched_ext has the same high-level need for donor/execution split, but it has an
extra external contract with the BPF scheduler:

- DSQ ownership and dispatch
- `SCX_TASK_IN_CUSTODY`
- `ops.enqueue`
- `ops.dispatch`
- `ops.running`
- `ops.stopping`
- kfuncs such as `scx_bpf_task_running()` and `scx_bpf_cpu_curr()`

The first SCX proxy-exec patch only changes selected internal uses of
`rq->curr` to `rq->donor`, including:

- `update_curr_scx()`
- local DSQ preemption decisions
- `dispatch_to_local_dsq()`
- `do_pick_task_scx()`
- `scx_can_stop_tick()`
- `kick_one_cpu()`
- SCX dump output

That is necessary but not sufficient.

The BPF-visible view still exposes execution context:

```c
scx_bpf_task_running(p) => task_rq(p)->curr == p
scx_bpf_cpu_curr(cpu)  => cpu_rq(cpu)->curr
```

`scx_flash` uses both `scx_bpf_task_running()` and
`__COMPAT_scx_bpf_cpu_curr()`.

## Primary Blocker

The primary blocker is that sched_ext commits BPF scheduler state for the donor
before proxy execution substitutes the execution context.

The ordering is:

1. `__schedule()` calls `pick_next_task(rq, rq->donor, &rf)`.
2. sched_ext implements `.pick_task`, not `.pick_next_task`.
3. Generic scheduler code calls `put_prev_set_next_task(rq, prev, p)` for the
   selected SCX task `p`.
4. That invokes SCX's `put_prev_task_scx()` and `set_next_task_scx()` for `p`.
5. `set_next_task_scx()` can invoke `ops.running(p)`.
6. Only after this does generic proxy exec call `find_proxy_task()` and possibly
   replace `next` with the mutex owner.
7. Core then sets `rq->curr = next`, calls `psi_sched_switch(prev, next, ...)`,
   and context-switches to `next`.

Therefore, the BPF scheduler can be told that the donor is running while core
actually runs the owner.

This is different from fair. Fair's internal `cfs_rq->curr` is allowed to mean
"selected donor". sched_ext's BPF contract currently treats running/curr helpers
as execution-context based.

## Secondary Blockers / Work Items

These are not proposed fixes yet. They are the areas that must be resolved for
proxy exec to work correctly with sched_ext.

1. Define sched_ext's proxy-exec contract.

   Decide what BPF should observe during proxy execution:

   - Should `ops.running()` be called for the donor, the owner, or both?
   - Should `ops.stopping()` correspond to donor scheduling state, execution
     state, or both?
   - Should BPF policies be aware of the owner at all?
   - Should a new kfunc expose the donor explicitly?

2. Fix the pick / proxy ordering.

   Current ordering lets SCX set-next/running callbacks happen before
   `find_proxy_task()` chooses the execution context. A correct design probably
   needs to either:

   - split "donor selected" from "execution context selected" callbacks; or
   - move proxy resolution earlier for sched_ext; or
   - add a sched_ext-specific proxy handoff path that updates BPF-visible state
     consistently.

3. Audit all BPF-visible `curr` semantics.

   Known execution-context kfuncs:

   ```c
   scx_bpf_task_running()
   scx_bpf_cpu_curr()
   scx_bpf_cpu_rq()
   scx_bpf_locked_rq()
   ```

   Any BPF scheduler using these can see execution context while SCX internal
   accounting uses donor context.

4. Audit SCX callbacks that assume selected task equals running task.

   At minimum:

   - `ops.running`
   - `ops.stopping`
   - `ops.enqueue`
   - `ops.dispatch`
   - `ops.tick`
   - `ops.cpu_acquire` / `ops.cpu_release`

5. Audit SCX custody and DSQ invariants.

   Proxy execution can run a mutex owner that BPF did not dispatch from a DSQ.
   That conflicts with the current sched_ext model where runnable tasks move
   through BPF custody and DSQs before running.

6. Audit PSI accounting.

   The observed warning shows a violated PSI on-CPU invariant:

   ```text
   psi_flags=0 clear=10 set=0
   ```

   This means PSI tried to clear `TSK_ONCPU` for a task that did not have it set.
   The likely source is inconsistent transition accounting between the SCX donor
   path and the final proxy execution context path. This must be proven with
   instrumentation or a vmcore; the saved journal did not contain a call trace.

7. Audit nested scheduler re-entry.

   Andrea's disable commit states that BPF kfuncs that re-enter scheduler paths
   can trigger nested `find_proxy_task()` while an outer proxy chain is still on
   the stack. This is a separate correctness problem from PSI and must be
   handled explicitly if proxy donation is re-enabled under sched_ext.

8. Enable better crash capture before further destructive testing.

   The previous crash had no vmcore and no pstore console trace. Before the next
   manual stress run, enable persistent crash capture if possible:

   - boot with a `crashkernel=` reservation;
   - load kdump so `/sys/kernel/kexec_crash_loaded` is `1`;
   - consider enabling pstore console support in the kernel config.

## Relevant Functions and Call Paths

This section records the source paths that matter for future debugging. Line
numbers may move as patches are edited, so treat function names as authoritative.

### Config and Runtime Gate

Files and symbols:

- `init/Kconfig`
  - `CONFIG_SCHED_PROXY_EXEC`
  - currently depends on `!PREEMPT_RT` and `EXPERT`
  - the experimental enablement removes `depends on !SCHED_CLASS_EXT`
- `kernel/Kconfig.preempt`
  - `CONFIG_SCHED_CLASS_EXT`
- `Documentation/admin-guide/kernel-parameters.txt`
  - `sched_proxy_exec=`
- `kernel/sched/core.c`
  - `DEFINE_STATIC_KEY_TRUE(__sched_proxy_exec)`
  - `setup_proxy_exec()`
- `include/linux/sched.h`
  - `sched_proxy_exec()`

Meaning:

- `CONFIG_SCHED_PROXY_EXEC=n`: `rq->donor` and `rq->curr` are a union and alias
  the same pointer.
- `CONFIG_SCHED_PROXY_EXEC=y`: `rq->donor` and `rq->curr` are separate fields.
- runtime `sched_proxy_exec=0` disables proxy behavior, but the fields remain
  separate when compiled in.

### Runqueue Donor / Curr State

Files and symbols:

- `kernel/sched/sched.h`
  - `struct rq::donor`
  - `struct rq::curr`
  - `rq_set_donor()`
  - `task_current()`
  - `task_current_donor()`
  - `task_is_blocked()`

Important invariant:

```text
rq->donor = scheduling context
rq->curr  = execution context
```

With proxy exec, these can differ.

### Mutex Blocked-On State

Files and symbols:

- `include/linux/sched.h`
  - `task_struct::blocked_on`
  - `task_struct::blocked_lock`
  - `PROXY_WAKING`
  - `__get_task_blocked_on()`
  - `__set_task_blocked_on()`
  - `__clear_task_blocked_on()`
  - `set_task_blocked_on_waking()`
  - `clear_task_blocked_on()`
- `kernel/locking/mutex.h`
  - `__mutex_owner()`
- `kernel/locking/mutex.c`
  - mutex slowpath sets `current->blocked_on`
  - mutex unlock path calls `set_task_blocked_on_waking()`
- `kernel/locking/ww_mutex.h`
  - uses `PROXY_WAKING` to avoid circular blocked-on relationships

Logical path:

```text
mutex_lock slowpath
  -> __set_task_blocked_on(current, lock)
  -> schedule()
  -> __schedule()
```

Unlock / wake path:

```text
mutex_unlock
  -> set_task_blocked_on_waking(next, lock)
  -> wake_q / try_to_wake_up path
```

### Core Scheduler Proxy Exec Path

Files and symbols:

- `kernel/sched/core.c`
  - `__schedule()`
  - `try_to_block_task()`
  - `pick_next_task()`
  - `__pick_next_task()`
  - `find_proxy_task()`
  - `proxy_resched_idle()`
  - `proxy_deactivate()`
  - `proxy_migrate_task()`
  - `proxy_force_return()`
  - `proxy_set_task_cpu()`
- `kernel/sched/sched.h`
  - `put_prev_set_next_task()`

Main path:

```text
__schedule()
  prev = rq->curr
  prev_state = READ_ONCE(prev->__state)

  if (!preempt && prev_state)
    try_to_block_task(rq, prev, &prev_state, !task_is_blocked(prev))

pick_again:
  next = pick_next_task(rq, rq->donor, &rf)
  rq->next_class = next->sched_class

  if (sched_proxy_exec()) {
    prev_donor = rq->donor
    rq_set_donor(rq, next)

    if (next->blocked_on) {
      next = find_proxy_task(rq, next, &rf)
      if (!next)
        goto pick_again
      if (next == rq->idle)
        goto keep_resched
    }
  } else {
    rq_set_donor(rq, next)
  }

  rq->curr = next
  psi_sched_switch(prev, next, sleep)
  context_switch(rq, prev, next, &rf)
```

Proxy chain walk:

```text
find_proxy_task(rq, donor, rf)
  for p = donor; p->blocked_on; p = owner:
    mutex = p->blocked_on
    owner = __mutex_owner(mutex)

    if mutex == PROXY_WAKING:
      if task_current(rq, p)
        clear_task_blocked_on(p, PROXY_WAKING)
        return p
      else
        proxy_force_return(...)
        return NULL

    if !owner:
      if task_current(rq, p)
        clear blocked_on and return p
      else
        proxy_force_return(...)
        return NULL

    if !owner->on_rq || owner->se.sched_delayed:
      if current task is in chain
        return proxy_resched_idle(rq)
      else
        proxy_deactivate(rq, donor)
        return NULL

    if task_cpu(owner) != cpu_of(rq):
      if current task is in chain
        return proxy_resched_idle(rq)
      else
        proxy_migrate_task(rq, rf, p, owner_cpu)
        return NULL

    if owner is migrating:
      return proxy_resched_idle(rq)

  return owner
```

Known incomplete core path:

```c
if (!READ_ONCE(owner->on_rq) || owner->se.sched_delayed) {
	/* XXX Don't handle blocked owners/delayed dequeue yet */
	...
}
```

### Generic Pick Path

Files and symbols:

- `kernel/sched/core.c`
  - `__pick_next_task()`
- `kernel/sched/sched.h`
  - `struct sched_class`
  - `put_prev_set_next_task()`

Important generic behavior:

```text
__pick_next_task()
  for_each_active_class(class):
    if class->pick_next_task:
      p = class->pick_next_task(rq, prev, rf)
      return p
    else:
      p = class->pick_task(rq, rf)
      if (p) {
        put_prev_set_next_task(rq, prev, p)
        return p
      }
```

This matters because fair implements both `.pick_task` and `.pick_next_task`,
while sched_ext currently implements `.pick_task` but not `.pick_next_task`.

### EEVDF / fair Path

Files and symbols:

- `kernel/sched/fair.c`
  - `pick_task_fair()`
  - `pick_next_task_fair()`
  - `put_prev_task_fair()`
  - `set_next_task_fair()`
  - `__set_next_task_fair()`
  - `update_curr()`
  - `update_curr_common()`
  - `yield_task_fair()`
  - `hrtick_update()`
  - fair migration paths using `task_current_donor()`

Fair pick path:

```text
pick_next_task_fair(rq, prev, rf)
  p = pick_task_fair(rq, rf)
  if prev != p:
    put_prev_entity(...)
    set_next_entity(...)
    __set_next_task_fair(rq, p, true)
  return p
```

Fair explicitly treats CFS current as donor:

```text
cfs_rq->curr = selected scheduling entity = rq->donor->se
rq->curr     = actual execution context
```

Fair donor-aware examples:

- `update_curr_common()` updates `rq->donor->se`.
- `update_curr()` distinguishes donor accounting from `rq->curr`.
- `yield_task_fair()` uses `rq->donor`.
- hrtick and priority-change paths use `task_current_donor()`.
- load balancing avoids migrating `task_current_donor()` and
  `task_is_blocked()` tasks.

### sched_ext Core Path

Files and symbols:

- `kernel/sched/ext.c`
  - `DEFINE_SCHED_CLASS(ext)`
  - `enqueue_task_scx()`
  - `dequeue_task_scx()`
  - `do_enqueue_task()`
  - `call_task_dequeue()`
  - `dispatch_to_local_dsq()`
  - `finish_dispatch()`
  - `flush_dispatch_buf()`
  - `balance_one()`
  - `do_pick_task_scx()`
  - `pick_task_scx()`
  - `put_prev_task_scx()`
  - `set_next_task_scx()`
  - `update_curr_scx()`
  - `task_tick_scx()`
  - `kick_one_cpu()`
  - `scx_can_stop_tick()`

SCX class registration:

```text
DEFINE_SCHED_CLASS(ext) = {
  .enqueue_task  = enqueue_task_scx,
  .dequeue_task  = dequeue_task_scx,
  .pick_task     = pick_task_scx,
  .put_prev_task = put_prev_task_scx,
  .set_next_task = set_next_task_scx,
  .task_tick     = task_tick_scx,
  ...
}
```

SCX pick path:

```text
__pick_next_task()
  class = ext_sched_class
  p = pick_task_scx(rq, rf)
    -> do_pick_task_scx(rq, rf, false)
       prev = rq->donor
       balance_one(rq, prev)
       if keep_prev:
         p = prev
       else:
         p = first_local_task(rq)
       return p

  put_prev_set_next_task(rq, prev, p)
    -> put_prev_task_scx(rq, prev, p)
    -> set_next_task_scx(rq, p, true)
```

Then core proxy exec may still do:

```text
rq_set_donor(rq, p)
if (p->blocked_on)
  next = find_proxy_task(rq, p, &rf)
rq->curr = next
```

This is the key ordering problem:

```text
SCX callbacks commit BPF-visible state for p
then proxy exec may run owner != p
```

### sched_ext BPF Callback Path

Files and symbols:

- `kernel/sched/ext.c`
  - `SCX_CALL_OP_TASK()`
  - `SCX_CALL_OP_TASK_RET()`
  - `ops.enqueue`
  - `ops.dispatch`
  - `ops.running`
  - `ops.stopping`
  - `ops.runnable`
  - `ops.quiescent`
  - `ops.tick`

Important SCX callbacks:

```text
enqueue_task_scx()
  set_task_runnable()
  SCX_CALL_OP_TASK(runnable, p)
  do_enqueue_task()
    SCX_CALL_OP_TASK(enqueue, p)

set_next_task_scx(p)
  ops_dequeue(..., p, SCX_DEQ_CORE_SCHED_EXEC) if needed
  SCX_CALL_OP_TASK(running, p)
  clr_task_runnable(p)

put_prev_task_scx(p)
  update_curr_scx(rq)
  SCX_CALL_OP_TASK(stopping, p, true)
  reinsert or enqueue p if still queued

task_tick_scx(curr)
  update_curr_scx(rq)
  SCX_CALL_OP_TASK(tick, curr)
```

The callback subject task `p` is assumed to have stable scheduler-protected
fields under its rq lock. Under proxy exec, the callback subject can be the
donor while `rq->curr` later becomes the owner.

### sched_ext BPF kfuncs Relevant to Proxy Exec

Files and symbols:

- `kernel/sched/ext.c`
  - `scx_bpf_task_running()`
  - `scx_bpf_task_cpu()`
  - `scx_bpf_cpu_curr()`
  - `scx_bpf_cpu_rq()`
  - `scx_bpf_locked_rq()`
  - `scx_bpf_task_set_slice()`
  - `scx_bpf_task_set_dsq_vtime()`
  - `scx_bpf_kick_cpu()`

Execution-context helpers:

```c
scx_bpf_task_running(p)
{
	return task_rq(p)->curr == p;
}

scx_bpf_cpu_curr(cpu)
{
	return cpu_rq(cpu)->curr;
}
```

These expose execution context, not donor context.

Potentially relevant rq-access helpers:

```text
scx_bpf_cpu_rq(cpu)     -> returns struct rq * but is deprecated
scx_bpf_locked_rq()     -> returns currently locked rq
```

These might allow BPF to inspect `rq->donor`, but that behavior has not been
validated for verifier acceptance or semantic correctness.

### PSI Accounting Path

Files and symbols:

- `include/linux/psi_types.h`
  - `TSK_IOWAIT`
  - `TSK_MEMSTALL`
  - `TSK_RUNNING`
  - `TSK_MEMSTALL_RUNNING`
  - `TSK_ONCPU`
- `kernel/sched/stats.h`
  - `psi_enqueue()`
  - `psi_dequeue()`
  - `psi_sched_switch()`
- `kernel/sched/psi.c`
  - `psi_flags_change()`
  - `psi_task_change()`
  - `psi_task_switch()`
- `kernel/sched/core.c`
  - `enqueue_task()`
  - `dequeue_task()`
  - `psi_account_irqtime()`
  - `psi_sched_switch()`

Core switch accounting:

```text
__schedule()
  rq->curr = next
  psi_account_irqtime(rq, prev, next)
  psi_sched_switch(prev, next, sleep)
  context_switch(rq, prev, next, &rf)
```

PSI switch logic:

```text
psi_sched_switch(prev, next, sleep)
  set TSK_ONCPU on next
  clear TSK_ONCPU from prev
  if sleep:
    also clear TSK_RUNNING / TSK_MEMSTALL_RUNNING as needed
```

Observed warning:

```text
psi_flags=0 clear=10 set=0
```

`clear=10` is hex `0x10`, i.e. `TSK_ONCPU`. The warning means PSI was asked to
clear `TSK_ONCPU` from a task whose PSI flags did not contain it.

### scx_flash Paths Used by the Manual Reproducer

Repository: `~/scx`

Files and symbols:

- `scheds/rust/scx_flash/src/bpf/main.bpf.c`
  - `flash_select_cpu()`
  - `try_direct_dispatch()`
  - `rr_enqueue()`
  - `flash_enqueue()`
  - `flash_dispatch()`
  - `flash_running()`
  - `flash_stopping()`
  - `flash_runnable()`
  - `tickless_timerfn()`
  - `flash_ops`

`scx_flash` uses execution-context helpers:

```text
flash_enqueue()
  prev_cpu = scx_bpf_task_cpu(p)
  is_running = scx_bpf_task_running(p)
  try_direct_dispatch(..., is_running)

rr_enqueue()
  scx_bpf_task_running(p)

tickless_timerfn()
  p = __COMPAT_scx_bpf_cpu_curr(cpu)
```

`scx_flash` callbacks that rely on coherent running/stopping semantics:

```text
flash_running(p)
  records last_run_at
  updates CPU load
  updates global vtime

flash_stopping(p, runnable)
  computes elapsed slice from last_run_at
  updates p->scx.dsq_vtime
  updates per-CPU runtime
```

If proxy exec causes `flash_running()` to run for the donor while the CPU
executes the owner, these runtime/vtime updates can become semantically wrong.

### Andrea's Disable Path

Commit:

```text
17e86247ab45 sched/core: Disable proxy-exec context donation under sched_ext
```

Core changes:

```text
try_to_block_task(..., !task_is_blocked(prev) || scx_enabled())
```

Meaning:

- When SCX is enabled, mutex-blocked tasks are actually blocked instead of kept
  runnable as proxy donors.

And:

```text
if (scx_enabled()) {
  if (unlikely(next->blocked_on))
    clear_task_blocked_on(next, PROXY_WAKING)
  goto picked
}
```

Meaning:

- When SCX is enabled, the scheduler skips `find_proxy_task()`.
- Any leftover `PROXY_WAKING` marker is cleared because the proxy walk will not
  process it.

That path allows the config combination but disables proxy-exec context donation
while sched_ext is active.

## Non-Goal

Do not paper over the crash by disabling PSI, suppressing warnings, or hiding
SCX errors. The goal is full semantic correctness of proxy exec with sched_ext.

## Current Conclusion

The blocker is not the donor/curr model itself. That model is correct and is how
proxy exec works for EEVDF/fair.

The blocker is that sched_ext has an additional BPF scheduler state machine. The
current partial conversion makes some SCX internals donor-aware, but the BPF
contract and core pick/proxy ordering still allow BPF to track one task while
the CPU executes another.

Until sched_ext has an explicit proxy-exec contract and matching implementation,
removing `depends on !SCHED_CLASS_EXT` is only a config enablement. It is not a
complete proxy-exec implementation for BPF schedulers.
