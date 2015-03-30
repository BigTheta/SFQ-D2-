/*
 * elevator zfq
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

#define DEBUG_FUN  0
#if DEBUG_FUN
#define DPRINTK( s, arg... ) printk( s, ##arg )
#else
#define DPRINTK( s, arg... ) 
#endif 


#define DEBUG_NUM  0
#if DEBUG_NUM
#define NPRINTK( s, arg... ) printk( s, ##arg )
#else
#define NPRINTK( s, arg... ) 
#endif 

#define DEBUG_PID  0
#if DEBUG_PID	
#define PPRINTK( s, arg... ) printk( s, ##arg )
#else
#define PPRINTK( s, arg... ) 
#endif 

#define RQ_ZFQQ(rq) (struct zfq_queue*)((rq)->elv.priv[0])
#define US_TO_NS 1000


//static spinlock_t qk;

static struct kmem_cache *zfq_pool;
static int rq_count = 0;
static int set_put_count = 0;
static struct request *last_rq = NULL;
static int need_switch = 0;
static unsigned int quantum = 8;
static unsigned long zfq_slice = HZ / 10;

//struct semaphore send_lock;

struct zfq_queue{
	struct zfq_data *zfqd;
	struct list_head pro_requests;  //Put requests inside the queue
	struct list_head list;		//Used by zfqq_head to put them together
	pid_t	pid;
	int	ref;
	long long time_quantum;
};

struct zfq_data {
	struct request_queue *queue;	//Block device request queue
	struct zfq_queue *active_queue;  //Current server zfq_queue
//	struct list_head zfqq_list; //Store the zfq_queue list for every process
	unsigned int zfq_quantum;
	unsigned int zfq_slice;
	struct list_head zfqq_head;
	struct timeval rq_start_time;
	struct timeval rq_end_time;
	int lock_num;
};

static struct zfq_queue *current_queue = NULL;

// API part
/*
static inline struct zfq_io_cq *icq_to_zic(struct io_cq *icq)
{
	return container_of(icq, struct zfq_io_cq, icq);
}
*/
//End API part

/*static void zfq_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	DPRINTK("IN=====>>>>>>%s>>>>>>=====\n", __FUNCTION__);
	list_del_init(&next->queuelist);
}
*/
/*static struct zfq_queue *prev_zfq_queue(struct zfq_queue *zfqq)
{
	return list_entry(zfqq->list.prev, struct zfq_queue, list);
}

*/
static struct zfq_queue *next_zfq_queue(struct zfq_data *zfqd, struct zfq_queue *zfqq)
{
	struct zfq_queue *tmp;
	DPRINTK("IN=====>>>>>>%s>>>>>>=====\n", __FUNCTION__);	
	if (list_is_singular(&zfqd->zfqq_head)) {
		return zfqq;
	} else {
		if (list_is_last(&zfqq->list, &zfqd->zfqq_head)) {
			tmp = list_entry(zfqd->zfqq_head.next, struct zfq_queue, list);
		} else {
			tmp = list_entry(zfqq->list.next, struct zfq_queue, list);
		}
		return tmp;
	}
}

