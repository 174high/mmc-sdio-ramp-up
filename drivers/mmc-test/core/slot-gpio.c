#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mmc/host.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "slot-gpio.h"

struct mmc_gpio {
        struct gpio_desc *ro_gpio;
        struct gpio_desc *cd_gpio;
        bool override_ro_active_level;
        bool override_cd_active_level;
        irqreturn_t (*cd_gpio_isr)(int irq, void *dev_id);
        char *ro_label;
        u32 cd_debounce_delay_ms;
        char cd_label[];
};

int mmc_gpio_alloc(struct mmc_host *host)
{
        size_t len = strlen(dev_name(host->parent)) + 4;
        struct mmc_gpio *ctx = devm_kzalloc(host->parent,
                                sizeof(*ctx) + 2 * len, GFP_KERNEL);

        if (ctx) {
                ctx->ro_label = ctx->cd_label + len;
                ctx->cd_debounce_delay_ms = 200;
                snprintf(ctx->cd_label, len, "%s cd", dev_name(host->parent));
                snprintf(ctx->ro_label, len, "%s ro", dev_name(host->parent));
                host->slot.handler_priv = ctx;
                host->slot.cd_irq = -EINVAL;
        }

        return ctx ? 0 : -ENOMEM;
}

