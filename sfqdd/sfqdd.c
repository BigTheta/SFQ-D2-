/*
 * elevator sfqd
 */
#include <linux/time.h>
#include <linux/jiffies.h>
#include <linux/blktrace_api.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
//#include <linux/atomic.h>

#define FUN_NAME "<%s>: "

#define DEBUG_FUN  1
#if DEBUG_FUN
#define DPRINTK( s, arg... ) printk( FUN_NAME s, __FUNCTION__, ##arg )
#else
#define DPRINTK( s, arg... ) 
#endif 


#define DEBUG_NUM  0
#if DEBUG_NUM
#define NPRINTK( s, arg... ) printk( FUN_NAME s, __FUNCTION__, ##arg )
#else
#define NPRINTK( s, arg... ) 
#endif 

#define DEBUG_PID  0
#if DEBUG_PID	
#define PPRINTK( s, arg... ) printk( FUN_NAME s, __FUNCTION__, ##arg )
#else
#define PPRINTK( s, arg... ) 
#endif 

#define RQ_SFQQ(rq) (struct sfqd_queue*)((rq)->elv.priv[0])
#define US_TO_NS 1000


//static spinlock_t qk;

static struct kmem_cache *sfqd_pool;
static int rq_count = 0;
static int set_put_count = 0;
static struct request *last_rq = NULL;
static int need_switch = 0;
static unsigned int quantum = 8;
static unsigned long sfqd_slice = HZ / 10;
static struct virt *vt;

struct virt{
	unsigned long long t;
	spinlock_t lock;
};

#if 0
void virt_inc(void){
	spin_lock(&vt->lock);
	vt->t++;
	spin_unlock(&vt->lock);
}

void virt_dec(void){
	spin_lock(&vt->lock);
	vt->t--;
	spin_unlock(&vt->lock);
}


unsigned long long virt_ret(void){
	return vt->t;
}
#endif

//struct semaphore send_lock;

struct sfqd_queue{
	struct sfqd_data *sfqdd;
	struct list_head pro_reqs;  //Put reqs inside the queue
	struct list_head list;		//Used by sfqdq_head to put them together
	pid_t	pid;
	int	ref;
	long long time_quantum;
	spinlock_t lock;
};

struct sfqd_data {
	struct request_queue *queue;	//Block device request queue
	struct sfqd_queue *active_queue;  //Current server sfqd_queue
//	struct list_head sfqdq_list; //Store the sfqd_queue list for every process
	unsigned int sfqd_quantum;
	unsigned int sfqd_slice;
//	struct radix_tree_root *sfqdq_head;
	struct radix_tree_root *qroot;
	struct timeval rq_start_time;
	struct timeval rq_end_time;
	int lock_num;
};

static struct sfqd_queue *current_queue = NULL;
// API part
/*
static inline struct sfqd_io_cq *icq_to_zic(struct io_cq *icq)
{
	return container_of(icq, struct sfqd_io_cq, icq);
}
*/
//End API part

/*static void sfqd_merged_reqs(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	DPRINTK("IN=====>>>>>>%s>>>>>>=====\n", __FUNCTION__);
	list_del_init(&next->queuelist);
}
*/
/*static struct sfqd_queue *prev_sfqd_queue(struct sfqd_queue *sfqdq)
{
	return list_entry(sfqdq->list.prev, struct sfqd_queue, list);
}

*/
static struct sfqd_queue *next_sfqd_queue(struct sfqd_data *sfqdd, struct sfqd_queue *sfqdq)
{
	struct sfqd_queue *tmp;
	DPRINTK("IN=====>>>>>>%s>>>>>>=====\n", __FUNCTION__);	
	if (list_is_singular(&sfqdd->sfqdq_head)) {
		return sfqdq;
	} else {
		if (list_is_last(&sfqdq->list, &sfqdd->sfqdq_head)) {
			tmp = list_entry(sfqdd->sfqdq_head.next, struct sfqd_queue, list);
		} else {
			tmp = list_entry(sfqdq->list.next, struct sfqd_queue, list);
		}
		return tmp;
	}
}

