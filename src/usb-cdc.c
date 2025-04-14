#include <hardware/watchdog.h>
#include <pico/bootrom.h>
#include <stdlib.h>
#include <tusb.h>

#include "cec-log.h"
#include "hdmi-cec.h"
#include "hdmi-ddc.h"
#include "nvs.h"
#include "tclie.h"
#include "usb-cdc.h"
#include "blink.h"
#include "ws2812.h"
#include "class/cdc/cdc_host.h"

#ifndef PICO_CEC_VERSION
#define PICO_CEC_VERSION "unknown"
#endif
#include "bsp/board_api.h"

static const char *messages[] = {
  [0x0A] = "remote gain\n",
  [0x13] = "remote phase\n",
  [0x28] = "remote ok\n",
  [0x45] = "remote back\n",
  [0x4F] = "remote menu\n",
  [0x51] = "remote down\n",
  [0x52] = "remote up\n",
};

static const char *power_messages[] = {
  "remote pwr\n",
  "pwr on\n",
};

void usb_device_task(void *param) {
  (void)param;
  
  tusb_rhport_init_t host_init = {
    .role = TUSB_ROLE_HOST,
    .speed = TUSB_SPEED_AUTO
  };
  tusb_init(BOARD_TUH_RHPORT, &host_init);

  if (board_init_after_tusb) {
    board_init_after_tusb();
  }

  // RTOS forever loop
  while (1) {
    tuh_task();
  }
}

void cdc_task(void *param) {
  QueueHandle_t *q = (QueueHandle_t *)param;
  static int state = 1;

  while (1) {
    // Poll every 10ms
    uint8_t key;
    BaseType_t r = xQueueReceive(*q, &key, pdMS_TO_TICKS(10));

    if (r == pdTRUE) {
      for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++) {
        if (tuh_cdc_mounted(idx)) {
          if (key == 0x50) {
            tuh_cdc_write(0, power_messages[state], strlen(power_messages[state]));
            tuh_cdc_write_flush(0);
            state = !state;
          } else if (key < sizeof(messages) / sizeof(messages[0]) && messages[key] != NULL) {
            tuh_cdc_write(0, messages[key], strlen(messages[key]));
            tuh_cdc_write_flush(0);
          }
        }
      }
    }
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) dev_addr;
  (void) instance;
  (void) report;
  (void) len;
}

void tuh_mount_cb(uint8_t dev_addr) {
  // application set-up
  printf("A device with address %u is mounted\r\n", dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr) {
  // application tear-down
  printf("A device with address %u is unmounted \r\n", dev_addr);
}

// Invoked when a device with CDC interface is mounted
// idx is index of cdc interface in the internal pool.
void tuh_cdc_mount_cb(uint8_t idx) {
  tuh_itf_info_t itf_info = {0};
  tuh_cdc_itf_get_info(idx, &itf_info);

  printf("CDC Interface is mounted: address = %u, itf_num = %u\r\n", itf_info.daddr,
         itf_info.desc.bInterfaceNumber);

#ifdef CFG_TUH_CDC_LINE_CODING_ON_ENUM
  // If CFG_TUH_CDC_LINE_CODING_ON_ENUM is defined, line coding will be set by tinyusb stack
  // while eneumerating new cdc device
  cdc_line_coding_t line_coding = {0};
  if (tuh_cdc_get_local_line_coding(idx, &line_coding)) {
    printf("  Baudrate: %" PRIu32 ", Stop Bits : %u\r\n", line_coding.bit_rate, line_coding.stop_bits);
    printf("  Parity  : %u, Data Width: %u\r\n", line_coding.parity, line_coding.data_bits);
  }
#else
  // Set Line Coding upon mounted
  cdc_line_coding_t new_line_coding = { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 };
  tuh_cdc_set_line_coding(idx, &new_line_coding, NULL, 0);
#endif
}

// Invoked when a device with CDC interface is unmounted
void tuh_cdc_umount_cb(uint8_t idx) {
  tuh_itf_info_t itf_info = {0};
  tuh_cdc_itf_get_info(idx, &itf_info);

  printf("CDC Interface is unmounted: address = %u, itf_num = %u\r\n", itf_info.daddr,
         itf_info.desc.bInterfaceNumber);
}