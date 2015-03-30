#include<linux/module.h>
#include<linux/init.h> 
#include<linux/list.h>
#include<linux/sched.h>
#include<linux/time.h>

#include<linux/types.h>
#include<linux/slab.h>
#include<linux/list_sort.h>
#include<linux/kthread.h>
#include<linux/delay.h>
#include<linux/mm.h>
#include<linux/mutex.h>
#include<linux/oom.h>
#include<linux/semaphore.h>
#include<linux/sysinfo.h>
#include<linux/jiffies.h>
#include<linux/kernel.h>
#include<linux/syscalls.h>
#include<asm/uaccess.h>

#define MAX_BOND 30
#define DEBUG  0
#if DEBUG
#define DPRINTK( s, arg... ) printk( s, ##arg )
#else
#define DPRINTK( s, arg... ) 
#endif 
#define MAX_LIST_SIZE 3000
//static DEFINE_MUTEX(scan_mutex);

//extern int get_bond_mem(int pid);
//extern int del_bond_mem(int pid); 
extern int (*set_mem)(int pid, int mem);

struct semaphore my_semaphore, kill_sema, low_mem;

struct list_head pro_lhead;
static struct task_struct *check_thread, *del_oom_thread, *kill_ob_thread;
static char cur_mark, ob_mark;
static unsigned long lowmemory = 150 * 1024;	// Pages
static unsigned long total_memory;
static int list_size = 0;
struct sysinfo mem_info;
static int time_count = 0;
static int release = 0;

struct pro_node{
	char name[TASK_COMM_LEN];
	pid_t pid;
	unsigned long total_vm;
	unsigned long total_rss;
	struct list_head list;
	int oom_adj;
	int oom_score_adj;
	int badness;
	int bond;
	char marker;
};

//As instructions requirements, the total time should be the time from boot up to now.
static unsigned long get_cpu_total_time(struct task_struct *p)
{
	unsigned long pro_run_time, start_time, now_time;
	start_time = (unsigned long)p->real_start_time.tv_sec * USEC_PER_SEC + p->real_start_time.tv_nsec/1000;
	now_time = (unsigned long)jiffies_to_usecs(jiffies);
	pro_run_time = now_time - start_time;
	return usecs_to_jiffies(pro_run_time);
}

static unsigned long get_cpu_process_time(struct task_struct *p)
{
	return (p->stime + p->utime);
}

static int get_cpu_usage(struct task_struct *p)
{
	unsigned long total_cputime;
	unsigned long proc_cputime;
	int pcpu;
	
	if (p->flags & PF_KTHREAD || p->signal == NULL || p->mm == NULL) return 0;
	total_cputime = get_cpu_total_time(p);
	proc_cputime = get_cpu_process_time(p);
		
	pcpu = 10000 * (proc_cputime) / (total_cputime);
	return pcpu;
}

static int get_badness(struct task_struct *p)
{
	int pro_mem, pro_cpu;
	int result;	
	if (p->flags & PF_KTHREAD || p->signal == NULL || p->mm == NULL) return 0;
	pro_mem = get_mm_rss(p->mm)*10000 / total_memory;
	pro_cpu = get_cpu_usage(p);
	result = (10000 - pro_cpu + pro_mem);
	return result;
}

static void add_pro(struct task_struct *p, int bond)
{
	struct pro_node *pt_pro = NULL;
	if (p->flags & PF_KTHREAD || p->signal == NULL || p->mm == NULL || p->pid < 0) return;
	if (p->signal->oom_adj < 5) return;
	if (list_size > MAX_LIST_SIZE) {
			printk(KERN_ALERT"The Pool is full!\n");
			return;
	}
	pt_pro = (struct pro_node*)kmalloc(sizeof(struct pro_node), GFP_KERNEL);	
	if (pt_pro == NULL) {
		printk(KERN_ALERT"KMALLOC error!");
	} else {
		strcpy(pt_pro->name,p->comm);
		pt_pro->pid = p->pid;
		pt_pro->total_vm = p->mm->total_vm;
		pt_pro->marker = cur_mark;
		pt_pro->oom_adj = p->signal->oom_adj;
		pt_pro->oom_score_adj = p->signal->oom_score_adj;
		if (p->mm == NULL) {
			kfree(pt_pro);
			return;
		}
		pt_pro->total_rss = get_mm_rss(p->mm);
		pt_pro->badness = get_badness(p);
		pt_pro->bond = bond;
		list_size++;
		DPRINTK(KERN_ALERT"---->>>>Add these in list: Name %s    PID %d   Memory %lu         RSS  %lu        OOM_adj  %d    Badness  %d    Boundary  %d\n", 
				pt_pro->name, pt_pro->pid, pt_pro->total_vm, pt_pro->total_rss, pt_pro->oom_adj, pt_pro->badness, pt_pro->bond);
		list_add(&pt_pro->list, &pro_lhead);
		pt_pro = NULL;
	}
}

