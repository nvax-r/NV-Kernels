# sched_ext Proxy Exec Design

Date: 2026-04-29

## Context

The current branch enables `CONFIG_SCHED_PROXY_EXEC` together with
`CONFIG_SCHED_CLASS_EXT`, and has a partial sched_ext conversion from
`rq->curr` to `rq->donor`.

That is necessary but not sufficient. With proxy execution, `rq->donor` is the
scheduling context and `rq->curr` is the execution context. For fair/EEVDF,
this split is internal to the scheduler class. `cfs_rq->curr` tracks the
selected donor, while `rq->curr` may become a mutex owner selected by
`find_proxy_task()`.

sched_ext has an additional BPF-visible state machine. Today, SCX calls
`ops.running()` from `set_next_task_scx()` for the selected SCX task before
core scheduler proxy resolution runs. After that, `find_proxy_task()` may
replace the execution context with the mutex owner. This can leave BPF seeing
the donor as "running" while the CPU executes the owner.

The design goal is to make sched_ext explicitly aware that donor and owner are
different, while keeping the old sched_ext callback contract intact until the
new proxy-exec surface is proven.

## Goal

Make proxy execution work correctly with sched_ext:

- the donor remains the SCX scheduling context;
- the owner can safely execute as the proxy execution context;
- BPF can observe the donor/owner relationship explicitly;
- proxy owner execution does not imply BPF dispatch or DSQ consumption;
- the system survives the manual `scx_flash` plus `stress-ng` test without
  warnings or crashes.

The destructive runtime test remains manual-only:

```sh
sudo ~/scx/target/release/scx_flash
nice -n 20 stress-ng --vm 0 --io 0
```

## Chosen Approach

Use an additive proxy-exec notification layer.

Existing callbacks stay donor-based:

- `ops.running(donor)` means the donor became the active SCX scheduling context.
- `ops.stopping(donor)` means donor scheduling service is stopping.
- `ops.tick(donor)` remains tied to donor slice and scheduling entitlement.

Add a separate proxy-aware surface for owner execution:

- `ops.proxy_running(donor, owner)` reports that `owner` is about to execute
  using `donor`'s scheduling entitlement.
- `ops.proxy_stopping(donor, owner)` reports that this proxy execution
  relationship is ending.

This preserves existing scheduler behavior while allowing BPF schedulers to
observe proxy execution directly.

## Rejected Alternatives

Do not move proxy resolution before SCX set-next in the first implementation.
That would change the core scheduler ordering around class callbacks and risks
affecting fair, RT, DL, and core scheduling assumptions.

Do not make the owner look normally dispatched. In particular, do not dequeue
the owner from its DSQ just because it proxy-executes. Proxy execution gives the
owner CPU time through the donor's scheduling entitlement; it is not a BPF
policy dispatch decision for the owner.

## Data Flow

Current high-level flow:

```text
pick_next_task(rq, rq->donor)
  -> pick_task_scx()
  -> put_prev_set_next_task()
     -> set_next_task_scx(donor)
        -> ops.running(donor)

rq_set_donor(rq, donor)

if donor->blocked_on:
  owner = find_proxy_task(rq, donor)

rq->curr = owner
context_switch(prev_exec, owner)
```

New flow:

```text
pick_next_task(...)
  -> existing SCX donor callbacks remain unchanged

rq_set_donor(rq, donor)

if donor->blocked_on:
  owner = find_proxy_task(rq, donor)
  if owner != donor and owner is an SCX task:
    mark owner proxy-executing
    call ops.proxy_running(donor, owner)

rq->curr = owner
context_switch(prev_exec, owner)
```

On the later schedule out:

```text
prev_exec = rq->curr

if prev_exec was proxy-executing:
  call ops.proxy_stopping(donor, prev_exec)
  clear owner proxy-executing mark

normal put_prev/set_next remains donor-based
```

`ops.running(donor)` still fires before `find_proxy_task()`. Its meaning is
restricted to "donor became active scheduling context." If `find_proxy_task()`
returns a different owner, the new proxy callback reports the execution
context.

## Interfaces

Add optional callbacks to `struct sched_ext_ops`:

```c
void (*proxy_running)(struct task_struct *donor,
                      struct task_struct *owner);

void (*proxy_stopping)(struct task_struct *donor,
                       struct task_struct *owner);
```

Add CPU/task-state kfunc visibility:

```c
struct task_struct *scx_bpf_cpu_donor(s32 cpu);
bool scx_bpf_task_proxy_executing(const struct task_struct *p);
```

Keep existing helpers unchanged:

```c
scx_bpf_cpu_curr(cpu)      /* execution context, may be proxy owner */
scx_bpf_task_running(p)    /* true if p == rq->curr */
```

Do not add an owner-side donor lookup in the first pass. The owner task does
not need to know who donated scheduling entitlement. BPF receives the
relationship in transition callbacks and can query CPU-scoped state with
`scx_bpf_cpu_donor()`.

## Internal State

Add an SCX task flag such as `SCX_TASK_PROXY_EXEC`.

The flag is set on the owner while it is executing as the proxy owner. The flag
is cleared when proxy execution stops.

Do not store a donor pointer in `struct sched_ext_entity` for the first pass.
The current donor is already represented by `rq->donor` while rq state is
coherent.

## DSQ And Custody Rules

Proxy owner execution must not imply DSQ consumption.

The owner stays in its existing DSQ and custody state unless normal SCX
dispatch/dequeue logic changes it. This avoids mixing proxy execution with SCX
task migration and dispatch ownership semantics.

While a task has `SCX_TASK_PROXY_EXEC`:

- `finish_dispatch()` must reject or skip dispatch of that task;
- DSQ move paths must reject or skip movement of that task;
- migration paths reached through SCX DSQ movement must not move that task;
- watchdog/runnable-list logic must not report the owner as stalled only
  because it remained queued while proxy-executing.

## Error Handling

If a BPF scheduler attempts to dispatch or move a proxy-executing owner, the
kernel should reject or skip the attempt rather than consuming the task from a
DSQ.

The first implementation should prefer clear SCX diagnostics over silent state
corruption. A rejected dispatch/move should not become a kernel crash. Whether
the rejection is counted as an SCX event, warning, or scheduler error should
follow the surrounding SCX convention at the call site.

The first implementation does not solve core proxy exec's existing incomplete
cases for blocked owners or delayed dequeue owners.

## Verification

Non-destructive checks from the agent side:

```sh
make olddefconfig
make kernel/sched/
make tools/sched_ext/
```

Do not build a special test scheduler in the first pass. A small scheduler that
records `proxy_running()` and `proxy_stopping()` events can be added later if
the runtime failure remains ambiguous.

Final runtime validation is manual-only:

```sh
sudo ~/scx/target/release/scx_flash
nice -n 20 stress-ng --vm 0 --io 0
```

Pass condition:

- `scx_flash` stays enabled;
- the workload runs;
- no kernel crash, Oops, WARN, BUG, lockdep splat, PSI warning, SCX error, or
  dmesg warning appears.
