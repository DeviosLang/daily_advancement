#include <linux/module.h>

MODULE_AUTHOR("devioslang");
MODULE_LICENSE("GPL");

static int __init vmx_init(void)
{
    printk(KERN_DEBUG "hello kernel world\n");
    return 0;
}
static void __exit vmx_exit(void)
{
    printk(KERN_DEBUG "bye kernel\n");
}
module_init(vmx_init);
module_exit(vmx_exit);