static int zfq_dispatch(struct request_queue *q, int force)
{
	struct zfq_data *zfqd = q->elevator->elevator_data;
	//struct zfq_queue *zfqq;
	struct request *rq;

	DPRINTK("\n\nIN=====>>>>>>%s>>>>>>=====\n", __FUNCTION__);
	NPRINTK("========Current lock is %d\n", zfqd->lock_num);
	if (zfqd->lock_num == 0) {
		NPRINTK("***********************Current lock num is 0 ,Do nothing!\n");
		return 0;
	}
	if (!list_empty(&zfqd->zfqq_head)){
		if (current_queue == NULL) { 
			NPRINTK("%s:******Get the queue from zfqq_head\n", __FUNCTION__);
			current_queue = list_entry(zfqd->zfqq_head.next, struct zfq_queue, list);
			current_queue->time_quantum = jiffies_to_usecs(zfq_slice) * US_TO_NS;
		//	if (current_queue == NULL || &zfqq_head == zfqq_head.next) {
		//		printk("%s:=========There is no zfq queue inside in list\n", __FUNCTION__);
		//		DPRINTK("out=====<<<<<<%s<<<<<<<=====\n\n", __FUNCTION__);
		//		return 0;
		//	}
		} else {
			if (current_queue->time_quantum <= 0){
				need_switch = 0;
				NPRINTK("%s:******Get the queue from next_zfq_queue, the current_queue is for PID %d*****\n",__FUNCTION__, current_queue->pid);
				current_queue = next_zfq_queue(zfqd, current_queue);
				current_queue->time_quantum = jiffies_to_usecs(zfq_slice) * US_TO_NS;
			} else {
				NPRINTK("%s:******Keep serving in this queue for PID %d*****\n",__FUNCTION__, current_queue->pid);	
			}
		}
		NPRINTK("%s:*********Current process queue is for PID %d*********\n",__FUNCTION__, current_queue->pid);
		if (!list_empty(&current_queue->pro_requests))
			rq = list_entry(current_queue->pro_requests.next, struct request, queuelist);
		else {
			NPRINTK("%s:*********Current process queue for PID %d is EMPTY!!!\n",__FUNCTION__, current_queue->pid);
			current_queue = next_zfq_queue(zfqd, current_queue);
			current_queue->time_quantum = jiffies_to_usecs(zfq_slice) * US_TO_NS;
			DPRINTK("out=====<<<<<<%s<<<<<<<=====\n\n", __FUNCTION__);
			return 0;
		}

		if (rq == NULL) {
			NPRINTK("%s:********Dispatch get a NULL request from the process queue******\n", __FUNCTION__);
		}

		rq_count--;
		NPRINTK("%s:*********Dispatch request  for %d, left [%d] requests, the set_put_count = [%d]*********\n", __FUNCTION__, current_queue->pid, rq_count, set_put_count);
		list_del_init(&rq->queuelist);
		do_gettimeofday(&zfqd->rq_start_time);
		NPRINTK("@@@@@@@@@@@@@@@@@@@@@@=========rq_start_time is %lld<<<<<<<=====\n\n", timeval_to_ns(&zfqd->rq_start_time));
		elv_dispatch_sort(q, rq);
		zfqd->lock_num = 0;
		DPRINTK("out=====<<<<<<%s<<<<<<<=====\n\n", __FUNCTION__);
		return 1;
	}

	DPRINTK("\n\nOUT!!!!!!!!=====>>>>>>%s>>>zfqq_head is empty!!!!!!, current requests is [%d], set_put_count = [%d]>>>=====\n", __FUNCTION__, rq_count, set_put_count);
	if (rq_count != 0) {
		NPRINTK("\n\n%s: Because current requests is [%d], it is not 0, so dispatch the last_rq, set_put_count = [%d]>>>=====\n", __FUNCTION__, rq_count, set_put_count);
		if (last_rq == NULL) {
			printk("important:***********************last_rq is NULL!!!!\n");
			return 0;
		}
		else {
			last_rq->elv.priv[0] = NULL;
			elv_dispatch_sort(q, last_rq);
			do_gettimeofday(&zfqd->rq_start_time);
			NPRINTK("@@@@@@@@@@@@@@@@@@@@@@=========rq_start_time is %lld<<<<<<<=====\n\n", timeval_to_ns(&zfqd->rq_start_time));
			rq_count--;
			last_rq = NULL;
			zfqd->lock_num = 0;
			return 1;
		}
	}	
	return 0;
	
	//if (!list_empty(&zfqd->queue)) {
	//	struct request *rq;
	//	rq = list_entry(zfqd->queue.next, struct request, queuelist);
	//	list_del_init(&rq->queuelist);
	//	elv_dispatch_sort(q, rq);
	//	return 1;
	//}
	//return 0;
}

static void zfq_add_request(struct request_queue *q, struct request *rq)
{
//	struct zfq_data *zfqd = q->elevator->elevator_data;
	struct zfq_queue *zfqq = RQ_ZFQQ(rq);
//	spin_lock_irq(q->queue_lock);	
	DPRINTK("IN==========%s======\n", __FUNCTION__);
	rq_count++;
	NPRINTK("%s:********Find the request is belong to Process PID %d********, it is the [%d] request\n", __FUNCTION__, zfqq->pid, rq_count); last_rq = rq;
	list_add_tail(&rq->queuelist, &zfqq->pro_requests);
//	spin_unlock_irq(q->queue_lock);
}

static struct zfq_queue *zfq_create_queue(struct zfq_data *zfqd, gfp_t gfp_mask)
{
	struct zfq_queue *zfqq = NULL;

	DPRINTK("IN==========%s======\n", __FUNCTION__);
//	zfqq = kmem_cache_alloc_node(zfq_pool, gfp_mask | __GFP_ZERO, zfqd->queue->node);
	zfqq = (struct zfq_queue*)kmalloc(sizeof(struct zfq_queue), gfp_mask);
	if (!zfqq) {
		printk("========Allocate the zfqq failed!\n");
		return NULL;
	}
	zfqq->zfqd = zfqd;
	zfqq->pid = current->pid;
	zfqq->ref = 0;
	NPRINTK("%s:***********Create a zfq_queue for process PID %d************\n",__FUNCTION__, current->pid);
	INIT_LIST_HEAD(&zfqq->pro_requests);
	return zfqq; 
}

