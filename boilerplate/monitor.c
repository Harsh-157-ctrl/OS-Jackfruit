#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mm.h> // <-- Added to fix get_mm_rss error

// Must match the struct defined in monitor_ioctl.h
typedef struct {
    int pid;
    int soft_limit;
    int hard_limit;
} monitor_cmd_t;

#define MONITOR_MAGIC 'M'
#define REGISTER_PROCESS _IOW(MONITOR_MAGIC, 1, monitor_cmd_t)

// The linked list node to track each container
struct monitored_task {
    int pid;
    int soft_limit;
    int hard_limit;
    bool soft_warned;
    struct list_head list;
};

static LIST_HEAD(task_list);
static DEFINE_MUTEX(list_mutex);
static struct task_struct *monitor_thread_ts;

// --- IOCTL RECEIVER ---
// This triggers when engine.c calls ioctl()
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    monitor_cmd_t ucmd;
    struct monitored_task *new_task;

    if (cmd != REGISTER_PROCESS) return -EINVAL;
    if (copy_from_user(&ucmd, (monitor_cmd_t __user *)arg, sizeof(ucmd))) return -EFAULT;

    new_task = kmalloc(sizeof(*new_task), GFP_KERNEL);
    if (!new_task) return -ENOMEM;

    new_task->pid = ucmd.pid;
    new_task->soft_limit = ucmd.soft_limit;
    new_task->hard_limit = ucmd.hard_limit;
    new_task->soft_warned = false;

    // Safely add the new container to our linked list
    mutex_lock(&list_mutex);
    list_add_tail(&new_task->list, &task_list);
    mutex_unlock(&list_mutex);

    pr_info("[Container Monitor] Registered PID %d (Soft: %d MiB, Hard: %d MiB)\n", ucmd.pid, ucmd.soft_limit, ucmd.hard_limit);
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static struct miscdevice monitor_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "container_monitor",
    .fops = &monitor_fops,
};

// --- BACKGROUND KERNEL THREAD ---
// This runs constantly in the background, checking memory usage
static int monitor_kthread(void *data) {
    struct monitored_task *node, *tmp;
    struct task_struct *task;
    long rss_mib;

    while (!kthread_should_stop()) {
        mutex_lock(&list_mutex);
        
        // Loop safely through the linked list
        list_for_each_entry_safe(node, tmp, &task_list, list) {
            
            // Safely fetch the process by its PID
            rcu_read_lock();
            task = pid_task(find_vpid(node->pid), PIDTYPE_PID);
            if (task) get_task_struct(task);
            rcu_read_unlock();

            // If the container died normally, clean it up from our list
            // FIXED: Using __state instead of state for Kernel 5.14+ compatibility
            if (!task || task->__state == TASK_DEAD) {
                if (task) put_task_struct(task);
                list_del(&node->list);
                kfree(node);
                continue;
            }

            // Check the process memory limits
            if (task->mm) {
                rss_mib = (get_mm_rss(task->mm) * PAGE_SIZE) / (1024 * 1024);

                if (rss_mib > node->hard_limit) {
                    pr_alert("[Container Monitor] HARD LIMIT EXCEEDED! PID %d using %ld MiB (Limit: %d MiB). Terminating...\n", node->pid, rss_mib, node->hard_limit);
                    send_sig(SIGKILL, task, 1);
                    list_del(&node->list);
                    kfree(node);
                } 
                else if (rss_mib > node->soft_limit && !node->soft_warned) {
                    pr_warn("[Container Monitor] SOFT LIMIT WARNING! PID %d using %ld MiB (Limit: %d MiB).\n", node->pid, rss_mib, node->soft_limit);
                    node->soft_warned = true; // Ensure we only warn once
                }
            }
            put_task_struct(task);
        }
        mutex_unlock(&list_mutex);
        msleep(1000); // Sleep for 1 second before checking again
    }
    return 0;
}

// --- MODULE INITIALIZATION & TEARDOWN ---
static int __init monitor_init(void) {
    int ret = misc_register(&monitor_misc);
    if (ret) {
        pr_err("Failed to register misc device\n");
        return ret;
    }

    monitor_thread_ts = kthread_run(monitor_kthread, NULL, "container_monitor_thread");
    if (IS_ERR(monitor_thread_ts)) {
        misc_deregister(&monitor_misc);
        return PTR_ERR(monitor_thread_ts);
    }

    pr_info("Container memory monitor loaded.\n");
    return 0;
}

static void __exit monitor_exit(void) {
    struct monitored_task *node, *tmp;

    kthread_stop(monitor_thread_ts);
    misc_deregister(&monitor_misc);

    // Free all remaining memory in the list to prevent memory leaks on unload
    mutex_lock(&list_mutex);
    list_for_each_entry_safe(node, tmp, &task_list, list) {
        list_del(&node->list);
        kfree(node);
    }
    mutex_unlock(&list_mutex);

    pr_info("Container memory monitor unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Container Memory Monitor");
