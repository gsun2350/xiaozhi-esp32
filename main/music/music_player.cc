#include "music_player.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include "audio_service.h"
#include "sd_card_manager.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "third_party/minimp3.h"

namespace {
constexpr char kTag[] = "MusicPlayer";
constexpr size_t kInputCapacity = 8192;
constexpr int kOutputSampleRate = 24000;

struct ResampleState {
    int source_rate = 0;
    uint64_t input_index = 0;
    uint64_t output_index = 0;
};

// 用整数相位保持跨 MP3 帧的采样位置，避免每帧取整造成播放速度漂移。
std::vector<int16_t> ResampleMono(const std::vector<int16_t>& input, int source_rate,
                                  ResampleState& state) {
    if (state.source_rate != source_rate) {
        state = {.source_rate = source_rate};
    }
    std::vector<int16_t> output;
    output.reserve(static_cast<size_t>(input.size()) * kOutputSampleRate / source_rate + 2);
    const uint64_t end_phase = (state.input_index + input.size()) * kOutputSampleRate;
    while (state.output_index * source_rate < end_phase) {
        const uint64_t position = state.output_index * source_rate;
        const size_t local_index =
            static_cast<size_t>(position / kOutputSampleRate - state.input_index);
        const int32_t left = input[local_index];
        const int32_t right = input[std::min(local_index + 1, input.size() - 1)];
        const int32_t fraction = static_cast<int32_t>(position % kOutputSampleRate);
        output.push_back(static_cast<int16_t>(
            (left * (kOutputSampleRate - fraction) + right * fraction) / kOutputSampleRate));
        ++state.output_index;
    }
    state.input_index += input.size();
    return output;
}

template <typename T>
T* AllocateBuffer(size_t count = 1) {
    void* memory = heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_8BIT);
    }
    return static_cast<T*>(memory);
}

}  // namespace

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer player;
    return player;
}

bool MusicPlayer::Initialize(AudioService& output) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ != nullptr) {
        return true;
    }
    output_ = &output;
    // 常驻任务只创建一次。大缓冲按曲目分配，并在停止或结束时释放。
    // MP3 解码的局部变量较多，预留 24 KB 栈处理不同码率曲目。
    if (xTaskCreate(RunTask, "tf_music", 24 * 1024, this, 3, &task_) != pdPASS) {
        task_ = nullptr;
        output_ = nullptr;
        error_ = "player_task_creation_failed";
        return false;
    }
    return true;
}

std::string MusicPlayer::Play(const std::string& song_id) {
    auto& card = SdCardManager::GetInstance();
    if (!card.IsValidSongId(song_id)) {
        return "invalid_song_id";
    }
    if (!card.IsMounted()) {
        return "card_not_mounted";
    }
    std::string file_song_id = song_id;
    if (!card.FileExists(file_song_id)) {
        const bool is_numeric =
            !song_id.empty() &&
            std::all_of(song_id.begin(), song_id.end(),
                        [](unsigned char value) { return value >= '0' && value <= '9'; });
        // 知识库 CSV 可能把 002 解析为 2；精确文件名优先，只有纯数字 ID 才补零。
        if (is_numeric && song_id.size() < 3) {
            std::string padded_id(3 - song_id.size(), '0');
            padded_id += song_id;
            if (card.FileExists(padded_id))
                file_song_id = std::move(padded_id);
        }
        if (!card.FileExists(file_song_id))
            return "file_not_found";
    }
    bool playing_now;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_ == nullptr) {
            return "player_unavailable";
        }
        ++generation_;
        song_id_ = file_song_id;
        position_ms_ = 0;
        error_.clear();
        state_ = voice_active_ ? MusicState::InterruptedByVoice : MusicState::Playing;
        playing_now = state_ == MusicState::Playing;
    }
    output_->SetMusicOutputEnabled(false);
    if (playing_now)
        output_->EnableWakeWordDetection(false);
    xTaskNotifyGive(task_);
    return "command_accepted";
}

std::string MusicPlayer::Pause() {
    bool restore_wake_word;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != MusicState::Playing && state_ != MusicState::InterruptedByVoice) {
            return "not_playing";
        }
        state_ = MusicState::Paused;
        restore_wake_word = !voice_active_;
    }
    output_->SetMusicOutputEnabled(false);
    if (restore_wake_word)
        output_->EnableWakeWordDetection(true);
    return "command_accepted";
}

