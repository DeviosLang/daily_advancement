#ifndef RB_H 
#define RB_H
struct ring_buffer{
    struct list_head head;
    int size;
    int cur_size;
    spinlock_t lock;
};
struct ring_buffer_node{
    struct list_head node;
    void* data;
};

struct info{
    int id;
    void *data;
};
#endif
