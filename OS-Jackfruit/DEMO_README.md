# Multi-Container OS Runtime — Full Demo Guide
### Everything you need to show ma'am, in order.

---

## What This Project Is

A lightweight Linux container runtime built from scratch in C. Like a mini Docker.

Two parts:
- **`engine`** — user-space program that creates and manages containers
- **`monitor.ko`** — a Linux kernel module that watches container memory and kills processes that exceed limits

---

## STEP 0 — Prerequisites (One-Time Setup)

### 0.1 Fix folder name (no spaces allowed in path)
```bash
# If your folder has spaces like "OS Jackfruit", rename it:
mv ~/Desktop/pending_projects/"OS Jackfruit" ~/Desktop/pending_projects/OS_Jackfruit
cd ~/Desktop/pending_projects/OS_Jackfruit/MultiContainer-OS-Runtime
```

### 0.2 Install dependencies
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## STEP 1 — Build Everything

```bash
cd boilerplate
make clean && make
```

**What you should see:**
```
gcc ... -o engine           ← main runtime compiled
gcc ... -o memory_hog       ← test workloads compiled
gcc ... -o cpu_hog
gcc ... -o io_pulse
LD [M]  monitor.ko          ← kernel module compiled
```

> **Explain to ma'am:** The `make` command compiles both the user-space engine and the kernel module. The `.ko` file is a Kernel Object — code that can be inserted directly into the running Linux kernel.

---

## STEP 2 — Set Up the Container Filesystem (One-Time Setup)

```bash
# Go to project root
cd ..

# Create the base Alpine Linux filesystem
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Make two container filesystems from the base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy test workloads into the container filesystem
cp boilerplate/cpu_hog rootfs-alpha/
cp boilerplate/memory_hog rootfs-alpha/
cp boilerplate/io_pulse rootfs-alpha/
```

> **Explain to ma'am:** Each container needs its own isolated filesystem. We use Alpine Linux — a minimal 5MB Linux distro. The `chroot` syscall makes the container think this folder is its entire `/` (root). We copy our test programs into it so the container can run them.

---

## STEP 3 — Load the Kernel Module

```bash
cd boilerplate
sudo insmod monitor.ko
```

**Verify it loaded:**
```bash
sudo dmesg | tail -5
```

**Expected output:**
```
[container_monitor] Module loaded. Device: /dev/container_monitor
```

> **Explain to ma'am:** `insmod` inserts our kernel module into the running kernel. It creates a device file at `/dev/container_monitor`. The engine talks to this device using `ioctl()` system calls to register containers for monitoring.

**To check if module is loaded at any time:**
```bash
lsmod | grep monitor
```

**To unload the module when done:**
```bash
sudo rmmod monitor
sudo dmesg | tail -3
# Should say: [container_monitor] Module unloaded.
```

---

## STEP 4 — Run the Supervisor (Terminal 1)

> Open a **dedicated terminal** for this. Keep it running the whole time.

```bash
cd ~/Desktop/pending_projects/OS_Jackfruit/MultiContainer-OS-Runtime/boilerplate
sudo ./engine supervisor ../rootfs-alpha
```

**Expected output:**
```
Supervisor started. base-rootfs=../rootfs-alpha socket=/tmp/mini_runtime.sock
```

> **Explain to ma'am:** The supervisor is a long-running daemon — like a background service. It listens on a Unix socket at `/tmp/mini_runtime.sock`. All other engine commands (start, stop, ps, logs) communicate with it through this socket. If a container crashes, the supervisor detects it and can restart it.

---

## STEP 5 — Demo Commands (Terminal 2)

> Open a **second terminal** for all the following commands.

---

### Demo 1: Run a CPU-bound container

```bash
sudo ./engine run alpha ../rootfs-alpha /cpu_hog
```

**Expected output:**
```
Container alpha exited with status 0
```

> **Explain to ma'am:** This starts a container named `alpha` using the `rootfs-alpha` filesystem and runs the `cpu_hog` program inside it. The container is an isolated process — it has its own hostname, PID namespace, and filesystem. `cpu_hog` burns CPU in a loop for 10 seconds (default), then exits cleanly with status 0.

---

### Demo 2: List running containers

```bash
# First start a container in background (open another terminal or use start instead of run)
sudo ./engine ps
```

**Expected output:**
```
ID       PID     STATE    STARTED
alpha    149648  running  1776662176
```

> **Explain to ma'am:** `ps` queries the supervisor via the Unix socket and lists all containers it's currently tracking — their ID, PID, state, and start time. This is like `docker ps`.

---

### Demo 3: View container logs

```bash
sudo ./engine logs alpha
```

**Expected output:**
```
See logs/alpha.log
```

Then read the actual log:
```bash
cat logs/alpha.log
```

> **Explain to ma'am:** The engine captures stdout of each container and writes it to a log file. Each container gets its own log file named by its ID. This lets us inspect what ran inside the container after it exits.

---

### Demo 4: Stop a container

```bash
sudo ./engine stop alpha
```

> **Explain to ma'am:** This sends a stop signal to the container via the supervisor. Internally it sends SIGTERM (or SIGKILL) to the container's PID and unregisters it from the kernel monitor.

---

### Demo 5: THE BIG ONE — Memory Limit Enforcement (Kernel Module Demo)

> This is the most important demo. The kernel module catches and kills a process exceeding memory limits.

**Run memory_hog with tight limits:**
```bash
sudo ./engine run mem ../rootfs-alpha /memory_hog --soft-mib 10 --hard-mib 30
```

