/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_http_client.h"

#define RED_PIN 21
#define GREEN_PIN 22
#define BLUE_PIN 23

#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY 4000
#define MAX_DUTY (1 << LEDC_DUTY_RES) - 1

static uint32_t gamma_table[256];
static volatile bool ws_connected = false;
static SemaphoreHandle_t rgb_mutex;
static uint8_t current_rgb[3] = {128, 128, 128};

static void init_gamma_table(void)
{
  for (int i = 0; i < 256; i++) {
    float normalized = i / 255.0f;
    gamma_table[i] = (uint32_t)(powf(normalized, 2.2f) * MAX_DUTY);
  }
}

static inline void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, gamma_table[r]);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, gamma_table[g]);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1);
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_2, gamma_table[b]);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_2);
}

static void update_leds_safe(uint8_t r, uint8_t g, uint8_t b)
{
  if (xSemaphoreTake(rgb_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    current_rgb[0] = r;
    current_rgb[1] = g;
    current_rgb[2] = b;
    set_rgb(r, g, b);
    xSemaphoreGive(rgb_mutex);
  }
}

static void sine_animation_task(void *pvParameters)
{
  const float periods[3] = {0.5f, 1.0f, 2.0f};
  
  while (1) {
    if (!ws_connected) {
      uint64_t time_us = esp_timer_get_time();
      float time_s = time_us / 1000000.0f;
      
      uint8_t rgb[3];
      for (int i = 0; i < 3; i++) {
        float phase = 2.0f * M_PI * time_s / periods[i];
        float sine_val = (sinf(phase) + 1.0f) * 0.5f;
        rgb[i] = (uint8_t)(sine_val * 255.0f);
      }
      
      update_leds_safe(rgb[0], rgb[1], rgb[2]);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static const struct {
  uint8_t pin;
  ledc_channel_t channel;
} led_config[] = {
  {RED_PIN, LEDC_CHANNEL_0},
  {GREEN_PIN, LEDC_CHANNEL_1},
  {BLUE_PIN, LEDC_CHANNEL_2}
};

static void pwm_init(void)
{
  ledc_timer_config_t timer_config = {
    .speed_mode = LEDC_MODE,
    .duty_resolution = LEDC_DUTY_RES,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = LEDC_FREQUENCY,
    .clk_cfg = LEDC_AUTO_CLK
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

  for (int i = 0; i < 3; i++) {
    ledc_channel_config_t channel_config = {
      .speed_mode = LEDC_MODE,
      .channel = led_config[i].channel,
      .timer_sel = LEDC_TIMER_0,
      .intr_type = LEDC_INTR_DISABLE,
      .gpio_num = led_config[i].pin,
      .duty = 0,
      .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
  }
}

static const char *TAG = "color_organ";

static void send_ip_to_server(void)
{
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == NULL) {
    ESP_LOGE(TAG, "Failed to get network interface");
    return;
  }
  
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get IP info");
    return;
  }
  
  char ip_str[16];
  sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
  ESP_LOGI(TAG, "Local IP: %s", ip_str);
  
  esp_http_client_config_t config = {
    .url = "http://kv.wfeng.dev/esp:ip",
    .method = HTTP_METHOD_POST,
  };
  
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return;
  }
  
  esp_http_client_set_header(client, "Content-Type", "text/plain");
  esp_http_client_set_post_field(client, ip_str, strlen(ip_str));
  
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "IP sent to server, status: %d", status_code);
  } else {
    ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
  }
  
  esp_http_client_cleanup(client);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
  if (req->method == HTTP_GET) {
    ws_connected = true;
    return ESP_OK;
  }
  
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    ws_connected = false;
    return ESP_OK;
  }
  
  if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
    ws_connected = false;
    return ESP_OK;
  }
  
  if (ws_pkt.len != 3) {
    return ESP_OK;
  }
  
  uint8_t rgb[3];
  ws_pkt.payload = rgb;
  ret = httpd_ws_recv_frame(req, &ws_pkt, 3);
  if (ret == ESP_OK && ws_pkt.len == 3) {
    ws_connected = true;
    if (rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255) {
      httpd_ws_send_frame(req, &ws_pkt);
    } else {
      update_leds_safe(rgb[0], rgb[1], rgb[2]);
    }
  } else {
    ws_connected = false;
  }
  
  return ESP_OK;
}

static const httpd_uri_t ws_uri = {
  .uri = "/ws",
  .method = HTTP_GET,
  .handler = ws_handler,
  .user_ctx = NULL,
  .is_websocket = true
};

static httpd_handle_t start_webserver(void)
{
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  
  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &ws_uri);
    return server;
  }
  return NULL;
}

void app_main(void)
{
  ESP_LOGI(TAG, "Starting Color Organ");
  
  rgb_mutex = xSemaphoreCreateMutex();
  if (rgb_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create RGB mutex");
    return;
  }
  
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  
  ESP_LOGI(TAG, "Connecting to network...");
  ESP_ERROR_CHECK(example_connect());
  ESP_LOGI(TAG, "Network connected");
  
  init_gamma_table();
  pwm_init();
  ESP_LOGI(TAG, "PWM initialized");
  
  set_rgb(128, 128, 128);
  ESP_LOGI(TAG, "Set default 50%% brightness");
  
  send_ip_to_server();
  
  xTaskCreate(sine_animation_task, "sine_anim", 4096, NULL, 5, NULL);
  ESP_LOGI(TAG, "Sine animation task started");
  
  httpd_handle_t server = start_webserver();
  if (server) {
    ESP_LOGI(TAG, "WebSocket server started");
  }
  
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
