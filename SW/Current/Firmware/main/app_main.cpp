// MiniLoRa v2 — entry point.
//
// Boot-loop guard then one full wake→sense→transmit→sleep cycle.
// The cycle never returns: it ends in esp_deep_sleep_start().

#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "app/App.hpp"

extern "C" void app_main(void)
{
    static const char *TAG = "app";
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "MiniLoRa v%s boot (reset reason: %d)",
             CONFIG_MINILORA_FW_VERSION, static_cast<int>(reason));

    // Phase 6 will extend this with the full recovery-mode policy; for now
    // the boot-loop guard lives in DiagStore::onBoot (called in runCycle).
    minilora::runCycle(false);
}