std::string MusicPlayer::Resume() {
    bool playing_now;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != MusicState::Paused) {
            return "not_paused";
        }
        state_ = voice_active_ ? MusicState::InterruptedByVoice : MusicState::Playing;
        playing_now = state_ == MusicState::Playing;
    }
    if (playing_now)
        output_->EnableWakeWordDetection(false);
    xTaskNotifyGive(task_);
    return "command_accepted";
}

std::string MusicPlayer::Stop() {
    bool restore_wake_word;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        state_ = MusicState::Stopped;
        song_id_.clear();
        position_ms_ = 0;
        error_.clear();
        restore_wake_word = !voice_active_;
    }
    if (output_ != nullptr) {
        output_->SetMusicOutputEnabled(false);
        if (restore_wake_word)
            output_->EnableWakeWordDetection(true);
    }
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
    return "command_accepted";
}

std::vector<std::string> MusicPlayer::ScanPlaylist() const {
    std::vector<std::string> songs;
    if (!SdCardManager::GetInstance().IsMounted()) {
        return songs;
    }
    DIR* directory = opendir("/sdcard/music");
    if (directory == nullptr) {
        return songs;
    }
    while (dirent* entry = readdir(directory)) {
        std::string filename = entry->d_name;
        if (filename.size() < 5 || filename.substr(filename.size() - 4) != ".mp3") {
            continue;
        }
        std::string song_id = filename.substr(0, filename.size() - 4);
        if (SdCardManager::IsValidSongId(song_id) &&
            SdCardManager::GetInstance().FileExists(song_id)) {
            songs.push_back(std::move(song_id));
        }
    }
    closedir(directory);
    std::sort(songs.begin(), songs.end());
    return songs;
}

std::string MusicPlayer::SelectAdjacent(bool forward) {
    const auto songs = ScanPlaylist();
    if (songs.empty()) {
        return "playlist_empty";
    }
    std::string current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = song_id_;
    }
    auto position = std::find(songs.begin(), songs.end(), current);
    size_t index = 0;
    if (position != songs.end()) {
        size_t current_index = static_cast<size_t>(position - songs.begin());
        index = forward ? (current_index + 1) % songs.size()
                        : (current_index + songs.size() - 1) % songs.size();
    }
    return Play(songs[index]);
}

std::string MusicPlayer::Next() { return SelectAdjacent(true); }
std::string MusicPlayer::Previous() { return SelectAdjacent(false); }

void MusicPlayer::OnVoiceInteraction(bool active) {
    bool disable_output = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        voice_active_ = active;
        if (active && state_ == MusicState::Playing) {
            state_ = MusicState::InterruptedByVoice;
            disable_output = true;
        } else if (!active && state_ == MusicState::InterruptedByVoice) {
            state_ = MusicState::Playing;
        }
    }
    if (disable_output && output_ != nullptr) {
        output_->SetMusicOutputEnabled(false);
    }
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
}

MusicStatus MusicPlayer::GetStatus() const {
    MusicStatus result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.state = state_;
        result.song_id = song_id_;
        result.position_ms = position_ms_;
        result.error = error_;
    }
    result.card_mounted = SdCardManager::GetInstance().IsMounted();
    return result;
}

void MusicPlayer::RunTask(void* argument) {
    static_cast<MusicPlayer*>(argument)->Run();
    vTaskDelete(nullptr);
}

