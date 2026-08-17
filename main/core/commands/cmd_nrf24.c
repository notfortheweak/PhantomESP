// cmd_nrf24.c
// NRF24 analyzer command.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/nrf24_remote_manager.h"
#include "sdkconfig.h"
#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE))
#include "managers/views/nrf24_analyzer_view.h"
#endif
#include <string.h>

void handle_nrf24_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: nrf24 <start|stop|pause|resume|status|state>\n");
        return;
    }

    const char *sub = argv[1];
    (void)sub;
    bool remote_request = false;

#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE))
    if (strcmp(sub, "state") == 0) {
        if (argc >= 3) {
            nrf24_analyzer_view_update_remote_state(argv[2]);
        }
        return;
    }
#endif

#ifdef CONFIG_HAS_NRF24
    bool stream_to_peer = remote_request && false;

    if (strcmp(sub, "start") == 0) {
        bool ok = nrf24_remote_manager_start(stream_to_peer);
        if (ok) {
            glog("NRF24 analyzer started\n");
            glog("NRF24 cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d\n",
                 CONFIG_NRF24_SPI_HOST,
                 CONFIG_NRF24_SPI_MOSI_PIN,
                 CONFIG_NRF24_SPI_MISO_PIN,
                 CONFIG_NRF24_SPI_SCK_PIN,
                 CONFIG_NRF24_CSN_PIN,
                 CONFIG_NRF24_CE_PIN);
        } else {
            glog("NRF24 analyzer failed to start: %s\n", nrf24_remote_manager_get_last_error());
            glog("NRF24 cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d\n",
                 CONFIG_NRF24_SPI_HOST,
                 CONFIG_NRF24_SPI_MOSI_PIN,
                 CONFIG_NRF24_SPI_MISO_PIN,
                 CONFIG_NRF24_SPI_SCK_PIN,
                 CONFIG_NRF24_CSN_PIN,
                 CONFIG_NRF24_CE_PIN);
        }
        return;
    }

    if (strcmp(sub, "stop") == 0) {
        if (!nrf24_remote_manager_is_running()) {
            glog("NRF24 analyzer already stopped\n");
            return;
        }
        nrf24_remote_manager_stop();
        glog("NRF24 analyzer stopping\n");
        return;
    }

    if (strcmp(sub, "pause") == 0) {
        if (!nrf24_remote_manager_is_running()) {
            glog("NRF24 analyzer is not running\n");
            return;
        }
        nrf24_remote_manager_set_paused(true);
        glog("NRF24 analyzer paused\n");
        return;
    }

    if (strcmp(sub, "resume") == 0) {
        if (!nrf24_remote_manager_is_running()) {
            glog("NRF24 analyzer is not running\n");
            return;
        }
        nrf24_remote_manager_set_paused(false);
        glog("NRF24 analyzer resumed\n");
        return;
    }

    if (strcmp(sub, "status") == 0) {
        glog("NRF24 running: %s\n", nrf24_remote_manager_is_running() ? "yes" : "no");
        glog("NRF24 paused: %s\n", nrf24_remote_manager_is_paused() ? "yes" : "no");
        glog("NRF24 last error: %s\n", nrf24_remote_manager_get_last_error());
        glog("NRF24 cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d\n",
             CONFIG_NRF24_SPI_HOST,
             CONFIG_NRF24_SPI_MOSI_PIN,
             CONFIG_NRF24_SPI_MISO_PIN,
             CONFIG_NRF24_SPI_SCK_PIN,
             CONFIG_NRF24_CSN_PIN,
             CONFIG_NRF24_CE_PIN);
        return;
    }

    glog("Unknown nrf24 subcommand: %s\n", sub);
#else
#ifdef CONFIG_HAS_NRF24_REMOTE
    glog("NRF24 local scanner not enabled on this build (remote/display role only)\n");
#else
    glog("NRF24 not enabled on this build\n");
#endif
    (void)remote_request;
#endif
}
