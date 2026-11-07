#pragma once

#include <cstdio>
#include <mutex>
#include <string>

#include <sdmmc_cmd.h>

class SdCardManager {
public:
    static SdCardManager& GetInstance();

    bool Mount();
    bool IsMounted() const;
    bool FileExists(const std::string& song_id) const;
    FILE* OpenSong(const std::string& song_id) const;
    static bool IsValidSongId(const std::string& song_id);
    static std::string SongPath(const std::string& song_id);
    std::string LastError() const;

private:
    SdCardManager() = default;
    mutable std::mutex mutex_;
    sdmmc_card_t* card_ = nullptr;
    bool spi_initialized_ = false;
    std::string last_error_;
};
