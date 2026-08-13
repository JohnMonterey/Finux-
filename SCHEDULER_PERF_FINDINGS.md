# Scheduler and performance findings — Finux 7.2.0-rc7

Target machine: AMD Ryzen 5 5500 (Zen 3, 6c/12t, **single LLC**), X470,
Fedora, GNOME/Wayland, NVIDIA out-of-tree modules, Steam/Wine + 32-bit.

This is the read-the-source pass that precedes measurement. Every row was
checked against this checkout, not against memory or folklore; the
`file:line` column is where you can go and disagree with me.

## Read this before the tables

**Nothing here has been benchmarked.** This container is not the target
machine — different CPU, no NVIDIA driver, no compositor, no Steam. Source
reading tells you where the work *is*; it cannot tell you whether removing
it is measurable on your desktop. Several rows below will turn out to be
noise. That is the expected outcome for most of them, and
`tools/testing/finux-perf/analyze.py` is deliberately built to say so.

**The Fedora baseline config has still not been captured.** For most
config rows the question "is this even on?" is currently unanswered. Run
`tools/testing/finux-perf/collect-baseline.sh` on the Ryzen box first; until
then the config table is a list of things to *check*, not a list of things
to change.

**Ranking is by expected effect, and "expected" is doing real work in that
sentence.** Where I could not honestly guess, the column says so.

### Legend

| Column | Meaning |
| --- | --- |
| **Effect** | High / Med / Low / **?** — my prior before measurement. **?** means I genuinely don't know. |
| **Risk** | What you lose or might break. "None" means reversible with no functional change. |
| **Reversible** | Boot = reboot to undo. Live = undo without rebooting. Build = requires a rebuild. |

---

## Table A — Runtime, no rebuild required (test these first)

These need no kernel build. Test them on the kernel you are already
running. If a row does nothing here, it will not suddenly do something
after being baked into a config.

| ID | Finding | Where | Effect | Action | Risk | Reversible |
| --- | --- | --- | --- | --- | --- | --- |
| **A1** | Once `auditd` has run, **every** `fork()` allocates an audit context and sets `SYSCALL_AUDIT` on the child — permanently, for every syscall that task makes | `kernel/auditsc.c:1057` (`audit_alloc`), guard at `:1063` | **High** for Wine/Proton | Boot with `audit=0`, mask `auditd` | Loses audit records; SELinux denials fall back to dmesg | Boot |
| **A2** | amd-pstate writes the **battery** EPP preference on a desktop: init unconditionally applies `epp_default_dc` = `BALANCE_PERFORMANCE` (0x80). `epp_default_ac` = `PERFORMANCE` (0x00) is only ever consulted by the dynamic-EPP path | `drivers/cpufreq/amd-pstate.c:1946`, set at `:1938-1939`, AC path at `:1156-1158` | **High** | `echo performance > /sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference` | More power and heat; higher idle clocks | Live |
| **A3** | `HRTICK` and `HRTICK_DL` default **true** in this tree (upstream historically defaulted them off) — a per-CPU hrtimer may be armed for the slice end on context switch | `kernel/sched/features.h:69-70`, enabled because `CONFIG_HRTIMER_REARM_DEFERRED` is `def_bool y` at `kernel/time/Kconfig:54-57` | **?** — see §Disagreement | `echo NO_HRTICK > /sys/kernel/debug/sched/features`, measure **both ways** | None | Live |
| **A4** | Fedora runs the `menu` idle governor because it outranks `teo` by one point. `teo` is generally the better predictor on modern client CPUs | `drivers/cpuidle/governors/menu.c:530` (rating 20) vs `teo.c:566` (rating 19) | **Med** | `cpuidle.governor=teo`, or write `/sys/devices/system/cpu/cpuidle/current_governor` | Different idle-exit latency profile | Live |
| **A5** | PSI adds a third clock read to the switch path and walks the cgroup ancestry to the root — twice when `prev`/`next` do not share an early common ancestor. A Fedora user-session service sits ~6 levels deep | `kernel/sched/psi.c:934` (`cpu_clock`), `:943` and `:984` (the two `for_each_group` walks) | **Med** | `cgroup_disable=pressure` (surgical) or `psi=0` (whole subsystem) | **Breaks systemd-oomd.** Check `systemctl is-active systemd-oomd` first | Boot |
| **A6** | `INIT_ON_ALLOC_DEFAULT_ON` zeroes the whole `task_struct` on allocation, then `dup_task_struct` immediately memcpys over it | `security/Kconfig.hardening:159`; kernel's own note: "*most cases see <1% impact… as high as 7%*" at `:169-170` | **Med** on fork-heavy | `init_on_alloc=0` | **Real hardening loss** — uninitialised-heap reads stop being zeros | Boot |
| **A7** | Tuning `base_slice_ns` via debugfs is **half-applied**: `set_protect_slice()` reads `normalized_sysctl_sched_base_slice`, a separate static that debugfs never writes | `kernel/sched/fair.c:79-80` (both statics, 700000), read at `:1086` | Trap, not a win | Know about it before tuning. Only bites with `NO_RUN_TO_PARITY` | — | — |
| **A8** | MGLRU is a live toggle worth an A/B under memory pressure (shader compile + game) | `/sys/kernel/mm/lru_gen/enabled` | **?** | Toggle, measure under real pressure | None | Live |

