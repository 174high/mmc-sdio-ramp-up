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
	{ 0x12da5bb2, "__kmalloc" },
	{ 0x97255bdf, "strlen" },
	{ 0xf5ce67f5, "of_alias_get_id" },
	{ 0xefd6cf06, "__aeabi_unwind_cpp_pr0" },
	{ 0xb81960ca, "snprintf" },
	{ 0xc2a469e2, "devm_kmalloc" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "C6A34D46378C961C9E2650A");