static struct zfq_queue *pid_to_zfqq(struct zfq_data *zfqd, int pid)
{
	struct zfq_queue* pos;

	list_for_each_entry(pos, &zfqd->zfqq_head, list){
		if (pos->pid == pid) {
			PPRINTK("========Found a match PID %d queue for this request!\n", pid);
			return pos;		
		}
	}
	PPRINTK("%s======Didn't find much zfqq for this PID %d====\n", __FUNCTION__, pid);
	return NULL;
}


/*
 * zfq_io_cq is the data structure that used to connect with io_cq and process
 * struct io_cq *icq is a data structure store in struct request, we use zfq_io_cq to
 * contain it with struct zfq_queue zfqq, then we can connect request with zfqq, the
 * zfq_io_cq *zic is our bridge
 */

static int zfq_set_request(struct request_queue *q, struct request *rq, gfp_t gfp_mask)
{
	struct zfq_data *zfqd = q->elevator->elevator_data;
	struct zfq_queue *zfqq = pid_to_zfqq(zfqd, current->pid);

	struct zfq_queue *pos;
	

	set_put_count++;	
	DPRINTK("IN==========%s======,the current requests have [%d], set_put_count = [%d]\n", __FUNCTION__,rq_count, set_put_count);
	
	spin_lock_irq(q->queue_lock);
	//spin_lock_irq(&qk);
	//Check if have the process queue for this request, if it is exist mark the request with zfqq
	//If not, create a new queue for this process
	if (!zfqq || zfqq->ref == 0) {
		NPRINTK("%s:*******There is no queue for process PID %d\n", __FUNCTION__,current->pid);
		zfqq = zfq_create_queue(zfqd, gfp_mask);
		//zic->zfqq = zfqq;
		list_add_tail(&zfqq->list, &zfqd->zfqq_head);
	}
	zfqq->ref++;
	NPRINTK("%s:****current zfqq->ref for %d is %d******\n",__FUNCTION__, zfqq->pid, zfqq->ref);
	rq->elv.priv[0] = zfqq;	
	//FIXME:DEBUG
	list_for_each_entry(pos, &zfqd->zfqq_head, list){
		NPRINTK("%s:*********The current queue for process we still have PID**********%d\n",__FUNCTION__, pos->pid);
		if (pos->list.next == &pos->list) {
			NPRINTK("%s:*******He point to him self!!!!!!!%d\n",__FUNCTION__, pos->pid);
			break;
		}
	}
//
//	spin_unlock_irq(&qk);
	spin_unlock_irq(q->queue_lock);	
	//If active_queue is empty, mark server it first
//	if (zfqd->active_queue == NULL) 
//		zfqd->active_queue = zfqq;
	return 0;
}

static void zfq_put_request(struct request *rq)
{
	struct zfq_queue *zfqq = RQ_ZFQQ(rq);
	//struct zfq_io_cq *zic = icq_to_zic(rq->elv.icq);
	struct zfq_queue *pos;

	set_put_count--;
	DPRINTK("IN==========%s======,current requests still have [%d], set_put_count = [%d]\n", __FUNCTION__, rq_count, set_put_count);	
//	spin_lock_irq(&qk);
	if (zfqq){
		rq->elv.priv[0] = NULL;
		//zic->zfqq = NULL;
		zfqq->ref--;
		NPRINTK("%s:****current zfqq->ref for %d is %d******\n",__FUNCTION__, zfqq->pid, zfqq->ref);
		if (zfqq->ref) {
			NPRINTK("%s:!!!!!!!!!!!!!!!!********This processes queue for PID %d didn't finish yet, keep it****\n",__FUNCTION__, zfqq->pid);
		//	spin_unlock_irq(&qk);
			return;
		}
		NPRINTK("%s:******The zfqq for PID %d is OVER, delete it, because its ref is %d**********\n",__FUNCTION__, zfqq->pid, zfqq->ref);
		list_del_init(&zfqq->list);
		list_for_each_entry(pos, &zfqq->zfqd->zfqq_head, list){
			NPRINTK("%s:*********The current queue for process we still have PID**********%d\n",__FUNCTION__, pos->pid);
			if (pos->list.next == &pos->list) {
				NPRINTK("%s:*******He point to him self!!!!!!!%d\n",__FUNCTION__, pos->pid);
				break;
			}
		}

		if (current_queue == zfqq) {
			current_queue = next_zfq_queue(zfqq->zfqd, current_queue);
			if (current_queue == zfqq) current_queue = NULL;
			NPRINTK("%s:******The current queue is equal to zfqq, set current queue to next, if just one queue, set NULL**********\n", __FUNCTION__);
		}
	//	kmem_cache_free(zfq_pool, zfqq);
		kfree(zfqq);
	}
//	spin_unlock_irq(&qk);
}

