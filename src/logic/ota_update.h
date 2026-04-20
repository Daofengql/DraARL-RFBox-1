#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <Arduino.h>

namespace ota_update {

enum class OTAState : uint8_t {
    IDLE = 0,
    CHECKING = 1,
    DOWNLOADING = 2,
    INSTALLING = 3,
    SUCCESS = 4,
    FAILED = 5,
};

struct FirmwareInfo {
    char version[32];
    char changelog[256];
    char download_url[256];
    char file_hash[65];
    char file_hash_algorithm[16];
    uint32_t file_size;
    bool has_update;
};

void init();
bool check_for_update(FirmwareInfo &info);
bool start_update(const char *url, const char *expected_hash, const char *expected_hash_algorithm = nullptr);
OTAState get_state();
uint8_t get_progress();
const char* get_error_message();
void reset_state();

} // namespace ota_update

#endif // OTA_UPDATE_H
