#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <asm/atomic.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include "rb.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("devios lang");

/* 说明:打印有点儿凶，可能在虚拟机下会导致机器变得很慢 
基本思路就是使用 链表实现了一个循环的buffer*/

/* write在向环中增加data，read从环中读取 */
static int nreaders = 1;
static int nwriters = 1;
static int ring_buffer_size = 10;

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


static struct ring_buffer* ring_buffer_alloc(int size)
{
    struct ring_buffer* rb = NULL;
    rb = kzalloc(sizeof(struct ring_buffer), GFP_KERNEL);
    if (!rb){
        return NULL;
    }
    rb = kzalloc(sizeof(struct ring_buffer), GFP_KERNEL);
    rb->size = size;
    rb->cur_size = 0;
    INIT_LIST_HEAD(&rb->head);
    spin_lock_init(&rb->lock);
    printk(KERN_INFO "haha:alloc rb\n");
    return rb;
}

static int ring_buffer_write(struct ring_buffer* rb, void* data)
{
    struct ring_buffer_node *node;
    node = kzalloc(sizeof(struct ring_buffer_node), GFP_KERNEL);
    if (NULL == node)
    {
         return -1;/*-ENOMEM*/;         
    }
    
    spin_lock(&rb->lock);
    if (rb->cur_size == rb->size)
    {
        spin_unlock(&rb->lock);
	kfree(node);
        return -1; /*full*/
    }
    rb->cur_size++;
    copy_data_to_rb(&node->data, data);
    list_add_tail(&node->node, &rb->head);/*尾部加，从头部取*/
    spin_unlock(&rb->lock);
    return 0;
}

static void *ring_buffer_read(struct ring_buffer* rb)
{
    struct list_head *list_node;
    struct ring_buffer_node *node;
    void *data;
    spin_lock(&rb->lock);
    if (0 == rb->cur_size)
    {
        spin_unlock(&rb->lock);
        return NULL;/*empty*/   
    }
    list_node = rb->head.next;
    list_del(list_node);	
    rb->cur_size--;
	spin_unlock(&rb->lock);
    node = (struct ring_buffer_node*)list_node;
	data = node->data;
    kfree(node);
    return read_data_from_rb(data);   
}

static void ring_buffer_free(struct ring_buffer* rb)
{
    struct list_head *n;
    struct list_head *pos;
    spin_lock(&rb->lock);
    list_for_each_safe(pos, n, &rb->head)
    {
        list_del(pos);
		kfree(pos);
    }
    spin_unlock(&rb->lock);
    kfree(rb);        
}

static int ring_buffer_resize(struct ring_buffer* rb, int new_size)
{
    spin_lock(&rb->lock);
    if(rb->cur_size < new_size){
        rb->size = new_size;
        spin_unlock(&rb->lock);
        return 0;
    }
	spin_unlock(&rb->lock);
    return -1; /*太短了，其他被占用*/
         
}
/*链表-------------------------------------------------end*/

static int rb_writer(void *arg)
{
  
    long long data;
    long long id = ((struct info*)arg)->id;
    id <<= 32;
 
    VERBOSE_PRINTK_STRING("rb_writer task started");
    set_user_nice(current, 19);

    do {
        data = (long long)(((struct info*)arg)->data);
        data &= id;
        ring_buffer_write(g_rb, (void*)data);
    	printk("rb_writer my ID %d write data:%lld\n", ((struct info*)arg)->id, data);
        ((struct info*)arg)->data = (void*)(data + 1);
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

/*
 * Detect and respond to a system shutdown.
 */
static int
rb_shutdown_notify(struct notifier_block *unused1,
			   unsigned long unused2, void *unused3)
{
	mutex_lock(&fullstop_mutex);
	if (fullstop == FULLSTOP_DONTSTOP)
		fullstop = FULLSTOP_SHUTDOWN;
	else
		printk(KERN_WARNING /* but going down anyway, so... */
		       "Concurrent 'rmmod rb' and shutdown illegal!\n");
	mutex_unlock(&fullstop_mutex);
	return NOTIFY_DONE;
}
static struct notifier_block rb_nb = {
	.notifier_call = rb_shutdown_notify,
};

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
	
	unregister_reboot_notifier(&rb_nb);
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
    
    g_rb = ring_buffer_alloc(ring_buffer_size);
    g_writer_info = kzalloc(nwriters * sizeof(struct info), GFP_KERNEL); 
    if (NULL == g_writer_info){
        firsterr = -ENOMEM;
        goto exception; 
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
    for (i = 0; i < nwriters; i++){
        writer_tasks[i] = kthread_run(rb_writer, &g_writer_info[i],
						  "rb_writer");
        
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
    
    }
    register_reboot_notifier(&rb_nb);
	mutex_unlock(&fullstop_mutex);
    return 0;
exception:
	mutex_unlock(&fullstop_mutex);
    ring_buffer_exit();
	return firsterr;
}



module_init(ring_buffer_init);
module_exit(ring_buffer_exit);

