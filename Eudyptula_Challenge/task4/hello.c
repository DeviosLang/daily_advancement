#include <linux/module.h>
MODULE_AUTHOR("devioslang");
MODULE_DESCRIPTION("code style test");
MODULE_LICENSE("GPL");

static int __init vmx_init(void)
{
	pr_debug("hello kernel world\n");
	return 0;
}

static void __exit vmx_exit(void)
{
	pr_debug("bye kernel\n");
}
module_init(vmx_init);
module_exit(vmx_exit);
