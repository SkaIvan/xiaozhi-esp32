#include "radio_player.h"
#include "board.h"
#include "application.h"
#include "audio/audio_codec.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_mp3_dec.h>
#include <esp_crt_bundle.h>
#include <vector>

static const char* TAG = "RadioPlayer";

void RadioPlayer::Play(const std::string& url) {
    Stop();
    current_url_ = url;
    is_playing_ = true;
    xTaskCreatePinnedToCore(StreamTask, "radio_stream", 8192, this, 4, &task_handle_, 1);
}

void RadioPlayer::Stop() {
    is_playing_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(150));
        task_handle_ = nullptr;
    }
}

void RadioPlayer::StreamTask(void* param) {
    auto* self = static_cast<RadioPlayer*>(param);
    auto* codec = Board::GetInstance().GetAudioCodec();
    auto& app = Application::GetInstance();

    vTaskDelay(pdMS_TO_TICKS(1500));

    if (app.GetDeviceState() == kDeviceStateListening) {
        app.SetDeviceState(kDeviceStateIdle);
    }

    esp_http_client_config_t config = {};
    config.url = self->current_url_.c_str();
    config.buffer_size = 4096;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP stream");
        esp_http_client_cleanup(client);
        self->is_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_fetch_headers(client);

    void* decoder = nullptr;
    esp_mp3_dec_open(nullptr, 0, &decoder);

    std::vector<uint8_t> mp3_buf(2048);
    std::vector<int16_t> pcm_buf(2304);
    size_t bytes_left = 0;

    int target_rate = codec->output_sample_rate();
    if (target_rate <= 0) target_rate = 24000;

    codec->EnableOutput(true);

    while (self->is_playing_) {
        if (app.GetDeviceState() != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int read_len = esp_http_client_read(client, (char*)mp3_buf.data() + bytes_left, mp3_buf.size() - bytes_left);
        if (read_len <= 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        bytes_left += read_len;

        esp_audio_dec_in_raw_t raw = {
            .buffer = mp3_buf.data(),
            .len = (uint32_t)bytes_left,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = (uint8_t*)pcm_buf.data(),
            .len = (uint32_t)(pcm_buf.size() * sizeof(int16_t)),
        };

        if (esp_mp3_dec_decode(decoder, &raw, &out_frame, nullptr) == ESP_AUDIO_ERR_OK) {
            size_t consumed = raw.consumed;
            if (consumed < bytes_left) {
                memmove(mp3_buf.data(), mp3_buf.data() + consumed, bytes_left - consumed);
            }
            bytes_left -= consumed;

            if (out_frame.decoded_size > 0 && app.GetDeviceState() == kDeviceStateIdle) {
                size_t in_samples = out_frame.decoded_size / sizeof(int16_t);
                // Пересчитываем частоту из 44100 Гц (радио) в частоту кодека (target_rate)
                double ratio = 44100.0 / (double)target_rate;
                size_t out_samples = (size_t)(in_samples / ratio);
                if (out_samples > 0) {
                    std::vector<int16_t> output_samples(out_samples);
                    for (size_t i = 0; i < out_samples; i++) {
                        size_t src_idx = (size_t)(i * ratio);
                        if (src_idx >= in_samples) src_idx = in_samples - 1;
                        output_samples[i] = static_cast<int16_t>(pcm_buf[src_idx] * 0.45f);
                    }
                    codec->OutputData(output_samples);
                }
            }
        } else {
            if (bytes_left > 1) {
                memmove(mp3_buf.data(), mp3_buf.data() + 1, bytes_left - 1);
                bytes_left--;
            }
        }
    }

    esp_mp3_dec_close(decoder);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(nullptr);
}