static void walk_process(void)
{
	struct task_struct *p;
//	if (mutex_lock_interruptible(&scan_mutex) < 0)	return;
	down(&my_semaphore);
	for_each_process(p)
	{
		add_pro(p, -1);
	}
//	mutex_unlock(&scan_mutex);
	up(&my_semaphore);
	p = NULL;
}

static void show_pro_list(void)
{
	struct pro_node *ops;
	printk(KERN_ALERT"Prcess Name			PID			Bondary               RSS                 OOM_adj             Badness\n");
	list_for_each_entry(ops, &pro_lhead, list) {
		printk(KERN_ALERT"%-30s%-24d%-24d%-24lu%-24d%-24d\n", ops->name, ops->pid, ops->bond, ops->total_rss, ops->oom_adj, ops->badness); }
}

static int cmp(void *priv, struct list_head *a, struct list_head *b)
{
	//DPRINTK(KERN_ALERT"cmp %lu and %lu\n", container_of(a, struct pro_node, list)->total_vm, container_of(b, struct pro_node, list)->total_vm);
	if (container_of(a, struct pro_node, list)->badness > container_of(b, struct pro_node, list)->badness)
		return 1;
	else
		return -1;
}

int set_bond_memory(int pid, int mem)
{
	struct pro_node *pos;
	struct task_struct *p;
	down(&my_semaphore);
	list_for_each_entry(pos, &pro_lhead, list) {	
		if (pos->pid == pid) {
			pos->bond = mem;	
			DPRINTK(KERN_ALERT"****************Set the process PID %d has bound %d\n", pos->pid, pos->bond);
			up(&my_semaphore);
			return 1;
		}
	}
	DPRINTK("Didn't find match process for PID %d, so add it into list\n", pid);
	for_each_process(p)
	{
		if (p->pid == pid) {
		add_pro(p, mem);
		DPRINTK(KERN_ALERT"Set the process PID %d has bound %d\n", pid, mem);
		up(&my_semaphore);	
		return 1;
		}
	}
	up(&my_semaphore);	
	show_pro_list();
	DPRINTK("##########################Set boundary for PID %d Fail!\n", pid);
	return -1;
}
EXPORT_SYMBOL(set_bond_memory);


static void sort_process(struct list_head *head)
{
	list_sort(NULL, head, &cmp);
}

static int kill_ob(void* data)
{
	struct task_struct *p;
	struct pro_node *pos, *tmp;
	while(1){
		down(&kill_sema);
		if (release == 1) return -1;
		down(&my_semaphore);
		list_for_each_entry_safe(pos, tmp, &pro_lhead, list) {
			if (pos->bond != -1 && pos->bond < pos->total_rss) {
				for_each_process(p) {
					if (p->pid == pos->pid) {
						printk(KERN_ALERT"*************Process PID %d, Name %s is out of Boundary %d, its rss is %lu\n", p->pid, p->comm, pos->bond, pos->total_rss);
						send_sig(SIGKILL, p, 0);
						break;
					}
				}
				list_del(&pos->list);
				list_size--;
				kfree(pos);
			}
		}
		up(&my_semaphore);	
		if(kthread_should_stop())
			return 0;
	}
}

