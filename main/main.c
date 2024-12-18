// Copyright 2020-2021 Espressif Systems (Shanghai) CO LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include "util.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "serial.h"
#include "tusb.h"
// #include "msc.h"
#include "hal/usb_phy_types.h"
#include "soc/usb_periph.h"
#include "rom/gpio.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/usb_phy.h"
// #include "eub_vendord.h"
// #include "eub_debug_probe.h"

static const char *TAG = "bridge_main";

#define _PID_MAP(itf, n) ((CFG_TUD_##itf) << (n))
#define USB_PID (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | \
								 _PID_MAP(MIDI, 3) | _PID_MAP(VENDOR, 4))

#define USB_VID 0xCafe
#define USB_BCD 0x0200

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

enum
{
	ITF_NUM_CDC_0 = 0,
	ITF_NUM_CDC_0_DATA,
	ITF_NUM_CDC_1,
	ITF_NUM_CDC_1_DATA,
	ITF_NUM_TOTAL
};

#define EPNUM_CDC_0_NOTIF 0x81
#define EPNUM_CDC_0_OUT 0x02
#define EPNUM_CDC_0_IN 0x82

#define EPNUM_CDC_1_NOTIF 0x83
#define EPNUM_CDC_1_OUT 0x04
#define EPNUM_CDC_1_IN 0x84

static const tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#ifdef CFG_TUD_ENDPOINT0_SIZE
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
#else  // earlier versions have a typo in the name
    .bMaxPacketSize0 = CFG_TUD_ENDOINT0_SIZE,
#endif
    .idVendor = CONFIG_BRIDGE_USB_VID,
    .idProduct = CONFIG_BRIDGE_USB_PID,
    .bcdDevice = BCDDEVICE,     // defined in CMakeLists.txt
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static uint8_t const desc_configuration[] = {
		// config number, interface count, string index, total length, attribute, power in mA
		TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, 0, 500),

		// 1st CDC: Interface number, string index, EP notification address and size, EP data address (out, in) and size.
		TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_0, 4, EPNUM_CDC_0_NOTIF, 8, EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),

		// 2nd CDC: Interface number, string index, EP notification address and size, EP data address (out, in) and size.
		TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_1, 4, EPNUM_CDC_1_NOTIF, 8, EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),
};

#define MAC_BYTES 6

static char serial_descriptor[MAC_BYTES * 2 + 1] = {'\0'}; // 2 chars per hexnumber + '\0'

static char const *string_desc_arr[] = {
		(const char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
		CONFIG_BRIDGE_MANUFACTURER, // 1: Manufacturer
#if CONFIG_BRIDGE_DEBUG_IFACE_JTAG
		CONFIG_BRIDGE_PRODUCT_NAME, // 2: Product
#else
		"CMSIS-DAP", // OpenOCD expects "CMSIS-DAP" as a product name
#endif
		serial_descriptor, // 3: Serials
		"CDC",
		CONFIG_BRIDGE_DEBUG_IFACE_NAME, // JTAG or CMSIS-DAP
		"MSC",

		/* JTAG_STR_DESC_INX 0x0A */
};

static uint16_t _desc_str[32];

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	return desc_configuration;
}

uint8_t const *tud_descriptor_device_cb(void)
{
	return (uint8_t const *)&descriptor_config;
}

// void tud_mount_cb(void)
// {
//     ESP_LOGI(TAG, "Mounted");

//     eub_vendord_start();
//     eub_debug_probe_init();
// }

static void init_serial_no(void)
{
	uint8_t m[MAC_BYTES] = {0};
	esp_err_t ret = esp_efuse_mac_get_default(m);

	if (ret != ESP_OK)
	{
		ESP_LOGD(TAG, "Cannot read MAC address and set the device serial number");
		eub_abort();
	}

	snprintf(serial_descriptor, sizeof(serial_descriptor),
					 "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

uint16_t const *tud_descriptor_string_cb(const uint8_t index, const uint16_t langid)
{
	uint8_t chr_count;

	if (index == 0)
	{
		memcpy(&_desc_str[1], string_desc_arr[0], 2);
		chr_count = 1;
		// } else if (index == EUB_DEBUG_PROBE_STR_DESC_INX) {
		//     chr_count = eub_debug_probe_get_proto_caps(&_desc_str[1]) / 2;
	}
	else
	{
		// Convert ASCII string into UTF-16

		if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
		{
			return NULL;
		}

		const char *str = string_desc_arr[index];

		// Cap at max char
		chr_count = strlen(str);
		if (chr_count > 31)
		{
			chr_count = 31;
		}

		for (uint8_t i = 0; i < chr_count; i++)
		{
			_desc_str[1 + i] = str[i];
		}
	}

	// first byte is length (including header), second byte is string type
	_desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

	return _desc_str;
}

static void tusb_device_task(void *pvParameters)
{
	while (1)
	{
		tud_task();
	}
	vTaskDelete(NULL);
}

static void init_led_gpios(void)
{
	gpio_config_t io_conf = {};
	io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = (1ULL << CONFIG_BRIDGE_GPIO_LED1) | (1ULL << CONFIG_BRIDGE_GPIO_LED2) |
												 (1ULL << CONFIG_BRIDGE_GPIO_LED3);
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 0;
	ESP_ERROR_CHECK(gpio_config(&io_conf));

	gpio_set_level(CONFIG_BRIDGE_GPIO_LED1, !CONFIG_BRIDGE_GPIO_LED1_ACTIVE);
	gpio_set_level(CONFIG_BRIDGE_GPIO_LED2, !CONFIG_BRIDGE_GPIO_LED2_ACTIVE);
	gpio_set_level(CONFIG_BRIDGE_GPIO_LED3, !CONFIG_BRIDGE_GPIO_LED3_ACTIVE);

	ESP_LOGI(TAG, "LED GPIO init done");
}

static void int_usb_phy(void)
{
	usb_phy_config_t phy_config = {
			.controller = USB_PHY_CTRL_OTG,
			.target = USB_PHY_TARGET_INT,
			.otg_mode = USB_OTG_MODE_DEVICE,
			.otg_speed = USB_PHY_SPEED_FULL,
			.ext_io_conf = NULL,
			.otg_io_conf = NULL,
	};
	usb_phy_handle_t phy_handle;
	usb_new_phy(&phy_config, &phy_handle);
}

void app_main(void)
{
	// init_led_gpios(); // Keep this at the begining. LEDs are used for error reporting.

	init_serial_no();

	int_usb_phy();

	tusb_init();
	// msc_init();
	serial_init();

	xTaskCreate(tusb_device_task, "tusb_device_task", 4 * 1024, NULL, 5, NULL);
}
