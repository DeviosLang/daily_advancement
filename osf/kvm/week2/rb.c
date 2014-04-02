#include <linux/module.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include "rb.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("devios lang");


static int nreaders = 1;
static int nwriters = 1;

static struct task_struct **writer_tasks;
static struct task_struct **reader_tasks;
struct ring_buffer *g_rb = NULL;
struct info *g_writer_info = NULL;
int *g_reader_id = NULL;
static int verbose = 1; /* Print more debug info. */

module_param(nreaders, int, 0444);
MODULE_PARM_DESC(nreaders, "Number of ring buffer reader threads");

module_param(nwriters, int, 0444);
MODULE_PARM_DESC(nwriters, "Number of ring buffer writer threads");

module_param(verbose, int, 0444);
MODULE_PARM_DESC(verbose, "Enable verbose debugging printk()s");

#define VERBOSE_PRINTK_STRING(s) \
        do { if (verbose) printk(KERN_ALERT "haha:%s\n", s); } while (0)
#define VERBOSE_PRINTK_ERRSTRING(s) \
        do { if (verbose) printk(KERN_ALERT "haha:%s!!!\n", s); } while (0)


#define FULLSTOP_DONTSTOP 0	/* Normal operation. */
#define FULLSTOP_SHUTDOWN 1	/* System shutdown with rcutorture running. */
#define FULLSTOP_RMMOD    2	/* Normal rmmod of rcutorture. */
static int fullstop = FULLSTOP_RMMOD;
DEFINE_MUTEX(fullstop_mutex);	/* Protect fullstop transitions and spawning */

static void copy_data_to_rb(void** to, void* from)
{
    *to = from;
}

static void* read_data_from_rb(void* data)
{
    return data;
}

static void free_data(void* data)
{   
    /* 无需free 因为使用的就是指针的字面值 */
	return;
}

static int ring_buffer_write(struct ring_buffer* rb, void* data)
{
    struct ring_buffer_node *node;

	int count = atomic_inc_return(&rb->lock);
	if (1 != count)
	{
		atomic_dec(&rb->lock);
		return 0;
	}
	
    node = kzalloc(sizeof(struct ring_buffer_node), GFP_KERNEL);
    if (NULL == node)
    {
         atomic_dec(&rb->lock);
         return -1;/*-ENOMEM*/;         
    }

    copy_data_to_rb(&node->data, data);
    list_add_tail(&node->node, &rb->head);
    atomic_dec(&rb->lock);
    return 0;
}

static void *ring_buffer_read(struct ring_buffer* rb)
{
    struct list_head *list_node;
    struct ring_buffer_node *node;
    void *data;
	
    int count = atomic_inc_return(&rb->lock);
	if (1 != count)
	{
		atomic_dec(&rb->lock);
		return NULL;
	}
	
    list_node = rb->head.next;
    list_del(list_node);	
    atomic_dec(&rb->lock);
    node = (struct ring_buffer_node*)list_node;
    node = list_entry(list_node, struct ring_buffer_node, node);
	data = node->data;
    kfree(node);
    return read_data_from_rb(data);   
}

static void ring_buffer_free(struct ring_buffer* rb)
{
    struct list_head *n;
    struct list_head *pos;
    struct ring_buffer_node *node;
    for(;;)
    {
        /* 这个只会在 exit模块时候调用，那么线程应该很快的被停止 */
		int count = atomic_inc_return(&rb->lock);
		if (1 != count)
		{
			atomic_dec(&rb->lock);
			msleep(1);   /* 在这里 休眠，不晓得是否合理，这是在rmod时候才会走到*/
			continue;
		}
		break;
	}
    list_for_each_safe(pos, n, &rb->head)
    {
        list_del(pos);
        node = list_entry(pos, struct ring_buffer_node, node);
        free_data(node->data);
		kfree(pos);
    }
    kfree(rb);        
}

static int rb_writer(void *arg)
{    
    long long id = ((struct info*)arg)->id;
    long long data;
    id <<= 32;
    
    VERBOSE_PRINTK_STRING("rb_writer task started");
    set_user_nice(current, 19);

    do {
        data = (long long)(((struct info*)arg)->data);
        data |= id;
        ring_buffer_write(g_rb, (void*)data);
    	printk("rb_writer my ID %d write data:%lld\n", ((struct info*)arg)->id, data);
    } while (!kthread_should_stop() && fullstop == FULLSTOP_DONTSTOP);
	
    VERBOSE_PRINTK_STRING("rb_writer task stopping");
    while (!kthread_should_stop())
        schedule_timeout_uninterruptible(1);
    return 0;    
}

