#define pr_fmt(fmt) "[" KBUILD_MODNAME "] %s():%d: " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/timex.h>
#include <linux/cdev.h>

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/genalloc.h>
#include <linux/uaccess.h>

#include <linux/of_dma.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>

#include "zdma.h"
#include "zdma_ioctl.h"
#include "macro.h"

#ifndef CONFIG_OF
#error "OpenFirmware is not configured in kernel\n"
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ioannis Galanommatis");
MODULE_DESCRIPTION("DMA client to xilinx_dma");
MODULE_VERSION("0.3");

//module_param(length, unsigned, S_IRUGO);
extern int xilinx_vdma_channel_set_config(void *, void *);	// FIXME dependency?

enum zdma_system_state {
	SYS_DOWN = 0,
	SYS_INIT,
	SYS_UP,
	SYS_DEINIT,
};

enum zdma_client_state {
	CLIENT_OPENED = 0,
	CLIENT_CONFIGURED,
	CLIENT_READY,
	CLIENT_CLOSING,
	CLIENT_CONFIG_ERROR,
	CLIENT_MMAP_ERROR,
};

enum zdma_core_state {
	CORE_EMPTY = 0,
	CORE_UNLOADED,
	CORE_LOADING,
	CORE_LOADED,
	CORE_UNLOADING,
};


static struct system {
	enum zdma_system_state state;
	atomic_t dmac_count, zone_count, client_count;
	spinlock_t dmacs_lock, cores_lock, zones_lock, mem_lock;

	struct zdma_dmac {
		struct list_head node;
		atomic_t load;
		struct dma_chan *txchanp, *rxchanp;
	} dmacs;

	struct zdma_core {
		struct list_head node;
		char name[16];
		int state;
		struct zdma_dmac avail_dmacs;
	} cores;

	struct zdma_zone {
		struct list_head node;
		struct device dev;
	} zones;

	struct gen_pool *mem;

	struct device *devp;
	dev_t cdev_num;
	struct cdev cdev;
	struct file_operations cdev_fops;
} sys;

struct client {
	struct zdma_core *core;
	struct zdma_dmac *dmac;
	struct dma_channel {
		struct gen_pool_chunk *bank;
		dma_addr_t handle;
		size_t size;
		void *vaddr;
		struct dma_async_tx_descriptor *descp;
		struct dma_slave_config conf;
		dma_cookie_t cookie;
		struct completion cmp;
	} tx, rx;
};


static void sync_callback(void *completion)
{
	// Indicate the DMA transaction completed to allow the other thread to finish processing
	complete(completion);
	return;
}

static int client_dmac_alloc(struct client *p)
{
	if (sys.state != SYS_UP) return -EBUSY;
	BUG_ON(IS_ERR_OR_NULL(p));

	if (p->dmac != NULL) {
		// FIXME wait for pending transfers
		// FIXME channel release?
		atomic_dec(&p->dmac->load);
	}

	int curr, min = INT_MAX;
	struct zdma_dmac *dmac;
	rcu_read_lock();
	list_for_each_entry_rcu(dmac, &sys.dmacs.node, node) {
		curr = atomic_read(&dmac->load);
		if (curr < min) {
			p->dmac = dmac;
			min = curr;
		}
	}
	rcu_read_unlock();

	spin_lock(&sys.dmacs_lock);
	atomic_inc(&p->dmac->load);			// increase usage counter of selected DMAC
	spin_unlock(&sys.dmacs_lock);

	pr_info("reserved DMAC %p with load %d\n", p->dmac, atomic_read(&p->dmac->load));
	return 0;
}


static int client_mem_alloc(struct client *p, size_t tx_size, size_t rx_size)
{
	if (sys.state != SYS_UP) return -EBUSY;

	BUG_ON(IS_ERR_OR_NULL(p));

	if (p->dmac == NULL) client_dmac_alloc(p);

	//if (p->tx.size != 0 && p->rx.size != 0) client_dmac_alloc(p);

	if (p->tx.vaddr) gen_pool_free(sys.mem, (unsigned long)p->tx.vaddr, p->tx.size);
	if (p->rx.vaddr) gen_pool_free(sys.mem, (unsigned long)p->rx.vaddr, p->rx.size);

	if (tx_size == 0 && rx_size == 0) {
		atomic_dec(&p->dmac->load);
		return 0;
	}

	pr_info("request to set DMA buffer size to TX: %zukiB, RX: %zukiB\n", tx_size/Ki, rx_size/Ki);

	if (gen_pool_dma_alloc_pair(sys.mem,
			tx_size, &p->tx.vaddr, &p->tx.handle, rx_size, &p->rx.vaddr, &p->rx.handle)) {
		pr_warn("unable to allocate memory for DMA buffers (TX: %zuKiB, RX: %zuKiB)\n", 
			tx_size/Ki, rx_size/Ki);
		return -ENOMEM;
	}

	// if no error...
	p->tx.size = tx_size;
	p->rx.size = rx_size;

	return 0;
}

