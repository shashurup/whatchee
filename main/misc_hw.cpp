#include "misc_hw.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/i2c_master.h>
#include <pcf8563.h>
#include <cstring>

#define VIB_MOTOR_PIN GPIO_NUM_13
#define BUTTON_MENU_GPIO GPIO_NUM_26
#define BUTTON_BACK_GPIO GPIO_NUM_25
#define BUTTON_UP_GPIO GPIO_NUM_35
#define BUTTON_DOWN_GPIO GPIO_NUM_4
#define ALL_BUTTONS ((1ULL << BUTTON_MENU_GPIO) | (1ULL << BUTTON_BACK_GPIO) | (1ULL << BUTTON_UP_GPIO) | (1ULL << BUTTON_DOWN_GPIO))
#define BOUNCE_TIMEOUT 32


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

// millivolts
#define BATTERY_MAX 2900
#define BATTERY_MIN 2100

void Battery::measure(struct tm* now) {
  time_t new_time = now ? mktime(now) : time(0);
  if ((new_time - prev_time) > 60 * 60 * 3) {
    int new_voltage = get_voltage();
    if (new_voltage > prev_voltage) {
      start_voltage = new_voltage;
      start_time = time(0);
      discharge_rate = 0;
    } else {
      discharge_rate = (start_voltage - new_voltage) * 100 * 24 * 3600 /
        ((new_time - start_time) * (BATTERY_MAX - BATTERY_MIN));
      ESP_LOGI(__FILE__, "Discharge rate: %u", discharge_rate);
    }
    prev_voltage = new_voltage;
    prev_time = new_time;
  }
}

uint8_t Battery::get_level() {
  int millivolts = get_voltage();
  uint8_t level = (millivolts - BATTERY_MIN) * 100 / (BATTERY_MAX - BATTERY_MIN);
  return level > 100 ? 100 : level;
}

int Battery::get_voltage() {
  int battery_voltage;
  // GPIO_34 corresponds to ADC1 channel 6
  ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_6, &battery_voltage));
  // 2 - battery voltage divided by two resistors
  // 2450 - max voltage for ADC_ATTEN_DB_12
  // 4096 - levels for 12bit ADC mode
  return battery_voltage * 2 * 2450 / 4096;
}

uint8_t Battery::get_discharge_rate() {
  return discharge_rate;
}

void on_debounce_timer(TimerHandle_t timer) {
  uint32_t btn = (uint32_t) pvTimerGetTimerID(timer);
  gpio_num_t gpio_pin = buttons_gpio[btn];
  int level = gpio_get_level(gpio_pin);
  ESP_LOGD(__FILE__, "Pin %lu level is %d\n", btn, level);
  struct Message msg = {level ? BUTTON_PRESSED : BUTTON_RELEASED, (void *)btn};
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
  struct Message msg = {BUTTON_CHANGED, (void *)btn};
  xQueueSendFromISR(main_queue, &msg, NULL);
}

bool handle_misc_hw_events(Message msg) {
  if (msg.type == BUTTON_CHANGED) {
    ESP_LOGD(__FILE__, "Button %u has changed", (unsigned)msg.data);
    debounce((unsigned)msg.data);
    return true;
  }
  return false;
}

i2c_master_bus_config_t i2c_mst_config = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_21,
    .scl_io_num = GPIO_NUM_22,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .intr_priority = 0,
    .trans_queue_depth = 0,
    .flags = {.enable_internal_pullup = 1,
              .allow_pd = 0}
};
i2c_master_bus_handle_t bus_handle;

i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = 0x51,
    .scl_speed_hz = 100000,
    .scl_wait_us = 0,
    .flags = {.disable_ack_check = 1}
};

i2c_master_dev_handle_t dev_handle;

int32_t i2c_read(void *handle, uint8_t address, uint8_t reg, uint8_t *buffer, uint16_t size) {
  return i2c_master_transmit_receive(dev_handle, &reg, 1, buffer, size, -1);
}

int32_t i2c_write(void *handle, uint8_t address, uint8_t reg, const uint8_t *buffer, uint16_t size) {
  uint8_t* buf = (uint8_t *)malloc(size + 1);
  buf[0] = reg;
  memcpy(buf + 1, buffer, size);
  int32_t err = i2c_master_transmit(dev_handle, buf, size + 1, -1);
  free(buf);
  return err;
}

pcf8563_t pcf = {.read = &i2c_read,
                 .write = &i2c_write,
                 .handle = 0};

bool get_rtc_time(tm* t) {
  if (int err = pcf8563_read(&pcf, t)) {
    ESP_LOGE(__FILE__, "Error getting RTC time %d", err);
    return false;
  }
  return true;
}

void set_rtc_time(tm* t) {
  if (int err = pcf8563_write(&pcf, t))
    ESP_LOGE(__FILE__, "Error setting RTC time %d", err);
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

void setup_rtc() {
  int err = i2c_new_master_bus(&i2c_mst_config, &bus_handle);
  if (err)
    ESP_LOGE(__FILE__, "Master bus init error, %d", err);
  err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
  if (err)
    ESP_LOGE(__FILE__, "Add i2c device error, %d", err);
  err = pcf8563_init(&pcf);
  if (err)
    ESP_LOGE(__FILE__, "PCF8563 init error, %d", err);
}

void setup_misc_hw() {
  setup_buttons();
  setup_battery_adc();
  setup_rtc();
}
