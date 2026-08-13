# Finux performance audit — Ryzen 5 5500 desktop configuration

**Status: BLOCKED on target-machine data. No configuration has been shipped
and no performance claim has been made.**

Branch: `perf/ryzen5500-scheduler-config`
Tree: Linux 7.2.0-rc7, HEAD `b4b7ed8e0b`
Date: 2026-08-13

---

## 1. Executive summary

The static half of this task is done and produced several findings that
change the plan. The runtime half cannot be started, for a reason that has
nothing to do with the plan's merits.

**What was verified** (against this checkout, not from memory):

| # | Finding | Effect on plan |
|---|---|---|
| 1 | `CONFIG_SCHED_INFO` is force-selected by **KVM** (`arch/x86/kvm/Kconfig:36`) | **Variant B's stated goal is unreachable** on any kernel with KVM |
| 2 | `PREEMPT_NONE` and `PREEMPT_VOLUNTARY` are **unselectable on x86** in this tree | Only `PREEMPT` and `PREEMPT_LAZY` exist; `preempt=none` is rejected at boot |
| 3 | `SCHED_CACHE` costs more than stated — a `for_each_possible_cpu()` walk touching a **remote runqueue cacheline per fork** | Variant A is better justified than claimed |
| 4 | `SCHED_CLASS_EXT` takes a **global raw spinlock on every fork and every exit**, with no BPF scheduler loaded | Variant D is better justified than claimed |
| 5 | `PREEMPT_LAZY` + `HZ_250` raises lazy-preemption latency from ~1 ms to ~4 ms | **Directly threatens the "no desktop-latency regression" objective** |
| 6 | `/sys/kernel/debug/sched/llc_balancing/enabled` does **not** report the static-key state | Would have produced a false verification |
| 7 | **Variant F is almost entirely a no-op** — `X86_AMD_PSTATE`, mode `3`, `CPU_IDLE` and `CPU_SUP_AMD` are already forced or default | Reduces to `-march=native` alone |
| 8 | `X86_DEBUG_FPU` **defaults to `y`** and is `=y` in `x86_64_defconfig` | Variant E has at least one real target |
| 9 | `SCHED_CLASS_EXT` requires `DEBUG_INFO_BTF`, which forces `DEBUG_INFO=y` | Disabling it may cut build time and image size substantially |

**What is blocked:** every runtime measurement, and therefore the target
defconfig itself.

---

## 2. Why the work is blocked

### 2.1 This is not the target machine

I am running in an ephemeral Firecracker microVM, not on your desktop:

| Property | Target (per your brief) | This environment |
|---|---|---|
| CPU | AMD Ryzen 5 5500, 6c/12t, Zen 3 | Intel Xeon @ 2.10 GHz, **4 vCPU**, virtualised |
| Vendor | AuthenticAMD | GenuineIntel |
| Kernel | Fedora, NVIDIA modules | `6.18.5-fc-v20`, Firecracker microVM |
| Userspace | Fedora, GNOME/Wayland | Ubuntu 24.04, no display server |
| cpufreq | amd-pstate + EPP | **absent** — no `scaling_driver` at all |
| cgroups | v2 unified (Fedora) | **v1** (`/sys/fs/cgroup` is tmpfs) |
| PSI | present | **absent** — no `/proc/pressure` |
| Microcode | real | `0x1` (synthetic) |
| GPU | NVIDIA proprietary | none |

`/proc/config.gz` here is the **microVM's** config. It is not your Fedora
kernel's config and must not be used as a baseline.

### 2.2 Consequences

Your brief says: *"If no known-bootable baseline config can be obtained,
stop and request it rather than inventing a production config."* That
condition is met, so `arch/x86/configs/finux_ryzen5500_defconfig` has
**not** been created. Generating one from `x86_64_defconfig` would produce
a file that looks authoritative, boots into something unlike your system,
and silently reframes every later measurement as "defconfig vs Fedora"
rather than "tuning change vs baseline".

Runtime benchmarking is equally impossible: 4 virtualised Intel cores
cannot stand in for 6 physical Zen 3 cores with SMT, and with no cpufreq,
no PSI, no turbostat and no `perf`, most of the requested measurements have
nothing to read.

**Nothing here has been called faster. No benchmark was run.**

---

## 3. Verified findings

Every claim below cites this checkout. Each was checked by reading the
source; the two most consequential were additionally confirmed empirically.

### 3.1 Variant B is not achievable as specified — KVM forces SCHED_INFO

