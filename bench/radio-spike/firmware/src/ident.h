/* Identity, USB device setup and the reboot into the bootloader. */
#pragma once
#include <zephyr/usb/usbd.h>

/* The bootloader's serial: "%04X%08X" of DEVICEADDR[1] | 0xC000 and
 * DEVICEADDR[0] (nRF5 SDK app_usbd_serial_num.c). */
void ident_serial(char out[13]);
int ident_usb_init(usbd_msg_cb_t cb);
/* Drive P0.19 low: on the PCA10059 it is wired to RESET, and the Open DFU
 * bootloader enters DFU after a pin reset. Does not return. */
FUNC_NORETURN void ident_reboot_to_bootloader(void);