static int rb_reader(void *arg)
{
    void *data;
    VERBOSE_PRINTK_STRING("rcu_torture_reader task started");
    set_user_nice(current, 19);
    do {
        data = ring_buffer_read(g_rb);
    	printk("rb_reader my ID %d my data:%lld\n", *((int*)arg), (long long)data);
    } while (!kthread_should_stop() && fullstop == FULLSTOP_DONTSTOP);

    VERBOSE_PRINTK_STRING("rcu_torture_reader task stopping");
    while (!kthread_should_stop())
         schedule_timeout_uninterruptible(1);
    return 0;
}


/*static struct notifier_block rb_nb = {
	.notifier_call = rb_shutdown_notify,
};*/

static void 
ring_buffer_exit(void)
{
    int i;
    mutex_lock(&fullstop_mutex);
	if (fullstop == FULLSTOP_SHUTDOWN) {
		printk(KERN_WARNING /* but going down anyway, so... */
		       "Concurrent 'rmmod rbtorture' and shutdown illegal!\n");
		mutex_unlock(&fullstop_mutex);
		
		return;
	}

	fullstop = FULLSTOP_RMMOD;
	mutex_unlock(&fullstop_mutex);
	
	if (writer_tasks){
        for (i = 0; i < nwriters; i++) {
        	if (writer_tasks) {
				VERBOSE_PRINTK_STRING("Stopping rb_writer task");
				kthread_stop(writer_tasks[i]);
			} 
			writer_tasks[i] = NULL;
		}
		kfree(writer_tasks);
		writer_tasks = NULL;
	}

	if (reader_tasks){
        for (i = 0; i < nreaders; i++) {
        	if (reader_tasks[i]) {
				VERBOSE_PRINTK_STRING(
					"Stopping rb_reader task");
				kthread_stop(reader_tasks[i]);
			} 
			reader_tasks[i] = NULL;
		}
		kfree(reader_tasks);
		reader_tasks = NULL;
	}
	ring_buffer_free(g_rb);
    kfree(g_writer_info);
    kfree(g_reader_id); 
	g_rb = NULL;
}


static int __init
ring_buffer_init(void)
{
    int i;
    int firsterr = 0;
    BUG_ON(nreaders<0);
    BUG_ON(nwriters<0);
    mutex_lock(&fullstop_mutex);
	fullstop = FULLSTOP_DONTSTOP;
    g_rb = kzalloc(nwriters * sizeof(struct ring_buffer), GFP_KERNEL);
    if (NULL == g_rb){
        firsterr = -ENOMEM;
        goto exception;
    }

    g_writer_info = kzalloc(nwriters * sizeof(struct info), GFP_KERNEL); 
    if (NULL == g_writer_info){
        firsterr = -ENOMEM;
        goto exception; 
    }

    for (i = 0; i < nwriters; ++i)
    {
		g_writer_info[i].id = i;
	}
	
    writer_tasks = kzalloc(nwriters * sizeof(writer_tasks[0]),
				   GFP_KERNEL);
    if (NULL == writer_tasks){
        firsterr = -ENOMEM;
        goto exception;
    }
    g_reader_id = kzalloc(nreaders * sizeof(int), GFP_KERNEL);
    if (NULL == g_reader_id){
        firsterr = -ENOMEM;
        goto exception;
    }

    for (i = 0; i < nreaders; ++i)
    {
		g_reader_id[i] = i;
	}
	
    for (i = 0; i < nwriters; i++){
        writer_tasks[i] = kthread_run(rb_writer, &g_writer_info[i],
						  "rb_writer");
		if(NULL == writer_tasks[i])
		{
		   firsterr = -1;
			goto exception;
		}
        
    }
    
    reader_tasks = kzalloc(nwriters * sizeof(reader_tasks[0]),
				   GFP_KERNEL);
    if (NULL == reader_tasks){
        firsterr = -ENOMEM;
        goto exception;
    }

	
    for (i = 0; i < nreaders; i++){
        reader_tasks[i] = kthread_run(rb_reader, &g_reader_id[i],
                          "rb_reader");
		if(NULL == reader_tasks[i])
		{
		   firsterr = -1;
			goto exception;
		}
    
    }
    /*register_reboot_notifier(&rb_nb);*/
	mutex_unlock(&fullstop_mutex);
    return 0;
exception:
	mutex_unlock(&fullstop_mutex);
    ring_buffer_exit();
	return firsterr;
}



module_init(ring_buffer_init);
module_exit(ring_buffer_exit);