static int dma_controller_add(struct device_node *np)
{
	if (sys.state == SYS_UP) return -EBUSY; // will never happen

	struct zdma_dmac *dmac = devm_kzalloc(sys.devp, sizeof(struct zdma_dmac), GFP_KERNEL);
	if (unlikely(dmac == NULL)) {
		pr_err("unable to allocate %zu bytes for DMA-C description structures!\n", 
			sizeof(*dmac));
		return -ENOMEM;
	}
	
	dmac->txchanp = of_dma_request_slave_channel(np, "tx");
	if (IS_ERR_OR_NULL(dmac->txchanp)) {
		pr_err("dmac %p: failed to reserve TX channel -- devicetree misconfiguration, "
			"DMA controller not present or driver not loaded.\n", dmac);
		return -ENODEV;	// FIXME DOES NOT EXIT GRACEFULLY FIXME // (later) TODO why not?
	}
	dmac->rxchanp = of_dma_request_slave_channel(np, "rx");
	if (IS_ERR_OR_NULL(dmac->rxchanp)) {
		pr_err("dmac %p: failed to reserve RX channel -- devicetree misconfiguration, "
			"DMA controller not present or driver not loaded.\n", dmac);
		dma_release_channel(dmac->txchanp);
		return -ENODEV;
	}
	spin_lock(&sys.dmacs_lock);
	list_add_rcu(&dmac->node, &sys.dmacs.node);
	spin_unlock(&sys.dmacs_lock);
	atomic_inc(&sys.dmac_count);
	return 0;
}

static int dma_zone_add(struct device_node *np, phys_addr_t paddr, size_t size)
{
	if (sys.state != SYS_INIT) return -EBUSY;	// this will never happen

	struct zdma_zone *zone = devm_kzalloc(sys.devp, sizeof(struct zdma_zone), GFP_KERNEL);
	if (unlikely(zone == NULL)) {
		pr_err("unable to allocate %zu bytes for zone description\n",
			sizeof(*zone));
		return -ENOMEM;
	}
	
	// create pseudo device structure -- FIXME will be leaked on error!!!
	device_initialize(&zone->dev);
	dev_set_name(&zone->dev, "%s@%p", "zone", zone);
	zone->dev.parent		= sys.devp; // do not inherit bus!
	zone->dev.coherent_dma_mask	= sys.devp->coherent_dma_mask;
	zone->dev.dma_mask		= sys.devp->dma_mask;
	zone->dev.release		= of_reserved_mem_device_release;
	int res = device_add(&zone->dev);
	if (res != 0) {
		pr_err("error %d while registering device for zone %p\n", res, zone);
		return res;
	}

	// seek to desired zone...
	if (of_reserved_mem_device_init_by_idx(&zone->dev, np, 0)) {
		pr_err("error: cannot initialize detected zone reserved memory!\n");
		return -ENOMEM;
	}

	// dmam_alloc_attrs() is implemented in 4.13, port when possible -- FIXME look for release
	dma_addr_t handle;
	void *vaddr = dma_alloc_attrs(&zone->dev, size, &handle, 
		GFP_KERNEL, DMA_ATTR_NO_KERNEL_MAPPING /*TODO check if this is wise in genpool */);
	if (!vaddr) {
		pr_err("error: cannot claim reserved memory of size %zuKiB\n", size/Ki);
		return -ENOMEM;
	}
	if (handle != phys_to_dma(&zone->dev, paddr)) {
		pr_crit("\n"
			"***              ARCHITECTURE INCOMPATIBILITY              ***\n"
			"*** The DMA addresses are different from physical address! ***\n"
			"*** In Xilinx Zynq UltraScale+ (Cortex-A53, ARM64) it may  ***\n"
			"*** have been caused by an IOMMU remapping of DMA space.   ***\n"
			"*** In Xilinx Zynq 7000 (Cortex-A9, ARM32) this should not ***\n"
			"*** really be happening due to the lack of IOMMU hardware. ***\n"
			"*** Current version of this driver is not tested for such  ***\n"
			"*** a configuration. Use at your own risk!!!               ***\n");
	}

	spin_lock(&sys.zones_lock);
	list_add_rcu(&zone->node, &sys.zones.node);
	spin_unlock(&sys.zones_lock);
	atomic_inc(&sys.zone_count);
	
	BUG_ON(IS_ERR_OR_NULL(sys.mem));
	gen_pool_add_virt(sys.mem, (unsigned long)vaddr, handle, size, -1);
	return 0;
}


