#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AudioService;

enum class MusicState {
    Stopped,
    Playing,
    Paused,
    InterruptedByVoice,
    Error,
};

struct MusicStatus {
    MusicState state = MusicState::Stopped;
    std::string song_id;
    uint32_t position_ms = 0;
    std::string error;
    bool card_mounted = false;
};

class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    bool Initialize(AudioService& output);
    std::string Play(const std::string& song_id);
    std::string Pause();
    std::string Resume();
    std::string Stop();
    std::string Next();
    std::string Previous();
    std::vector<std::string> ScanPlaylist() const;
    void OnVoiceInteraction(bool active);
    MusicStatus GetStatus() const;

private:
    MusicPlayer() = default;
    static void RunTask(void* argument);
    void Run();
    std::string SelectAdjacent(bool forward);

    mutable std::mutex mutex_;
    AudioService* output_ = nullptr;
    TaskHandle_t task_ = nullptr;
    MusicState state_ = MusicState::Stopped;
    std::string song_id_;
    uint32_t position_ms_ = 0;
    std::string error_;
    bool voice_active_ = false;
    uint32_t generation_ = 0;
};
