#pragma once
#include <string>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RadioPlayer {
public:
    static RadioPlayer& GetInstance() {
        static RadioPlayer instance;
        return instance;
    }

    void Play(const std::string& url);
    void Stop();
    bool IsPlaying() const { return is_playing_; }

private:
    RadioPlayer() = default;
    static void StreamTask(void* param);

    std::string current_url_;
    std::atomic<bool> is_playing_{false};
    TaskHandle_t task_handle_ = nullptr;
};
