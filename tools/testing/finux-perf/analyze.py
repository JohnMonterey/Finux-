#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compare benchmark samples across kernel variants.

Reads the samples.jsonl files written by bench-kernel.sh and reports, for
every benchmark and metric, whether a variant differs from the baseline by
more than measurement noise.

The statistics are deliberately conservative.  A kernel configuration
change that cannot be distinguished from noise should be reverted, not
kept on the grounds that it "should" be faster, and the default output is
built to make that the easy conclusion to reach:

  * Median rather than mean.  Benchmark distributions are right-skewed -
    a rep that collided with a background wakeup adds a long tail - and
    the mean chases those tails.

  * Mann-Whitney U rather than a t-test.  It assumes nothing about the
    distribution shape, which matters because these are not normal.

  * A bootstrap confidence interval on the median difference, so the
    report says how uncertain it is rather than just whether p < 0.05.

  * Holm-Bonferroni correction across all metrics in a comparison.

  * A noise floor.  With 15 reps on a 6-core desktop running a compositor,
    differences below a couple of percent are not credible regardless of
    what the p-value says.  Anything inside the floor is reported as
    NOISE even when it is statistically significant, because statistical
    significance on a small effect mostly measures how well the machine
    was quiesced.

Usage:
    analyze.py <baseline.jsonl> <variant.jsonl> [variant2.jsonl ...]
    analyze.py finux-bench/*/samples.jsonl
"""

import json
import math
import random
import sys
from collections import defaultdict

# Below this relative difference, refuse to call it a change.
NOISE_FLOOR_PCT = 2.0
# Bootstrap resamples for the CI on the median difference.
BOOTSTRAP_N = 10000
ALPHA = 0.05

# Metrics where a larger number is better.  Everything else is
# lower-is-better (times, latencies, counts of scheduler work).
HIGHER_IS_BETTER = {"instructions-per-cycle", "ops-per-sec", "throughput"}


def load(path):
    """Return {(bench, metric): [values]} plus the tag for one file."""
    data = defaultdict(list)
    tag = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            tag = rec.get("tag", tag)
            data[(rec["bench"], rec["metric"])].append(float(rec["value"]))
    return tag, data


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return float("nan")
    mid = n // 2
    return s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2.0


def percentile(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * (p / 100.0)
    lo, hi = math.floor(k), math.ceil(k)
    if lo == hi:
        return s[int(k)]
    return s[lo] * (hi - k) + s[hi] * (k - lo)


def mad(xs):
    """Median absolute deviation - a spread measure the outliers cannot move."""
    m = median(xs)
    return median([abs(x - m) for x in xs])


def mann_whitney_u(a, b):
    """Two-sided Mann-Whitney U with a normal approximation and tie
    correction.  Returns an approximate p-value.

    Exact for our purposes: with n>=15 per group the normal approximation
    is sound, and we are not making fine distinctions near the threshold.
    """
    n1, n2 = len(a), len(b)
    if n1 == 0 or n2 == 0:
        return 1.0

    combined = sorted([(v, 0) for v in a] + [(v, 1) for v in b])
    ranks = [0.0] * len(combined)
    i = 0
    tie_correction = 0.0
    while i < len(combined):
        j = i
        while j + 1 < len(combined) and combined[j + 1][0] == combined[i][0]:
            j += 1
        avg_rank = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[k] = avg_rank
        t = j - i + 1
        if t > 1:
            tie_correction += t ** 3 - t
        i = j + 1

    r1 = sum(r for r, (_, g) in zip(ranks, combined) if g == 0)
    u1 = r1 - n1 * (n1 + 1) / 2.0
    u2 = n1 * n2 - u1
    u = min(u1, u2)

    mu = n1 * n2 / 2.0
    n = n1 + n2
    sigma_sq = (n1 * n2 / 12.0) * ((n + 1) - tie_correction / (n * (n - 1)))
    if sigma_sq <= 0:
        return 1.0
    z = (u - mu) / math.sqrt(sigma_sq)
    # Two-sided normal tail.
    return max(0.0, min(1.0, math.erfc(abs(z) / math.sqrt(2))))


def bootstrap_median_diff_ci(base, var, n=BOOTSTRAP_N, alpha=ALPHA):
    """Percentile bootstrap CI for (median(var) - median(base))."""
    rng = random.Random(12345)  # fixed seed: same data gives same interval
    diffs = []
    for _ in range(n):
        rb = [base[rng.randrange(len(base))] for _ in range(len(base))]
        rv = [var[rng.randrange(len(var))] for _ in range(len(var))]
        diffs.append(median(rv) - median(rb))
    diffs.sort()
    lo = diffs[int((alpha / 2) * n)]
    hi = diffs[min(n - 1, int((1 - alpha / 2) * n))]
    return lo, hi


def holm(pvals):
    """Holm-Bonferroni adjusted p-values, order preserved.

    Eight benchmarks tested at alpha=0.05 give a 34% chance of at least one
    false positive; add eight perf counters each and it is 96%. Without a
    correction this report would reliably manufacture a result from noise.
    """
    m = len(pvals)
    order = sorted(range(m), key=lambda i: pvals[i])
    adj = [0.0] * m
    running = 0.0
    for rank, i in enumerate(order):
        val = (m - rank) * pvals[i]
        running = max(running, val)          # enforce monotonicity
        adj[i] = min(1.0, running)
    return adj


def verdict(pct, p, ci_lo_pct, ci_hi_pct, higher_better):
    if math.isnan(pct):
        return "NO DATA"
    # A CI straddling zero means the direction itself is unresolved.
    if ci_lo_pct <= 0 <= ci_hi_pct:
        return "NOISE (CI spans 0)"
    if abs(pct) < NOISE_FLOOR_PCT:
        return f"NOISE (<{NOISE_FLOOR_PCT}%)"
    if p >= ALPHA:
        return f"NOT SIGNIFICANT (p={p:.3f})"
    improved = (pct > 0) if higher_better else (pct < 0)
    return "FASTER" if improved else "SLOWER"


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    files = argv[1:]
    loaded = []
    for path in files:
        try:
            tag, data = load(path)
        except OSError as e:
            print(f"cannot read {path}: {e}", file=sys.stderr)
            return 1
        loaded.append((tag or path, data, path))

    base_tag, base_data, base_path = loaded[0]
    print(f"baseline: {base_tag}  ({base_path})")
    print(f"noise floor: {NOISE_FLOOR_PCT}%   alpha: {ALPHA}   "
          f"bootstrap: {BOOTSTRAP_N}")
    print()

    any_regression = False

    for tag, data, path in loaded[1:]:
        print("=" * 78)
        print(f"variant: {tag}   vs baseline {base_tag}")
        print("=" * 78)
        header = (f"{'benchmark/metric':<36} {'base':>11} {'variant':>11} "
                  f"{'delta%':>8}  verdict")
        print(header)
        print("-" * 78)

        rows, pvals = [], []
        for key in sorted(set(base_data) | set(data)):
            bench, metric = key
            b = base_data.get(key, [])
            v = data.get(key, [])
            label = f"{bench}/{metric}"

            if len(b) < 3 or len(v) < 3:
                print(f"{label:<36} {'-':>11} {'-':>11} {'-':>8}  "
                      f"INSUFFICIENT DATA (n={len(b)}/{len(v)})")
                continue

            mb, mv = median(b), median(v)
            pct = ((mv - mb) / mb * 100.0) if mb else float("nan")
            p = mann_whitney_u(b, v)
            lo, hi = bootstrap_median_diff_ci(b, v)
            rows.append((label, metric, b, v, mb, mv, pct, lo, hi))
            pvals.append(p)

        adj = holm(pvals) if pvals else []

        for (label, metric, b, v, mb, mv, pct, lo, hi), p in zip(rows, adj):
            lo_pct = (lo / mb * 100.0) if mb else float("nan")
            hi_pct = (hi / mb * 100.0) if mb else float("nan")
            hb = metric in HIGHER_IS_BETTER
            vd = verdict(pct, p, lo_pct, hi_pct, hb)

            if vd == "SLOWER":
                any_regression = True

            print(f"{label:<36} {mb:>11.4g} {mv:>11.4g} {pct:>+7.2f}%  {vd}")
            print(f"{'':<36} {'n=' + str(len(b)):>11} "
                  f"{'n=' + str(len(v)):>11}          "
                  f"95% CI [{lo_pct:+.2f}%, {hi_pct:+.2f}%]  "
                  f"p(Holm)={p:.4f}")
            # Percentiles across ~15 reps are descriptive only: estimating
            # a true p99 needs ~299 samples.  Shown to expose outliers,
            # never as a result.
            print(f"{'':<36} {'MAD ' + format(mad(b), '.3g'):>11} "
                  f"{'MAD ' + format(mad(v), '.3g'):>11}          "
                  f"[descriptive] max {max(b):.4g} -> {max(v):.4g}")
        print()

    print("=" * 78)
    if any_regression:
        print("At least one metric regressed.  A variant that makes anything")
        print("meaningfully slower should not be kept on the strength of an")
        print("improvement elsewhere without saying so explicitly.")
    else:
        print("No metric regressed beyond the noise floor.")
    print()
    print("Before trusting any of this, run the same kernel twice under two")
    print("different tags and analyse it as if it were an A/B test.  The")
    print("largest difference that appears in that A/A run is the real noise")
    print("floor for this machine; no claim below it is credible.")
    print("Anything marked NOISE or NOT SIGNIFICANT has not been shown to")
    print("help.  Revert it rather than keeping it on the assumption that")
    print("less code must be faster.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
