 // ESP32 Web Debugger — Pure ESP-IDF Rewrite
// Supports GPIO, PWM, ADC, UART, I2C, WebSocket, SPIFFS Web UI
#include "sdkconfig.h"
#include <esp_http_server.h>

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include "esp_spiffs.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/uart.h>
#include <driver/i2c.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <esp_adc/adc_oneshot.h>

#define TAG "WebDebug"
#define WIFI_SSID "SSID"
#define WIFI_PASS "PASSWORD"

#define I2C_MASTER_SDA 21
#define I2C_MASTER_SCL 22
#define I2C_PORT I2C_NUM_0
#define UART_PORT UART_NUM_1
#define UART_TX 17
#define UART_RX 16

#define OSCILLO_ADC_CHANNEL ADC_CHANNEL_6  // GPIO34 = ADC1_CH6
#define MAX_WS_CLIENTS 8

static httpd_handle_t server = NULL;

static const int usable_pins[] = {
    0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19,
    21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39
};
#define NUM_PINS (sizeof(usable_pins) / sizeof(usable_pins[0]))

static bool pin_modes[NUM_PINS];
static bool pin_states[NUM_PINS];
static unsigned int bounce_counters[NUM_PINS];
static unsigned long last_change_times[NUM_PINS];
static int pwm_frequencies[NUM_PINS];

static adc_oneshot_unit_handle_t adc_handle;
static int oscillo_pin = 34;
static float last_voltage = 0.0f;
static const float smoothing_factor = 0.3f;

static int ws_clients[MAX_WS_CLIENTS];

static int get_pin_index(int pin) {
    for (int i = 0; i < NUM_PINS; i++) {
        if (usable_pins[i] == pin) return i;
    }
    return -1;
}

static void set_pin_mode(int pin, bool output) {
    gpio_set_direction(pin, output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    int idx = get_pin_index(pin);
    if (idx >= 0) {
        pin_modes[idx] = output;
        pwm_frequencies[idx] = 0;
    }
}

static void write_pin(int pin, bool value) {
    int idx = get_pin_index(pin);
    if (idx >= 0 && pin_modes[idx]) {
        gpio_set_level(pin, value);
        pin_states[idx] = value;
    }
}

static void set_pwm(int pin, int freq) {
    int idx = get_pin_index(pin);
    if (idx >= 0) pwm_frequencies[idx] = freq;

    int channel = pin % 8;

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .channel = channel,
        .timer_sel = LEDC_TIMER_0,
        .duty = 127,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
}

static void uart_send(const char* str) {
    uart_write_bytes(UART_PORT, str, strlen(str));
}

static void scan_i2c(char* out, size_t max_len) {
    size_t pos = 0;
    pos += snprintf(out + pos, max_len - pos, "{\"i2c\":[");
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            pos += snprintf(out + pos, max_len - pos, "%d,", addr);
        }
    }
    if (pos > 7 && out[pos - 1] == ',') pos--;  a
    snprintf(out + pos, max_len - pos, "]}");
}

static void notify_clients(const char* message) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] >= 0) {
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)message,
                .len = strlen(message),
                .final = true
            };
            httpd_ws_send_frame_async(server, ws_clients[i], &frame);
        }
    }
}


