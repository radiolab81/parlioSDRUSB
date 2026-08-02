//Konfiguration Waveshare ESP32_P4_MODULE_DEV_KIT
#include "esp32_p4_module_dev_kit.h"
//#include "esp32_p4_nano.h"
//#include "esp32_p4_wifi6_poe_ethernet.h"
//#include "esp32_p4_wifi6_dev_kit.h"
//#include "esp32_p4_eth.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ESP System & FreeRTOS
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Hardware Treiber
#include "driver/parlio_tx.h"
#include "driver/gpio.h"

// USB Stack (TinyUSB)
#include "tusb.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h" // Falls CDC für Debugging
#define TAG "PARLIOSDR_USB"

// Konfiguration
#define BUFFER_SIZE (8 * 1024 * 1024) // 8 MB Ringbuffer im PSRAM
#define CACHE_ALIGN 64
#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(addr, align) ((addr) & ~((align) - 1))

// USB Endpunkte (P4 High-Speed)
#define EPNUM_VENDOR_OUT 0x01
#define EPNUM_VENDOR_IN  0x81

// Globale Variablen
uint8_t *buffer_a = NULL;
parlio_tx_unit_handle_t tx_unit = NULL;
volatile size_t write_ptr = 0;
SemaphoreHandle_t config_mutex = NULL;

float current_rate = 5.0f; 
int current_width = 8;     

// --- USB DESKRIPTOREN ---
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200, // USB 2.0 (High Speed)
    .bDeviceClass       = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A, 
    .idProduct          = 0x4000, 
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

static const tusb_desc_device_qualifier_t desc_device_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_VENDOR_SPECIFIC,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0x00
};

static const uint8_t desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN), 0, 500),
    // Vendor Interface: Interface number, string index, EP Out & In address, EP size
    TUD_VENDOR_DESCRIPTOR(0, 0, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 512) 
};

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 }, // 0: English
    "RADIOLAB81",                   // 1: Manufacturer
    "ESP32-PARLIOSDR-HS",           // 2: Product
    "SDR-001",                      // 3: Serial
};

// --- PARLIO-IO ---
void update_parlio_settings(float msps, int width) {
    if (!config_mutex) return;
    xSemaphoreTake(config_mutex, portMAX_DELAY);
    
    if (tx_unit != NULL) {
        parlio_tx_unit_disable(tx_unit);
        parlio_del_tx_unit(tx_unit);
        tx_unit = NULL; // Wichtig, nach Löschung nullen!
    }

    // Umrechnung von Mega-Samples in Hertz
    uint32_t freq_hz = (uint32_t)(msps * 1000000.0f);

    // Dynamische Konfiguration (ohne #ifdef)
    parlio_tx_unit_config_t unit_config = {
        .clk_src = PARLIO_CLK_SRC_PLL_F160M,
        .data_width = (uint32_t)width,
        .clk_out_gpio_num = DAC_CLK_PIN,
        .clk_in_gpio_num = ADC_CLK_PIN,
        .valid_gpio_num = -1,
        .output_clk_freq_hz = freq_hz,
        .trans_queue_depth = 2,
        .max_transfer_size = BUFFER_SIZE,
        /*.flags.clk_gate_en = false,*/
    };


    // Pins dynamisch je nach gewählter Breite zuweisen
    if (width == 16) {
        int pins_16[] = {SD0, SD1, SD2, SD3, SD4, SD5, SD6, SD7, SD8, SD9, SD10, SD11, SD12, SD13, SD14, SD15};
        for(int i=0; i<16; i++) unit_config.data_gpio_nums[i] = pins_16[i];
    } else {
        // Fallback: 8 Bit
        int pins_8[] = {SD0, SD1, SD2, SD3, SD4, SD5, SD6, SD7};
        for(int i=0; i<8; i++) unit_config.data_gpio_nums[i] = pins_8[i];
    }

    ESP_ERROR_CHECK(parlio_new_tx_unit(&unit_config, &tx_unit));
    ESP_ERROR_CHECK(parlio_tx_unit_enable(tx_unit));

    // Starte permanenten Hardware-Loop
    parlio_transmit_config_t transmit_config = {
        .flags.loop_transmission = 1,
    };

    ESP_ERROR_CHECK(parlio_tx_unit_transmit(tx_unit, buffer_a, (size_t)BUFFER_SIZE * 8, &transmit_config));
    
    ESP_LOGI(TAG, "PARLIO: %.2f MSPS, %d Bit", msps, width);
    xSemaphoreGive(config_mutex);
}