---

## Table B — Config changes (require a rebuild)

Sorted by expected effect. **Verify each against the captured Fedora
baseline before assuming it is on** — several of these may already be off.
`gen-variants.sh` proves an option actually took the value you asked for,
which matters because Kconfig silently re-selects.

| ID | Finding | Where | Effect | Action | Risk |
| --- | --- | --- | --- | --- | --- |
| **B1** | `MITIGATION_CALL_DEPTH_TRACKING` is `default y` and pulls `CALL_THUNKS` → `CALL_PADDING` → `-fpatchable-function-entry=16,16` across the **whole kernel**. It mitigates an **Intel** SKL RSB-underflow bug, is off unless you pass `retbleed=stuff`, and does nothing on Zen 3 | `arch/x86/Kconfig:2455-2460`; chain at `:2396-2398` → `:2378`; flags at `arch/x86/Makefile:228`. Kernel's own help text at `:2468`: "*increases text size by ~5%… For non affected systems this space is unused*" | **High** — ~5% of text is padding, i.e. I-cache and iTLB density on every path | `CONFIG_MITIGATION_CALL_DEPTH_TRACKING=n`. It has a prompt, so you can disable it directly and keep `CPU_SUP_INTEL=y` | **Not a mitigation removal on this CPU** — inactive on AMD in every configuration. Do not ship this in a generic multi-vendor kernel |
| **B2** | `X86_DEBUG_FPU` is `default y`. It converts an inline pointer-add into an out-of-line exported call on the context-switch path *and* on every exit-to-user | `arch/x86/Kconfig.debug:200-203` (`depends on DEBUG_KERNEL`, which Fedora sets) | **Med–High** | `CONFIG_X86_DEBUG_FPU=n` | None on a production kernel |
| **B3** | `CONFIG_SCHED_CLASS_EXT` takes a **global raw spinlock on every fork and every exit**, whether or not a BPF scheduler is loaded — the `scoped_guard` sits outside the `scx_init_task_enabled` check | `kernel/sched/ext/ext.c:67` (the lock), `:3869-3871` (fork), `:3930-3934` (exit); also `percpu_up_read(&scx_fork_rwsem)` at `:3876` | **Med** on 12 threads; scales worse than it looks | `CONFIG_SCHED_CLASS_EXT=n` unless you actually run a `sched_ext` scheduler | Lose BPF schedulers (LAVD, bpfland, scx_rusty) — relevant if you were considering them for gaming |
| **B4** | `CONFIG_SCHED_CACHE` allocates a **per-CPU** struct per `mm` and loops `for_each_possible_cpu` to initialise it on every `fork()`+`exec()` — while the balancer it feeds is fully NOP-patched on a single-LLC machine | `include/linux/mm_types.h:1615-1623` (`alloc_percpu` + init), loop at `kernel/sched/fair.c:1581`; called from `kernel/fork.c:1138`, freed at `:732`/`:1149` | **Med** on fork-heavy | `CONFIG_SCHED_CACHE=n` | None on single-LLC. Would matter on a dual-CCD part |
| **B5** | `CONFIG_SCHED_INFO` puts `sched_info_arrive`/`sched_info_depart` in the switch path with **no static key** — unlike the rest of schedstats. The min/max bookkeeping runs always; `ktime_get_real_ts64()` fires on each new maximum run-delay | `kernel/sched/stats.h:232` (the only guard), `:251` and `:278` (the wall-clock reads), `:321-333` (`sched_info_switch`) | **Med**, but see risk | **Blocked:** measured empirically that `CONFIG_KVM=m` forces `SCHED_INFO=y` even with `TASK_DELAY_ACCT=n` and `SCHEDSTATS=n` | Unreachable without dropping virtualisation. **Your call, not mine** |
| **B6** | `CONFIG_LATENCYTOP` embeds `latency_record[32]` in every `task_struct` — 32 × ~120 B ≈ **3.8 KB per task** — and `select`s `SCHEDSTATS`, so it silently defeats any attempt to turn schedstats off | `include/linux/sched.h:1461-1462`; `struct latency_record` at `include/linux/latencytop.h:21-26` | **Med** (slab pressure per task) | `CONFIG_LATENCYTOP=n` | Lose `/proc/latency_stats` |
| **B7** | `CONFIG_LOCKDEP` embeds `held_locks[48]` — 48 × ~48 B ≈ **2.3 KB per task**, more with `LOCK_STAT` | `include/linux/sched.h:1287,1291`; `struct held_lock` at `include/linux/lockdep_types.h:206-226` | **High if on** | Should already be off in Fedora's production kernel — **verify**, don't assume | None on production |
| **B8** | `CONFIG_DEBUG_VM` adds a `__read_cr3()` to **every** `switch_mm_irqs_off()`. The comment says as much: "*Only do this check if CONFIG_DEBUG_VM=y because `__read_cr3()` …*" | `arch/x86/mm/tlb.c:805-810` | **Med if on** | Fedora ships it off in production, on in the debug kernel — verify | None on production |
| **B9** | `CONFIG_CGROUP_CPUACCT` is a cgroup-**v1**-only controller, redundant with v2's `cpu.stat`. Fedora is unified v2 | `kernel/sched/cpuacct.c` | **Low**, but free | `CONFIG_CGROUP_CPUACCT=n` | None unless something mounts cgroup v1 |
| **B10** | `CONFIG_SCHED_PROXY_EXEC` splits `donor`/`curr` into two pointers instead of a union, adding 8 bytes to the hot `struct rq` cacheline | `kernel/sched/sched.h:1151-1159` | **Low** | `CONFIG_SCHED_PROXY_EXEC=n` | Lose proxy-execution priority inheritance |
| **B11** | `-march=native` — real, but the smallest and least certain of the build changes | `X86_NATIVE_CPU` | **Low–?** | Variant F already covers it | Kernel only boots on this CPU family. Fine for a personal build |

