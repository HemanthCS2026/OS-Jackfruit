/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Linked-list node struct
 * This stores info about each container we are tracking
 * ============================================================== */
struct monitored_entry {
    pid_t pid;                              /* process ID of the container */
    char container_id[MONITOR_NAME_LEN];   /* name/ID of the container */
    unsigned long soft_limit_bytes;        /* warn if RSS exceeds this */
    unsigned long hard_limit_bytes;        /* kill if RSS exceeds this */
    int soft_warned;                       /* have we already warned? 1=yes */
    struct list_head list;                 /* kernel linked list hook */
};

/* ==============================================================
 * TODO 2: Global list and mutex lock
 * monitored_list = the list of all containers being tracked
 * list_lock = protects the list from race conditions
 * We use mutex (not spinlock) because our timer callback can sleep
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(list_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 * Returns memory usage in bytes for a given PID
 * Returns -1 if the process no longer exists
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit warning helper
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit kill helper
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * TODO 3: Timer Callback
 * This runs every 1 second and checks memory of all containers
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    long rss;

    mutex_lock(&list_lock);

    /* iterate safely - use list_for_each_entry_safe so we can delete */
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {

        rss = get_rss_bytes(entry->pid);

        /* process no longer exists - remove it */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] Process gone container=%s pid=%d, removing.\n",
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* check hard limit first - kill if exceeded */
        if ((unsigned long)rss > entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* check soft limit - warn once */
        if ((unsigned long)rss > entry->soft_limit_bytes && !entry->soft_warned) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&list_lock);

    /* reschedule timer for next second */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler - called when engine.c uses ioctl()
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitored_entry *entry, *tmp;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    /* copy the request data from user space into kernel space */
    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a new container to the monitored list
         * ============================================================== */

        /* validate limits */
        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_WARNING
                   "[container_monitor] Invalid limits: soft > hard for container=%s\n",
                   req.container_id);
            return -EINVAL;
        }

        /* allocate memory for new entry */
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        /* fill in the entry */
        entry->pid = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned = 0;
        strncpy(entry->container_id, req.container_id,
                sizeof(entry->container_id) - 1);
        entry->container_id[sizeof(entry->container_id) - 1] = '\0';

        /* add to list safely */
        mutex_lock(&list_lock);
        list_add_tail(&entry->list, &monitored_list);
        mutex_unlock(&list_lock);

        return 0;
    }

    /* MONITOR_UNREGISTER path */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a container from the monitored list
     * ============================================================== */
    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        if (entry->pid == req.pid &&
            strncmp(entry->container_id, req.container_id,
                    MONITOR_NAME_LEN) == 0) {
            list_del(&entry->list);
            kfree(entry);
            mutex_unlock(&list_lock);
            return 0;
        }
    }
    mutex_unlock(&list_lock);

    return -ENOENT;
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    del_timer_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining entries when module is unloaded
     * ============================================================== */
    mutex_lock(&list_lock);
    {
        struct monitored_entry *entry, *tmp;
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            list_del(&entry->list);
            kfree(entry);
        }
    }
    mutex_unlock(&list_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