`CONFIG_SCHED_INFO` is a hidden bool (`lib/Kconfig.debug:1392`, `default n`)
with **six** selectors:

```
init/Kconfig:696            TASK_DELAY_ACCT
lib/Kconfig.debug:1399      SCHEDSTATS
arch/x86/kvm/Kconfig:36     KVM          <-- the problem
arch/arm64/kvm/Kconfig:37   KVM
arch/riscv/kvm/Kconfig:33   KVM
arch/loongarch/kvm/Kconfig:33  KVM
```

Confirmed empirically in this tree:

```
$ ./scripts/config --module KVM --disable TASK_DELAY_ACCT --disable SCHEDSTATS
$ make olddefconfig
# CONFIG_SCHEDSTATS is not set
# CONFIG_TASK_DELAY_ACCT is not set
CONFIG_KVM=m
CONFIG_SCHED_INFO=y          <-- still on

$ ./scripts/config --disable KVM --disable VIRTUALIZATION ...
$ make olddefconfig
(SCHED_INFO absent)          <-- only without KVM
```

A `select` from a tristate forces a bool to `y`, so `CONFIG_KVM=m` is
enough. Every Fedora kernel ships KVM.

**Implication.** Variant B can still set `TASK_DELAY_ACCT=n` and
`SCHEDSTATS=n`, which removes the `schedstat_*` counters and the
`rq_sched_info` updates. But the headline cost — `sched_info_enqueue`,
`sched_info_dequeue` and `sched_info_switch` on every enqueue, dequeue and
context switch — **stays**, because those are gated on `CONFIG_SCHED_INFO`
alone. Getting rid of them requires giving up KVM, i.e. virt-manager,
GNOME Boxes and every VM workflow. That trade is not authorised by your
brief and I have not made it.

Worth knowing regardless: when `SCHED_INFO=y` there is **no runtime gate at
all** on that code (`kernel/sched/stats.h:232-339`) — no static key, no
sysctl. Turning delay accounting off at runtime does not disable it, and
`sched_info_arrive`/`sched_info_dequeue` can reach a real
`ktime_get_real_ts64()` clocksource read (`stats.h:251`, `:278`) when a new
maximum run-delay is observed.

### 3.2 Variant C's preemption model — half the options do not exist

```
kernel/Kconfig.preempt:25   PREEMPT_NONE       depends on ARCH_NO_PREEMPT
kernel/Kconfig.preempt:40   PREEMPT_VOLUNTARY  depends on !ARCH_HAS_PREEMPT_LAZY
arch/x86/Kconfig:98         select ARCH_HAS_PREEMPT_LAZY
```

x86 sets `ARCH_HAS_PREEMPT_LAZY` and does not set `ARCH_NO_PREEMPT`, so on
this tree **x86_64 can only choose `PREEMPT` or `PREEMPT_LAZY`**. The same
restriction applies at runtime: `preempt=none` and `preempt=voluntary` are
rejected on the kernel command line (`kernel/sched/core.c:7917`).

Your Variant C asks for `PREEMPT_LAZY=y`, which is available and is in fact
the choice default — so the variant is valid. This is recorded because any
instinct to reach for `PREEMPT_VOLUNTARY` as a desktop tuning knob will
fail, and because it constrains the HZ discussion below.

### 3.3 Variant C carries a latency cost that must be measured, not assumed

`PREEMPT_LAZY` defers a reschedule request to the next tick
(`kernel/sched/core.c:5786`):

```c
if (dynamic_preempt_lazy() && tif_test_bit(TIF_NEED_RESCHED_LAZY))
        resched_curr(rq);
```

So HZ directly bounds the added latency: **~1 ms at HZ=1000, ~4 ms at
HZ=250**. Variant C changes both at once, in the direction that increases
it four-fold.

Your stated objectives include "no unacceptable desktop-latency
regression". On a GNOME/Wayland compositor at 60 Hz the frame budget is
16.7 ms, so a 4 ms worst-case preemption delay is a meaningful fraction of
it. **Variant C must be split** into HZ-only and PREEMPT_LAZY-only arms, or
its two effects cannot be attributed — and the p99 wakeup latency result
matters more here than any throughput number.

`NO_HZ_IDLE` does not interact: `sched_can_stop_tick()` keeps the tick
running whenever more than one task is queued (`core.c:1461`), so the
lazy-to-eager promotion is never lost when there is something to preempt
to.

### 3.4 Variant A is better justified than your brief claims