---

## Table C — Firmware, BIOS, and userspace

Not kernel changes, and potentially larger than several kernel rows.

| ID | Finding | How to check | Effect | Action |
| --- | --- | --- | --- | --- |
| **C1** | X470 boards frequently ship with **CPPC disabled in BIOS**. amd-pstate then falls back to the ACPI shared-memory path with a **1000 µs** transition delay instead of direct MSR writes — every frequency decision becomes a millisecond late | `cat /sys/devices/system/cpu/cpufreq/policy0/scaling_driver` (want `amd-pstate-epp`, not `acpi-cpufreq`); `dmesg \| grep -i "amd.pstate\|cppc"` | **High if disabled** | Enable CPPC / "AMD Cool'n'Quiet" + CPPC in BIOS |
| **C2** | A current AGESA + microcode can retire the SRSO return-thunk tax and the TSA `VERW` on kernel exit — a mitigation cost paid on *every* exit today | `grep . /sys/devices/system/cpu/vulnerabilities/*` | **Med** | Update BIOS/AGESA; keep `linux-firmware` current |
| **C3** | Zen 3's actual exposure is **SRSO, TSA, VMSCAPE, Spectre v1/v2, SSB** — *not* Meltdown, MDS, L1TF, or retbleed. PTI is already off on AMD | Same sysfs directory | Context, not an action | Means `mitigations=off` buys far less here than the Intel-oriented advice implies — and it is out of scope by your own brief |

---

## Table D — Do **not** do these

Every row is either standard internet advice that is wrong for this
hardware, or excluded by your brief. Listed so nobody re-derives them in
three months.

| ID | The advice | Why it is wrong here |
| --- | --- | --- |
| **D1** | `HZ=250` "reduces timer overhead" | **Backwards.** Both idle governors refuse to stop the tick when the predicted idle is below `TICK_NSEC` — `menu.c:371` and `teo.c:507`. Lowering HZ *raises* that threshold, so more idle periods keep the tick running. Worse idle behaviour, not better. **Keep HZ=1000.** Variant C must be split so `PREEMPT_LAZY` is tested on its own |
| **D2** | `tsc=reliable` | The kernel already trusts an invariant TSC where it can. This flag only disables the watchdog that would have caught a real desync |
| **D3** | `timer_migration=0` | Pins timers to their originating CPU, defeating the idle consolidation that lets cores reach deep C-states |
| **D4** | `rcu_nocbs=all` | Offloads RCU callbacks to kthreads. On a 6-core desktop this *adds* wakeups and scheduling work. It is a NOHZ_FULL / RT / loaded-server tool |
| **D5** | `processor.max_cstate=1`, `idle=poll`, `nohlt` | Excluded by your brief, and they trade away the thermal headroom that boost clocks depend on. On a 65 W part that usually costs more than it buys |
| **D6** | `mitigations=off` | Excluded by your brief. See **C3** — the payoff on Zen 3 is much smaller than the Intel-derived benchmarks suggest |
| **D7** | Disabling `CONFIG_CFS_BANDWIDTH` | It is static-key gated and genuinely free when no cgroup sets a quota. Removing it buys nothing and breaks systemd slices that do |
| **D8** | Disabling `SCHED_SMT`, cgroups, `IA32_EMULATION`, `PERF_EVENTS` | Each directly breaks the stated workload (12 threads, systemd, 32-bit Steam/Wine, profiling). Already pinned in `gen-variants.sh`'s `INVARIANTS` list |

