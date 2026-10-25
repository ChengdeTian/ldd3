#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/init.h>

/*
 * 匹配的设备：
 *   8086:2413  LDD3 原例的 Intel 82801AA IDE 控制器（i440fx 平台）
 *   8086:100e  QEMU 的 e1000 网卡（qemu_start.sh 里已加 -device e1000）
 *
 * 第二个 ID 是为了在 aarch64 virt 机器上能真正触发 probe：
 * virt 没有 8086:2413，只能借这块测试网卡。
 */
static struct pci_device_id pci_skel_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AA_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x100e) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, pci_skel_ids); /* 只为 modprobe 自动加载提供 alias，不创建设备 */

static unsigned char skel_get_revision(struct pci_dev *dev)
{
	u8 revision;

	pci_read_config_byte(dev, PCI_REVISION_ID, &revision);
	return revision;
}

static int pci_skel_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	int ret;

	ret = pci_enable_device(dev);
	if (ret) {
		dev_err(&dev->dev, "pci_skel: pci_enable_device failed (%d)\n", ret);
		return ret;
	}

    printk(KERN_ALERT "pci_skel: revision=%02x\n", skel_get_revision(dev));
	// /* LDD3 原例：revision 0x42 视为不支持的板子 */
	// if (skel_get_revision(dev) == 0x42) {
	// 	pci_disable_device(dev);
	// 	return -ENODEV;
	// }

	dev_info(&dev->dev, "pci_skel: probe %04x:%04x rev %02x\n",
		 dev->vendor, dev->device, skel_get_revision(dev));
	return 0;
}

static void pci_skel_remove(struct pci_dev *dev)
{
	dev_info(&dev->dev, "pci_skel: remove\n");
	pci_disable_device(dev);
}

static struct pci_driver pci_skel_driver = {
	.name     = "pci_skel",
	.id_table = pci_skel_ids,
	.probe    = pci_skel_probe,
	.remove   = pci_skel_remove,   /* Linux 6.12 里返回 void */
};

/*
 * module_pci_driver() 等价于：
 *   module_init   → pci_register_driver()
 *   module_exit   → pci_unregister_driver()（对每个已绑定设备调用 .remove）
 */
module_pci_driver(pci_skel_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LDD3 chapter-12 PCI skeleton driver");
