# MultiContainer-OS-Runtime

**Course:** Operating Systems (UE24CS242B)
**Team:**
| Name | SRN |
|------|-----|
| GIRISH G N | PES1UG24CS571 |
| ASHITH RAO K | PES1UG24CS559 |

---

## What Did We Build?

We built a **mini version of Docker from scratch in C**.

Docker is a tool that runs applications inside isolated boxes called **containers**. We built the same thing — but without using Docker itself. Just pure C code and Linux system calls.

Our project has **two parts**:
- **engine.c** → The brain that creates and manages containers
- **monitor.c** → A kernel module that watches how much RAM each container uses

---

## What is a Container? (Simple Explanation)

Imagine you have one house (your Linux system). Inside that house, you create **separate rooms**. Each room thinks it is its own house — it has its own front door, its own address, its own stuff. But all rooms share the same foundation (the Linux kernel).

That's a container. It is an **isolated process** that:
- Has its own process IDs (thinks PID 1 is itself)
- Has its own hostname
- Has its own filesystem (sees only its own files)
- But shares the same Linux kernel underneath

---

## How Does Isolation Work?

We use a Linux system call called **`clone()`** with 3 flags:

| Flag | What it does | Simple meaning |
|------|-------------|----------------|
| `CLONE_NEWPID` | PID namespace | Container sees its own process IDs |
| `CLONE_NEWUTS` | UTS namespace | Container has its own hostname |
| `CLONE_NEWNS` | Mount namespace | Container has its own filesystem |

After creating the container with `clone()`, we:
1. Use **`chroot()`** to lock the container inside its own folder
2. Mount **`/proc`** so commands like `ps` work inside the container
3. Run the command the user gave us (like `/cpu_hog`)

---

## How the System Works (Step by Step)

```
You type:  ./engine start alpha ./rootfs-alpha "/cpu_hog 30"
                |
                | (sends message through UNIX socket)
                v
         SUPERVISOR (always running in background)
                |
                | calls clone() to create isolated process
                v
         CONTAINER ALPHA (isolated process)
                |
                | chroot() into rootfs-alpha folder
                | mount /proc
                | run /cpu_hog for 30 seconds
                |
                | (container output goes through pipe)
                v
         LOG FILE: logs/alpha.log
```

At the same time, the supervisor tells the kernel module:
```
"Hey kernel, please watch container alpha (PID 5081).
 Warn me if it uses more than 40MB RAM.
 Kill it if it uses more than 64MB RAM."
```

---

## The Kernel Module (monitor.c)

This is a piece of code that runs **inside the Linux kernel** itself.

Every **1 second**, it wakes up and checks:
- How much RAM is each container using?
- If RAM > soft limit → print a WARNING
- If RAM > hard limit → KILL the container immediately

### Why inside the kernel?
Because a misbehaving container **cannot stop** the kernel. If we did this in user space, the container could interfere with the monitoring. The kernel is more powerful and runs at a higher privilege level.

---

## The Logging System (Bounded Buffer)

Every container's output (stdout) is captured and saved to a log file.

We use a **producer-consumer** pattern:

```
Container is running and printing output
        |
        | (through a pipe)
        v
Log Reader Thread --> [  BUFFER  ] --> Logging Thread --> logs/alpha.log
   (producer)         (16 slots)          (consumer)
```

- If buffer is **full** → producer waits
- If buffer is **empty** → consumer waits
- We use a **mutex lock** to make sure only one thread touches the buffer at a time
- We use **condition variables** to wake up threads when needed

---

## Build and Run Commands

### Step 1: Build the project
```bash
cd ~/projects/MultiContainer-OS-Runtime/boilerplate
make clean && make
```
This compiles everything — the engine, kernel module, and test workloads.

### Step 2: Setup the container filesystem (only once)
```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp cpu_hog memory_hog io_pulse rootfs-alpha/
cp cpu_hog memory_hog io_pulse rootfs-beta/
```
This downloads a small Alpine Linux filesystem that our containers will use as their "room".

### Step 3: Load the kernel module
```bash
sudo insmod monitor.ko
sudo dmesg | tail -5
```
This loads our memory monitor into the Linux kernel.
You will see: `[container_monitor] Module loaded. Device: /dev/container_monitor`

### Step 4: Start the Supervisor — Terminal 1
```bash
sudo ./engine supervisor ./rootfs-base
```
This starts the main daemon. It stays running and waits for commands.
You will see: `Supervisor started. base-rootfs=./rootfs-base socket=/tmp/mini_runtime.sock`

