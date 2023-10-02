#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char* TAG_LED  = "led";
static const char* TAG_MAIN = "main";
static const char* TAG_NET  = "network";
static const char* TAG_SLP  = "sleep";

// LED functions
void led_green_on() {
    ESP_LOGI(TAG_LED, "Turning green LED on.");
}

void led_green_off() {
    ESP_LOGI(TAG_LED, "Turning green LED off.");
}

void led_red_on() {
    ESP_LOGI(TAG_LED, "Turning red LED on.");
}

void led_red_off() {
    ESP_LOGI(TAG_LED, "Turning red LED off.");
}

// Sleep functions

void setup_gpio_wakeup() {
    ESP_LOGI(TAG_SLP, "Enabling EXT1 wakeup on GPIO 4");

    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << 4, ESP_EXT1_WAKEUP_ALL_LOW));

    // TODO(Isaiah): If we need internal pullup/pulldown resistors, enable them here...
}

// network functions
// int connect_to_wifi() {}
// int shutdown_wifi() {}

int disable_blocking() {
    ESP_LOGI(TAG_NET, "Disabling Pi-Hole add blocking.");
    return ESP_OK;
}


void app_main(void) {
    ESP_LOGI(TAG_MAIN, "Wakeup!");

    led_red_on();

    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();

    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG_MAIN, "Wakeup from GPIO event.");

        if (disable_blocking() == ESP_OK) {
            ESP_LOGI(TAG_MAIN, "Disabled add blocking.");
            led_green_on();
            led_red_off();
        } else {
            ESP_LOGI(TAG_MAIN, "Failed to disable add blocking.");
            led_green_on();
        }

        ESP_LOGI(TAG_MAIN, "Waiting for one minute.");
        vTaskDelay((5 * 1000) / portTICK_PERIOD_MS);  // TODO(Isaiah): Correct the time
    } else {
        ESP_LOGI(TAG_MAIN, "Wakeup NOT caused by GPIO event.");
    }

    // Wait 1 second, to prevent fast wakeup loops
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    led_green_off();
    led_red_off();

    setup_gpio_wakeup();

    ESP_LOGI(TAG_MAIN, "Going into deep sleep mode.");
    esp_deep_sleep_start();
}