static int dma_issue(struct client *p)
{
//	cycles_t ta, tb;
//	ta = get_cycles();
	if (sys.state != SYS_UP) return -EAGAIN;

	if ((p->tx.descp = dmaengine_prep_slave_single(p->dmac->txchanp, p->tx.handle, p->tx.size, 
			DMA_MEM_TO_DEV, DMA_CTRL_ACK|DMA_PREP_INTERRUPT)) == NULL) {
		pr_err("dmaengine_prep_slave_single() returned NULL!\n");
		return p->tx.cookie = -EBUSY;
	}
	
	if ((p->rx.descp = dmaengine_prep_slave_single(p->dmac->rxchanp, p->rx.handle, p->rx.size,
			DMA_DEV_TO_MEM, DMA_CTRL_ACK|DMA_PREP_INTERRUPT)) == NULL) {
		pr_err("dmaengine_prep_slave_single() returned NULL!\n");
		return p->rx.cookie = -EBUSY;
	}

	p->tx.descp->callback = p->rx.descp->callback = sync_callback;
	p->tx.descp->callback_param = &p->tx.cmp;
	p->rx.descp->callback_param = &p->rx.cmp;

	p->tx.cookie = dmaengine_submit(p->tx.descp);
	p->rx.cookie = dmaengine_submit(p->rx.descp);

	if (dma_submit_error(p->tx.cookie) || dma_submit_error(p->rx.cookie)) {
		pr_err("cookie contains error!\n");
		return -EIO;
	}
	
	// initialize the completion before using it and then start the DMA transaction
	// which was previously queued up in the DMA engine
	init_completion(&p->tx.cmp);
	init_completion(&p->rx.cmp);

	//

	unsigned long timeout = 3000;
	enum dma_status status;
//	cycles_t t0, t1;

	dma_async_issue_pending(p->dmac->txchanp);
	dma_async_issue_pending(p->dmac->rxchanp);

//	t0 = get_cycles();
	timeout = wait_for_completion_timeout(&p->tx.cmp, msecs_to_jiffies(timeout));
	timeout = wait_for_completion_timeout(&p->rx.cmp, msecs_to_jiffies(timeout));
//	t1 = get_cycles();

	status = dma_async_is_tx_complete(p->dmac->txchanp, p->tx.cookie, NULL, NULL);

	// determine if the transaction completed without a timeout and withtout any errors
	if (timeout == 0) {
		pr_crit("*** DMA OPERATION TIMED OUT ***\n");
		return -ETIMEDOUT;
	} 
	if (status != DMA_COMPLETE) {
		pr_err("DMA returned completion callback status of: %s\n", 
			status == DMA_ERROR ? "error" : "in progress");
		return -EIO;
	}
/*	tb = get_cycles();
	if (t1-t0) pr_info("DMA size: %4zu->%4zu kiB, xfer time: %lu, total: %lu\n", 
		p->tx.size/Ki, p->rx.size/Ki, t1-t0, tb-ta);
	else pr_warn("this kernel does not support get_cycles()\n");
*/
	return 0;
}


static int dev_open(struct inode *inodep, struct file *filep)
{
	struct client *p;
	if ((p = filep->private_data = devm_kzalloc(sys.devp, sizeof(struct client), GFP_KERNEL)) == NULL) {
		pr_err("error allocating memory for client private data!\n");
		return -ENOMEM;
	}
	atomic_inc(&sys.client_count);
	return 0;
}


static int dev_release(struct inode *inodep, struct file *filep)
{
	client_mem_alloc(filep->private_data, 0, 0);
	devm_kfree(sys.devp, filep->private_data);
	atomic_dec(&sys.client_count);
	return 0;
}