### Step 5: Use the CLI — Terminal 2

**Start a CPU-heavy container:**
```bash
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog 30"
```
This creates an isolated container that burns CPU for 30 seconds.

**Start an I/O-heavy container:**
```bash
sudo ./engine start beta ./rootfs-beta "/io_pulse 20"
```
This creates a container that writes data to disk repeatedly.

**See all running containers:**
```bash
sudo ./engine ps
```
Output:
```
ID         PID      STATE      STARTED
beta       5088     running    1776521770
alpha      5081     running    1776521770
```

**Check kernel logs:**
```bash
sudo dmesg | grep container_monitor
```
Output:
```
[container_monitor] Registering container=alpha pid=5081 soft=41943040 hard=67108864
[container_monitor] Registering container=beta  pid=5088 soft=41943040 hard=67108864
```
This shows the kernel is tracking both containers with memory limits.

**View container logs:**
```bash
sudo cat logs/alpha.log
```
Shows everything the container printed to stdout.

**Stop a container:**
```bash
sudo ./engine stop alpha
sudo ./engine ps
```
Container state changes from `running` to `stopped`.

---

## Memory Limit Test

```bash
sudo ./engine start memtest ./rootfs-alpha "/memory_hog 4 500" --soft-mib 20 --hard-mib 40
```
This starts a container that grabs 4MB of RAM every 500 milliseconds.
- After ~5 seconds it crosses 20MB → kernel prints a WARNING
- After ~10 seconds it crosses 40MB → kernel KILLS the container

```bash
sudo dmesg | grep container_monitor
```
You will see:
```
[container_monitor] SOFT LIMIT container=memtest pid=XXXX rss=21000000 limit=20971520
[container_monitor] HARD LIMIT container=memtest pid=XXXX rss=41000000 limit=41943040
```

---

## Scheduler Experiment

We ran experiments to see how Linux shares CPU between containers.

### Experiment 1: Two equal containers
```bash
sudo ./engine start cpu1 ./rootfs-alpha "/cpu_hog 20" --nice 0
sudo ./engine start cpu2 ./rootfs-beta  "/cpu_hog 20" --nice 0
```
**Result:** Both containers finished in roughly the same time.
**Why:** Linux CFS (Completely Fair Scheduler) gives equal CPU to equal priority tasks.

### Experiment 2: Different priorities
```bash
sudo ./engine start fast ./rootfs-alpha "/cpu_hog 20" --nice 0
sudo ./engine start slow ./rootfs-beta  "/cpu_hog 20" --nice 10
```
**Result:** `fast` finished about 2x sooner than `slow`.
**Why:** nice=10 means lower priority. Linux gives it less CPU time.

### Experiment 3: CPU vs I/O containers
```bash
sudo ./engine start cpu1 ./rootfs-alpha "/cpu_hog 20"
sudo ./engine start io1  ./rootfs-beta  "/io_pulse 20 200"
```
**Result:** The I/O container stayed responsive even while the CPU container was maxing out the processor.
**Why:** The I/O container sleeps between writes. Linux rewards sleeping tasks by scheduling them first when they wake up.

---

## Cleanup

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Press Ctrl+C in Terminal 1 to stop supervisor
sudo rmmod monitor
sudo dmesg | tail -3
```
You will see: `[container_monitor] Module unloaded.`
This confirms everything shut down cleanly with no memory leaks.

---

## Summary of What We Implemented

| Component | What we wrote | What it does |
|-----------|--------------|-------------|
| `engine.c` — bounded_buffer_push/pop | Producer-consumer buffer | Safely passes log data between threads |
| `engine.c` — logging_thread | Consumer thread | Writes container logs to files |
| `engine.c` — child_fn | Container setup | chroot, mount /proc, exec command |
| `engine.c` — run_supervisor | Main daemon | Accepts CLI commands, manages containers |
| `engine.c` — send_control_request | CLI client | Sends commands to supervisor via socket |
| `monitor.c` — monitored_entry struct | Data structure | Stores PID, limits per container |
| `monitor.c` — timer_callback | Memory checker | Every 1 sec, checks RAM of all containers |
| `monitor.c` — ioctl REGISTER | Add to watchlist | Kernel starts tracking a new container |
| `monitor.c` — ioctl UNREGISTER | Remove from watchlist | Kernel stops tracking a container |
| `monitor.c` — module exit cleanup | Memory free | Frees all kernel memory on unload |

---

## GitHub Repository

https://github.com/GIRISH-G-N/MultiContainer-OS-Runtime
