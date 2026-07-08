# Linux Container Runtime

A container runtime built from scratch in C — no Docker, no libcontainer, just raw Linux primitives. It recreates how containers actually work: namespaces, `pivot_root`/`chroot`, RLIMITs, `clone()`, and `/proc`, wired together into a working CLI with persistent state, logging, monitoring, and a custom scheduler.

This is an educational systems project. The goal is to demonstrate real understanding of container internals, not to ship a production OCI runtime.

---

## What it does

A "container" here is a regular Linux process given an isolated view of the system:

- **Its own process tree** — PID namespace, so the container's first process is PID 1
- **Its own hostname** — UTS namespace
- **Its own filesystem root** — mount namespace + `pivot_root` (falls back to `chroot`)
- **Its own network stack** — network namespace with loopback brought up
- **Resource ceilings** — CPU time, memory, and process count enforced via `setrlimit()`

On top of that: persistent container lifecycle state, a user-space round-robin scheduler (`SIGSTOP`/`SIGCONT`), tabular logs and live `/proc`-based stats, and 5 automated integration test suites.

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

### How `run` works

1. CLI parses `run` into a `ContainerSpec` → `container_create()` allocates a container, preps its rootfs, saves metadata.
2. `namespace_start_container()` calls `clone()` with `CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET`.
3. Parent writes the child's `uid_map`/`gid_map` for user namespace mapping, then releases it via a pipe.
4. Child sets UID/GID 0, sets hostname, switches root filesystem, remounts `/proc`, brings up loopback, applies RLIMITs, and `exec`s the target command.
5. Parent tracks the host PID, marks the container `RUNNING`, and logs the event. On exit, it logs `CONTAINER_STOPPED`/`RESOURCE_LIMIT_HIT` and updates state.

`runbg` follows the same path but returns immediately instead of blocking on exit.

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

Resource flags: `--cpu SEC`, `--mem MB`, `--pids N` (apply to `run`/`runbg`/`create`).

---

## Design tradeoffs

- **RLIMITs, not cgroups** — enforces limits per-process without needing root or cgroupfs.
- **Loopback-only networking** — proves namespace isolation without bridge/NAT complexity.
- **Flat-file persistence** — atomic writes (temp file + rename), no database or daemon.
- **User-space scheduler** — makes process scheduling visible and controllable via a dedicated pthread, rather than relying solely on the kernel.

## Known limitations

- Not OCI/Docker-compatible — educational scope by design
- `stop` uses `SIGKILL`, not graceful shutdown
- Quoted sub-arguments in the container command aren't fully supported (naive space-split)
- Unprivileged user namespaces may require `sudo` depending on host kernel config
- `pivot_root` falls back to `chroot` if the host doesn't permit it

## Status

`make` builds cleanly · `setup-rootfs.sh` succeeds · `tests/run_all.sh` passes 5/5 suites
