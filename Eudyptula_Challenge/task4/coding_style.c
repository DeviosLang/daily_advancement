#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>

MODULE_AUTHOR("dump shell");
MODULE_DESCRIPTION("code style test");
MODULE_LICENSE("GPL");

static int do_work(int *my_int)
{
	int x, *y = *my_int;

	for (x = 0; x < y; ++x)
		udelay(10);

	/*
	 * That was a long sleep, tell userspace about it
	 */
	if (y < 10)
		pr_info("We slept a long time!");

	*my_int = x * y;
	return 0;
}


static init __init my_init(void)
{
	int r;
	int x = 10;

	return do_work(&x);
}

static void __exit my_exit(void)
{

}

module_init(my_init);
module_exit(my_exit);
