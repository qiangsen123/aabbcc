#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"

#include "driver/i2s_std.h"
#include "cJSON.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec.h"

/* ---------- 用户配置 ---------- */

#define WIFI_SSID       "OPPOA5"
#define WIFI_PASSWORD   "12345678"

#define WS_PORT         8080

#define I2S_BCLK        4
#define I2S_DOUT        5
#define I2S_LRCK        6

#define HTTP_BUF_SIZE   4096
#define PCM_BUF_SIZE    (64 * 1024)
#define WS_MAX_MESSAGE  8192

static const char *TAG = "MUSICFREE";

/* ---------- 全局变量 ---------- */

static EventGroupHandle_t wifi_event_group;
static QueueHandle_t command_queue;

static i2s_chan_handle_t i2s_tx;
static httpd_handle_t http_server = NULL;

static int websocket_fd = -1;

static volatile bool player_paused = false;
static volatile bool player_stop = false;
static volatile bool session_running = false;

typedef enum {
    CMD_PLAY,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP
} command_type_t;

typedef struct {
    command_type_t type;
    char *json;
} player_command_t;

#define WIFI_CONNECTED_BIT BIT0

/* ---------- WebSocket 状态反馈 ---------- */

static void send_event(const char *event, const char *state,
                       const char *message)
{
    if (!http_server || websocket_fd < 0) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();

    if (!root || !data) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return;
    }

    cJSON_AddStringToObject(root, "event", event);

    if (state) {
        cJSON_AddStringToObject(data, "state", state);
    }

    if (message) {
        cJSON_AddStringToObject(data, "message", message);
    }

    cJSON_AddItemToObject(root, "data", data);

    char *json = cJSON_PrintUnformatted(root);

    if (json) {
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json)
        };

        httpd_ws_send_frame_async(
            http_server,
            websocket_fd,
            &frame
        );

        free(json);
    }

    cJSON_Delete(root);
}

/* ---------- Wi-Fi ---------- */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        xEventGroupClearBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");

        esp_wifi_connect();

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "ESP32 IP: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );

        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
        );
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        WIFI_SSID,
        sizeof(wifi_config.sta.ssid)
    );

    strncpy(
        (char *)wifi_config.sta.password,
        WIFI_PASSWORD,
        sizeof(wifi_config.sta.password)
    );

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );
}

/* ---------- I2S / PCM5102A ---------- */

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_0,
            I2S_ROLE_MASTER
        );

    chan_cfg.auto_clear = true;

    ESP_ERROR_CHECK(
        i2s_new_channel(
            &chan_cfg,
            &i2s_tx,
            NULL
        )
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),

        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
        ),

        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_LRCK,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,

            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(
        i2s_channel_init_std_mode(
            i2s_tx,
            &std_cfg
        )
    );

    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));

    ESP_LOGI(TAG, "PCM5102A I2S initialized");
}

static esp_err_t i2s_change_sample_rate(uint32_t rate)
{
    if (rate < 8000 || rate > 192000) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(i2s_channel_disable(i2s_tx));

    i2s_std_clk_config_t clk_cfg =
        I2S_STD_CLK_DEFAULT_CONFIG(rate);

    esp_err_t err =
        i2s_channel_reconfig_std_clock(
            i2s_tx,
            &clk_cfg
        );

    if (err == ESP_OK) {
        err = i2s_channel_enable(i2s_tx);
    }

    return err;
}

static esp_err_t i2s_write_pcm(
    const uint8_t *data,
    size_t length)
{
    size_t written = 0;

    while (written < length) {
        size_t current = 0;

        esp_err_t err = i2s_channel_write(
            i2s_tx,
            data + written,
            length - written,
            &current,
            portMAX_DELAY
        );

        if (err != ESP_OK) {
            return err;
        }

        written += current;
    }

    return ESP_OK;
}

/* ---------- 判断音频格式 ---------- */