**Expected output:**
```
Container mem exited with status 137
```

> Status 137 = 128 + 9 = killed by signal 9 (SIGKILL). The kernel module killed it.

**Now check the kernel logs:**
```bash
sudo dmesg | tail -20
```

**Expected output:**
```
[container_monitor] Registering container=mem pid=153628 soft=10485760 hard=31457280
[container_monitor] SOFT LIMIT container=mem pid=153628 rss=17375232 limit=10485760
[container_monitor] HARD LIMIT container=mem pid=153628 rss=34021376 limit=31457280
```

> **Explain to ma'am (this is your money explanation):**
>
> 1. When the container started, `engine` called `ioctl(MONITOR_REGISTER)` to tell the kernel module to watch PID 153628 with soft=10MiB and hard=30MiB limits.
> 2. The kernel module's timer fires every 1 second and reads the RSS (Resident Set Size — actual RAM used) of the process using `get_mm_rss()` on the process's `mm_struct`.
> 3. After ~1 second, RSS crossed 10MiB → kernel logs a SOFT LIMIT warning.
> 4. After ~3 seconds, RSS crossed 30MiB → kernel sends SIGKILL directly to the process.
> 5. The process exits with status 137 (128 + SIGKILL signal number 9).
> 6. The kernel module removes the entry from its linked list.

---

### Demo 6: Run an I/O workload

```bash
sudo ./engine run iopulse ../rootfs-alpha /io_pulse
```

> **Explain to ma'am:** `io_pulse` repeatedly writes small chunks to a file and sleeps between writes. This simulates an I/O-bound workload — useful for comparing scheduler behaviour between CPU-heavy and I/O-heavy processes under different nice values.

---

### Demo 7: Run with scheduling priority (nice value)

```bash
# Run cpu_hog with lower priority (nice 10 = less CPU priority)
sudo ./engine run alpha ../rootfs-alpha /cpu_hog --nice 10

# Run another with higher priority (nice -5 = more CPU priority)
sudo ./engine run beta ../rootfs-beta /cpu_hog --nice -5
```

> **Explain to ma'am:** The `--nice` flag sets the scheduling priority of the container process. In Linux, nice values range from -20 (highest priority) to +19 (lowest priority). By running two containers with different nice values, we can observe how the Linux CFS (Completely Fair Scheduler) allocates CPU time differently between them.

---

## STEP 6 — Clean Up After Demo

```bash
# Unload the kernel module
sudo rmmod monitor

# Verify unloaded
sudo dmesg | tail -3
# Should print: [container_monitor] Module unloaded.

# Stop the supervisor (Ctrl+C in Terminal 1)
# Then clean build artifacts
make clean
```

---

## Key Concepts Cheat Sheet (For Viva Questions)

| Term | What it means |
|---|---|
| **Container** | An isolated process using `unshare` (namespaces) + `chroot` (filesystem) |
| **`chroot`** | Makes a directory look like `/` to a process — gives it its own filesystem |
| **`unshare`** | Separates kernel namespaces (PID, mount, etc.) — process gets its own process tree |
| **Kernel Module (.ko)** | Code loaded into the running kernel. Has direct access to kernel internals |
| **`insmod` / `rmmod`** | Insert / remove a kernel module |
| **`ioctl`** | System call to send commands from user-space to a kernel driver |
| **RSS** | Resident Set Size — actual physical RAM a process is using right now |
| **`mm_struct`** | Kernel data structure holding memory info for a process |
| **`get_mm_rss()`** | Kernel function that returns RSS in pages. Multiply by PAGE_SIZE (4096) for bytes |
| **Soft limit** | Warning threshold — kernel logs a message but process keeps running |
| **Hard limit** | Kill threshold — kernel sends SIGKILL to the process |
| **SIGKILL (signal 9)** | Unblockable kill signal. Process cannot catch or ignore it |
| **Exit status 137** | 128 + 9 = process was killed by SIGKILL |
| **`list_for_each_entry_safe`** | Kernel linked list iteration that's safe to use when deleting nodes inside the loop |
| **Mutex** | Lock that prevents two threads/callbacks modifying the same data simultaneously |
| **Unix socket** | IPC mechanism — how the CLI commands talk to the supervisor daemon |
| **Alpine Linux** | Minimal 5MB Linux distro used as the container filesystem |
| **`dmesg`** | Shows kernel log messages — where `printk()` output appears |

---

## Full Flow Summary (Draw this if asked)

```
You type:  sudo ./engine run mem ../rootfs-alpha /memory_hog --soft-mib 10 --hard-mib 30
                |
                ▼
          engine connects to supervisor via Unix socket (/tmp/mini_runtime.sock)
                |
                ▼
          supervisor calls fork() + unshare() + chroot() → container process starts
                |
                ▼
          engine opens /dev/container_monitor, calls ioctl(MONITOR_REGISTER)
          sends: { pid, soft_limit=10MiB, hard_limit=30MiB, container_id="mem" }
                |
                ▼
          kernel module adds entry to linked list (monitored_list)
                |
                ▼ (every 1 second)
          timer_callback() fires → reads RSS via get_mm_rss()
                |
          RSS > soft_limit?  → printk SOFT LIMIT warning to dmesg
          RSS > hard_limit?  → send_sig(SIGKILL) → process dies → exit status 137
                |
                ▼
          kernel module removes entry from linked list, frees memory (kfree)
```

---

*Built on Ubuntu 24.04, kernel 6.17.0-20-generic*