static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	if (sys.state != SYS_UP) return -EAGAIN;
	struct client *p = filep->private_data;
	struct dma_config conf;

	switch (cmd) {
	case ZDMA_IOCTL_DEBUG:
		break;
	case ZDMA_IOCTL_CONFIG:
		if (copy_from_user(&conf, (void __user *)arg, sizeof(conf))) {
			pr_err("could not read all configuration data, ioctl ignored\n");
			return -EIO;
		}
		if (client_mem_alloc(p, conf.tx_size, conf.rx_size)) {
			pr_err("error allocating new buffers\n");
			return -ENOMEM;
		}
		p->tx.size = conf.tx_size;
		p->rx.size = conf.rx_size;
		// find core!
		//p->conf.flags = conf.flags;
		break;	
	case ZDMA_IOCTL_ENQUEUE:
		return dma_issue(p);
		break;
	default:
		pr_err("unknown ioctl: command %x, argument %lu\n", cmd, arg);
		return -ENOTTY;
	}
	return 0l;
}


static int dev_mmap(struct file *filep, struct vm_area_struct *vma)
{
	if (sys.state != SYS_UP) {
		pr_warn("system is not yet up.\n");
		return -EAGAIN;
	}

	struct client *p = filep->private_data;
	if ((vma->vm_flags & VM_READ) == (vma->vm_flags & VM_WRITE)) {
		pr_warn("invalid protection flags -- please use MAP_WRITE for TX or MAP_READ for RX\n");
		return -EINVAL;
	}
	
	struct dma_channel *chan = (vma->vm_flags & VM_WRITE) ? &p->tx : &p->rx;

	phys_addr_t
		off = vma->vm_pgoff << PAGE_SHIFT,
		paddr = dma_to_phys(sys.devp, chan->handle),
		vsize = vma->vm_end - vma->vm_start,
		psize = chan->size - off;
	
	if (chan->size == 0) {
		pr_warn("attempt to map a zero length channel buffer\n");
		return -EINVAL;
	}

	if (vsize != ALIGN(psize, PAGE_SIZE)) {
		pr_crit("internal error: virtual and physical size calculations do not match!\n");
		return -EINVAL;
	}

	if (!chan->handle) {
		pr_warn("Internal error: DMA buffer has not yet been allocated!\n.");
		return -ENOMEM;
	}

	if (dma_mmap_attrs(sys.devp, vma, p->tx.vaddr, p->tx.handle, vsize, DMA_ATTR_NO_KERNEL_MAPPING) < 0) {
		pr_warn("failed to map user buffer!\n");
		return -ENOMEM;
	}

	pr_info("%s: phys 0x%8p mapped at 0x%8p, size=%zuKiB\n", 
		(vma->vm_flags & VM_WRITE) ? "TX" : "RX", 
		(void *)paddr,
		(vma->vm_flags & VM_WRITE) ? p->tx.vaddr : p->rx.vaddr,
		(size_t)psize/Ki);

	return 0;
}


static int zdma_remove(struct platform_device *pdev)
{
	sys.state = SYS_DEINIT;
	pr_info("zdma shutting down...\n");
//	dev_set_drvdata(&pdev->dev, NULL);	//TODO understand what this does
	
//	while (hw.dmac_count--) {
//		pr_info("releasing dmac[%d]\n", hw.dmac_count);
//		dma_release_channel(hw.dmac[hw.dmac_count].rxchanp);
//		dma_release_channel(hw.dmac[hw.dmac_count].txchanp);
//	}
//	for (int i = 0; i < INDEX_RANGE; ++i) {
//		if (driver.zone_devp[i] == NULL) continue;
//		pr_info("releasing zone[%d]\n", i);
//		device_del(driver.zone_devp[i]);
//		put_device(driver.zone_devp[i]);
//	}
	gen_pool_destroy(sys.mem);
	sys.state = SYS_DOWN;
	return 0;
}