static void send_all_pin_states() {
    char buffer[2048];
    int pos = 0;
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "[");

    for (int i = 0; i < NUM_PINS; i++) {
        const char* mode_str = pin_modes[i] ? (pwm_frequencies[i] > 0 ? "PWM" : "OUTPUT") : "INPUT";
        pos += snprintf(buffer + pos, sizeof(buffer) - pos,
            "{\"pin\":%d,\"mode\":\"%s\",\"state\":%d,\"bounce\":%d}%s",
            usable_pins[i], mode_str, pin_states[i], bounce_counters[i],
            (i < NUM_PINS - 1) ? "," : ""
        );
    }

    snprintf(buffer + pos, sizeof(buffer) - pos, "]");
    notify_clients(buffer);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        int sockfd = httpd_req_to_sockfd(req);

        // Register client in ws_clients[]
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (ws_clients[i] < 0) {
                ws_clients[i] = sockfd;
                break;
            }
        }

        ESP_LOGI(TAG, "WebSocket client connected: sockfd=%d", sockfd);
        send_all_pin_states();  
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = NULL,
    };

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK || frame.len == 0) return ret;

    frame.payload = malloc(frame.len + 1);
    if (!frame.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        free(frame.payload);
        return ret;
    }

    frame.payload[frame.len] = '\0';
    char* msg = (char*)frame.payload;

    if (strncmp(msg, "MODE:", 5) == 0) {
        int pin = atoi(msg + 5);
        char* mode = strchr(msg + 5, ',');
        if (mode) set_pin_mode(pin, strstr(mode, "OUTPUT") != NULL);
        send_all_pin_states();
    } else if (strncmp(msg, "WRITE:", 6) == 0) {
        int pin = atoi(msg + 6);
        int val = atoi(strchr(msg + 6, ',') + 1);
        write_pin(pin, val);
        send_all_pin_states();
    } else if (strncmp(msg, "PWM:", 4) == 0) {
        int pin = atoi(msg + 4);
        int freq = atoi(strchr(msg + 4, ',') + 1);
        set_pwm(pin, freq);
        send_all_pin_states();
    } else if (strncmp(msg, "OSCILLO:", 8) == 0) {
        oscillo_pin = atoi(msg + 8);
    } else if (strncmp(msg, "UART_SEND:", 10) == 0) {
        uart_send(msg + 10);
    } else if (strcmp(msg, "I2C_SCAN") == 0) {
        char buf[128];
        scan_i2c(buf, sizeof(buf));
        notify_clients(buf);
    } else if (strcmp(msg, "UART_READ") == 0) {
        char out[128] = "{\"uart\":\"";
        int len = strlen(out);
        uint8_t byte;
        while (uart_read_bytes(UART_PORT, &byte, 1, 10 / portTICK_PERIOD_MS) > 0 && len < 120) {
            out[len++] = byte;
        }
        out[len++] = '\"'; out[len++] = '}'; out[len] = 0;
        notify_clients(out);
    }

    free(frame.payload);
    return ESP_OK;
}



static httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
};

static void debounce_task(void* arg) {
    while (1) {
        bool changed = false;
        uint32_t now = esp_log_timestamp();
        for (int i = 0; i < NUM_PINS; i++) {
            if (!pin_modes[i]) {
                int level = gpio_get_level(usable_pins[i]);
                if (level != pin_states[i] && (now - last_change_times[i]) > 20) {
                    pin_states[i] = level;
                    bounce_counters[i]++;
                    last_change_times[i] = now;
                    changed = true;
                }
            }
        }
        if (changed) {
            send_all_pin_states();  
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void oscilloscope_task(void* arg) {
    while (1) {
        int raw = 0;
        adc_oneshot_read(adc_handle, OSCILLO_ADC_CHANNEL, &raw);
        float voltage = raw * 3.3f / 4095.0f;
        last_voltage = last_voltage * (1.0f - smoothing_factor) + voltage * smoothing_factor;

        char json[64];
        snprintf(json, sizeof(json), "{\"oscilloscope\":{\"pin\":%d,\"voltage\":%.2f}}", oscillo_pin, last_voltage);
        notify_clients(json);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void mount_spiffs() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 8,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

static esp_err_t static_file_handler(httpd_req_t *req) {
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);

    FILE* f = fopen(filepath, "r");
    if (!f) return httpd_resp_send_404(req);

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_sendstr_chunk(req, line);
    }
    fclose(f);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t index_handler(httpd_req_t *req) {
    FILE* f = fopen("/spiffs/index.html", "r");
    if (!f) return httpd_resp_send_404(req);

    char line[128];
    httpd_resp_set_type(req, "text/html");
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_sendstr_chunk(req, line);
    }
    fclose(f);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void start_web_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t static_handler = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler
    };
    httpd_register_uri_handler(server, &static_handler);

    httpd_register_uri_handler(server, &ws_uri);
}

static void wifi_init() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    memset(ws_clients, -1, sizeof(ws_clients));
    mount_spiffs();
    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(2000));  

    // UART Setup
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_PORT, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // I2C Setup
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA,
        .scl_io_num = I2C_MASTER_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };
    i2c_param_config(I2C_PORT, &i2c_cfg);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);

    // ADC Setup
    adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&adc_cfg, &adc_handle);
    adc_oneshot_config_channel(adc_handle, OSCILLO_ADC_CHANNEL, &(adc_oneshot_chan_cfg_t){
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_11
    });

    for (int i = 0; i < NUM_PINS; i++) {
        gpio_reset_pin(usable_pins[i]);
        gpio_set_direction(usable_pins[i], GPIO_MODE_INPUT);
        pin_modes[i] = false;
        pin_states[i] = gpio_get_level(usable_pins[i]);
        bounce_counters[i] = 0;
        last_change_times[i] = 0;
        pwm_frequencies[i] = 0;
    }

    start_web_server();
      

    xTaskCreate(debounce_task, "debounce_task", 4096, NULL, 5, NULL);
    xTaskCreate(oscilloscope_task, "oscilloscope_task", 4096, NULL, 5, NULL);
}
