# Linux Container Runtime

A container runtime built from scratch in C — no Docker, no libcontainer, just raw Linux primitives. It recreates how containers actually work: namespaces, `pivot_root`/`chroot`, RLIMITs, `clone()`, and `/proc`, wired together into a working CLI with persistent state, logging, monitoring, and a custom scheduler.

This is a systems-level project built to demonstrate real, working understanding of container internals — how isolation, resource control, and process management are implemented at the OS level, not abstracted away behind a container engine.

---

## What it does

A "container" here is a regular Linux process given an isolated view of the system:

- **Its own process tree** — PID namespace, so the container's first process is PID 1
- **Its own hostname** — UTS namespace
- **Its own filesystem root** — mount namespace + `pivot_root` (falls back to `chroot`)
- **Its own network stack** — network namespace with loopback brought up
- **Resource ceilings** — CPU time, memory, and process count enforced via `setrlimit()`

On top of that: persistent container lifecycle state, a user-space round-robin scheduler (`SIGSTOP`/`SIGCONT`), tabular event logs, live `/proc`-based resource stats, and 5 automated integration test suites covering every subsystem.

---

## Architecture

```
┌─────────────────────────────────────────────┐
│                CLI (main.c)                  │
│      parses commands → ContainerSpec         │
└───────────────────┬───────────────────────────┘
                     │
┌───────────────────▼───────────────────────────┐
│          Manager (container.c)                │
│  lifecycle, IDs, state, persistence, tables    │
└──────┬──────────────────┬──────────────┬───────┘
       │                  │              │
┌──────▼──────┐   ┌───────▼──────┐ ┌─────▼──────┐
│ Isolation   │   │ Observability│ │ Scheduler  │
│ namespace.c │   │ logger.c     │ │ scheduler.c│
│ filesystem.c│   │ monitor.c    │ │ (pthread)  │
│ resource.c  │   └──────────────┘ └────────────┘
│ network.c   │
└─────────────┘
```

| Layer | Files | Responsibility |
|---|---|---|
| CLI | `main.c` | Parses commands into `ContainerSpec` objects |
| Manager | `container.c` | Container list, IDs, state, persistence, scheduler hooks |
| Isolation | `namespace.c`, `filesystem.c`, `resource.c`, `network.c` | Builds the isolated runtime environment |
| Observability | `logger.c`, `monitor.c` | Event logs, live `/proc` stats |
| Validation | `workload_*.c`, `tests/*.sh` | Proves each subsystem behaves correctly |

### How `run` works, end to end

1. CLI parses `run` into a `ContainerSpec` → `container_create()` allocates a container, assigns it an ID (`container-0001`), preps its rootfs, and persists metadata.
2. `namespace_start_container()` calls `clone()` with `CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET`.
3. The parent writes the child's `uid_map`/`gid_map` to configure user namespace mapping, then releases it via a synchronization pipe.
4. The child sets UID/GID 0 inside its namespace, sets its hostname, switches its filesystem root, remounts `/proc`, brings up loopback, applies RLIMITs, becomes its own process group leader, and `exec`s the target command.
5. The parent tracks the host PID, marks the container `RUNNING`, saves metadata, and logs `CONTAINER_STARTED`. On exit, it logs `CONTAINER_STOPPED` or `RESOURCE_LIMIT_HIT`, updates state to `STOPPED`, and persists again.

`runbg` follows the identical path but returns immediately rather than blocking on exit, so the container keeps running in the background and shows up in `list`, `stats`, and `logs`.

---

## Core subsystems

**Isolation** — `clone()` with five namespace flags builds the sandbox; `filesystem.c` bootstraps a minimal rootfs (binaries, shared libs, `/proc`, `/dev`, `/tmp`) and switches into it via `pivot_root`, falling back to `chroot` if the host doesn't permit it.

**Resource control** — `resource.c` applies `RLIMIT_CPU`, `RLIMIT_AS`, and `RLIMIT_NPROC` before `exec`, capping CPU time, memory, and process count per container. A limit of `0` means unlimited.

