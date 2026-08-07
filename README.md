# Linux Working-Set Observability

> RSS shows residency. Idle page tracking shows access. Refault feedback shows the cost of eviction.

Controlled workloads and kernel instrumentation for observing Linux memory
beyond RSS: `mapped capacity != resident set != observed working set`.

## Components

| Path | Purpose |
| --- | --- |
| [`idle-page-tracking/`](idle-page-tracking/) | Compare repeated, one-time, untouched, and mapped-only regions |
| [`mglru-refault/`](mglru-refault/) | Generate reclaim and refault under memory pressure |
| [`patches/`](patches/) | Add MGLRU shadow-lifecycle tracepoints |
| [`trace-shadow/`](trace-shadow/) | Validate four refault control-flow cases |

Build the workloads with `make -C idle-page-tracking && make -C mglru-refault`.

## Reproduce the shadow trace

Tested as `7.1.0-mglru-shadow`; patch baseline `6b5a2b7d9bc156e505f09e698d85d6a1547c1206`
(`v7.1-2765-g6b5a2b7d9bc1`). Apply the series in order, then build and boot the
kernel; the patches add observability without changing reclaim decisions.

The experiment requires root, MGLRU, cgroup v2 memory controller, debugfs,
tracefs, `bpftrace`, `taskset`, a C compiler, and disk-backed temporary storage.

Confirm that MGLRU and all four custom tracepoints are available:

```bash
cat /sys/kernel/mm/lru_gen/enabled
sudo bpftrace -lv 'tracepoint:workingset:mm_workingset_*shadow*'
```

Start the observer first, then run the workload in another terminal:

```bash
# Terminal 1: observer
kernel_source=/path/to/patched/linux
kernel_build=$(readlink -f "/lib/modules/$(uname -r)/build")
sudo env BPFTRACE_KERNEL_SOURCE="$kernel_source" \
  BPFTRACE_KERNEL_BUILD="$kernel_build" \
  bpftrace ./trace-shadow/matrix_trace.bt 2>&1 | tee ./mglru-shadow-trace.log

# Terminal 2: workload
sudo ./trace-shadow/run_matrix.sh 2>&1 | tee ./mglru-shadow-workload.log
```

Stop `bpftrace` after the workload finishes. A valid run reports `PASS-A` through `PASS-D` exactly once each. Temporary files and cgroups are removed
automatically unless `KEEP=1` is set.