static int sfqd_dispatch(struct request_queue *q, int force)
{
	struct sfqd_data *sfqdd = q->elevator->elevator_data;
	//struct sfqd_queue *sfqdq;
	struct request *rq;

	DPRINTK("\n\nIN=====>>>>>>%s>>>>>>=====\n", __FUNCTION__);
	NPRINTK("========Current lock is %d\n", sfqdd->lock_num);
	if (sfqdd->lock_num == 0) {
		NPRINTK("***********************Current lock num is 0 ,Do nothing!\n");
		return 0;
	}
	if (!list_empty(&sfqdd->sfqdq_head)){
		if (current_queue == NULL) { 
			NPRINTK("%s:******Get the queue from sfqdq_head\n", __FUNCTION__);
			current_queue = list_entry(sfqdd->sfqdq_head.next, struct sfqd_queue, list);
			current_queue->time_quantum = jiffies_to_usecs(sfqd_slice) * US_TO_NS;
		//	if (current_queue == NULL || &sfqdq_head == sfqdq_head.next) {
		//		printk("%s:=========There is no sfqd queue inside in list\n", __FUNCTION__);
		//		DPRINTK("out=====<<<<<<%s<<<<<<<=====\n\n", __FUNCTION__);
		//		return 0;
		//	}
		} else {
			if (current_queue->time_quantum <= 0){
				need_switch = 0;
				NPRINTK("%s:******Get the queue from next_sfqd_queue, the current_queue is for PID %d*****\n",__FUNCTION__, current_queue->pid);
				current_queue = next_sfqd_queue(sfqdd, current_queue);
				current_queue->time_quantum = jiffies_to_usecs(sfqd_slice) * US_TO_NS;
			} else {
				NPRINTK("%s:******Keep serving in this queue for PID %d*****\n",__FUNCTION__, current_queue->pid);	
			}
		}
		NPRINTK("%s:*********Current process queue is for PID %d*********\n",__FUNCTION__, current_queue->pid);
		if (!list_empty(&current_queue->pro_reqs))
			rq = list_entry(current_queue->pro_reqs.next, struct request, queuelist);
		else {
			NPRINTK("%s:*********Current process queue for PID %d is EMPTY!!!\n",__FUNCTION__, current_queue->pid);
			current_queue = next_sfqd_queue(sfqdd, current_queue);
			current_queue->time_quantum = jiffies_to_usecs(sfqd_slice) * US_TO_NS;
			DPRINTK("out=====<<<<<<%s<<<<<<<=====\n\n", __FUNCTION__);
			return 0;
		}

		if (rq == NULL) {
			NPRINTK("%s:********Dispatch get a NULL request from the process queue******\n", __FUNCTION__);
		}

		rq_count--;
		NPRINTK("%s:*********Dispatch request  for %d, left [%d] reqs, the set_put_count = [%d]*********\n", __FUNCTION__, current_queue->pid, rq_count, set_put_count);
		list_del_init(&rq->queuelist);
		do_gettimeofday(&sfqdd->rq_start_time);
		NPRINTK("@@@@@@@@@@@@@@@@@@@@@@=========rq_start_time is %lld<<<<<<<=====\n\n", timeval_to_ns(&sfqdd->rq_start_time));
		elv_dispatch_sort(q, rq);
		sfqdd->lock_num = 0;
		DPRINTK("out=====<<<<<<%s<<<<<<<=====\n\n", __FUNCTION__);
		return 1;
	}

	DPRINTK("\n\nOUT!!!!!!!!=====>>>>>>%s>>>sfqdq_head is empty!!!!!!, current reqs is [%d], set_put_count = [%d]>>>=====\n", __FUNCTION__, rq_count, set_put_count);
	if (rq_count != 0) {
		NPRINTK("\n\n%s: Because current reqs is [%d], it is not 0, so dispatch the last_rq, set_put_count = [%d]>>>=====\n", __FUNCTION__, rq_count, set_put_count);
		if (last_rq == NULL) {
			printk("important:***********************last_rq is NULL!!!!\n");
			return 0;
		}
		else {
			last_rq->elv.priv[0] = NULL;
			elv_dispatch_sort(q, last_rq);
			do_gettimeofday(&sfqdd->rq_start_time);
			NPRINTK("@@@@@@@@@@@@@@@@@@@@@@=========rq_start_time is %lld<<<<<<<=====\n\n", timeval_to_ns(&sfqdd->rq_start_time));
			rq_count--;
			last_rq = NULL;
			sfqdd->lock_num = 0;
			return 1;
		}
	}	
	return 0;
	
	//if (!list_empty(&sfqdd->queue)) {
	//	struct request *rq;
	//	rq = list_entry(sfqdd->queue.next, struct request, queuelist);
	//	list_del_init(&rq->queuelist);
	//	elv_dispatch_sort(q, rq);
	//	return 1;
	//}
	//return 0;
}

static void sfqd_add_request(struct request_queue *q, struct request *rq)
{
//	struct sfqd_data *sfqdd = q->elevator->elevator_data;
	struct sfqd_queue *sfqdq = RQ_SFQQ(rq);
//	spin_lock_irq(q->queue_lock);	
	rq_count++;
	NPRINTK("%s:********Find the request is belong to Process PID %d********, it is the [%d] request\n", __FUNCTION__, sfqdq->pid, rq_count); last_rq = rq;
	list_add_tail(&rq->queuelist, &sfqdq->pro_reqs);
//	spin_unlock_irq(q->queue_lock);
}