static void zfq_completed_request(struct request_queue *q, struct request* rq)
{
	struct zfq_queue *zfqq = RQ_ZFQQ(rq);
	struct zfq_data *zfqd = q->elevator->elevator_data;
	DPRINTK("IN==========%s======\n", __FUNCTION__);
	do_gettimeofday(&zfqd->rq_end_time);
	NPRINTK("@@@@@@@@@@@@@@@@@@@@@@=========rq_start_time is %lld<<<<<<<=====\n\n", timeval_to_ns(&zfqd->rq_end_time));
	DPRINTK("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@%s: Last request server time is: %lld ns\n", __FUNCTION__, timeval_to_ns(&zfqd->rq_end_time) - timeval_to_ns(&zfqd->rq_start_time));
	zfqq->time_quantum = zfqq->time_quantum - (timeval_to_ns(&zfqd->rq_end_time) - timeval_to_ns(&zfqd->rq_start_time));
	DPRINTK("=========Current lock is %d======\n", zfqd->lock_num);
	zfqd->lock_num = 1;
	DPRINTK("=========Current lock is %d======\n", zfqd->lock_num);
	
}

/*
static struct request *
zfq_former_request(struct request_queue *q, struct request *rq)
{
	//struct zfq_data *zfqd = q->elevator->elevator_data;
	//struct zfq_queue *zfqq = RQ_ZFQQ(rq);

	if (rq->queuelist.prev != rq->queuelist)
		return list_entry(rq->queuelist.prev, struct request, queuelist);
	else if (zfqq_head->next != zfqq_head)
	{
		zfqq = prev_zfq_queue();
		
	}

	DPRINTK("IN==========%s======\n", __FUNCTION__);
	if (rq->queuelist.prev == &rq->queuelist)
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
zfq_latter_request(struct request_queue *q, struct request *rq)
{
	//struct zfq_data *zfqd = q->elevator->elevator_data;

	DPRINTK("IN==========%s======\n", __FUNCTION__);
	if (rq->queuelist.next == &rq->queuelist)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}
*/

static void *pid_zfq_init_queue(struct request_queue *q)
{
	static struct zfq_data *zfqd;
	printk("IN==========%s======\n", __FUNCTION__);
	zfqd = kmalloc_node(sizeof(*zfqd), GFP_KERNEL, q->node);
	if (!zfqd)
		return NULL;
	zfqd->queue = q;
	zfqd->zfq_quantum = quantum;
	zfqd->zfq_slice = zfq_slice;
	zfqd->lock_num = 1;
	INIT_LIST_HEAD(&zfqd->zfqq_head);
	return zfqd;
}

static void pid_zfq_exit_queue(struct elevator_queue *e)
{
	struct zfq_data *zfqd = e->elevator_data;

	printk("IN==========%s======\n", __FUNCTION__);
	kfree(zfqd);
}

static struct elevator_type elevator_zfq = {
	.ops = {
//		.elevator_merge_req_fn		= zfq_merged_requests,
		.elevator_dispatch_fn		= zfq_dispatch,
		.elevator_add_req_fn		= zfq_add_request,
//		.elevator_former_req_fn		= zfq_former_request,
//		.elevator_latter_req_fn		= zfq_latter_request,
		.elevator_set_req_fn		= zfq_set_request, //To set the request property
		.elevator_completed_req_fn	= zfq_completed_request,
		.elevator_put_req_fn		= zfq_put_request,
		.elevator_init_fn		= pid_zfq_init_queue,
		.elevator_exit_fn		= pid_zfq_exit_queue,
	},
	.elevator_name = "pid_zfq",
	.elevator_owner = THIS_MODULE,
};

static int __init pid_zfq_init(void)
{
	printk("IN=====use zfqd=====%s======\n", __FUNCTION__);
	printk("=======HZ====%d\n", HZ);
	printk("1 jiffies is %dus\n", jiffies_to_usecs(1));
//	sema_init(&send_lock, 1);
	return elv_register(&elevator_zfq);
}

static void __exit pid_zfq_exit(void)
{
	printk("IN==========%s======\n", __FUNCTION__);
	kmem_cache_destroy(zfq_pool);
	elv_unregister(&elevator_zfq);
}

module_init(pid_zfq_init);
module_exit(pid_zfq_exit);


MODULE_AUTHOR("Wenji Li");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PID_ZFQ IO scheduler");