`CONFIG_SCHED_CACHE` exists (`init/Kconfig:1025`), is `default y`, and
depends only on `SMP` — so it is on in any normal build.

The per-mm cost is unconditional, with **no** static-key guard
(`kernel/fork.c:1138`, in `mm_init()`):

```c
if (mm_alloc_sched(mm))
        goto fail_sched;
```

`mm_init_sched()` (`kernel/sched/fair.c:1575`) then does something worse
than a memset:

```c
for_each_possible_cpu(i) {
        struct sched_cache_time *pcpu_sched = per_cpu_ptr(_pcpu_sched, i);
        struct rq *rq = cpu_rq(i);
        pcpu_sched->runtime = 0;
        pcpu_sched->epoch = rq->cpu_epoch;   /* remote rq cacheline */
        epoch = rq->cpu_epoch;
}
```

Every process creation touches a **remote runqueue cacheline per possible
CPU**. Plus 16 bytes of percpu data per CPU per mm (192 B at 12 threads),
plus a `____cacheline_aligned_in_smp` `struct sched_cache_stat` embedded in
every `mm_struct` (`include/linux/mm_types.h:1214`).

And on a single-LLC machine this buys nothing. `sd_in_multi_llcs()`
(`topology.c:1067`) returns false, so `sched_cache_present` and then
`sched_cache_active` are both disabled (`topology.c:1028`, `:947`) — the
hot path is nop'd out while the allocation cost is still paid in full.

**One correction to your brief:** the hot path is not *entirely*
static-key eliminated. `account_llc_enqueue()` / `account_llc_dequeue()`
(`fair.c:1502`, `:1536`) are called from `account_entity_enqueue/dequeue`
(`fair.c:4476`, `:4491`) with no `sched_cache_enabled()` check — they are
gated only by a `p->preferred_llc < 0` test, so a load-and-branch survives
on every enqueue and dequeue.

### 3.5 Variant D is much better justified than "unused feature"

`SCHED_CLASS_EXT` (`kernel/Kconfig.preempt:169`, depends on `BPF_SYSCALL &&
BPF_JIT && DEBUG_INFO_BTF` — all present on Fedora) is **not** free when no
BPF scheduler is loaded.

`pick_next_task` *is* properly static-key gated (`core.c:6132`) and costs
nothing. But fork and exit are not:

```c
/* kernel/sched/ext/ext.c:3869, from sched_post_fork() unconditionally */
scoped_guard(raw_spinlock_irq, &scx_tasks_lock) {
        list_add_tail(&p->scx.tasks_node, &scx_tasks);

/* kernel/sched/ext/ext.c:3930, from finish_task_switch() unconditionally */
scoped_guard(raw_spinlock_irqsave, &scx_tasks_lock) {
        list_del_init(&p->scx.tasks_node);
```

`scx_tasks_lock` is a **global** raw spinlock (`ext.c:67`). Every fork and
every exit on the system takes it, whether or not sched_ext is in use. Add
a percpu-rwsem read (`scx_pre_fork`, `ext.c:3811`), a TID allocation, and a
~216–232 byte `struct sched_ext_entity` memset per task.

On a 12-thread fork-heavy workload this is a real scalability cost, and it
makes Variant D worth measuring on its own rather than bundling it into a
cleanup.

*Note: your brief cites `kernel/sched/ext.c`. In this tree sched_ext is a
directory — `kernel/sched/ext/{ext.c,idle.c,arena.c,cid.c,...}`.*

### 3.6 A verification trap that would have produced a false result

```
/sys/kernel/debug/sched/llc_balancing/enabled
```

reads back `sysctl_sched_cache_user` (`kernel/sched/debug.c:234`), which is
initialised to `1` (`topology.c:857`) and **stays 1 on hardware where the
feature is inactive**. It reports user intent, not the static-key state.
Reading `1` here on your single-LLC 5500 would mean nothing.

Use instead:

```sh
cat /sys/kernel/debug/sched/domains/cpu0/domain*/name    # expect SMT, MC only
cat /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list | sort -u
                                                          # expect one line: 0-11
```

A third domain above `MC`, or two distinct L3 masks, means the machine is
not single-LLC and Variant A's premise fails.

### 3.7 Variant F is almost entirely a no-op

Three of its five options are already forced or already the default on
x86_64, so setting them changes nothing:

