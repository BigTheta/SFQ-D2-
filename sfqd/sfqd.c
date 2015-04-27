/*
 * elevator sfq
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

#define PS 4096

#define FUN_NAME "<%s>: "

#define DEBUG_FUN 1
#if DEBUG_FUN
#define DPRINTK( s, arg... ) printk( FUN_NAME s, __FUNCTION__, ##arg )
#else
#define DPRINTK( s, arg... ) 
#endif 


#define DEBUG_NUM 0
#if DEBUG_NUM
#define NPRINTK( s, arg... ) printk( FUN_NAME s, __FUNCTION__, ##arg )
#else
#define NPRINTK( s, arg... ) 
#endif 

#define DEBUG_PID 0
#if DEBUG_PID	
#define PPRINTK( s, arg... ) printk( FUN_NAME s, __FUNCTION__, ##arg )
#else
#define PPRINTK( s, arg... ) 
#endif 

#define RQ_SFQQ(rq) (struct sfq_queue*)((rq)->elv.priv[0])
#define RQ_SFQR(rq) (struct sfq_req*)((rq)->elv.priv[1])
#define US_TO_NS 1000

#define CAL_SIZE 1000

static int rq_count = 0;
static int set_put_count = 0;

static struct virt *vt;

struct virt{ //global virtual time
	unsigned long long t;
	unsigned long long lazyt;
	spinlock_t lock;
};

struct sfq_queue{ //per process
	struct sfq_data *sfqd;
	struct list_head pro_reqs;  //Put reqs inside the queue
	struct list_head list;		//Used by plist to put them together
	pid_t	pid;
	int	ref;
	spinlock_t lock;
	unsigned long long last_ft;
	
};

struct sfq_req{ //per request
	unsigned long long st; //Start tag
	unsigned long long ft; //Finish tag
	ktime_t skt; //Start ktime
	ktime_t fkt; //Finish ktime
	unsigned long long us_lat; //It should be us 
	struct list_head oslist; // All the request will put in a queue base on their start tag, out standing request list. Use for vt determin
	struct list_head wlist; // All the request arrival but not dispatch, use for dispatch request
	struct request *rq; // The request pointer
	struct sfq_queue *q;
};

struct sfq_data{ //global
	struct request_queue *queue;	//Block device request queue
	struct radix_tree_root *qroot;
	struct list_head plist;
	struct list_head oslist_head;
	/* struct list_head wlist_head; */ //obsolete
	unsigned long long *wl; //write latency array
	unsigned long long *rl; //read latency array
	int wp,rp; //The read and write pointer in read/write latency array
	int lock_num;
	int dispatched, depth;
	spinlock_t queue_lock, oslock;
};


static int sfq_dispatch(struct request_queue *q, int force)
{
	struct sfq_data *sfqd = q->elevator->elevator_data;
	struct sfq_queue *sfqq;
	struct sfq_req *sfqr, *min_rq = NULL;
	struct sfq_queue *process;
	
	DPRINTK("In Dispatch\n");

	if(sfqd->dispatched >= sfqd->depth){
		return 0;
	}

	/* if (!list_empty(&sfqd->wlist_head)) { */
	/* 	//FIXed: Replace with process loop */
	/* 	sfqr = list_first_entry(&sfqd->wlist_head, struct sfq_req, wlist); */
	/* 	list_del(&sfqr->wlist);	 */
	/* 	min_rq = sfqr; */
	/* } */

	DPRINTK("Getting Queue_lock\n");
	spin_lock(&sfqd->queue_lock);
	DPRINTK("Got Queue_lock\n");
	list_for_each_entry(process, &sfqd->plist, list) {
		if(!list_empty(&process->pro_reqs)){
			DPRINTK("getting process %d lock\n", process->pid);
			spin_lock(&process->lock);
			DPRINTK("got process %d lock\n", process->pid);
			sfqr = list_first_entry(&process->pro_reqs, struct sfq_req, wlist);
			
			if(min_rq == NULL) {
				sfqq = process;
				min_rq = sfqr;
			}
			else if(min_rq->st > sfqr->st) {
				sfqq = process;
				min_rq = sfqr;
			}
			DPRINTK("releasing process %d lock\n", process->pid);
			spin_unlock(&process->lock);
			DPRINTK("released process %d lock\n", process->pid);
						  
		}
	} 
	DPRINTK("releasing Queue_lock\n");
	spin_unlock(&sfqd->queue_lock);
	DPRINTK("released Queue_lock\n");

	if (min_rq != NULL) {
		/* min_rq->skt = ktime_get(); */
	  
		DPRINTK("getting process %d lock\n", sfqq->pid);
		spin_lock(&sfqq->lock);
		DPRINTK("got process %d lock\n", sfqq->pid);
		list_del_init(&min_rq->wlist);
		DPRINTK("releasing process %d lock\n", sfqq->pid);
		spin_unlock(&sfqq->lock);
		DPRINTK("released process %d lock\n", sfqq->pid);
	  
		DPRINTK("getting oslock\n");
		spin_lock(&sfqd->oslock);
		DPRINTK("got oslock\n");
		list_add_tail(&min_rq->oslist, &sfqd->oslist_head);
		DPRINTK("releasing oslock\n");
		spin_unlock(&sfqd->oslock);
		DPRINTK("released oslock\n");

		elv_dispatch_sort(q, min_rq->rq);
		sfqd->dispatched++;
		return 1;
	} else {
		/* DDPRINTK("No request to dispatch.\n"); */
		return 0;
	}
}

