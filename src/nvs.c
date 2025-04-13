#include <stdio.h>
#include <string.h>

#include <hardware/flash.h>
#include <hardware/sync.h>

#include "crc/crc32.h"

#include "cec-config.h"
#include "nvs.h"

/**
 * Configuration header block, fixed size (40 bytes).
 *
 * Structure is packed to ensure checksum correctness.
 */
typedef struct __attribute__((packed)) {
  /** Version on the configuration body. */
  uint8_t version;
  /** Length, in bytes, of the configuration body. */
  uint32_t length;
} cec_config_header_nvs_t;

/**
 * CEC configuration block NVS representation (version 1)
 *
 * Structure is packed to ensure checksum correctness.
 */
typedef struct __attribute__((packed)) {
  /** DDC EDID delay in milliseconds. */
  uint32_t edid_delay_ms;

  /** CEC physical address. */
  uint16_t physical_address;

  /** User Control key mapping table. */
  uint8_t keymap[UINT8_MAX];
} cec_config_nvs_v1_t;

/**
 * CEC configuration block NVS representation.
 *
 * Structure is packed to ensure checksum correctness.
 */
typedef struct __attribute__((packed)) {
  /** DDC EDID delay in milliseconds. */
  uint32_t edid_delay_ms;

  /** CEC physical address. */
  uint16_t physical_address;

  /** CEC logical address (unused). */
  uint8_t logical_address;

  /** CEC device type (unused). */
  uint8_t device_type;

  /** Keymap. */
  cec_config_keymap_t keymap_type;

  /** User Control key mapping table. */
  uint8_t keymap[UINT8_MAX];
} cec_config_nvs_t;

/**
 * Serialised at-rest format.
 *
 * Structure is aligned to comply with requirement for page sized flash writes.
 */
typedef struct __attribute__((aligned(FLASH_PAGE_SIZE))) {
  /** Header. */
  cec_config_header_nvs_t header;

  /** CRC32 of the header block. */
  uint32_t header_crc;

  /** Configuration. */
  cec_config_nvs_t config;

  /** CRC32 of the config block. */
  uint32_t config_crc;
} pico_cec_nvs_t;

// Symbols resolved from link script
extern uint32_t CEC_NVS_BASE_ADDR[];
extern uint32_t __CEC_NVS_LEN[];

#define CEC_NVS_LEN ((uint32_t)(&__CEC_NVS_LEN))

const uint8_t CEC_CONFIG_VERSION_01 = 0x01;
const uint8_t CEC_CONFIG_VERSION = 0x02;
const size_t CEC_CONFIG_SIZE = sizeof(cec_config_t);

static uint32_t nvs_get_flash_address(void) {
  return ((uint32_t)CEC_NVS_BASE_ADDR - XIP_BASE);
}

/**
 * Migrate v1 config to current config.
 */
static bool migrate_v1(const pico_cec_nvs_t *nvs, cec_config_t *config) {
  if (crc32((unsigned char *)&nvs->config, sizeof(cec_config_nvs_v1_t)) == nvs->config_crc) {
    cec_config_nvs_v1_t *configv1 = (cec_config_nvs_v1_t *)&nvs->config;
    // deserialise and migrate
    config->edid_delay_ms = configv1->edid_delay_ms;
    config->physical_address = configv1->physical_address;
    for (uint8_t n = 0; n < UINT8_MAX; n++) {
      config->keymap[n].key = configv1->keymap[n];
    }

    return true;
  }

  return false;
}

/**
 * Load current config.
 */
static bool load_config(pico_cec_nvs_t *nvs, cec_config_t *config) {
  if (crc32((unsigned char *)&nvs->config, sizeof(nvs->config)) == nvs->config_crc) {
    // deserialise
    config->edid_delay_ms = nvs->config.edid_delay_ms;
    config->physical_address = nvs->config.physical_address;
    config->logical_address = nvs->config.logical_address;
    config->device_type = nvs->config.device_type;
    // hack to support previous unused setting
    if (config->device_type == CEC_CONFIG_DEVICE_TYPE_TV) {
      config->device_type = CEC_CONFIG_DEVICE_TYPE_PLAYBACK;
    }
    config->keymap_type = nvs->config.keymap_type;
    for (uint8_t n = 0; n < UINT8_MAX; n++) {
      config->keymap[n].key = nvs->config.keymap[n];
    }

    return true;
  }

  return false;
}

bool nvs_read_config(cec_config_t *config) {
  bool success = false;

  // flash is mmapped for read
  pico_cec_nvs_t *cec_nvs = (pico_cec_nvs_t *)(CEC_NVS_BASE_ADDR);

  // start with default config, then overlay from nvs
  cec_config_set_default(config);

  // read and check header/config CRCs
  if (crc32((unsigned char *)&cec_nvs->header, sizeof(cec_nvs->header)) == cec_nvs->header_crc) {
    if (cec_nvs->header.version == CEC_CONFIG_VERSION_01) {
      success = migrate_v1(cec_nvs, config);
    } else if (cec_nvs->header.version == CEC_CONFIG_VERSION) {
      success = load_config(cec_nvs, config);
    }
  }

  return success;
}

void nvs_load_config(cec_config_t *config) {
  nvs_read_config(config);

  switch (config->keymap_type) {
    case CEC_CONFIG_KEYMAP_KODI:
    case CEC_CONFIG_KEYMAP_MISTER:
      cec_config_set_keymap(config);
      break;
    case CEC_CONFIG_KEYMAP_CUSTOM:
      // should already be loaded
      break;
    default:
      break;
  }

  cec_config_complete(config);

  return;
}

bool nvs_save_config(const cec_config_t *config) {
  pico_cec_nvs_t cec_nvs = {0x0};

  if (sizeof(cec_nvs) > CEC_NVS_LEN) {
    return false;
  }

  // serialise and checksum header
  cec_nvs.header.version = CEC_CONFIG_VERSION;
  cec_nvs.header.length = CEC_CONFIG_SIZE;
  cec_nvs.header_crc = crc32((unsigned char *)&cec_nvs.header, sizeof(cec_nvs.header));

  // serialise and checksum config
  cec_nvs.config.edid_delay_ms = config->edid_delay_ms;
  cec_nvs.config.physical_address = config->physical_address;
  cec_nvs.config.logical_address = config->logical_address;
  cec_nvs.config.device_type = config->device_type;
  cec_nvs.config.keymap_type = config->keymap_type;

  for (unsigned int n = 0; n < UINT8_MAX; n++) {
    cec_nvs.config.keymap[n] = config->keymap[n].key;
  }

  cec_nvs.config_crc = crc32((unsigned char *)&cec_nvs.config, sizeof(cec_nvs.config));

  // minimum number of flash sectors to erase in bytes
  unsigned int n = sizeof(cec_nvs) / FLASH_SECTOR_SIZE;
  unsigned int size = sizeof(cec_nvs) % FLASH_SECTOR_SIZE == 0 ? n * FLASH_SECTOR_SIZE
                                                               : (n + 1) * FLASH_SECTOR_SIZE;

  // interrupts must be disabled to safely program flash
  uint32_t irqs = save_and_disable_interrupts();
  flash_range_erase(nvs_get_flash_address(), size);

  // struct alignment should guarantee flash pages multiples
  flash_range_program(nvs_get_flash_address(), (uint8_t *)&cec_nvs, sizeof(cec_nvs));

  restore_interrupts(irqs);

  return true;
}