// --- USB CALLBACKS ---
// Wird von TinyUSB aufgerufen, wenn Bulk-Daten empfangen wurden
void tud_vendor_rx_cb(uint8_t itf, uint8_t const* buffer, uint16_t bufsize) {
    // 1. Wie viel Daten liegen insgesamt im USB-FIFO bereit?
    // In deiner ESP_IDF6.1/TinyUSB ohne 'itf' Argument:
    uint32_t total_available = tud_vendor_available(); 
    if (total_available == 0) return;

    // 2. Wie viel Platz ist noch bis zum Ende des 8MB-Puffers?
    uint32_t space_to_end = BUFFER_SIZE - write_ptr;

    if (total_available <= space_to_end) {
        // FALL A: Alles passt hintereinander, Lesevorgang signalisiert USB für den Empfang weiterer Daten
        uint32_t read_actual = tud_vendor_read(buffer_a + write_ptr, total_available);
        
        // Cache-Sync (64-Byte Alignment für P4)
        uintptr_t sync_start = (uintptr_t)(buffer_a + write_ptr) & ~(63);
        uintptr_t sync_end = ((uintptr_t)(buffer_a + write_ptr + read_actual) + 63) & ~(63);
        esp_cache_msync((void *)sync_start, sync_end - sync_start, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        write_ptr += read_actual;
        if (write_ptr >= BUFFER_SIZE) write_ptr = 0;
    } 
    else {
        // FALL B: Wrap-around!
        
        // Teil 1: Bis zum Ende des Puffers lesen
        uint32_t read_p1 = tud_vendor_read(buffer_a + write_ptr, space_to_end);
        esp_cache_msync((void*)(buffer_a + write_ptr), space_to_end, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        
        // Teil 2: Den Rest an den Anfang schreiben
        uint32_t remaining = total_available - space_to_end;
        uint32_t read_p2 = tud_vendor_read(buffer_a, remaining);
        
        uintptr_t sync_end_p2 = ((uintptr_t)(buffer_a + read_p2) + 63) & ~(63);
        esp_cache_msync(buffer_a, sync_end_p2 - (uintptr_t)buffer_a, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        
        write_ptr = read_p2;

        // Nur um die Warnung zu unterdrücken, falls man die Rückgabewerte nicht loggt
        (void)read_p1;
        (void)read_p2;
    }
}

// USB Control Requests (bmRequestType: 0x40)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bRequest == 0x01) { // Rate
       float val = (float)request->wValue / 10.0f;
       if (val > 0.1f && val <= 40.0f) {
           current_rate = val;
           update_parlio_settings(current_rate, current_width);
       }
       return tud_control_status(rhport, request);
    } 
    else if (request->bRequest == 0x02) { // Width
        int w = (int)request->wValue;
        if (w == 8 || w == 16) {
            current_width = w;
            update_parlio_settings(current_rate, current_width);
        }
        return tud_control_status(rhport, request);
    }
    return false; 
}


// Dieser Task sorgt dafür, dass USB-Daten verarbeitet werden
void usb_device_task(void *param) {
    while (1) {
        tud_task();
        taskYIELD();
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    config_mutex = xSemaphoreCreateMutex();

    // 1. PSRAM Ringbuffer
    buffer_a = heap_caps_aligned_alloc(CACHE_ALIGN, BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!buffer_a) {
        ESP_LOGE(TAG, "PSRAM Fehler!");
        return;
    }
    
    memset(buffer_a, 0, BUFFER_SIZE);
    esp_cache_msync(buffer_a, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // 2. USB Config
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device,
        .configuration_descriptor = desc_configuration,    // Für Full-Speed
        .hs_configuration_descriptor = desc_configuration, // Für High-Speed (P4)
        .qualifier_descriptor = &desc_device_qualifier,
        .string_descriptor = string_desc_arr,
        .string_descriptor_count = 4,
        .external_phy = false, 
        .self_powered = false, // WICHTIG: Das sagt dem Stack, er zieht Strom vom PC
        .vbus_monitor_io = -1, // -1 bedeutet: Ignoriere VBUS-Pin-Check (immer an)
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // 3. Start Hardware
    update_parlio_settings(current_rate, current_width);
 
    // NEU: USB-Task starten
    xTaskCreatePinnedToCore(usb_device_task, "usbd", 4096, NULL, 10, NULL, 1);


    ESP_LOGI(TAG, "USB High-Speed SDR gestartet (Bulk Endpoint 0x01)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