static void sfq_add_request(struct request_queue *q, struct request *rq)
{
	/* struct sfq_data *sfqd = q->elevator->elevator_data; */
	struct sfq_queue *sfqq = RQ_SFQQ(rq);
	struct sfq_req *sfqr;

	DPRINTK("\n");

	sfqr = (struct sfq_req *)kmalloc(sizeof(*sfqr), GFP_KERNEL);
	rq_count++;

	NPRINTK("Add request[%llu]->PID[%d]\n", rq->bio->bi_sector, sfqq->pid); 

	DPRINTK("Getting vt lock\n");
	spin_lock(&vt->lock);
	DPRINTK("Got vt lock\n");
	if(vt->t > sfqq->last_ft) {
		sfqr->st = vt->t;
		sfqr->ft = sfqr->st + blk_rq_bytes(rq) / PS; 
	} else {
		sfqr->st = sfqq->last_ft;	
		sfqr->ft = sfqr->st + blk_rq_bytes(rq) / PS;
	} 
	DPRINTK("Releasing vt lock\n");
	spin_unlock(&vt->lock);
	DPRINTK("Released vt lock\n");
		
	/* DPRINTK("request[%d]-stag[%llu]-ftag[%llu], size[%u]\n",  */
	/* 	rq_count, sfqr->st, sfqr->ft, blk_rq_bytes(rq)); */
	
	sfqr->rq = rq;
	sfqr->q = sfqq;

	//FIXed: Make for every process & lock it
	//list_add_tail(&sfqr->oslist, &sfqd->oslist_head); //should only include submitted requests
	//	list_add_tail(&sfqr->wlist, &sfqd->wlist_head); //should be per process (line 158)

	sfqq->last_ft = sfqr->ft;

	NPRINTK("Sfq ref for PID[%d] is [%d]\n", sfqq->pid, sfqq->ref);

	rq->elv.priv[1] = sfqr;
	//list_add_tail(&rq->queuelist, &sfqq->pro_reqs); */ //needs to be sfq_req start tag
	
	DPRINTK("getting process %d lock\n", sfqq->pid);
	spin_lock(&sfqq->lock);
	DPRINTK("got process %d lock\n", sfqq->pid);
	list_add_tail(&sfqr->wlist, &sfqq->pro_reqs);
	DPRINTK("releasing process %d lock\n", sfqq->pid);
	spin_unlock(&sfqq->lock);
	DPRINTK("released process %d lock\n", sfqq->pid);
}

static struct sfq_queue *sfq_create_queue(struct sfq_data *sfqd, gfp_t gfp_mask)
{
	struct sfq_queue *sfqq = NULL;

	DPRINTK("\n");
	sfqq = (struct sfq_queue*)kmalloc(sizeof(struct sfq_queue), gfp_mask);
	if (!sfqq) {
		printk("<%s>: Allocate the sfqq failed!\n", __FUNCTION__);
		return NULL;
	}
	sfqq->sfqd = sfqd;
	sfqq->pid = current->pid;
	sfqq->ref = 0;
	sfqq->last_ft = 0;
	spin_lock_init(&sfqq->lock);
	NPRINTK("Create a sfq_queue for process PID %d\n", sfqq->pid);
	INIT_LIST_HEAD(&sfqq->pro_reqs);
	return sfqq; 
}

static struct sfq_queue *pid_to_sfqq(struct sfq_data *sfqd, int pid)
{
	struct sfq_queue* pos;
	pos = radix_tree_lookup(sfqd->qroot, pid);	
	PPRINTK("No match queue for PID[%d]\n", pid);
	return pos;
}