static esp_audio_simple_dec_type_t get_decoder_type(
    const char *url)
{
    const char *end = strpbrk(url, "?#");

    size_t length = end
        ? (size_t)(end - url)
        : strlen(url);

    const char *dot = NULL;

    for (size_t i = 0; i < length; i++) {
        if (url[i] == '.') {
            dot = url + i;
        }
    }

    if (!dot) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }

    if (strncasecmp(dot, ".mp3", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }

    if (strncasecmp(dot, ".flac", 5) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }

    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/* ---------- PCM 转换为 16bit 双声道 ---------- */

static uint8_t *convert_pcm(
    const uint8_t *src,
    size_t src_len,
    uint8_t bits,
    uint8_t channels,
    size_t *out_len)
{
    *out_len = 0;

    if (channels < 1 || channels > 2) {
        return NULL;
    }

    if (bits != 16 && bits != 24 && bits != 32) {
        return NULL;
    }

    size_t bytes_per_sample = bits / 8;

    size_t samples = src_len / bytes_per_sample;

    size_t frames = samples / channels;

    size_t output_size =
        frames * 2 * sizeof(int16_t);

    int16_t *output = heap_caps_malloc(
        output_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!output) {
        return NULL;
    }

    for (size_t frame = 0; frame < frames; frame++) {

        for (int ch = 0; ch < 2; ch++) {

            size_t input_index =
                frame * channels +
                (channels == 1 ? 0 : ch);

            int32_t value = 0;

            if (bits == 16) {

                int16_t v;

                memcpy(
                    &v,
                    src + input_index * 2,
                    sizeof(v)
                );

                value = v;

            } else if (bits == 24) {

                const uint8_t *p =
                    src + input_index * 3;

                value =
                    (int32_t)(
                        (uint32_t)p[0] |
                        ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16)
                    );

                if (value & 0x800000) {
                    value |= (int32_t)0xFF000000;
                }

                value >>= 8;

            } else {

                int32_t v;

                memcpy(
                    &v,
                    src + input_index * 4,
                    sizeof(v)
                );

                value = v >> 16;
            }

            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;

            output[frame * 2 + ch] = (int16_t)value;
        }
    }

    *out_len = output_size;

    return (uint8_t *)output;
}

/* ---------- 网络音频播放任务 ---------- */