| Option | Reality |
|---|---|
| `X86_AMD_PSTATE=y` | **Already forced.** `SCHED_MC_PRIO` (`arch/x86/Kconfig:1049`, `default y`) does `select X86_AMD_PSTATE if CPU_SUP_AMD && ACPI` (`:1053`). `--disable X86_AMD_PSTATE` is a no-op. |
| `X86_AMD_PSTATE_DEFAULT_MODE=3` | **Already the default** (`drivers/cpufreq/Kconfig.x86:57`, `range 1 4`). 3 = `AMD_PSTATE_ACTIVE` (EPP mode), `drivers/cpufreq/amd-pstate.h:145`. |
| `CPU_IDLE=y` | **Already forced**, twice over: `default y if ACPI`, and transitively `SCHED_MC_PRIO -> X86_AMD_PSTATE -> ACPI_PROCESSOR -> ACPI_PROCESSOR_IDLE -> CPU_IDLE`. |
| `CPU_SUP_AMD=y` | **Already `default y`**, and not freely disableable anyway — `CPU_SUP_HYGON` (`arch/x86/Kconfig.cpu:346`) selects it. |
| `X86_NATIVE_CPU=y` | **The only real change.** `arch/x86/Makefile:162` swaps `-march=x86-64 -mtune=generic` for `-march=native`. |

So Variant F should be relabelled: it is a *compiler* variant, not a power
variant. That also makes it the one variant that **must** be compiled on
the 5500 itself, and the one whose resulting kernel will not boot on other
hardware.

### 3.8 Other reverse dependencies that would have silently defeated changes

| Target | Silently re-enabled by | Location |
|---|---|---|
| `SCHEDSTATS=n` | `LATENCYTOP` | `lib/Kconfig.debug:1911` |
| `PROVE_LOCKING=n` | `DEBUG_NET_SMALL_RTNL` | `net/Kconfig.debug:31` |
| `FAIR_GROUP_SCHED=n` | `SCHED_AUTOGROUP` | `init/Kconfig:1487` |
| `X86_AMD_PSTATE=n` | `SCHED_MC_PRIO` | `arch/x86/Kconfig:1053` |
| `CPU_SUP_AMD=n` | `CPU_SUP_HYGON` | `arch/x86/Kconfig.cpu:346` |

`gen-variants.sh` checks for exactly this class of failure after every
`olddefconfig` and reports any option that did not take the requested
value.

Two more worth recording:

- **`SCHED_MM_CID` is not user-selectable** (`init/Kconfig:1211`,
  `def_bool y`, no prompt). Your brief says not to change it; it could not
  be changed anyway without disabling `SMP` or `RSEQ`.
- **`SCHED_CLASS_EXT` drags in weight.** It requires `DEBUG_INFO_BTF`,
  which forces `DEBUG_INFO=y` and needs pahole >= 1.22 at build time, and
  enabling it also pulls in `EXT_GROUP_SCHED`, `EXT_SUB_SCHED`,
  `GROUP_SCHED_WEIGHT`, `GROUP_SCHED_BANDWIDTH` and `STACKTRACE`. If
  Variant D wins, the build-time and image-size saving may exceed the
  runtime one — but check whether anything else you run needs BTF
  (bpftrace, some eBPF tooling) before dropping it.

### 3.9 A caution about `x86_64_defconfig` as a reference point

`arch/x86/configs/x86_64_defconfig:7` sets `CONFIG_PREEMPT_VOLUNTARY=y`,
which per §3.2 is unselectable on x86 in this tree. Kconfig discards it
silently, and `make defconfig` actually yields `PREEMPT_LAZY=y` plus
`PREEMPT_DYNAMIC=y`. Anyone describing that baseline as "voluntary
preemption" would be wrong.

This is a second, independent reason not to use `x86_64_defconfig` as the
baseline: it does not even describe itself accurately.

---

## 3.10 Methodology corrections

The proposed benchmark plan has defects that would have produced confident
wrong answers. The harness in this branch implements the fixes.

**Tooling reality in this tree.** `hackbench` and `schbench` are **not**
present. `perf bench sched messaging` *is* hackbench — same benchmark,
different defaults — so running both and treating agreement as
corroboration double-counts. `perf bench syscall fork` and `execve` do
exist (`tools/perf/bench/syscall.c`) and are preferable to an ad-hoc
benchmark because they are versioned with the tree; the harness runs both
those and a local `forkbench` that separates thread from fork from exec,
which is what localises a per-mm cost as opposed to a per-task one.

**`perf bench -r` does not apply to `sched pipe` or `sched messaging`.**
`bench_repeat` is honoured only by the futex and breakpoint benchmarks. A
harness relying on `-r 15` would run **one** repetition and report it as
fifteen. Repetition must come from outside, as it does here.