**Scheduler** — `scheduler.c` runs a dedicated pthread implementing round-robin scheduling with `SIGSTOP`/`SIGCONT`, rotating between tracked container PIDs on a configurable time slice (default 200ms). This makes process scheduling explicit and controllable rather than leaving it entirely to the kernel.

**Persistence** — container state is written to `containers.meta` (tab-separated, one record per line) using an atomic temp-file-and-rename pattern. On startup, any container marked `RUNNING` whose PID no longer exists is automatically reconciled to `STOPPED`.

**Observability** — `monitor.c` reads `/proc/<pid>/stat` and `/proc/<pid>/status` directly to compute CPU time, thread count, and memory usage, with a `--watch` mode for live-refreshing stats. `logger.c` renders the event log as a table, reconstructing container name/PID from prior lifecycle events.

**Validation** — four purpose-built workload binaries (`workload_cpu`, `workload_mem`, `workload_fork`, `workload_netcheck`) exist specifically to exercise and prove each isolation/resource guarantee, backed by 5 bash integration test suites run through the real CLI.

---

## Quick start

```bash
make
bash scripts/setup-rootfs.sh
rm -f containers.meta containers.meta.tmp container.log
./container-sim          # use sudo if namespace/mount ops are blocked
```

Run the test suite:

```bash
bash tests/run_all.sh    # 5/5 suites: lifecycle, isolation, resources, scheduler, monitoring
```

## Key commands

| Command | Purpose |
|---|---|
| `run <name> <hostname> <rootfs> <cmd>` | Create + start a container in the foreground |
| `runbg ...` | Same, but in the background |
| `list` / `stats [id]` / `logs [id]` | Inspect state, live resource usage, event history |
| `sched on` / `sched status` | Enable and inspect the round-robin scheduler |
| `stop <id>` / `delete <id>` | Stop and remove a container |

Resource flags: `--cpu SEC`, `--mem MB`, `--pids N` (apply to `run`, `runbg`, and `create`).

---

## Structure

Linux-Container-Runtime/
├── src/
│   ├── main.c              # CLI entrypoint and command parsing
│   ├── container.c/.h      # Manager: lifecycle, persistence, state, tables
│   ├── namespace.c/.h      # clone() + namespace setup
│   ├── filesystem.c/.h     # rootfs prep and isolation
│   ├── resource.c/.h       # RLIMIT-based CPU/memory/process limits
│   ├── scheduler.c/.h      # user-space round-robin scheduler
│   ├── network.c/.h        # loopback setup inside network namespace
│   ├── monitor.c/.h        # /proc-based runtime stats
│   ├── logger.c/.h         # event logging + tabular rendering
│   ├── workload_cpu.c      # CPU stress workload
│   ├── workload_mem.c      # memory allocation workload
│   ├── workload_fork.c     # fork/process-count workload
│   └── workload_netcheck.c # network namespace visibility workload
│
├── scripts/
│   └── setup-rootfs.sh     # builds demo/test rootfs directories
│
├── tests/
│   ├── run_all.sh           # runs all test suites
│   ├── test_basic.sh        # lifecycle smoke test
│   ├── test_isolation.sh    # namespace + rootfs isolation test
│   ├── test_resources.sh    # CPU/memory/PID limit test
│   ├── test_scheduler.sh    # scheduler behavior test
│   └── test_monitoring.sh   # logs/stats observability test
│
├── rootfs/                  # generated by setup-rootfs.sh
│   ├── test-basic/
│   ├── test-isolation/
│   ├── test-mon/
│   ├── test-resources/
│   └── test-sched/
│
├── bin/                     # compiled workload binaries (build output)
├── .github/                 # CI workflow (runs `make` on push/PR)
├── .vscode/
├── .codex/
├── .agents/
│
├── Makefile
├── README.md
├── LICENSE
├── .gitignore
├── containers.meta          # runtime state (regenerated, not committed)
├── containers.meta.tmp      # atomic-write temp file (regenerated)
└── container.log            # event log (regenerated)

## License

MIT