static void playback_task(void *arg)
{
    char *json_text = (char *)arg;

    cJSON *root = cJSON_Parse(json_text);

    cJSON *data = cJSON_GetObjectItem(root, "data");

    cJSON *url_item = cJSON_GetObjectItem(data, "url");

    cJSON *headers = cJSON_GetObjectItem(data, "headers");

    esp_http_client_handle_t client = NULL;

    esp_audio_simple_dec_handle_t decoder = NULL;

    uint8_t *input_buffer = NULL;
    uint8_t *output_buffer = NULL;

    if (!cJSON_IsString(url_item)) {
        send_event("error", "error", "Missing audio URL");
        goto cleanup;
    }

    const char *url = url_item->valuestring;

    esp_audio_simple_dec_type_t decoder_type =
        get_decoder_type(url);

    if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        send_event(
            "error",
            "error",
            "Only direct MP3 and FLAC URLs are supported"
        );

        goto cleanup;
    }

    /* HTTP / HTTPS */

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = HTTP_BUF_SIZE,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .max_redirection_count = 5
    };

    client = esp_http_client_init(&http_cfg);

    if (!client) {
        send_event("error", "error", "HTTP init failed");
        goto cleanup;
    }

    /* 请求 Headers */

    if (cJSON_IsObject(headers)) {

        cJSON *header = NULL;

        cJSON_ArrayForEach(header, headers) {

            if (header->string &&
                cJSON_IsString(header)) {

                esp_http_client_set_header(
                    client,
                    header->string,
                    header->valuestring
                );
            }
        }
    }

    esp_err_t err = esp_http_client_open(client, 0);

    if (err != ESP_OK) {
        send_event("error", "error", "HTTP connection failed");
        goto cleanup;
    }

    int64_t content_length =
        esp_http_client_fetch_headers(client);

    int status =
        esp_http_client_get_status_code(client);

    ESP_LOGI(
        TAG,
        "HTTP status=%d, length=%lld",
        status,
        (long long)content_length
    );

    if (status < 200 || status >= 300) {
        send_event("error", "error", "HTTP server returned error");
        goto cleanup;
    }

    /* 打开解码器 */

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = decoder_type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false
    };

    esp_audio_err_t audio_err =
        esp_audio_simple_dec_open(
            &dec_cfg,
            &decoder
        );

    if (audio_err != ESP_AUDIO_ERR_OK) {
        send_event("error", "error", "Decoder open failed");
        goto cleanup;
    }

    input_buffer = heap_caps_malloc(
        HTTP_BUF_SIZE + 16,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    output_buffer = heap_caps_malloc(
        PCM_BUF_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!input_buffer || !output_buffer) {
        send_event("error", "error", "PSRAM allocation failed");
        goto cleanup;
    }

    player_stop = false;
    session_running = true;

    send_event("state", "playing", NULL);

    size_t pending = 0;
    bool eof = false;
    uint32_t current_rate = 0;

    while (!player_stop) {

        while (player_paused && !player_stop) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }

        if (player_stop) {
            break;
        }

        /* 保留上一次没有被解码器消费的数据 */

        int n = 0;

        if (!eof) {
            n = esp_http_client_read(
                client,
                (char *)input_buffer + pending,
                HTTP_BUF_SIZE - pending
            );

            if (n < 0) {
                send_event("error", "error", "HTTP read failed");
                break;
            }

            if (n == 0) {
                eof = true;
            }
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input_buffer,
            .len = pending + (size_t)n,
            .eos = eof,
            .consumed = 0
        };

        do {

            esp_audio_simple_dec_out_t out = {
                .buffer = output_buffer,
                .len = PCM_BUF_SIZE,
                .needed_size = 0,
                .decoded_size = 0
            };

            audio_err = esp_audio_simple_dec_process(
                decoder,
                &raw,
                &out
            );

            if (audio_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {

                if (out.needed_size > 1024 * 1024) {
                    send_event("error", "error", "Decoder output too large");
                    player_stop = true;
                    break;
                }

                uint8_t *larger = heap_caps_realloc(
                    output_buffer,
                    out.needed_size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                );

                if (!larger) {
                    player_stop = true;
                    break;
                }

                output_buffer = larger;
                continue;
            }

            if (audio_err != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Decoder error: %d", audio_err);
                player_stop = true;
                break;
            }

            if (out.decoded_size > 0) {

                esp_audio_simple_dec_info_t info = {0};

                if (esp_audio_simple_dec_get_info(
                        decoder,
                        &info) == ESP_AUDIO_ERR_OK) {

                    if (info.sample_rate &&
                        info.sample_rate != current_rate) {

                        if (i2s_change_sample_rate(
                                info.sample_rate) == ESP_OK) {

                            current_rate = info.sample_rate;

                            ESP_LOGI(
                                TAG,
                                "Sample rate: %lu",
                                (unsigned long)current_rate
                            );
                        }
                    }

                    size_t pcm_len = 0;

                    uint8_t *pcm16 = convert_pcm(
                        out.buffer,
                        out.decoded_size,
                        info.bits_per_sample,
                        info.channel,
                        &pcm_len
                    );

                    if (pcm16 && pcm_len) {

                        i2s_write_pcm(pcm16, pcm_len);

                        heap_caps_free(pcm16);

                    } else {

                        send_event(
                            "error",
                            "error",
                            "Unsupported PCM format"
                        );

                        player_stop = true;
                        break;
                    }
                }
            }

            /* 更新输入指针，不能覆盖未消费的数据 */

            if (raw.consumed == 0) {
                break;
            }

            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
            raw.consumed = 0;

        } while (raw.len > 0 && !player_stop);

        pending = raw.len;

        if (pending > 0) {
            memmove(
                input_buffer,
                raw.buffer,
                pending
            );
        }

        if (eof) {
            break;
        }
    }

    if (!player_stop) {
        send_event("state", "stopped", "end_of_stream");
    } else {
        send_event("state", "stopped", "stopped_by_user");
    }

cleanup:

    session_running = false;

    if (decoder) {
        esp_audio_simple_dec_close(decoder);
    }

    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (input_buffer) {
        heap_caps_free(input_buffer);
    }

    if (output_buffer) {
        heap_caps_free(output_buffer);
    }

    if (root) {
        cJSON_Delete(root);
    }

    free(json_text);

    player_paused = false;

    vTaskDelete(NULL);
}

/* ---------- 播放控制任务 ---------- */

