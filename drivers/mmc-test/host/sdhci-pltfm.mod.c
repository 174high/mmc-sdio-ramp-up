#include <linux/build-salt.h>
#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.arch = MODULE_ARCH_INIT,
};

MODULE_INFO(intree, "Y");

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0x4738e17d, "module_layout" },
	{ 0x3d903289, "sdhci_alloc_host" },
	{ 0x2d80be8a, "devm_ioremap_resource" },
	{ 0x16fda893, "sdhci_set_clock" },
	{ 0xdaa06000, "platform_get_resource" },
	{ 0xac6fc67, "_dev_err" },
	{ 0x89337d0b, "sdhci_reset" },
	{ 0x5d00219f, "sdhci_set_uhs_signaling" },
	{ 0xefd6cf06, "__aeabi_unwind_cpp_pr0" },
	{ 0x58bb00d6, "platform_get_irq" },
	{ 0x99e1e149, "sdhci_set_bus_width" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=sdhci";


MODULE_INFO(srcversion, "53306F616408E42505F7E10");