**`--format=simple` prints milliseconds.** For a sub-second benchmark that
is ~0.5% quantisation before any real noise. The harness uses
`--format=default` for `sched pipe`, which reports `usecs/op`.

**`sched pipe` is bimodal and the mode is not under your control.** If the
wakee lands on an idle sibling, every iteration pays a reschedule IPI and a
C-state exit, and you are measuring idle-exit latency rather than the
scheduler. The harness records `cpu-migrations` alongside every pipe result
so the mode is visible: ~0 means same-CPU, ~2M means you measured something
else.

**Percentiles across 15 reps are void.** Establishing a true p99 needs
~299 samples; the 15th order statistic estimates roughly the 96.7th
percentile with enormous variance. The analyzer now reports the observed
maximum as *descriptive* and refuses to present p95/p99 across reps as a
result. Percentiles are meaningful only from a benchmark that samples
millions of events internally.

**Multiple comparisons were unaddressed.** Eight benchmarks at α=0.05 give
a 34% chance of at least one false positive; adding eight perf counters
each takes it to 96%. The analyzer applies Holm–Bonferroni across every
metric in a comparison.

**Run an A/A experiment first.** Boot one kernel, run the campaign twice
under two different tags, and analyse it as an A/B test. The largest
difference that appears is this machine's real noise floor, and no claim
below it is credible. The analyzer prints this instruction on every run.

**Thermal drift will manufacture a regression.** The 5500 is a 65 W part
with Precision Boost 2, whose all-core clock falls *continuously* as
package temperature rises. Running all reps of A then all reps of B
confounds variant with temperature perfectly. The harness randomises
benchmark order within each repetition; across kernels, interleave whole
passes rather than completing one kernel before starting the next.

**Fedora-specific traps.** `power-profiles-daemon` and `tuned` silently
reset the governor — stop them *before* setting it, and re-read
`scaling_governor` after. If a variant's NVIDIA DKMS build fails the system
falls back to nouveau **silently**, changing idle power by tens of watts;
assert `lsmod | grep -q '^nvidia'` per boot. `tracker-miner-fs-3` indexes
for minutes *after* a kernel build finishes, contaminating whatever runs
next.

**The kernel build is a regression guard, not an improvement detector.**
Kernel-mode cycles are typically 10–15% of a build, and scheduler code well
under 2% of total; the transfer coefficient is roughly 0.01–0.02, so a 20%
scheduler improvement surfaces as 0.2–0.4% wall time — at or below the best
achievable noise floor. It can catch a pathology. It cannot substantiate a
positive claim, and this document will not use it for one.

**Rep count.** 15 reps detects an effect of roughly one coefficient of
variation. That is adequate for pinned `sched pipe` (CV ~0.5%) and the
kernel build, marginal for `sched messaging` and fork/exec, and inadequate
for tail latency. 25 blocks is the better default for everything except
pinned `sched pipe`.

---

## 4. What I need from you to proceed

Run this on the Ryzen box and send back the directory. It is read-only:
installs nothing, changes no setting, touches no boot entry.

```sh
git checkout perf/ryzen5500-scheduler-config
tools/testing/finux-perf/collect-baseline.sh
tar czf finux-baseline.tar.gz finux-baseline-*
```

It captures the running kernel config, LLC/SMT topology, cpufreq driver and
EPP, microcode, cgroup version, PSI consumers, sched_ext state, toolchain,
NVIDIA/32-bit status, and the diagnostic options already enabled in your
baseline.

**The single blocking item is the running kernel's config.** If
`/proc/config.gz` and `/boot/config-$(uname -r)` are both absent, point me
at the `.config` from the Finux build that produced your running kernel.

### Decisions I need from you

1. **KVM vs Variant B.** Keeping KVM means `SCHED_INFO=y` stays. Is
   dropping virtualisation on the table? My recommendation is **no** — the
   functionality loss is large and the gain is unquantified.
2. **Variant C split.** I intend to test HZ_250 and PREEMPT_LAZY
   separately, because bundling them makes a latency regression
   unattributable. Confirm that is acceptable.
3. **Governor policy.** Scheduler deltas on a 6-core desktop are easily
   swamped by DVFS. Pin `performance` for all variants (cleaner signal,
   less representative of daily use) or leave `schedutil`/EPP as you run it
   (representative, noisier)? I lean toward measuring both for the
   headline benchmark and pinning for the microbenchmarks.

---

## 5. Deliverables present in this branch