void MusicPlayer::Run() {
    // 在播放任务中扫描曲库，启动主线程无需等待 TF 目录遍历。
    const auto songs = ScanPlaylist();
    ESP_LOGI(kTag, "TF playlist: %u valid MP3 files", static_cast<unsigned>(songs.size()));
    for (const auto& song_id : songs)
        ESP_LOGI(kTag, "TF song_id: %s", song_id.c_str());
    FILE* file = nullptr;
    std::string opened_song;
    uint32_t opened_generation = 0;
    mp3dec_t* decoder = nullptr;
    uint8_t* input = nullptr;
    int16_t* pcm = nullptr;
    size_t available = 0;
    size_t offset = 0;
    bool eof = false;
    bool decoded_any = false;
    uint64_t submitted_samples = 0;
    ResampleState resample_state;

    auto close_track = [&]() {
        output_->SetMusicOutputEnabled(false);
        if (file != nullptr)
            fclose(file);
        if (decoder != nullptr)
            heap_caps_free(decoder);
        if (input != nullptr)
            heap_caps_free(input);
        if (pcm != nullptr)
            heap_caps_free(pcm);
        file = nullptr;
        decoder = nullptr;
        input = nullptr;
        pcm = nullptr;
        opened_song.clear();
        available = offset = 0;
        eof = false;
        decoded_any = false;
        submitted_samples = 0;
        resample_state = {};
    };

    while (true) {
        MusicState state;
        std::string song;
        uint32_t generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state = state_;
            song = song_id_;
            generation = generation_;
        }
        if (state == MusicState::Stopped || state == MusicState::Error || song.empty()) {
            if (file != nullptr)
                close_track();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (file != nullptr && (song != opened_song || generation != opened_generation)) {
            close_track();
        }
        if (state != MusicState::Playing) {
            output_->SetMusicOutputEnabled(false);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }
        if (!output_->IsPlaybackIdle()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        if (file == nullptr) {
            file = SdCardManager::GetInstance().OpenSong(song);
            decoder = AllocateBuffer<mp3dec_t>();
            input = AllocateBuffer<uint8_t>(kInputCapacity);
            pcm = AllocateBuffer<int16_t>(MINIMP3_MAX_SAMPLES_PER_FRAME);
            if (file == nullptr || decoder == nullptr || input == nullptr || pcm == nullptr) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = "open_or_allocation_failed";
                    state_ = MusicState::Error;
                }
                close_track();
                continue;
            }
            mp3dec_init(decoder);
            opened_song = song;
            opened_generation = generation;
        }

        output_->SetMusicOutputEnabled(true);
        if (available < 4096 && !eof) {
            if (available > 0 && offset > 0)
                memmove(input, input + offset, available);
            offset = 0;
            size_t bytes = fread(input + available, 1, kInputCapacity - available, file);
            available += bytes;
            if (bytes == 0) {
                eof = true;
            }
        }
        if (available == 0) {
            const int read_error = ferror(file);
            if (decoded_any && !read_error && !output_->IsMusicPlaybackIdle()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
                continue;
            }
            ESP_LOGI(kTag, "MP3 end: decoded=%d feof=%d ferror=%d", decoded_any, feof(file),
                     read_error);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation_ == generation) {
                    state_ = decoded_any && !read_error ? MusicState::Stopped : MusicState::Error;
                    if (state_ == MusicState::Error)
                        error_ = "invalid_or_unreadable_mp3";
                }
            }
            close_track();
            continue;
        }

        mp3dec_frame_info_t frame_info = {};
        int samples = mp3dec_decode_frame(decoder, input + offset, available, pcm, &frame_info);
        if (frame_info.frame_bytes > static_cast<int>(available)) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "invalid_mp3_frame";
            state_ = MusicState::Error;
            continue;
        }
        if (frame_info.frame_bytes > 0) {
            offset += frame_info.frame_bytes;
            available -= frame_info.frame_bytes;
        } else if (samples == 0) {
            // 损坏字节逐个跳过，保证解码循环始终前进。
            ++offset;
            --available;
        }
        if (samples <= 0)
            continue;
        if (!decoded_any) {
            ESP_LOGI(kTag, "MP3 format: %d Hz, %d channels", frame_info.hz, frame_info.channels);
        }
        decoded_any = true;
        if (frame_info.channels < 1 || frame_info.channels > 2 || frame_info.hz <= 0 ||
            samples * frame_info.channels > MINIMP3_MAX_SAMPLES_PER_FRAME) {
            continue;
        }

        std::vector<int16_t> mono(samples);
        for (int index = 0; index < samples; ++index) {
            mono[index] =
                frame_info.channels == 1
                    ? pcm[index]
                    : static_cast<int16_t>(
                          (static_cast<int32_t>(pcm[index * 2]) + pcm[index * 2 + 1]) / 2);
        }

        if (frame_info.hz == kOutputSampleRate)
            resample_state = {};
        std::vector<int16_t> output_pcm = frame_info.hz == kOutputSampleRate
                                              ? std::move(mono)
                                              : ResampleMono(mono, frame_info.hz, resample_state);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_ != generation || state_ != MusicState::Playing)
                continue;
        }
        const size_t output_samples = output_pcm.size();
        bool submitted = false;
        while (!submitted) {
            submitted = output_->SubmitMusicPcm(std::move(output_pcm));
            if (submitted)
                break;
            bool still_playing;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                still_playing = generation_ == generation && state_ == MusicState::Playing;
            }
            if (!still_playing)
                break;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            output_->SetMusicOutputEnabled(true);
        }
        if (submitted) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_ == generation) {
                submitted_samples += output_samples;
                position_ms_ = static_cast<uint32_t>(submitted_samples * 1000 / kOutputSampleRate);
            }
        }
    }
}
