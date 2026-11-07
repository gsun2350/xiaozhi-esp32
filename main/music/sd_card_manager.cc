#include "sd_card_manager.h"

#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sys/stat.h>

#include "boards/nologo/xingzhi-cube-2.0tft-wifi/config.h"

namespace {
constexpr char kTag[] = "SdCardManager";
}

SdCardManager& SdCardManager::GetInstance() {
    static SdCardManager manager;
    return manager;
}

bool SdCardManager::Mount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (card_ != nullptr) {
        return true;
    }

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = SD_MOSI;
    bus_config.miso_io_num = SD_MISO;
    bus_config.sclk_io_num = SD_SCLK;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = 4096;
    esp_err_t error = spi_bus_initialize(SD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        last_error_ = esp_err_to_name(error);
        ESP_LOGW(kTag, "TF SPI initialization failed: %s", last_error_.c_str());
        return false;
    }
    spi_initialized_ = (error == ESP_OK);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = SD_CS;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;
    error = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &card_);
    if (error != ESP_OK) {
        card_ = nullptr;
        last_error_ = esp_err_to_name(error);
        ESP_LOGW(kTag, "TF mount failed; AI remains available: %s", last_error_.c_str());
        if (spi_initialized_) {
            spi_bus_free(SD_SPI_HOST);
            spi_initialized_ = false;
        }
        return false;
    }
    last_error_.clear();
    ESP_LOGI(kTag, "TF card mounted: %llu MB",
             static_cast<unsigned long long>(card_->csd.capacity) * card_->csd.sector_size /
                 (1024 * 1024));
    return true;
}

bool SdCardManager::IsMounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return card_ != nullptr;
}

bool SdCardManager::IsValidSongId(const std::string& song_id) {
    if (song_id.empty() || song_id.size() > 32) {
        return false;
    }
    for (unsigned char character : song_id) {
        if (!((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

std::string SdCardManager::SongPath(const std::string& song_id) {
    if (!IsValidSongId(song_id)) {
        return {};
    }
    return std::string(SD_MOUNT_POINT) + "/music/" + song_id + ".mp3";
}

bool SdCardManager::FileExists(const std::string& song_id) const {
    const auto path = SongPath(song_id);
    if (path.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (card_ == nullptr) {
        return false;
    }
    struct stat information = {};
    return stat(path.c_str(), &information) == 0 && S_ISREG(information.st_mode);
}

FILE* SdCardManager::OpenSong(const std::string& song_id) const {
    const auto path = SongPath(song_id);
    if (path.empty()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return card_ == nullptr ? nullptr : fopen(path.c_str(), "rb");
}

std::string SdCardManager::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}
