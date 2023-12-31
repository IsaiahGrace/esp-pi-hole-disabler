#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "net.h"

static const char* TAG_LED  = "led";
static const char* TAG_MAIN = "main";
static const char* TAG_SLP  = "sleep";

// LED functions
static const gpio_num_t led_green = GPIO_NUM_22;
static const gpio_num_t led_red   = GPIO_NUM_23;

void led_setup(void) {
    ESP_LOGI(TAG_LED, "Configuring LED pins.");

    gpio_reset_pin(led_green);
    gpio_reset_pin(led_red);

    gpio_set_direction(led_green, GPIO_MODE_OUTPUT);
    gpio_set_direction(led_red, GPIO_MODE_OUTPUT);
}

void led_green_on(void) {
    ESP_LOGI(TAG_LED, "Turning on green LED.");
    ESP_ERROR_CHECK(gpio_set_level(led_green, 1));
}

void led_green_off(void) {
    ESP_LOGI(TAG_LED, "Turning off green LED.");
    ESP_ERROR_CHECK(gpio_set_level(led_green, 0));
}

void led_red_on(void) {
    ESP_LOGI(TAG_LED, "Turning on red LED.");
    ESP_ERROR_CHECK(gpio_set_level(led_red, 1));
}

void led_red_off(void) {
    ESP_LOGI(TAG_LED, "Turning off red LED.");
    ESP_ERROR_CHECK(gpio_set_level(led_red, 0));
}

// Sleep functions

void setup_gpio_wakeup(void) {
    ESP_LOGI(TAG_SLP, "Enabling EXT1 wakeup on GPIO 4");

    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << 4, ESP_EXT1_WAKEUP_ALL_LOW));

    // TODO(Isaiah): If we need internal pullup/pulldown resistors, enable them here...
}

void app_main(void) {
    ESP_LOGI(TAG_MAIN, "Wakeup!");

    led_setup();
    led_red_on();
    led_green_on();

    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();

    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG_MAIN, "Wakeup from GPIO event.");

        if (net_disable_add_blocking() == ESP_OK) {
            ESP_LOGI(TAG_MAIN, "Disabled add blocking.");
            led_red_off();

            ESP_LOGI(TAG_MAIN, "Waiting for one minute.");
            vTaskDelay((59 * 1000) / portTICK_PERIOD_MS);
        } else {
            ESP_LOGE(TAG_MAIN, "Failed to disable add blocking.");
            led_green_off();

            ESP_LOGI(TAG_MAIN, "Waiting for 10 seconds.");
            vTaskDelay((9 * 1000) / portTICK_PERIOD_MS);
        }
    } else {
        ESP_LOGW(TAG_MAIN, "Wakeup NOT caused by GPIO event.");
    }

    // Wait 1 second, to prevent fast wakeup loops
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    led_green_off();
    led_red_off();

    setup_gpio_wakeup();

    ESP_LOGI(TAG_MAIN, "Going into deep sleep mode.");
    esp_deep_sleep_start();
}