static void player_manager_task(void *arg)
{
    player_command_t cmd;

    while (1) {

        if (xQueueReceive(
                command_queue,
                &cmd,
                portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (cmd.type) {

        case CMD_PLAY:

            /* 停止旧曲目 */

            player_stop = true;
            player_paused = false;

            while (session_running) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            player_stop = false;

            if (cmd.json) {
                xTaskCreatePinnedToCore(
                    playback_task,
                    "audio_playback",
                    12288,
                    cmd.json,
                    6,
                    NULL,
                    1
                );
            }

            break;

        case CMD_PAUSE:

            player_paused = true;

            send_event("state", "paused", NULL);

            break;

        case CMD_RESUME:

            player_paused = false;

            send_event("state", "playing", NULL);

            break;

        case CMD_STOP:

            player_stop = true;
            player_paused = false;

            send_event("state", "stopping", NULL);

            break;
        }
    }
}

/* ---------- WebSocket 服务端 ---------- */

static esp_err_t websocket_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {

        websocket_fd = httpd_req_to_sockfd(req);

        ESP_LOGI(TAG, "WebSocket client connected");

        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT
    };

    esp_err_t err = httpd_ws_recv_frame(
        req,
        &frame,
        0
    );

    if (err != ESP_OK ||
        frame.len == 0 ||
        frame.len > WS_MAX_MESSAGE) {

        return ESP_FAIL;
    }

    char *payload = calloc(
        1,
        frame.len + 1
    );

    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    frame.payload = (uint8_t *)payload;

    err = httpd_ws_recv_frame(
        req,
        &frame,
        frame.len
    );

    if (err != ESP_OK) {
        free(payload);
        return err;
    }

    cJSON *root = cJSON_Parse(payload);

    cJSON *action =
        cJSON_GetObjectItem(root, "action");

    player_command_t cmd = {
        .json = NULL
    };

    bool valid = true;

    if (!cJSON_IsString(action)) {

        valid = false;

    } else if (!strcmp(action->valuestring, "play")) {

        cmd.type = CMD_PLAY;
        cmd.json = strdup(payload);

        if (!cmd.json) {
            valid = false;
        }

    } else if (!strcmp(action->valuestring, "pause")) {

        cmd.type = CMD_PAUSE;

    } else if (!strcmp(action->valuestring, "resume")) {

        cmd.type = CMD_RESUME;

    } else if (!strcmp(action->valuestring, "stop")) {

        cmd.type = CMD_STOP;

    } else {

        valid = false;
    }

    if (valid) {

        if (xQueueSend(
                command_queue,
                &cmd,
                0) != pdTRUE) {

            if (cmd.json) {
                free(cmd.json);
            }

            valid = false;
        }
    }

    if (!valid) {
        send_event("error", "error", "Invalid command");
    }

    cJSON_Delete(root);
    free(payload);

    return ESP_OK;
}

static void websocket_server_start(void)
{
    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.server_port = WS_PORT;
    config.stack_size = 8192;

    ESP_ERROR_CHECK(
        httpd_start(
            &http_server,
            &config
        )
    );

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };

    ESP_ERROR_CHECK(
        httpd_register_uri_handler(
            http_server,
            &ws_uri
        )
    );

    ESP_LOGI(
        TAG,
        "WebSocket: ws://ESP32_IP:%d/ws",
        WS_PORT
    );
}

/* ---------- 程序入口 ---------- */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());

        ESP_ERROR_CHECK(nvs_flash_init());
    }

    command_queue = xQueueCreate(
        8,
        sizeof(player_command_t)
    );

    ESP_ERROR_CHECK(
        command_queue ? ESP_OK : ESP_ERR_NO_MEM
    );

    /* 初始化音频解码器注册表 */

    ESP_ERROR_CHECK(
        esp_audio_dec_register_default()
    );

    ESP_ERROR_CHECK(
        esp_audio_simple_dec_register_default()
    );

    wifi_init();

    i2s_init();

    xTaskCreatePinnedToCore(
        player_manager_task,
        "player_manager",
        4096,
        NULL,
        8,
        NULL,
        0
    );

    websocket_server_start();

    ESP_LOGI(TAG, "MusicFree ESP32 player ready");
}