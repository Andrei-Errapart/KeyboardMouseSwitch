#include "ident.h"

#include <hal/nrf_gpio.h>
#include <nrfx.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define KMS_VID 0x1209
#define KMS_PID 0x0001
#define RESET_PIN 19

USBD_DEVICE_DEFINE(kms_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), KMS_VID, KMS_PID);
USBD_DESC_LANG_DEFINE(kms_lang);
USBD_DESC_MANUFACTURER_DEFINE(kms_mfr, "KMSwitch");
USBD_DESC_PRODUCT_DEFINE(kms_product, "KMS radio bench");
USBD_DESC_CONFIG_DEFINE(kms_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(kms_fs_config, 0, 50 /* x 2 mA */, &kms_fs_cfg_desc);

/* Filled from FICR before the stack starts; read on every host request. */
static uint8_t sn_ascii[13];
static struct usbd_desc_node kms_sn = {
	.str = {
		.utype = USBD_DUT_STRING_SERIAL_NUMBER,
		.ascii7 = true,
	},
	.ptr = sn_ascii,
	.bLength = USB_STRING_DESCRIPTOR_LENGTH("000000000000"),
	.bDescriptorType = USB_DESC_STRING,
};

void ident_serial(char out[13])
{
	uint16_t hi = (uint16_t)NRF_FICR->DEVICEADDR[1] | 0xC000;
	uint32_t lo = NRF_FICR->DEVICEADDR[0];

	snprintk(out, 13, "%04X%08X", hi, lo);
}

int ident_usb_init(usbd_msg_cb_t cb)
{
	int err;

	ident_serial((char *)sn_ascii);
	err = usbd_add_descriptor(&kms_usbd, &kms_lang);
	err = err ?: usbd_add_descriptor(&kms_usbd, &kms_mfr);
	err = err ?: usbd_add_descriptor(&kms_usbd, &kms_product);
	err = err ?: usbd_add_descriptor(&kms_usbd, &kms_sn);
	err = err ?: usbd_add_configuration(&kms_usbd, USBD_SPEED_FS, &kms_fs_config);
	err = err ?: usbd_register_all_classes(&kms_usbd, USBD_SPEED_FS, 1, NULL);
	/* CDC ACM uses an interface association. */
	err = err ?: usbd_device_set_code_triple(&kms_usbd, USBD_SPEED_FS,
						 USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	err = err ?: usbd_msg_register_cb(&kms_usbd, cb);
	err = err ?: usbd_init(&kms_usbd);
	if (!err && !usbd_can_detect_vbus(&kms_usbd)) {
		err = usbd_enable(&kms_usbd);
	}
	return err;
}

FUNC_NORETURN void ident_reboot_to_bootloader(void)
{
	(void)irq_lock();
	nrf_gpio_cfg_output(RESET_PIN);
	nrf_gpio_pin_clear(RESET_PIN);
	for (;;) {
	}
}
