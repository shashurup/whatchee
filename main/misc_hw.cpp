#include "misc_hw.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <esp_adc/adc_oneshot.h>

#define VIB_MOTOR_PIN GPIO_NUM_13
#define BUTTON_MENU_GPIO GPIO_NUM_26
#define BUTTON_BACK_GPIO GPIO_NUM_25
#define BUTTON_UP_GPIO GPIO_NUM_35
#define BUTTON_DOWN_GPIO GPIO_NUM_4
#define ALL_BUTTONS ((1ULL << BUTTON_MENU_GPIO) | (1ULL << BUTTON_BACK_GPIO) | (1ULL << BUTTON_UP_GPIO) | (1ULL << BUTTON_DOWN_GPIO))
#define BOUNCE_TIMEOUT 64


adc_oneshot_unit_handle_t adc_handle; 
gpio_num_t buttons_gpio[4] = {BUTTON_MENU_GPIO, BUTTON_BACK_GPIO, BUTTON_UP_GPIO, BUTTON_DOWN_GPIO};
TimerHandle_t debounce_timers[4];


void vibrate(uint8_t intervalMs, uint8_t length) {
  gpio_reset_pin(VIB_MOTOR_PIN);
  gpio_set_direction(VIB_MOTOR_PIN, GPIO_MODE_OUTPUT);
  bool motorOn = false;
  for (int i = 0; i < length; i++) {
    motorOn = !motorOn;
    gpio_set_level(VIB_MOTOR_PIN, motorOn);
    vTaskDelay(intervalMs / portTICK_PERIOD_MS);
  }
}

int get_battery_millivolts() {
  int battery_voltage;
  // GPIO_34 corresponds to ADC1 channel 6
  ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_6, &battery_voltage));
  // 2 - battery voltage divided by two resistors
  // 2450 - max voltage for ADC_ATTEN_DB_12
  // 4096 - levels for 12bit ADC mode
  return battery_voltage * 2 * 2450 / 4096;
}

void on_debounce_timer(TimerHandle_t timer) {
  uint32_t btn = (uint32_t) pvTimerGetTimerID(timer);
  gpio_num_t gpio_pin = buttons_gpio[btn];
  int level = gpio_get_level(gpio_pin);
  ESP_LOGD(__FILE__, "Pin %lu level is %d\n", btn, level);
  struct Message msg = {level ? BUTTON_PRESSED : BUTTON_RELEASED, btn};
  xQueueSend(main_queue, &msg, 0);
  gpio_set_intr_type(gpio_pin,
                     level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  gpio_intr_enable(gpio_pin);
}

void debounce(unsigned btn) {
  if (!debounce_timers[btn]) {
    debounce_timers[btn] = xTimerCreate(NULL,
                                        pdMS_TO_TICKS(BOUNCE_TIMEOUT),
                                        pdFALSE,
                                        (void *)btn,
                                        on_debounce_timer);
  }
  if (xTimerIsTimerActive(debounce_timers[btn]) == pdFALSE) {
    xTimerStart(debounce_timers[btn], 0);
  }
}

void button_handler(void* arg) {
  uint32_t btn = (uint32_t) arg;
  gpio_intr_disable(buttons_gpio[btn]);
  struct Message msg = {BUTTON_CHANGED, btn};
  xQueueSendFromISR(main_queue, &msg, NULL);
}

bool handle_misc_hw_events(Message msg) {
  if (msg.type == BUTTON_CHANGED) {
    ESP_LOGD(__FILE__, "Button %u has changed", msg.value);
    debounce(msg.value);
    return true;
  }
  return false;
}

void setup_battery_adc() {
  adc_oneshot_unit_init_cfg_t init_config = {
    .unit_id = ADC_UNIT_1,
    .clk_src = (adc_oneshot_clk_src_t)0,
    .ulp_mode = ADC_ULP_MODE_DISABLE
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));
  adc_oneshot_chan_cfg_t config = {
    .atten = ADC_ATTEN_DB_12, //  = 3 (up to 2450 millivolts)
    .bitwidth = ADC_BITWIDTH_DEFAULT, // = 12
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_6, &config));
}

void setup_buttons() {
  gpio_config_t io_conf = {}; 
  io_conf.intr_type = GPIO_INTR_HIGH_LEVEL;
  io_conf.pin_bit_mask = ALL_BUTTONS;
  io_conf.mode = GPIO_MODE_INPUT;
  gpio_config(&io_conf);
  gpio_install_isr_service(0);
  for (unsigned i = 0; i < 4; i++) {
    gpio_wakeup_enable(buttons_gpio[i], GPIO_INTR_HIGH_LEVEL);
    gpio_isr_handler_add(buttons_gpio[i], button_handler, (void*) i);
  }
}

void setup_misc_hw() {
  setup_buttons();
  setup_battery_adc();
}