/*
 * sfq_io_cq is the data structure that used to connect with io_cq and process
 * struct io_cq *icq is a data structure store in struct request, we use sfq_io_cq to
 * contain it with struct sfq_queue sfqq, then we can connect request with sfqq, the
 * sfq_io_cq *zic is our bridge
 */
static int sfq_set_request(struct request_queue *q, struct request *rq, struct bio *bio, gfp_t gfp_mask) { 
	struct sfq_data *sfqd = q->elevator->elevator_data;
	struct sfq_queue *sfqq;
	
	/* DPRINTK("Cur virt time[%llu]\n", vt->t);
	 */

	DPRINTK("\n");
	
	/* printk("set request%llu\n", bio->bi_sector); */
		
	//Check if have the process queue for this request, if it is exist mark the request with sfqq
	//If not, create a new queue for this process
	DPRINTK("Getting Queue_lock\n");
	spin_lock(&sfqd->queue_lock);	
	DPRINTK("Got queue_lock\n");
	sfqq = pid_to_sfqq(sfqd, current->pid);
	DPRINTK("1\n");
	if (!sfqq) {
		DPRINTK("2\n");
		NPRINTK("No queue for PID [%d]\n", current->pid);
		DPRINTK("3\n");
		sfqq = sfq_create_queue(sfqd, gfp_mask);
		DPRINTK("4\n");
		radix_tree_insert(sfqd->qroot, current->pid, (void *) sfqq);
		DPRINTK("5\n");
		list_add_tail(&sfqq->list, &sfqd->plist);
		DPRINTK("6\n");
	}
	DPRINTK("7\n");
	DPRINTK("releasing Queue_lock\n");
	spin_unlock(&sfqd->queue_lock);
	DPRINTK("Released queue_lock\n");
			
	/* spin_lock(&sfqq->lock); */
	/* sfqq->ref++; */
	/* spin_unlock(&sfqq->lock); */
	NPRINTK("Sfq ref for [%d] is [%d]\n", sfqq->pid, sfqq->ref);
	rq->elv.priv[0] = sfqq;		
	return 0;
}

static void sfq_put_request(struct request *rq)
{
	/* struct sfq_queue *sfqq = RQ_SFQQ(rq); */
	DPRINTK("Request for sfq done\n");

	set_put_count--;
	/* spin_lock(&sfqq->lock); */
	/* sfqq->ref--; */
	/* spin_unlock(&sfqq->lock); */
}

static void sfq_completed_request(struct request_queue *q, struct request* rq)
{
	struct sfq_data *sfqd = q->elevator->elevator_data;
	struct sfq_queue *sfqq = RQ_SFQQ(rq), *process;
	struct sfq_req *sfqr = RQ_SFQR(rq), *req;
	/* struct sfq_req *my_rq; */
	unsigned long long lat, temp;
	
	DPRINTK("\n");

	/* sfqr->fkt = ktime_get(); */
	DPRINTK("getting oslock\n");
	spin_lock(&sfqd->oslock);
	DPRINTK("got oslock\n");

	list_del(&sfqr->oslist);
	
	DPRINTK("releasing oslock\n");
	spin_unlock(&sfqd->oslock);
	DPRINTK("released oslock\n");
	sfqd->dispatched--;
	/* while(sfqd->dispatched < sfqd->depth) */
	sfq_dispatch(q, 1);		
	/* lat = ktime_us_delta(sfqr->fkt, sfqr->skt);	 */

	/* DPRINTK("Sfqd vt[%llu] for PID[%d] request complete with [%llu]us\n", vt->t, sfqq->pid, lat); */
	DPRINTK("getting oslock\n");
	spin_lock(&sfqd->oslock);
	DPRINTK("got oslock\n");
	if(!list_empty(&sfqd->oslist_head)) {
		temp=((struct sfq_req *)list_first_entry(&sfqd->oslist_head ,struct sfq_req, oslist))->st;
		list_for_each_entry(req, &sfqd->oslist_head, oslist) {
			if(temp > req->st)
				temp = req->st;
		}
		DPRINTK("releasing oslock\n");
		spin_unlock(&sfqd->oslock);
		DPRINTK("released oslock\n");

	}
	else {
		DPRINTK("releasing oslock\n");			
		spin_unlock(&sfqd->oslock);
		DPRINTK("released oslock\n");
		temp = sfqr->ft;
		DPRINTK("Getting Queue_lock\n");
		spin_lock(&sfqd->queue_lock);
		DPRINTK("Got Queue_lock\n");
		list_for_each_entry(process, &sfqd->plist, list) {
			DPRINTK("getting process %d lock\n", process->pid);
			spin_lock(&process->lock);
			DPRINTK("got process %d lock\n", process->pid);
			if(!list_empty(&process->pro_reqs)){
				req = list_first_entry(&process->pro_reqs, struct sfq_req, wlist);
				if(temp > req->st)
					temp = req->st;
			}
			DPRINTK("releasing process %d lock\n", process->pid);
			spin_unlock(&process->lock);
			DPRINTK("released process %d lock\n", process->pid);
		}
		DPRINTK("releasing Queue_lock\n");
		spin_unlock(&sfqd->queue_lock);
		DPRINTK("released Queue_lock\n");
	}
	DPRINTK("Getting vt lock\n");			
	spin_lock(&vt->lock);
	DPRINTK("Got vt lock\n");

	/* if (!list_empty(&sfqd->oslist_head)) { */
	/* 	my_rq = list_first_entry(&sfqd->oslist_head, struct sfq_req, oslist); */
	/* 	vt->t = my_rq->st; */
	/* } */
	vt->t = temp;
	DPRINTK("Releasing vt lock\n");
	spin_unlock(&vt->lock);
	DPRINTK("Released vt lock\n");

	/* DPRINTK("The new vt[%llu]\n", vt->t); */

	kfree(sfqr);
}

