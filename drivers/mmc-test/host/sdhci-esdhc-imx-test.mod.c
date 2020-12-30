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
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
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
	{ 0xf386a92d, "platform_driver_unregister" },
	{ 0x75b25b69, "__platform_driver_register" },
	{ 0x34a32d8e, "_dev_warn" },
	{ 0x822137e2, "arm_heavy_mb" },
	{ 0xa56d662d, "pinctrl_lookup_state" },
	{ 0x989cd66c, "devm_pinctrl_get" },
	{ 0x13bec41d, "release_bus_freq" },
	{ 0x8570f468, "pm_qos_add_request" },
	{ 0x3f59d6dd, "request_bus_freq" },
	{ 0x4df391d4, "pm_qos_remove_request" },
	{ 0xb077e70a, "clk_unprepare" },
	{ 0xb6e6d99d, "clk_disable" },
	{ 0x815588a6, "clk_enable" },
	{ 0x6e044df, "sdhci_pltfm_free" },
	{ 0x7c9a7371, "clk_prepare" },
	{ 0x556e4390, "clk_get_rate" },
	{ 0xbec8cd13, "devm_clk_get" },
	{ 0x3075a1e, "sdhci_pltfm_init" },
	{ 0x7c32d0f0, "printk" },
	{ 0x75bb957a, "of_match_device" },
	{ 0xefd6cf06, "__aeabi_unwind_cpp_pr0" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=sdhci-pltfm";

MODULE_ALIAS("of:N*T*Cfsl,imx25-esdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx25-esdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx35-esdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx35-esdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx51-esdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx51-esdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx53-esdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx53-esdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx6sx-usdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx6sx-usdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx6sl-usdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx6sl-usdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx6q-usdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx6q-usdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx6ull-usdhc-test");
MODULE_ALIAS("of:N*T*Cfsl,imx6ull-usdhc-testC*");
MODULE_ALIAS("of:N*T*Cfsl,imx7d-usdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx7d-usdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx7ulp-usdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx7ulp-usdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx8qxp-usdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx8qxp-usdhcC*");
MODULE_ALIAS("of:N*T*Cfsl,imx8mm-usdhc");
MODULE_ALIAS("of:N*T*Cfsl,imx8mm-usdhcC*");
MODULE_ALIAS("platform:sdhci-esdhc-imx25");
MODULE_ALIAS("platform:sdhci-esdhc-imx35");
MODULE_ALIAS("platform:sdhci-esdhc-imx51");

MODULE_INFO(srcversion, "158D129B015B317B89C64DD");
