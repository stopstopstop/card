
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "serial.h"
#include "tusb.h"
#include "jtag.h"
#include "msc.h"
#include "hal/usb_hal.h"
#include "soc/usb_periph.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#if CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/gpio.h"
#elif CONFIG_IDF_TARGET_ESP32S2
#include "esp32s2/rom/gpio.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/gpio.h"
#else
#error Target CONFIG_IDF_TARGET is not supported
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
#include "esp_mac.h"
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_private/periph_ctrl.h"
#else
#include "driver/periph_ctrl.h"
#endif

#include "esp_bridge_task.h"
#include "esp_bridge_port.h"

static const char *TAG = "bridge task";

EventGroupHandle_t bridge_event_group;
static TaskHandle_t bridge_task_hdl = NULL;

#define EPNUM_CDC       2
#define EPNUM_VENDOR    3
#define EPNUM_MSC       4

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_VENDOR,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

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
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

/*
    ESP usb builtin jtag subclass and protocol is 0xFF and 0x01 respectively.
    However, Tinyusb default values are 0x00.
    In order to use same protocol without tinyusb customization we are re-defining
    vendor descriptor here.
*/
// Interface number, string index, EP Out & IN address, EP size
#define TUD_VENDOR_EUB_DESCRIPTOR(_itfnum, _stridx, _epout, _epin, _epsize) \
  /* Interface */\
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, 0xFF, 0x01, _stridx,\
  /* Endpoint Out */\
  7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,\
  /* Endpoint In */\
  7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

static uint8_t const desc_configuration[] = {
    // config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, 0x81, 8, EPNUM_CDC, 0x80 | EPNUM_CDC, TUD_OPT_HIGH_SPEED ? 512 : 64),

    // Interface number, string index, EP Out & IN address, EP size
    TUD_VENDOR_EUB_DESCRIPTOR(ITF_NUM_VENDOR, 5, EPNUM_VENDOR, 0x80 | EPNUM_VENDOR, 64),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 6, EPNUM_MSC, 0x80 | EPNUM_MSC, 64),
};

#define MAC_BYTES       6

static char serial_descriptor[MAC_BYTES * 2 + 1] = {'\0'}; // 2 chars per hexnumber + '\0'

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
    CONFIG_BRIDGE_MANUFACTURER,    // 1: Manufacturer
    CONFIG_BRIDGE_PRODUCT_NAME,    // 2: Product
    serial_descriptor,             // 3: Serials
    "CDC",
    "JTAG",
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
    return (uint8_t const *) &descriptor_config;
}

static void init_serial_no(void)
{
    uint8_t m[MAC_BYTES] = {0};
    esp_err_t ret = esp_efuse_mac_get_default(m);

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Cannot read MAC address and set the device serial number");
        eub_abort();
    }

    snprintf(serial_descriptor, sizeof(serial_descriptor),
             "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

uint16_t const *tud_descriptor_string_cb(const uint8_t index, const uint16_t langid)
{
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == JTAG_STR_DESC_INX) {
        chr_count = jtag_get_proto_caps(&_desc_str[1]) / 2;
    } else {
        // Convert ASCII string into UTF-16

        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        const char *str = string_desc_arr[index];

        // Cap at max char
        chr_count = strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2 * chr_count + 2);

    return _desc_str;
}

static void configure_pins(usb_hal_context_t *usb)
{
    /* usb_periph_iopins currently configures USB_OTG as USB Device.
     * Introduce additional parameters in usb_hal_context_t when adding support
     * for USB Host.
     */
    for (const usb_iopin_dsc_t *iopin = usb_periph_iopins; iopin->pin != -1; ++iopin) {
        if ((usb->use_external_phy) || (iopin->ext_phy_only == 0)) {
            gpio_pad_select_gpio(iopin->pin);
            if (iopin->is_output) {
                gpio_matrix_out(iopin->pin, iopin->func, false, false);
            } else {
                gpio_matrix_in(iopin->pin, iopin->func, false);
                gpio_pad_input_enable(iopin->pin);
            }
            gpio_pad_unhold(iopin->pin);
        }
    }
    if (!usb->use_external_phy) {
        gpio_set_drive_capability(USBPHY_DM_NUM, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability(USBPHY_DP_NUM, GPIO_DRIVE_CAP_3);
    }
}

static void tusb_device_task(void *pvParameters)
{
    while (1) {
        tud_task();
    }
    vTaskDelete(NULL);
}

static void periodic_timer_callback(void* arg)
{
    int32_t time_since_boot = esp_timer_get_time()/1000000;
    ESP_LOGI("timer", "since %ds", time_since_boot);
}


/*******************************************************************************
**函数信息 ：
**功能描述 ：
**输入参数 ：
**输出参数 ：
********************************************************************************/	
static void bridge_task(void *pvParameter)
{
    while(true){
		EventBits_t uxBits = xEventGroupWaitBits(
            bridge_event_group,
            NFC_ACD_IRQ | NFC_CARD_IN | NFC_CARD_EX,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );
    }
	esp_bridge_deinit();
}

/*******************************************************************************
**函数信息 ：
**功能描述 ：
**输入参数 ：
**输出参数 ：
********************************************************************************/
void esp_bridge_init(void)
{
    // if(esp_bridge_port_init()==ESP_OK){
	// 	bridge_event_group = xEventGroupCreate();
	// 	xTaskCreate(bridge_task, "bridge_task", TASK_BRIDGE_SIZE, NULL, TASK_BRIDGE_PRIO, &bridge_task_hdl);
	// }
    init_serial_no();
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_enable(PERIPH_USB_MODULE);

    usb_hal_context_t hal = {
        .use_external_phy = false
    };
    usb_hal_init(&hal);
    configure_pins(&hal);

    tusb_init();
    msc_init();
    serial_init();
    jtag_init();

    // const esp_timer_create_args_t periodic_timer_args = {
    //         .callback = &periodic_timer_callback,
    //         .name = "periodic"
    // };
    // esp_timer_handle_t periodic_timer;
    // ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    // ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 10*1000*1000));
    xTaskCreatePinnedToCore(tusb_device_task, "tusb_device_task", 4 * 1024, NULL, TASK_BRIDGE_PRIO, &bridge_task_hdl, 1);
    // xTaskCreatePinnedToCore(lvgl_task, "lvgl_Task", 6 * 1024, NULL, configMAX_PRIORITIES - 3, &g_lvgl_task_handle, 1);
    bridge_event_group = xEventGroupCreate();
}

/*******************************************************************************
**函数信息 ：
**功能描述 ：
**输入参数 ：
**输出参数 ：
********************************************************************************/
void esp_bridge_deinit(void)
{
	esp_bridge_port_deinit();
	vTaskDelete(bridge_task_hdl);
}