---

## Table E — Kernel code defects found while reading

Out of scope for a config-only pass — patches, not settings. **E1 looks
like a genuine upstream bug** and is worth reporting regardless of what
happens to this config study.

| ID | Defect | Where | Why it matters |
| --- | --- | --- | --- |
| **E1** | `check_mm()` runs `percpu_counter_sum()` × `NR_MM_COUNTERS` on **every** `__mmdrop()`, unconditionally — not under `CONFIG_DEBUG_VM`. `percpu_counter_sum()` takes the counter's raw spinlock and loops every online CPU. That is 4 × 12 remote cacheline reads under a raw spinlock **per process exit**, purely to print a warning that never fires | `kernel/fork.c:627-644`, called from `__mmdrop` at `:737` | Every process teardown on the machine. The cheap `percpu_counter_read()` exists and would do for a sanity check |
| **E2** | `sched_info` has no static key, unlike every other schedstat | `kernel/sched/stats.h:232, 251, 278` | See **B5**. The natural fix is a `static_branch`, which would also unblock B5 without dropping KVM |
| **E3** | `struct rq` puts write-every-context-switch `rq->curr` on the same cacheline as fields `select_idle_cpu()` scans **remotely** across all 12 CPUs | `kernel/sched/sched.h:1144-1161` | Classic false sharing. The comment at `:1140-1143` acknowledges the tension and resolves it the other way |
| **E4** | `sched_clock_cpu(cpu)` is called and its result discarded | `kernel/sched/core.c:4059` | Intentional (comment: "*Sync clocks across CPUs*"), but it is a clock read on the wakeup fast path |
| **E5** | `tg->load_avg` is the one genuinely contended cacheline in the scheduler — all 12 CPUs read-modify it | `kernel/sched/fair.c:4812` | Inherent to `FAIR_GROUP_SCHED` + autogroup. Not fixable by configuration without giving up group scheduling |
| **E6** | `percpu_counters_lock` — a truly global spinlock — is taken once per `mm` creation and once per destruction to maintain the CPU-hotplug list | `lib/percpu_counter.c:15`, taken at `:218-221` and `:242-245`, both under `CONFIG_HOTPLUG_CPU` (y on Fedora) | Same fork/exit path as E1. Only one acquisition per `mm` — the `_many` variant batches all four RSS counters — so it is the least bad row here, but it is genuinely global |

---

## The one open disagreement: HRTICK (A3)

Two of the six investigations reached opposite conclusions and I am not
going to paper over it:

- **Keep it on.** `HRTICK` gives EEVDF slice-accurate preemption. With
  `base_slice = 700 µs` and `HZ = 1000`, a 1 ms tick cannot resolve a
  700 µs slice, so without HRTICK the scheduler systematically overruns
  slices. That directly affects frame pacing.
- **Turn it off.** It arms and cancels a per-CPU hrtimer around context
  switches. On a 12-thread desktop switching tens of thousands of times a
  second, that is real work for a guarantee most tasks never need.

Both arguments are sound. Neither is evidence. Settle it with
`echo NO_HRTICK > /sys/kernel/debug/sched/features` and a measurement —
and measure **frame-time consistency**, not throughput, because that is
where the two predictions actually differ. A throughput benchmark will
likely show nothing and prove neither side right.

---

## Suggested order

1. **Capture the baseline.** `collect-baseline.sh` on the Ryzen box. Half
   of Table B is unanswerable without it.
2. **Run an A/A test.** Same kernel, two tags, through `analyze.py`. The
   largest difference that appears is your real noise floor. Every claim
   below it is unfalsifiable on this machine.
3. **Work Table A**, one row at a time, reverting between each. No rebuild,
   so it is cheap, and a row that does nothing here is not worth a build.
4. **Check C1** — the BIOS CPPC state is a five-minute check with a
   plausibly larger payoff than anything in Table B.
5. **Then Table B**, starting with B1. Use `gen-variants.sh` so each option
   is proved to have applied.

Anything that comes back NOISE gets reverted. "It should be faster" is not
a result — that is the whole point of the noise floor in `analyze.py`.

## Still unknown

- Whether the Fedora production kernel already has B2, B7, B8 off.
- Whether `systemd-oomd` is active (gates A5).
- Whether CPPC is enabled in this board's BIOS (gates C1).
- Whether the user wants to trade KVM for B5, split Variant C per D1, or
  change the governor policy. Three decisions still outstanding.
- Whether **any** of this is measurable. That is not a hedge; it is the
  expected outcome for most rows, and the tooling is built to detect it.