static int pid_sfq_init_queue(struct request_queue *q)
{
	struct sfq_data *sfqd;
	int i;

	sfqd = kmalloc_node(sizeof(*sfqd), GFP_KERNEL, q->node);
	if (!sfqd) {
		printk("Error: No memory\n");
		return -ENOMEM;
	}
	vt->t = 0;
	sfqd->queue = q;
	sfqd->lock_num = 1;
	sfqd->qroot = (struct radix_tree_root *)vmalloc(sizeof(*sfqd->qroot));

	sfqd->rl = (unsigned long long *)vmalloc(sizeof(unsigned long long) * CAL_SIZE);
	sfqd->wl = (unsigned long long *)vmalloc(sizeof(unsigned long long) * CAL_SIZE);
	sfqd->rp = 0;
	sfqd->wp = 0;

	if (sfqd->qroot == NULL) {
		printk("Cannot allocate memory.\n");
		return -ENOMEM;
	}

	for (i = 0; i < CAL_SIZE; i++) {
		sfqd->rl[i] = 0;
		sfqd->wl[i] = 0;
	}

	INIT_RADIX_TREE(sfqd->qroot, GFP_NOIO);
	INIT_LIST_HEAD(&sfqd->plist);	
	INIT_LIST_HEAD(&sfqd->oslist_head);
	spin_lock_init(&sfqd->queue_lock);
	spin_lock_init(&sfqd->oslock);
	/* INIT_LIST_HEAD(&sfqd->wlist_head); */
	q->elevator->elevator_data = sfqd;
	sfqd->depth = 16;
	sfqd->dispatched = 0;

	return 0;
}

static void pid_sfq_exit_queue(struct elevator_queue *e)
{
	struct sfq_data *sfqd = e->elevator_data;
	//FIXME:Kfree all the nodes in radix tree
	printk("<%s> Exit sfqd\n", __FUNCTION__);
	kfree(sfqd);
}

static struct elevator_type elevator_sfq = {
	.ops = {
		//		.elevator_merge_req_fn		= sfq_merged_reqs,
		.elevator_dispatch_fn		= sfq_dispatch,
		.elevator_add_req_fn		= sfq_add_request,
		//		.elevator_former_req_fn		= sfq_former_request,
		//		.elevator_latter_req_fn		= sfq_latter_request,
		.elevator_set_req_fn		= sfq_set_request, //To set the request property
		.elevator_completed_req_fn	= sfq_completed_request,
		.elevator_put_req_fn		= sfq_put_request,
		.elevator_init_fn		= pid_sfq_init_queue,
		.elevator_exit_fn		= pid_sfq_exit_queue,
	},
	.elevator_name = "sfqd",
	.elevator_owner = THIS_MODULE,
};

static int __init pid_sfq_init(void)
{
	vt = (struct virt *)vmalloc(sizeof(*vt));
	vt->t = 0;
	vt->lazyt = -1;
	spin_lock_init(&vt->lock);
	DPRINTK("sfqd init.\n");
	return elv_register(&elevator_sfq);
}

static void __exit pid_sfq_exit(void)
{
	printk("IN==========%s======\n", __FUNCTION__);
	elv_unregister(&elevator_sfq);
}

module_init(pid_sfq_init);
module_exit(pid_sfq_exit);


MODULE_AUTHOR("Wenji Li");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PID_ZFQ IO scheduler");