static struct sfqd_queue *sfqd_create_queue(struct sfqd_data *sfqdd, gfp_t gfp_mask)
{
	struct sfqd_queue *sfqdq = NULL;

	DPRINTK("Create a new queue\n");
	sfqdq = (struct sfqd_queue*)kmalloc(sizeof(struct sfqd_queue), gfp_mask);
	if (!sfqdq) {
		printk("<%s>: Allocate the sfqdq failed!\n", __FUNCTION__);
		return NULL;
	}
	sfqdq->sfqdd = sfqdd;
	sfqdq->pid = current->pid;
	sfqdq->ref = 0;
	spin_lock_init(&sfqdq->lock);
	NPRINTK("Create a sfqd_queue for process PID %d\n", sfqdq->pid);
	INIT_LIST_HEAD(&sfqdq->pro_reqs);
	return sfqdq; 
}

static struct sfqd_queue *pid_to_sfqdq(struct sfqd_data *sfqdd, int pid)
{
	struct sfqd_queue* pos;
	pos = radix_tree_lookup(sfqdd->qroot, pid);	
	PPRINTK("No match queue for PID[%d]\n", pid);
	return pos;
}


/*
 * sfqd_io_cq is the data structure that used to connect with io_cq and process
 * struct io_cq *icq is a data structure store in struct request, we use sfqd_io_cq to
 * contain it with struct sfqd_queue sfqdq, then we can connect request with sfqdq, the
 * sfqd_io_cq *zic is our bridge
 */

static int sfqd_set_request(struct request_queue *q, struct request *rq, gfp_t gfp_mask)
{
	struct sfqd_data *sfqdd = q->elevator->elevator_data;
	struct sfqd_queue *sfqdq = pid_to_sfqdq(sfqdd, current->pid);
	struct sfqd_queue *pos;
	
	DPRINTK("Cur virt time[%llu]\n", vt->t);
	
	//Check if have the process queue for this request, if it is exist mark the request with sfqdq
	//If not, create a new queue for this process
	if (!sfqdq) {
		NPRINTK("No queue for PID [%d]\n", current->pid);
		sfqdq = sfqd_create_queue(sfqdd, gfp_mask);
		radix_tree_insert(sfqdd->qroot, current->pid, (void *) sfqdq);
//		list_add_tail(&sfqdq->list, &sfqdd->sfqdq_head);
	}
	spin_lock(&sfqdq->lock);
	sfqdq->ref++;
	spin_unlock(&sfqdq->lock);
	NPRINTK("Sfq ref for [%d] is [%d]\n", sfqdq->pid, sfqdq->ref);
	rq->elv.priv[0] = sfqdq;	
	return 0;
}

static void sfqd_put_request(struct request *rq)
{
	struct sfqd_queue *sfqdq = RQ_ZFQQ(rq);
	//struct sfqd_io_cq *zic = icq_to_zic(rq->elv.icq);
	struct sfqd_queue *pos;

	set_put_count--;
	DPRINTK("IN==========%s======,current reqs still have [%d], set_put_count = [%d]\n", __FUNCTION__, rq_count, set_put_count);	
//	spin_lock_irq(&qk);
	if (sfqdq){
		rq->elv.priv[0] = NULL;
		//zic->sfqdq = NULL;
		sfqdq->ref--;
		NPRINTK("%s:****current sfqdq->ref for %d is %d******\n",__FUNCTION__, sfqdq->pid, sfqdq->ref);
		if (sfqdq->ref) {
			NPRINTK("%s:!!!!!!!!!!!!!!!!********This processes queue for PID %d didn't finish yet, keep it****\n",__FUNCTION__, sfqdq->pid);
		//	spin_unlock_irq(&qk);
			return;
		}
		NPRINTK("%s:******The sfqdq for PID %d is OVER, delete it, because its ref is %d**********\n",__FUNCTION__, sfqdq->pid, sfqdq->ref);
		list_del_init(&sfqdq->list);
		list_for_each_entry(pos, &sfqdq->sfqdd->sfqdq_head, list){
			NPRINTK("%s:*********The current queue for process we still have PID**********%d\n",__FUNCTION__, pos->pid);
			if (pos->list.next == &pos->list) {
				NPRINTK("%s:*******He point to him self!!!!!!!%d\n",__FUNCTION__, pos->pid);
				break;
			}
		}

		if (current_queue == sfqdq) {
			current_queue = next_sfqd_queue(sfqdq->sfqdd, current_queue);
			if (current_queue == sfqdq) current_queue = NULL;
			NPRINTK("%s:******The current queue is equal to sfqdq, set current queue to next, if just one queue, set NULL**********\n", __FUNCTION__);
		}
	//	kmem_cache_free(sfqd_pool, sfqdq);
		kfree(sfqdq);
	}
//	spin_unlock_irq(&qk);
}

