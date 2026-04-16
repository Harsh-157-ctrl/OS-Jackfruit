Multi-Container Runtime: Engineering Report
1. Project Overview
This project implements a lightweight Linux container runtime in C. It features a long-running user-space Supervisor for process management and a custom Linux Kernel Module (LKM) for proactive resource enforcement.

The system demonstrates several core Operating Systems concepts:

Namespace Isolation: Process and mount point separation using clone() and chroot().

Concurrent Logging: A thread-safe Producer-Consumer pipeline using Pthreads.

Kernel-Space Monitoring: Real-time resource enforcement via an LKM and ioctl bridge.

Scheduling Analysis: Observation of the Linux Completely Fair Scheduler (CFS).

2. Implementation Tasks & Technical Evidence
Task 1 & 2: Multi-Container Supervision & Namespaces
<img width="940" height="92" alt="image" src="https://github.com/user-attachments/assets/a6871de3-5098-471f-bbae-ee71460d5e84" />

<img width="902" height="139" alt="image" src="https://github.com/user-attachments/assets/d4147bd1-a950-46bd-8ec7-c61402f52454" />


The engine tracks multiple containers by maintaining a global metadata table in the supervisor. We utilized CLONE_NEWPID, CLONE_NEWNS, and CLONE_NEWUTS to provide process-level and filesystem isolation.
<img width="922" height="145" alt="image" src="https://github.com/user-attachments/assets/6314cf1d-29cf-4129-b10d-5628c861cb5f" />

Description: This shows the supervisor correctly identifying and tracking two distinct containers (c1 and c2) with their unique Host PIDs.

Task 3: Bounded-Buffer Logging
To prevent the supervisor from blocking on disk I/O, we implemented a Producer-Consumer pattern. Producer threads read from container pipes and push data into a synchronized buffer, while a Consumer thread writes that data to host log files.
<img width="940" height="519" alt="image" src="https://github.com/user-attachments/assets/1f069ab3-85a7-4552-9ba6-2c277ceac3d6" />

Description: This demonstrates successful logging. The cat logger.log command shows output that was generated inside a container and captured by the supervisor.

Task 4: Kernel-Space Memory Enforcement
We developed a Kernel Module that monitors the Resident Set Size (RSS) of container PIDs. If a container breaches its limits, the kernel takes immediate action.
<img width="940" height="105" alt="image" src="https://github.com/user-attachments/assets/fa8acf82-7986-462e-a205-fc7b0f90cd09" />
Description: Evidence of the Soft Limit Warning in dmesg when a container exceeds its memory threshold (54 MiB used vs 40 MiB limit).

<img width="940" height="55" alt="image" src="https://github.com/user-attachments/assets/d8729356-f14b-44f1-872b-3a03b1226706" />
Description: Evidence of the Hard Limit Enforcement, where the kernel module issues a SIGKILL to a memory-heavy process (72 MiB used vs 64 MiB limit).

Task 5: Scheduling Experiments
By running CPU-bound (cpu_hog) and I/O-bound (io_pulse) tasks, we observed how the Linux scheduler allocates time-slices based on workload type.
<img width="800" height="520" alt="image" src="https://github.com/user-attachments/assets/359921c2-a0e8-4205-83dd-7ad3b4851214" />

Description: The top utility shows a cpu_hog process utilizing ~100% of a CPU core, demonstrating a CPU-bound workload.

Task 6: Resource Cleanup
A robust OS component must clean up all resources upon exit to prevent memory leaks or zombie processes.
<img width="800" height="306" alt="image" src="https://github.com/user-attachments/assets/9182213a-dbe3-48b2-a1dd-1dfcf826a3c9" />

Description: The dmesg output confirming the Kernel Module was successfully unloaded, releasing all tracked memory.

3. Engineering Analysis
3.1 Namespace Isolation Logic
The use of CLONE_NEWPID ensures that the process inside the container believes it is PID 1, despite having a much higher PID on the host. This is critical for security, as it prevents a containerized process from "seeing" or signaling processes outside its own environment.

3.2 Synchronization & Deadlock Prevention
Our logging system utilizes pthread_mutex_t and pthread_cond_t.

Mutexes ensure that multiple threads do not attempt to write to the log buffer simultaneously, preventing data corruption.

Condition Variables prevent "busy-waiting," allowing the consumer thread to remain in a low-power sleep state until new logs are available.

3.3 The Necessity of Kernel Monitoring
Traditional user-space monitoring (like polling /proc) is too slow to stop an aggressive "Memory Hog." By moving the monitoring logic into Ring 0 (Kernel Space), we can access the task_struct directly and terminate a process before it causes a system-wide crash.

4. How to Build & Run
Compile all components: make

Load the Kernel Module: sudo insmod monitor.ko

Start the Supervisor: sudo ./engine supervisor ./rootfs-base

Launch a Container: sudo ./engine start <id> <rootfs> <cmd>

Cleanup: sudo rmmod monitor and stop the supervisor.