static int zdma_probe(struct platform_device *pdev)
{
	int res;
//	while (atomic_read(&driver.state) == SHUTTING_DOWN); //wait to finish shutdown

	if (sys.state == SYS_UP) {
		pr_crit("\n"
			"***   An unexpected attempt was made to initialize an already running system.  ***\n"
			"***              This may have happened due to several reasons:                ***\n"
			"***   1. Device-tree misconfiguration, i.e. multiple zdma instance entries.    ***\n"
			"***   2. Device-tree corruption at run-time from userland actors.              ***\n"
			"***   3. An improper or incomplete module removal and re-insertion after a     ***\n"
			"***    kernel bug or hardware failure. This should not have happened.          ***\n"
			"***   The request will be ignored but the system may be in an unstable state   ***\n");
		return -EBUSY;
	}

	sys.devp = &pdev->dev;

	// basic data structure initialization

	INIT_LIST_HEAD(&sys.dmacs.node);
	INIT_LIST_HEAD(&sys.cores.node);
	INIT_LIST_HEAD(&sys.zones.node);
	atomic_set(&sys.dmac_count, 0);
	atomic_set(&sys.zone_count, 0);
	atomic_set(&sys.client_count, 0);
	sys.mem = devm_gen_pool_create(sys.devp, PAGE_SHIFT, NUMA_NO_NODE, NULL);
	gen_pool_set_algo(sys.mem, gen_pool_first_fit, NULL);
	gen_pool_set_chunk_algo(sys.mem, gen_chunk_least_used);

	// find dma clients in device tree
	struct device_node *np = NULL, *tnp = NULL;

	for_each_compatible_node(np, NULL, "tuc,dma-client") {
		res = dma_controller_add(np);
		if (res) return res;
	}
	if (atomic_read(&sys.dmac_count) == 0) {
		pr_err("devicetree: no DMA controllers were detected\n");
		return -ENODEV;
	}
	
	// discover memory region info
	np = NULL;
	for_each_compatible_node(np, NULL, "tuc,zone") {
		tnp = of_parse_phandle(np, "memory-region", 0);
		if (!tnp) {
			pr_err("devicetree: memory region %d does not contain a phandle to a memory bank\n",
				atomic_read(&sys.zone_count));
			return -ENODEV;
		}
		u64 size;
		phys_addr_t paddr = of_translate_address(tnp, of_get_address(tnp, 0, &size, NULL));
		res = dma_zone_add(np, paddr, (size_t)size);
		if (res) return res;
	}
	
	if (atomic_read(&sys.zone_count) == 0) {
		pr_err("devicetree: no suitable memory zones are defined!\n");
		return -ENODEV;
	}
	
	pr_info("devicetree: found %d DMA controller(s) and %d memory zone(s)\n", 
		atomic_read(&sys.dmac_count), atomic_read(&sys.zone_count));

	sys.state = SYS_UP;
	return 0;
}

static struct of_device_id zdma_of_match[] = {
	{ .compatible = "tuc,zdma", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, zdma_of_match);


static struct platform_driver zdma_driver = {
	.driver = {
		.name = "zdma",
		.owner = THIS_MODULE,
		.of_match_table = zdma_of_match,
	},
	.probe = zdma_probe,
	.remove = zdma_remove,
};


static void __exit mod_exit(void)
{
	platform_driver_unregister(&zdma_driver);
	cdev_del(&sys.cdev);
	unregister_chrdev_region(sys.cdev_num, 1 /* count */);
}


static int __init mod_init(void)
{
	sys.state = SYS_INIT;

	spin_lock_init(&sys.dmacs_lock);
	spin_lock_init(&sys.zones_lock);
	spin_lock_init(&sys.cores_lock);
	
	// create character device
	sys.cdev_fops.owner = THIS_MODULE;
	sys.cdev_fops.open = dev_open;
	sys.cdev_fops.release = dev_release;
	sys.cdev_fops.mmap = dev_mmap;
	sys.cdev_fops.unlocked_ioctl = dev_ioctl;

	if (alloc_chrdev_region(&sys.cdev_num, 0 /* first minor */, 1 /* count */, KBUILD_MODNAME) < 0) {
		pr_err("error registering /dev entry\n");
		return -ENOSPC;
	}
	
	cdev_init(&sys.cdev, &sys.cdev_fops);
	if (cdev_add(&sys.cdev, sys.cdev_num, 1 /* count */)) {
		pr_err("error registering character device\n");
		return -ENOSPC;
	}

	// register the driver to the kernel
	if (platform_driver_register(&zdma_driver)) {
		pr_err("error registering zdma platform driver\n");
		return -1;
	}

	return 0;
}


module_init(mod_init);
module_exit(mod_exit);