| Path | State |
|---|---|
| `tools/testing/finux-perf/collect-baseline.sh` | Complete, smoke-tested |
| `tools/testing/finux-perf/gen-variants.sh` | Complete, exercised end-to-end |
| `tools/testing/finux-perf/bench-kernel.sh` | Complete, syntax-checked; unrun (no target) |
| `tools/testing/finux-perf/forkbench.c` | Complete, compiles clean, smoke-tested |
| `tools/testing/finux-perf/analyze.py` | Complete, validated against synthetic effects |
| `PERFORMANCE_AUDIT.md` | This document |
| `arch/x86/configs/finux_ryzen5500_defconfig` | **Deliberately absent — see §2.2** |

### Notes on the tooling

**`gen-variants.sh`** applies each variant, runs `olddefconfig`, then
verifies every requested option actually took the value asked for. This
matters: a symbol with a reverse dependency comes back silently, and a
variant that changed nothing would otherwise be benchmarked as if it had.
It also checks a list of invariants — `IA32_EMULATION`, `CPU_MITIGATIONS`,
`SCHED_SMT`, `CPU_IDLE`, `CGROUPS`, `PERF_EVENTS` and others — and fails if
a tuning pass moved any of them.

Exercised against this VM's config, it correctly reported
`SCHED_CLASS_EXT` as not applied, and distinguished "dependencies unmet"
from "symbol does not exist".

**`analyze.py`** uses median (not mean — these distributions are
right-skewed), Mann-Whitney U (no normality assumption), a bootstrap CI on
the median difference, and a 2% noise floor. Validated against synthetic
data with a planted −6% improvement, a planted +4% regression, and a
planted null: it identified all three correctly, including refusing to call
the null a change.

**`bench-kernel.sh`** randomises benchmark order per repetition so thermal
drift cannot masquerade as a between-benchmark difference, refuses to start
a repetition until load settles, waits out the first 120 s of uptime, and
records governor/EPP/microcode per run so incomparable runs can be
detected after the fact. It installs nothing; missing tools are reported
and their benchmarks skipped.

---

## 6. Risks and compatibility notes

| Item | Risk |
|---|---|
| `SCHED_INFO=n` | Unreachable with KVM. Not attempted. |
| `HZ_250` + `PREEMPT_LAZY` | Lazy-preemption latency 1 ms → 4 ms. Must be measured against a 16.7 ms frame budget. |
| `PSI_DEFAULT_DISABLED=y` | Loses `/proc/pressure` and cgroup pressure. `systemd-oomd` is enabled by default on Fedora and consumes PSI; the collector checks this before the variant is considered. |
| `X86_NATIVE_CPU=y` | Only valid if the kernel is compiled **on** the 5500. Produces a kernel that may not boot on other hardware. |
| `SCHED_CLASS_EXT=n` | Forecloses future sched_ext / scx schedulers without a rebuild. |
| `CGROUP_CPUACCT=n` | v1 accounting only; check for v1 consumers before relying on it. |

Untouched throughout, as instructed: CPU mitigations, SMT, cpuidle, cpufreq,
cgroup v2, `FAIR_GROUP_SCHED`, perf, `IA32_EMULATION`, `SCHED_MM_CID`,
HRTICK, fair-server runtime. No `idle=poll`, no `nohlt`, no
`mitigations=off`.

---

## 7. Remaining research candidates

Recorded, not acted on:

- **`SCHED_CACHE` runtime toggle.** If the box turns out to be multi-LLC,
  compare via `/sys/kernel/debug/sched/llc_balancing/` rather than a
  rebuild — but read §3.6 first, the obvious file lies.
- **`split_llc=` boot parameter** (`arch/x86/kernel/smpboot.c:427`) is new
  in this tree and can artificially fragment the LLC domain. Useful to
  force Variant A's alternate path for testing; a trap if set accidentally.
- **`DEBUG_INFO_BTF`** is required by `SCHED_CLASS_EXT` and costs
  significant build time and image size. If Variant D wins, check whether
  BTF is still needed for anything else you run (bpftrace, some eBPF
  tooling) before dropping it.
- **Fedora baseline diagnostics.** Distribution kernels often ship
  `DEBUG_LIST`, `DEBUG_SG`, `SLUB_DEBUG` or `DEBUG_KOBJECT` enabled.
  Variant E may turn out to be the largest single win, or a no-op — the
  collector reports which.
- **`nmi_watchdog`** costs a per-CPU perf event. Not a config change, but
  worth recording during benchmarking.