static int check_pro(void* data)
{
	struct task_struct *p;
	struct pro_node *pos;
	struct pro_node *tmp;
	int exist;
	while(1){
		down(&my_semaphore);
		cur_mark = ~cur_mark;
		for_each_process(p)
		{
			exist = 0;
			if (p->mm == NULL) continue;
			if (p->signal->oom_adj < 5) continue;
			list_for_each_entry(pos, &pro_lhead, list) {
				if (p->pid == pos->pid) {
					if (p->mm == NULL) break;
					if (pos->bond < get_mm_rss(p->mm)) ob_mark = 1;
					exist = 1;
					pos->marker = cur_mark;
					pos->total_rss = get_mm_rss(p->mm);
					pos->badness = get_badness(p);
					break;
				}
			}
			if (exist == 0) {
				DPRINTK(KERN_ALERT"---->>>>Found a new process %d,%s\n", p->pid, p->comm);
				add_pro(p, -1);
			}
		}

		list_for_each_entry_safe(pos, tmp, &pro_lhead, list) {
			if (pos->marker != cur_mark) {
				DPRINTK(KERN_ALERT"----->>>>>Del the process %d,%s\n", pos->pid, pos->name);
				list_del(&pos->list);
				list_size--;
				kfree(pos);
			}
		}
		//sort_process(&pro_lhead);
		if (time_count > 2000) {
			printk(KERN_ALERT"Try to show proc_list\n");
			show_pro_list();
			time_count = 0;
		}
		up(&my_semaphore);
		if (ob_mark == 1) {
			printk(KERN_ALERT"@@@@@@ob_mark = 1 again, try to wake up kill_ob_thread\n");
			up(&kill_sema);
		}
		if (global_page_state(NR_FREE_PAGES) < lowmemory) { 
			printk(KERN_ALERT"==============The memory is low! The free pages is %lu\n", global_page_state(NR_FREE_PAGES));
			up(&low_mem);
		}
		ob_mark = 0;
		if(kthread_should_stop())
			return 0;
		usleep(1000);
		time_count++;
	}
}

static int del_oom(void* data)
{
	struct task_struct *p;
	struct pro_node *pos;
	while(1){	
		down(&low_mem);
		if (release == 1) return -1;
		down(&my_semaphore);
		if ( !list_empty(&pro_lhead) ) {
			sort_process(&pro_lhead);
			pos = list_entry((&pro_lhead)->prev, struct pro_node, list); 
			if (pos != NULL) {
				printk(KERN_ALERT"======>>>>>>Kill the process %d,%s badness %d\n",pos->pid, pos->name, pos->badness);
				for_each_process(p) {
					if (p->pid == pos->pid) {
						send_sig(SIGKILL, p, 0);
						break;
					}
				}
				list_del(&pos->list);
				list_size--;
				kfree(pos);
			}
		}
		up(&my_semaphore);
		if(kthread_should_stop())
			return 0;
	}
}

static int __init init_lab2(void)
{
	char thrd_check_name[11] = "thrd_check";
	char thrd_del_name[11] = "thrd_del";
	char thrd_ob_name[15] = "thrd_kill_ob";
	cur_mark = 1;
	ob_mark = 0;
	printk(KERN_ALERT"------>>>Enter lab2 module<<<-------\n");
	
	set_mem = set_bond_memory;

	si_meminfo(&mem_info);
	total_memory = mem_info.totalram * 4;
	printk(KERN_ALERT"------>>>Total Memory is %lu KB<<<-------\n", total_memory);
	sema_init(&my_semaphore, 1);
	sema_init(&kill_sema, 0);
	sema_init(&low_mem, 0);
	INIT_LIST_HEAD(&pro_lhead);
	walk_process();
	sort_process(&pro_lhead);
	show_pro_list();
	check_thread = kthread_run(check_pro, NULL, thrd_check_name);
	del_oom_thread = kthread_run(del_oom, NULL, thrd_del_name);
	kill_ob_thread = kthread_run(kill_ob, NULL, thrd_ob_name);
	return 0;
}

static void __exit exit_lab2(void)
{
	printk(KERN_ALERT"------<<<Exit lab2 module>>>-------\n");
	set_mem = NULL;
	release = 1;
	up(&kill_sema);
	up(&low_mem);
	kthread_stop(check_thread);
	return;
}

module_init(init_lab2);
module_exit(exit_lab2);

MODULE_AUTHOR("Wenji");
MODULE_LICENSE("GPL");