static void sfqd_completed_request(struct request_queue *q, struct request* rq)
{
	struct sfqd_queue *sfqdq = RQ_ZFQQ(rq);
	struct sfqd_data *sfqdd = q->elevator->elevator_data;
	DPRINTK("IN==========%s======\n", __FUNCTION__);
	do_gettimeofday(&sfqdd->rq_end_time);
	NPRINTK("@@@@@@@@@@@@@@@@@@@@@@=========rq_start_time is %lld<<<<<<<=====\n\n", timeval_to_ns(&sfqdd->rq_end_time));
	DPRINTK("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@%s: Last request server time is: %lld ns\n", __FUNCTION__, timeval_to_ns(&sfqdd->rq_end_time) - timeval_to_ns(&sfqdd->rq_start_time));
	sfqdq->time_quantum = sfqdq->time_quantum - (timeval_to_ns(&sfqdd->rq_end_time) - timeval_to_ns(&sfqdd->rq_start_time));
	DPRINTK("=========Current lock is %d======\n", sfqdd->lock_num);
	sfqdd->lock_num = 1;
	DPRINTK("=========Current lock is %d======\n", sfqdd->lock_num);
	
}

/*
static struct request *
sfqd_former_request(struct request_queue *q, struct request *rq)
{
	//struct sfqd_data *sfqdd = q->elevator->elevator_data;
	//struct sfqd_queue *sfqdq = RQ_ZFQQ(rq);

	if (rq->queuelist.prev != rq->queuelist)
		return list_entry(rq->queuelist.prev, struct request, queuelist);
	else if (sfqdq_head->next != sfqdq_head)
	{
		sfqdq = prev_sfqd_queue();
		
	}

	DPRINTK("IN==========%s======\n", __FUNCTION__);
	if (rq->queuelist.prev == &rq->queuelist)
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
sfqd_latter_request(struct request_queue *q, struct request *rq)
{
	//struct sfqd_data *sfqdd = q->elevator->elevator_data;

	DPRINTK("IN==========%s======\n", __FUNCTION__);
	if (rq->queuelist.next == &rq->queuelist)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}
*/

static void *pid_sfqd_init_queue(struct request_queue *q)
{
	static struct sfqd_data *sfqdd;
	printk("IN==========%s======\n", __FUNCTION__);
	sfqdd = kmalloc_node(sizeof(*sfqdd), GFP_KERNEL, q->node);
	if (!sfqdd)
		return NULL;
	sfqdd->queue = q;
	sfqdd->sfqd_quantum = quantum;
	sfqdd->sfqd_slice = sfqd_slice;
	sfqdd->lock_num = 1;
	sfqdd->qroot = (struct radix_tree_root *)vmalloc(sizeof(*sfqdd->qroot));
	if (sfqdd->qroot == NULL) {
		printk("Cannot allocate memory.\n");
		return NULL;
	}
	INIT_RADIX_TREE(sfqdd->qroot, GFP_NOIO);
	INIT_LIST_HEAD(&sfqdd->sfqdq_head);	
	return sfqdd;
}

static void pid_sfqd_exit_queue(struct elevator_queue *e)
{
	struct sfqd_data *sfqdd = e->elevator_data;

	printk("IN==========%s======\n", __FUNCTION__);
	kfree(sfqdd);
}

static struct elevator_type elevator_sfqd = {
	.ops = {
//		.elevator_merge_req_fn		= sfqd_merged_reqs,
		.elevator_dispatch_fn		= sfqd_dispatch,
		.elevator_add_req_fn		= sfqd_add_request,
//		.elevator_former_req_fn		= sfqd_former_request,
//		.elevator_latter_req_fn		= sfqd_latter_request,
		.elevator_set_req_fn		= sfqd_set_request, //To set the request property
		.elevator_completed_req_fn	= sfqd_completed_request,
		.elevator_put_req_fn		= sfqd_put_request,
		.elevator_init_fn		= pid_sfqd_init_queue,
		.elevator_exit_fn		= pid_sfqd_exit_queue,
	},
	.elevator_name = "pid_sfqd",
	.elevator_owner = THIS_MODULE,
};

static int __init pid_sfqd_init(void)
{
	vt = (struct virt *)vmalloc(sizeof(*vt));
	spin_lock_init(&vt->lock);
	DPRINTK("Try this!\n");
	return elv_register(&elevator_sfqd);
}

static void __exit pid_sfqd_exit(void)
{
	printk("IN==========%s======\n", __FUNCTION__);
	kmem_cache_destroy(sfqd_pool);
	elv_unregister(&elevator_sfqd);
}

module_init(pid_sfqd_init);
module_exit(pid_sfqd_exit);


MODULE_AUTHOR("Wenji Li");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PID_ZFQ IO scheduler");
