// wifi_manager.c

#include "managers/wifi_manager.h"
#include "managers/ghostscript_runtime.h"
#include "core/callbacks.h"  // For callback function declarations
#include "core/network_constants.h" // For common port definitions
#include "core/ouis.h"       // For OUI vendor lookup
#include "vendor/pcap.h"     // For pcap_is_wireshark_mode()
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h" // Add include for heap stats
#include "core/memory_debug.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/ota_manager.h"
#include "managers/peer_ota_manager.h"
#include "managers/self_ota_manager.h"
#include "managers/status_display_manager.h"
#include "gui/toast.h"
#include "nvs_flash.h"
#include <core/dns_server.h>
#include <ctype.h>
#include <dhcpserver/dhcpserver.h>
#include <esp_http_server.h>
#include <esp_random.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mdns.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#if defined(CONFIG_WITH_SCREEN) || defined(WITH_SCREEN)
#include "managers/views/music_visualizer.h"
#endif
#include "managers/sd_card_manager.h"
#include "managers/wigle_manager.h"
#include "core/scan_saver.h"
#include "managers/views/terminal_screen.h"
#include "core/glog.h"
#include "core/ghostesp_version.h"
#include "core/esp_comm_manager.h"
#include "core/utils.h" // Add utils include
#include <inttypes.h>
#include "core/commandline.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/entropy.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/private/hmac_drbg.h"
#include "mbedtls/private/bignum.h"
#include "core/serial_manager.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/airspace_monitor.h"
#include "scans/wifi/station_scan.h"
#include "scans/wifi/wifi_channels.h"

void music_visualizer_view_update(const uint8_t *amplitudes,
                                  const char *track_name,
                                  const char *artist_name);

#include "core/network_constants.h"

#define MAX_DEVICES 255
#define CHUNK_SIZE 4096
#define MDNS_NAME_BUF_LEN 65
#define ARP_DELAY_MS 500

// Persistent mDNS result storage (heap-allocated)
static mdns_device_t *g_mdns_results = NULL;
static int g_mdns_result_count = 0;
static volatile bool g_mdns_scan_running = false;
static volatile bool g_mdns_scan_done = false;

#define BEACON_LIST_MAX 16
#define BEACON_SSID_MAX_LEN 32

// limit how many ap records we keep to avoid memory bloat/crashes
#define MAX_SCANNED_APS 100


// Forward declarations for live AP scan
static void live_ap_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
static esp_err_t start_live_ap_channel_hopping(void);
static void stop_live_ap_channel_hopping(void);
static bool callback_uses_selected_ap_capture_plan(wifi_promiscuous_cb_t_t callback);
static void apply_selected_ap_capture_channel_plan(wifi_promiscuous_cb_t_t callback);
static esp_timer_handle_t live_ap_channel_hop_timer = NULL;
static volatile bool live_ap_hopping_active = false;
static uint32_t last_live_print_ms = 0;
static uint16_t live_last_printed_index = 0;

#if defined(CONFIG_IDF_TARGET_ESP32C5)
static const uint8_t live_ap_channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,
    36,40,44,48,52,56,60,64,
    100,104,108,112,116,120,124,128,132,136,140,144,
    149,153,157,161,165
};
#else
static const uint8_t live_ap_channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13
};
#endif
static const size_t live_ap_channels_len = sizeof(live_ap_channels) / sizeof(live_ap_channels[0]);
static size_t live_ap_channel_index = 0;

const char *TAG = "WiFiManager";

// Station scan variables moved to station_scan.c module
bool manual_disconnect = false;
static volatile bool wifi_connect_cancel_requested = false;
static esp_timer_handle_t wifi_reconnect_timer = NULL;
static int wifi_reconnect_count = 0;
static volatile bool wifi_monitor_capture_active = false;
static volatile bool wifi_timed_scan_active = false;
#define WIFI_MAX_RECONNECT_ATTEMPTS  5
#define WIFI_OTA_AUTO_CHECK_TIMEOUT_MS 30000
static volatile bool visualizer_stop_requested = false;
static volatile int visualizer_socket = -1;
static volatile bool ota_auto_check_running = false;
static volatile bool ota_auto_check_done = false;

volatile bool ap_sta_has_ip = false;

// Port definitions moved to core/network_constants.c

EXT_RAM_BSS_ATTR static char PORTALURL[512] = "";
EXT_RAM_BSS_ATTR static char domain_str[128] = "";
EventGroupHandle_t wifi_event_group;
wifi_ap_record_t selected_ap;
wifi_ap_record_t *selected_aps = NULL;
int selected_ap_count = 0;
// selected_station and station_selected moved to station_scan.c module
bool redirect_handled = false;
dns_server_handle_t dns_handle;
static portMUX_TYPE dns_handle_mux = portMUX_INITIALIZER_UNLOCKED;

dns_server_handle_t dns_handle_take(void) {
    portENTER_CRITICAL(&dns_handle_mux);
    dns_server_handle_t h = dns_handle;
    dns_handle = NULL;
    portEXIT_CRITICAL(&dns_handle_mux);
    return h;
}

esp_netif_t *wifiAP;
esp_netif_t *wifiSTA;
static bool login_done = false;
static int ap_connection_count = 0;

#define MAX_HTML_BUFFER_SIZE 2048

static char* html_buffer = NULL;
static size_t html_buffer_size = 0;
static bool use_html_buffer = false;

static SemaphoreHandle_t g_wifi_ctrl_mutex = NULL;
static bool g_ap_diag_registered = false;
static esp_event_handler_instance_t g_ap_diag_wifi_inst;
static esp_event_handler_instance_t g_ap_diag_ip_inst;

static void wifi_ap_diag_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                       void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "ap: started");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "ap: stopped");
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                const wifi_event_ap_staconnected_t *e = (const wifi_event_ap_staconnected_t *)event_data;
                if (e) {
                    ESP_LOGI(TAG, "ap: sta connected: %02x:%02x:%02x:%02x:%02x:%02x aid=%d",
                             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], (int)e->aid);
                }
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                const wifi_event_ap_stadisconnected_t *e = (const wifi_event_ap_stadisconnected_t *)event_data;
                if (e) {
                    ESP_LOGI(TAG, "ap: sta disconnected: %02x:%02x:%02x:%02x:%02x:%02x aid=%d reason=%d",
                             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                             (int)e->aid, (int)e->reason);
                }
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_AP_STAIPASSIGNED: {
                const ip_event_ap_staipassigned_t *e = (const ip_event_ap_staipassigned_t *)event_data;
                if (e) {
                    ESP_LOGI(TAG, "ap: dhcp assigned ip: " IPSTR, IP2STR(&e->ip));
                } else {
                    ESP_LOGI(TAG, "ap: dhcp assigned ip");
                }
                break;
            }
            default:
                break;
        }
    }
}

static bool wifi_ctrl_lock(TickType_t ticks_to_wait) {
    if (g_wifi_ctrl_mutex == NULL) {
        g_wifi_ctrl_mutex = xSemaphoreCreateMutex();
        if (g_wifi_ctrl_mutex == NULL) return false;
    }
    return xSemaphoreTake(g_wifi_ctrl_mutex, ticks_to_wait) == pdTRUE;
}

static void wifi_ctrl_unlock(void) {
    if (g_wifi_ctrl_mutex) xSemaphoreGive(g_wifi_ctrl_mutex);
}

static esp_err_t wifi_stop_safely(void) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t st = esp_wifi_get_mode(&mode);
    if (st == ESP_ERR_WIFI_NOT_INIT) return ESP_OK;
    if (st != ESP_OK) return st;

    esp_err_t r = esp_wifi_stop();
    if (r == ESP_ERR_WIFI_NOT_STARTED) return ESP_OK;
    return r;
}

// single reusable transfer buffer for streaming to reduce heap churn
static char *g_stream_buf = NULL;
static SemaphoreHandle_t g_stream_buf_mutex = NULL;
static inline bool stream_buf_lock(void) {
    if (g_stream_buf_mutex == NULL) {
        g_stream_buf_mutex = xSemaphoreCreateMutex();
        if (g_stream_buf_mutex == NULL) return false;
    }
    // Bounded wait: prevents one stalled client from monopolizing the shared
    // streaming buffer when the portal is under heavy load.
    if (xSemaphoreTake(g_stream_buf_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
    if (g_stream_buf == NULL) {
#if CONFIG_SPIRAM
        g_stream_buf = (char *)heap_caps_malloc(CHUNK_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_stream_buf == NULL) {
            ESP_LOGW(TAG, "PSRAM g_stream_buf failed, trying internal RAM");
            g_stream_buf = (char *)malloc(CHUNK_SIZE + 1);
        }
#else
        g_stream_buf = (char *)malloc(CHUNK_SIZE + 1);
#endif
        if (g_stream_buf == NULL) {
            xSemaphoreGive(g_stream_buf_mutex);
            return false;
        }
    }
    return true;
}
static inline void stream_buf_unlock(void) {
    if (g_stream_buf_mutex) {
        xSemaphoreGive(g_stream_buf_mutex);
    }
}

void wifi_manager_release_stream_buffer(void) {
    SemaphoreHandle_t mutex = g_stream_buf_mutex;
    if (!mutex) return;

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    free(g_stream_buf);
    g_stream_buf = NULL;
    g_stream_buf_mutex = NULL;
    xSemaphoreGive(mutex);
    vSemaphoreDelete(mutex);
}

// Station scan channel hopping moved to station_scan.c module

// Wireshark Capture Channel Hopping Globals
static esp_timer_handle_t wireshark_channel_hop_timer = NULL;
static size_t wireshark_channel_index = 0;
static bool wireshark_hopping_active = false;
#define WIRESHARK_CHANNEL_HOP_INTERVAL_MS 150
uint8_t wireshark_channels[50];
size_t wireshark_channels_count = 0;

// Helper function forward declaration
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size);

struct service_info {
    const char *query;
    const char *type;
};

// Store in flash: const ensures this large-ish static table is placed in .rodata
static const struct service_info services[] = {{"_http", "Web Server Enabled Device"},
                                              {"_ssh", "SSH Server"},
                                              {"_ipp", "Printer (IPP)"},
                                              {"_googlecast", "Google Cast"},
                                              {"_raop", "AirPlay"},
                                              {"_smb", "SMB File Sharing"},
                                              {"_hap", "HomeKit Accessory"},
                                              {"_spotify-connect", "Spotify Connect Device"},
                                              {"_printer", "Printer (Generic)"},
                                              {"_mqtt", "MQTT Broker"}};

#define NUM_SERVICES (sizeof(services) / sizeof(services[0]))

struct DeviceInfo {
    struct ip4_addr ip;
    struct eth_addr mac;
};

static void wifi_reconnect_timer_stop(void) {
    if (wifi_reconnect_timer) {
        esp_timer_stop(wifi_reconnect_timer);
        esp_timer_delete(wifi_reconnect_timer);
        wifi_reconnect_timer = NULL;
    }
}

static bool wifi_reconnect_hold = false;

static bool wifi_reconnect_blocked(const char **reason_out) {
    if (wifi_reconnect_hold) {
        if (reason_out) *reason_out = "radio reserved";
        return true;
    }
    if (wifi_monitor_capture_active) {
        if (reason_out) *reason_out = "monitor mode";
        return true;
    }

    if (ap_scan_is_running() || wifi_timed_scan_active) {
        if (reason_out) *reason_out = "AP scan active";
        return true;
    }

    bool promiscuous_enabled = false;
    esp_err_t promisc_err = esp_wifi_get_promiscuous(&promiscuous_enabled);
    if (promisc_err == ESP_OK && promiscuous_enabled) {
        if (reason_out) *reason_out = "promiscuous mode";
        return true;
    }

    return false;
}

void wifi_manager_set_reconnect_hold(bool hold) {
    wifi_reconnect_hold = hold;
    if (hold) wifi_manager_stop_reconnect();
}

static void wifi_reconnect_reset(void) {
    wifi_reconnect_timer_stop();
    wifi_reconnect_count = 0;
}

static void wifi_reconnect_timer_cb(void *arg) {
    const char *reason = NULL;
    if (wifi_reconnect_blocked(&reason)) {
        ESP_LOGI(TAG, "Skipping auto-reconnect while %s", reason ? reason : "busy");
        if (wifi_reconnect_timer) {
            esp_timer_start_once(wifi_reconnect_timer, 3000 * 1000);
        }
        return;
    }

    const char *saved_ssid = settings_get_sta_ssid(&G_Settings);
    if (saved_ssid && strlen(saved_ssid) > 0) {
        int saved_count = wifi_reconnect_count;
        glog("Auto-reconnect attempt %d/%d to %s\n", saved_count, WIFI_MAX_RECONNECT_ATTEMPTS, saved_ssid);
        wifi_manager_configure_sta_from_settings();
        wifi_reconnect_count = saved_count;
    }
}

static void wifi_reconnect_schedule(void) {
    wifi_reconnect_timer_stop();

    if (!settings_get_wifi_auto_reconnect(&G_Settings)) {
        return;
    }

    if (wifi_reconnect_count > 0 && wifi_reconnect_count <= WIFI_MAX_RECONNECT_ATTEMPTS) {
        static const int backoff_ms[] = {3000, 5000, 10000, 20000, 30000};
        int delay_ms = backoff_ms[wifi_reconnect_count - 1];
        esp_timer_create_args_t args = {
            .callback = wifi_reconnect_timer_cb,
            .name = "wifi_reconnect"
        };
        if (esp_timer_create(&args, &wifi_reconnect_timer) == ESP_OK) {
            esp_timer_start_once(wifi_reconnect_timer, delay_ms * 1000);
        }
    }
}

void wifi_manager_set_manual_disconnect(bool disconnect) {
    manual_disconnect = disconnect;
}

void wifi_manager_cancel_connect(void) {
    wifi_connect_cancel_requested = true;
    wifi_ap_record_t ap_info;
    manual_disconnect = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    wifi_reconnect_reset();
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "cancel_connect: esp_wifi_disconnect returned %s", esp_err_to_name(err));
    }
}

void wifi_manager_stop_reconnect(void) {
    wifi_reconnect_reset();
}

void wifi_manager_stop_visualizer(void) {
    visualizer_stop_requested = true;

    if (visualizer_socket >= 0) {
        shutdown(visualizer_socket, 0);
    }
}

void wifi_manager_start_visualizer(bool for_screen) {
    if (VisualizerHandle != NULL) {
        return;
    }

    if (for_screen) {
#if defined(CONFIG_WITH_SCREEN) || defined(WITH_SCREEN)
        xTaskCreate(screen_music_visualizer_task, "udp_server", 4096, NULL, 5, &VisualizerHandle);
#endif
    } else {
        xTaskCreate(animate_led_based_on_amplitude, "udp_server", 4096, NULL, 5, &VisualizerHandle);
    }
}

static void tolower_str(const uint8_t *src, char *dst) {
    for (int i = 0; i < 33 && src[i] != '\0'; i++) {
        dst[i] = tolower((char)src[i]);
    }
    dst[32] = '\0'; // Ensure null-termination
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            glog("WiFi_manager: AP started\n");
            break;
        case WIFI_EVENT_AP_STOP:
            glog("WiFi_manager: AP stopped\n");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ap_connection_count++;
            glog("WiFi_manager: Station connected to AP\n");
            toast_show("Target connected", TOAST_INFO);
            esp_wifi_set_ps(WIFI_PS_NONE);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (ap_connection_count > 0) ap_connection_count--;
            glog("WiFi_manager: Station disconnected from AP\n");
            toast_show("Target disconnected", TOAST_WARN);
            login_done = false;
            if (ap_connection_count == 0) {
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                ap_sta_has_ip = false;
            }
            break;
        case WIFI_EVENT_STA_START:
            glog("STA started\n");
            // No auto-connect here - handled by wifi_event_handler
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (manual_disconnect) {
                glog("Disconnected from Wi-Fi (manual)\n");
                manual_disconnect = false; // Reset flag
            } else {
                glog("Disconnected from Wi-Fi\n");
                // No auto-reconnection
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            break;
        case IP_EVENT_AP_STAIPASSIGNED:
            glog("Assigned IP to STA\n");
            toast_show("Target got IP", TOAST_INFO);
            ap_sta_has_ip = true;
            break;
        default:
            break;
        }
    }
}


static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data);
static void wifi_retry_timer_callback(void* arg);

static bool wait_for_ota_check_result(uint32_t timeout_ms) {
    bool saw_checking = false;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        OtaStatus status = ota_manager_get_status();
        if (status.state == OTA_STATE_UPDATE_AVAILABLE) return true;
        if (status.state == OTA_STATE_CHECKING) {
            saw_checking = true;
        } else if (saw_checking || waited >= 500) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return false;
}

static bool wait_for_self_ota_check_result(uint32_t timeout_ms) {
    bool saw_checking = false;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        SelfOtaStatus status = self_ota_manager_get_status();
        if (status.state == SELF_OTA_STATE_UPDATE_AVAILABLE) return true;
        if (status.state == SELF_OTA_STATE_CHECKING) {
            saw_checking = true;
        } else if (saw_checking || waited >= 500) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return false;
}

static bool wait_for_peer_ota_check_result(uint32_t timeout_ms) {
    bool saw_checking = false;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        PeerOtaStatus status = peer_ota_manager_get_status();
        if (status.state == PEER_OTA_STATE_UPDATE_AVAILABLE) return true;
        if (status.state == PEER_OTA_STATE_CHECKING) {
            saw_checking = true;
        } else if (saw_checking || waited >= 500) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return false;
}

static void ota_auto_check_task(void *arg) {
    (void)arg;

#ifndef CONFIG_SPIRAM
    ESP_LOGI(TAG, "Skipping OTA auto-check on no-PSRAM build");
    ota_auto_check_done = true;
    ota_auto_check_running = false;
    vTaskDelete(NULL);
    return;
#endif

    bool update_available = false;

    if (ota_manager_is_supported()) {
        if (ota_manager_check_now() == ESP_OK && wait_for_ota_check_result(WIFI_OTA_AUTO_CHECK_TIMEOUT_MS)) {
            OtaStatus status = ota_manager_get_status();
            if (status.latest_build_number > (long)GHOSTESP_BUILD_NUMBER) {
                glog("There is a new update available: device firmware %s (build %ld > %ld)\n",
                     status.latest_version, status.latest_build_number, (long)GHOSTESP_BUILD_NUMBER);
                update_available = true;
            }
        }
    } else if (self_ota_manager_is_supported()) {
        if (self_ota_manager_check_now() == ESP_OK && wait_for_self_ota_check_result(WIFI_OTA_AUTO_CHECK_TIMEOUT_MS)) {
            SelfOtaStatus status = self_ota_manager_get_status();
            if (status.latest_build_number > (long)GHOSTESP_BUILD_NUMBER) {
                glog("There is a new update available: device firmware %s (build %ld > %ld)\n",
                     status.latest_version, status.latest_build_number, (long)GHOSTESP_BUILD_NUMBER);
                update_available = true;
            }
        }
    }

    if (peer_ota_manager_is_supported() && esp_comm_manager_is_connected()) {
        if (peer_ota_manager_check_now() == ESP_OK && wait_for_peer_ota_check_result(WIFI_OTA_AUTO_CHECK_TIMEOUT_MS)) {
            PeerOtaStatus status = peer_ota_manager_get_status();
            if (status.peer_current_build_number >= 0 &&
                status.peer_build_number > status.peer_current_build_number) {
                glog("There is a new update available: GhostLink peer firmware %s (build %ld > %ld)\n",
                     status.peer_version, status.peer_build_number, status.peer_current_build_number);
                update_available = true;
            }
        }
    }

    if (update_available) {
        toast_show("There is a new update available", TOAST_INFO);
    }

    ota_auto_check_done = true;
    ota_auto_check_running = false;
    vTaskDelete(NULL);
}

static void schedule_ota_auto_check(void) {
    if (ota_auto_check_running || ota_auto_check_done) return;
    ota_auto_check_running = true;
    BaseType_t rc = xTaskCreate(ota_auto_check_task, "ota_auto_chk", 6144, NULL,
                                tskIDLE_PRIORITY + 1, NULL);
    if (rc != pdPASS) {
        ota_auto_check_running = false;
        glog("Failed to start automatic firmware update check\n");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGD(TAG, "STA started; saved-network connect is explicit/reconnect-only");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        
        // Provide more detailed reason descriptions
        const char* reason_str = "Unknown";
        switch(disconnected->reason) {
            case 2: reason_str = "Auth Expired"; break;
            case 3: reason_str = "Auth Leave"; break;
            case 4: reason_str = "Assoc Expire"; break;
            case 5: reason_str = "Assoc Too Many"; break;
            case 6: reason_str = "Not Authed"; break;
            case 7: reason_str = "Not Assoc"; break;
            case 8: reason_str = "Assoc Leave"; break;
            case 15: reason_str = "4Way Handshake Timeout"; break;
            case 201: reason_str = "Beacon Timeout"; break;
            case 202: reason_str = "No AP Found"; break;
            case 203: reason_str = "Auth Fail"; break;
            case 204: reason_str = "Assoc Fail"; break;
            case 205: reason_str = "Handshake Timeout"; break;
        }
        
        // Clean, single-line disconnect logging
        const char *reason = NULL;
        if (wifi_reconnect_blocked(&reason)) {
            glog("WiFi disconnected while %s\n", reason ? reason : "busy");
            manual_disconnect = false;
            if (wifi_reconnect_count == 0) {
                wifi_reconnect_count = 1;
            }
            wifi_reconnect_schedule();
        } else if (manual_disconnect) {
            glog("WiFi disconnected manually\n");
            status_display_show_status("WiFi Disconnected");
            toast_show("WiFi disconnected", TOAST_WARN);
            manual_disconnect = false;
            wifi_reconnect_reset();
        } else {
            glog("WiFi disconnected: %s (reason %d)\n", reason_str, disconnected->reason);
            status_display_show_status("WiFi Lost");
            toast_show("WiFi lost", TOAST_WARN);

            if (!settings_get_wifi_auto_reconnect(&G_Settings)) {
                glog("Auto-reconnect disabled; not retrying\n");
                wifi_reconnect_reset();
            } else {
                wifi_reconnect_count++;
                if (wifi_reconnect_count <= WIFI_MAX_RECONNECT_ATTEMPTS) {
                    glog("Scheduling reconnect %d/%d\n", wifi_reconnect_count, WIFI_MAX_RECONNECT_ATTEMPTS);
                    wifi_reconnect_schedule();
                } else {
                    glog("Max reconnect attempts (%d) reached\n", WIFI_MAX_RECONNECT_ATTEMPTS);
                    wifi_reconnect_timer_stop();
                }
            }
        }
        
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        {
            dns_server_handle_t h = dns_handle_take();
            if (h) stop_dns_server(h);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        glog("Got IP: %s\n", ip4addr_ntoa(&event->ip_info.ip));
        status_display_show_status("WiFi Connected");
        toast_show("WiFi connected", TOAST_SUCCESS);

        /* Set reliable fallback DNS servers so external resolution doesn't
         * depend entirely on the router's DNS. DHCP sets DNS_MAIN (index 0);
         * we set BACKUP and FALLBACK here after DHCP has run. */
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
        esp_netif_set_dns_info(wifiSTA, ESP_NETIF_DNS_BACKUP, &dns);
        dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
        esp_netif_set_dns_info(wifiSTA, ESP_NETIF_DNS_FALLBACK, &dns);

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_reconnect_reset();
        if (settings_get_wigle_auto_upload(&G_Settings)) {
            wigle_upload_all_async();
        }
        schedule_ota_auto_check();
    }
}
// Removed old wifi_retry_timer_callback - using unified retry system

// Station scan helper functions moved to station_scan.c module


// Evil captive-portal server removed (counter-surveillance fork: detect-only, no
// credential/keystroke capture). Public API retained as inert stubs for callers.
esp_err_t wifi_manager_start_evil_portal(const char *URL, const char *SSID,
                                         const char *Password,
                                         const char *ap_ssid,
                                         const char *domain) {
    (void)URL; (void)SSID; (void)Password; (void)ap_ssid; (void)domain;
    return ESP_ERR_NOT_SUPPORTED;
}

void wifi_manager_stop_evil_portal(void) {}

void wifi_manager_stop_evil_portal_keep_wifi(void) {}

bool wifi_manager_is_evil_portal_active(void) { return false; }

// Release scan result buffers - delegated to ap_scan module
void wifi_manager_clear_scan_results(void) {
    ap_scan_clear_results();
}

void wifi_manager_start_monitor_mode(wifi_promiscuous_cb_t_t callback) {
    wifi_monitor_capture_active = true;
    wifi_reconnect_reset();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disconnect STA if connected — an associated STA locks the radio to the
    // AP's channel, causing esp_wifi_set_channel() to fail (ESP_FAIL) and
    // preventing channel hopping (e.g. wardriving only sees one channel).
    manual_disconnect = true;
    esp_wifi_disconnect();

    apply_selected_ap_capture_channel_plan(callback);

    // Set hardware-level promiscuous filter based on callback type
    wifi_promiscuous_filter_t filter = {0};
    
    // Determine filter mask based on callback function
    if (callback == wifi_beacon_scan_callback || callback == wifi_probe_scan_callback || 
        callback == wifi_deauth_scan_callback || callback == wifi_pwn_scan_callback ||
        callback == wifi_wps_detection_callback || callback == wifi_listen_probes_callback ||
        callback == wifi_pineap_detector_callback || callback == wardriving_scan_callback) {
        // Management frames only
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    } else if (callback == wifi_eapol_scan_callback) {
        // capture mgmt, data, and ctrl for full handshake context
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL;
    } else {
        // Default: capture all frame types (for raw capture, SAE flood, etc.)
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_LOGI("WIFI_MANAGER", "Set hardware filter mask: 0x%02" PRIx32, filter.filter_mask);

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    // Verify current channel for capture callbacks that use selected AP channel plans.
    if (callback_uses_selected_ap_capture_plan(callback)) {
        uint8_t ch_primary = 0; wifi_second_chan_t ch_second = WIFI_SECOND_CHAN_NONE;
        esp_err_t get_err = esp_wifi_get_channel(&ch_primary, &ch_second);
        if (get_err == ESP_OK) {
            const char *cap_name = "CAPTURE";
            if (callback == wifi_probe_scan_callback) cap_name = "PROBE";
            else if (callback == wifi_deauth_scan_callback) cap_name = "DEAUTH";
            else if (callback == wifi_beacon_scan_callback) cap_name = "BEACON";
            else if (callback == wifi_raw_scan_callback) cap_name = "RAW";
            else if (callback == wifi_eapol_scan_callback) cap_name = "EAPOL";
            else if (callback == wifi_pwn_scan_callback) cap_name = "PWN";
            else if (callback == wifi_wps_detection_callback) cap_name = "WPS";
            printf("%s: current channel verified as %u\n", cap_name, ch_primary);
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(callback));

    const char *cap_desc = "monitor";
    if (callback == wifi_eapol_scan_callback) cap_desc = "EAPOL";
    else if (callback == wifi_beacon_scan_callback) cap_desc = "beacon";
    else if (callback == wifi_probe_scan_callback) cap_desc = "probe";
    else if (callback == wifi_deauth_scan_callback) cap_desc = "deauth";
    else if (callback == wifi_wps_detection_callback) cap_desc = "wps";
    else if (callback == wifi_raw_scan_callback) cap_desc = "raw";
    else if (callback == wifi_airspace_monitor_callback) cap_desc = "airspace";

    uint8_t ch_primary = 0; wifi_second_chan_t ch_second = WIFI_SECOND_CHAN_NONE;
    (void)esp_wifi_get_channel(&ch_primary, &ch_second);

    const char *filter_desc = "all";
    if (filter.filter_mask == WIFI_PROMIS_FILTER_MASK_MGMT) filter_desc = "mgmt";
    else if (filter.filter_mask == WIFI_PROMIS_FILTER_MASK_DATA) filter_desc = "data";
    else if (filter.filter_mask == (WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA)) filter_desc = "mgmt+data";

    if (!pcap_is_wireshark_mode()) {
        printf("WiFi capture started.\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi capture started.\n");
        printf("Type: %s\n", cap_desc);
        TERMINAL_VIEW_ADD_TEXT("Type: %s\n", cap_desc);
        printf("Channel: %u\n", (unsigned)ch_primary);
        TERMINAL_VIEW_ADD_TEXT("Channel: %u\n", (unsigned)ch_primary);
        printf("Filter: %s\n", filter_desc);
        TERMINAL_VIEW_ADD_TEXT("Filter: %s\n", filter_desc);
    }
    status_display_show_status("Monitor Started");
}
void wifi_manager_stop_monitor_mode() {
    wifi_monitor_capture_active = false;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t wifi_status = esp_wifi_get_mode(&mode);
    if (wifi_status == ESP_ERR_WIFI_NOT_INIT || mode == WIFI_MODE_NULL) {
        ESP_LOGW("WIFI_MANAGER", "Monitor stop called while Wi-Fi driver inactive (status=%s, mode=%d)",
                 esp_err_to_name(wifi_status), mode);
        return;
    } else if (wifi_status != ESP_OK) {
        ESP_LOGE("WIFI_MANAGER", "Failed to query Wi-Fi driver state: %s", esp_err_to_name(wifi_status));
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(NULL));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    status_display_show_status("Monitor Stopped");

    // Stop ALL channel hopping timers
    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (live_ap_hopping_active) {
        stop_live_ap_channel_hopping();
    }
    if (wireshark_hopping_active) {
        wifi_manager_stop_wireshark_channel_hop();
    }
    if (airspace_monitor_is_active()) {
        airspace_monitor_stop();
    }

    // NOTE: Stopping the PineAP timer (channel_hop_timer) is handled by stop_pineap_detection() in callbacks.c
}

void wifi_manager_init(void) {
    size_t mem_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "wifi_manager_init: starting with %d bytes INTERNAL RAM free", (int)mem_start);

    // --- Memory check before WiFi init ---
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_heap < (45 * 1024)) {
        ESP_LOGW(TAG, "WARNING: Less than 45KB of free internal RAM available (%d bytes). WiFi may fail to initialize or operate reliably!", (int)free_heap);
        TERMINAL_VIEW_ADD_TEXT("WARNING: <45KB internal RAM free (%d bytes). WiFi may not initialize or operate reliably!\n", (int)free_heap);
    }

    esp_log_level_set("wifi", ESP_LOG_ERROR); // Only show errors, not warnings

    // Disable WiFi power saving to improve connection stability
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "wifi_manager: initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_LOGI(TAG, "wifi_manager: NVS init done, free internal RAM: %d bytes (used: %d)", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
             (int)(mem_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    // Initialize the TCP/IP stack and WiFi driver
    ESP_LOGI(TAG, "wifi_manager: initializing netif...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();
    ESP_LOGI(TAG, "wifi_manager: netif init done, free internal RAM: %d bytes (used: %d)", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
             (int)(mem_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    // Initialize WiFi with default settings
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "wifi_manager: initializing WiFi driver...");
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_LOGI(TAG, "wifi_manager: WiFi driver init done, free internal RAM: %d bytes (used: %d)", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
             (int)(mem_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    // configure country based on saved setting
    static const struct { const char *code; uint8_t schan; uint8_t nchan; } country_table[] = {
        {"US", 1, 11}, {"GB", 1, 13}, {"JP", 1, 14}, {"AU", 1, 13}, {"CN", 1, 13}, {"01", 1, 11}
    };
    uint8_t country_idx = settings_get_wifi_country(&G_Settings);
    if (country_idx >= sizeof(country_table)/sizeof(country_table[0])) country_idx = 5; // default to World Safe
    
#if CONFIG_IDF_TARGET_ESP32C5
    esp_err_t country_err = esp_wifi_set_country_code(country_table[country_idx].code, true);
    if (country_err == ESP_OK) {
        ESP_LOGI(TAG, "ESP32-C5 Country set to: %s", country_table[country_idx].code);
    } else {
        ESP_LOGW(TAG, "ESP32-C5: Failed to set country: %s", esp_err_to_name(country_err));
    }
#else
    wifi_country_t country_to_set = {
        .cc     = {country_table[country_idx].code[0], country_table[country_idx].code[1], 0},
        .schan  = country_table[country_idx].schan,
        .nchan  = country_table[country_idx].nchan,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    ESP_LOGI(TAG, "Setting country: CC='%s', schan=%d, nchan=%d",
             country_to_set.cc, country_to_set.schan, country_to_set.nchan);
    ESP_ERROR_CHECK(esp_wifi_set_country(&country_to_set));
#endif

    // Create the WiFi event group
    ESP_LOGI(TAG, "wifi_manager: creating event group...");
    wifi_event_group = xEventGroupCreate();
    ESP_LOGI(TAG, "wifi_manager: event group created, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Register the event handler for WiFi events
    ESP_LOGI(TAG, "wifi_manager: registering event handlers...");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_LOGI(TAG, "wifi_manager: event handlers registered, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // AP-side diagnostics (connect/disconnect + dhcp assignment). helps debug portal connect failures.
    if (!g_ap_diag_registered) {
        esp_err_t r1 = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_ap_diag_event_handler, NULL, &g_ap_diag_wifi_inst);
        esp_err_t r2 = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                          &wifi_ap_diag_event_handler, NULL, &g_ap_diag_ip_inst);
        if (r1 == ESP_OK && r2 == ESP_OK) {
            g_ap_diag_registered = true;
        } else {
            ESP_LOGW(TAG, "ap diag handler register failed: wifi=%s ip=%s",
                     esp_err_to_name(r1), esp_err_to_name(r2));
        }
    }

    // Bring the AP interface up only when the SoftAP is actually enabled in
    // settings. Running STA-only otherwise keeps the AP MAC (beaconing + AP
    // TX buffers) from activating, reclaiming internal RAM on starved boards.
    // The AP netif itself stays allocated above, so wifiAP is never NULL and
    // portal/AP code paths are unaffected. Attacks that inject on WIFI_IF_AP
    // (deauth, beacon spam, channel-switch) set AP mode themselves before TX;
    // eapol-logoff is hardened to do the same. Promiscuous/monitor and STA
    // scanning all work in STA mode, so sniffing/wardriving is unaffected.
    bool ap_enabled = settings_get_ap_enabled(&G_Settings);
    if (ap_enabled) {
        ESP_LOGI(TAG, "wifi_manager: setting WiFi mode to APSTA...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else {
        ESP_LOGI(TAG, "wifi_manager: AP disabled in settings, setting WiFi mode to STA...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    ESP_LOGI(TAG, "wifi_manager: WiFi mode set, free internal RAM: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (ap_enabled) {
        // Configure the SoftAP settings
        ESP_LOGI(TAG, "wifi_manager: configuring AP...");
        wifi_config_t ap_config = {
            .ap = {.ssid = "",
                   .ssid_len = strlen(""),
                   .password = "",
                   .channel = 1,
                   .authmode = WIFI_AUTH_OPEN,
                   .max_connection = 4,
                   .ssid_hidden = 1},
        };

        // Apply the AP configuration
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }

    // Start Wi-Fi
    ESP_LOGI(TAG, "wifi_manager: starting WiFi (esp_wifi_start)...");
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_manager: WiFi started, free internal RAM: %d bytes", 
              (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
     
    // Additional WiFi stability settings
    // Set maximum TX power to improve signal strength
    esp_wifi_set_max_tx_power(78); // 19.5 dBm (78/4)
    
    // Set connection timeout to be more lenient
    esp_wifi_set_inactive_time(WIFI_IF_STA, 60); // 60 seconds before considering connection inactive

    // Initialize global CA certificate store
    ESP_LOGI(TAG, "wifi_manager: attaching CA certificate bundle...");
    ret = esp_crt_bundle_attach(NULL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "wifi_manager: CA bundle attached, free internal RAM: %d bytes", 
                 (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        printf("Global CA certificate store initialized successfully.\n");
    } else {
        printf("Failed to initialize global CA certificate store: %s\n", esp_err_to_name(ret));
    }
    
    size_t mem_end = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "wifi_manager_init: COMPLETE. Total used: %d bytes, free internal RAM: %d bytes", 
             (int)(mem_start - mem_end), (int)mem_end);
}

void wifi_manager_configure_sta_from_settings(void) {
    wifi_reconnect_reset();

    // Configure STA with saved credentials for boot-time connection
    const char *saved_ssid = settings_get_sta_ssid(&G_Settings);
    const char *saved_password = settings_get_sta_password(&G_Settings);
    if (saved_ssid && strlen(saved_ssid) > 0) {
        wifi_config_t sta_config = {
            .sta = {
                .threshold.authmode = (saved_password && strlen(saved_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
                .pmf_cfg = {.capable = true, .required = false},
            },
        };
        
        strlcpy((char *)sta_config.sta.ssid, saved_ssid, sizeof(sta_config.sta.ssid));
        if (saved_password) {
            strlcpy((char *)sta_config.sta.password, saved_password, sizeof(sta_config.sta.password));
        }
        
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (err == ESP_OK) {
            printf("STA configured with saved credentials: %s\n", saved_ssid);
            
            printf("Attempting boot-time connection to: %s\n", saved_ssid);
            TERMINAL_VIEW_ADD_TEXT("Connecting to saved network: %s\n", saved_ssid);
            
            esp_err_t connect_err = esp_wifi_connect();
            if (connect_err != ESP_OK) {
                printf("Failed to initiate connection: %s\n", esp_err_to_name(connect_err));
                TERMINAL_VIEW_ADD_TEXT("Failed to connect to saved network\n");
            }
        } else {
            printf("Failed to configure STA: %s\n", esp_err_to_name(err));
        }
    } else {
        printf("No saved WiFi credentials found\n");
    }
}

// Start WiFi scan - delegated to ap_scan module
void wifi_manager_start_scan() {
    ap_scan_start();
}

// Stop scanning for networks
void wifi_manager_stop_scan() {
    esp_err_t err;

    log_heap_status(TAG, "scan_stop_pre");
    err = esp_wifi_scan_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {

        // commented out for now because it's cleaner without and stop commands send this when not
        // really needed

        /*printf("WiFi scan was not active.\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi scan was not active.\n"); */

        return;
    } else if (err != ESP_OK) {
        printf("Failed to stop WiFi scan: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to stop WiFi scan\n");
        return;
    }

    wifi_manager_stop_monitor_mode();
    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

    uint16_t initial_ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&initial_ap_count);
    if (err != ESP_OK) {
        printf("Failed to get AP count: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to get AP count: %s\n", esp_err_to_name(err));
        return;
    }

    // only print AP count once, no need for both "Initial" and "Actual"
    printf("Found %u access points\n", initial_ap_count);
    TERMINAL_VIEW_ADD_TEXT("Found %u access points\n", initial_ap_count);

    // truncate to avoid excessive memory usage
    if (initial_ap_count > MAX_SCANNED_APS) {
        printf("too many aps (%u). truncating list to first %d\n", initial_ap_count, MAX_SCANNED_APS);
        TERMINAL_VIEW_ADD_TEXT("showing first %d aps (truncated)\n", MAX_SCANNED_APS);
        initial_ap_count = MAX_SCANNED_APS;
    }
    if (initial_ap_count > 0) {
        if (scanned_aps != NULL) {
            free(scanned_aps);
            scanned_aps = NULL;
        }
        
        if (selected_aps != NULL) {
            free(selected_aps);
            selected_aps = NULL;
            selected_ap_count = 0;
        }

        scanned_aps = spiram_calloc(initial_ap_count, sizeof(wifi_ap_record_t));
        if (scanned_aps == NULL) {
            printf("Failed to allocate memory for AP info\n");
            ap_count = 0;
            return;
        }

        uint16_t actual_ap_count = initial_ap_count;
        err = esp_wifi_scan_get_ap_records(&actual_ap_count, scanned_aps);
        if (err != ESP_OK) {
            printf("Failed to get AP records: %s\n", esp_err_to_name(err));
            free(scanned_aps);
            scanned_aps = NULL;
            ap_count = 0;
            return;
        }

        ap_count = actual_ap_count;
    } else {
        printf("No access points found\n");
        ap_count = 0;
    }
}

// List stations - delegated to station_scan module
void wifi_manager_list_stations() {
    station_scan_print_results();
}

// Select AP - delegated to ap_scan module
void wifi_manager_select_ap(int index) {
    esp_err_t err = ap_scan_select(index);
    if (err == ESP_OK) {
        // Update local selected_ap for compatibility with other functions
        ap_scan_get_selection(&selected_ap);
        // Sync multi-AP selection so capture channel plan can lock to the AP's channel
        if (selected_aps != NULL) {
            free(selected_aps);
            selected_aps = NULL;
        }
        wifi_ap_record_t *scan_aps = NULL;
        int scan_count = 0;
        ap_scan_get_selected(&scan_aps, &scan_count);
        if (scan_count > 0 && scan_aps != NULL) {
            selected_aps = spiram_malloc((size_t)scan_count * sizeof(wifi_ap_record_t));
            if (selected_aps != NULL) {
                memcpy(selected_aps, scan_aps, (size_t)scan_count * sizeof(wifi_ap_record_t));
                selected_ap_count = scan_count;
            } else {
                selected_ap_count = 0;
            }
        } else {
            selected_ap_count = 0;
        }
    }
}

void wifi_manager_select_multiple_aps(int *indices, int count) {
    if (ap_count == 0) {
        printf("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        return;
    }

    if (scanned_aps == NULL) {
        printf("No AP info available (scanned_aps is NULL)\n");
        TERMINAL_VIEW_ADD_TEXT("No AP info available (scanned_aps is NULL)\n");
        return;
    }

    if (count <= 0) {
        printf("Invalid count: %d\n", count);
        TERMINAL_VIEW_ADD_TEXT("Invalid count: %d\n", count);
        return;
    }

    for (int i = 0; i < count; i++) {
        if (indices[i] < 0 || indices[i] >= ap_count) {
            printf("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            TERMINAL_VIEW_ADD_TEXT("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            return;
        }
    }

    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
    }

    selected_aps = spiram_malloc((size_t)count * sizeof(wifi_ap_record_t));
    if (selected_aps == NULL) {
        printf("Failed to allocate memory for selected APs\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to allocate memory for selected APs\n");
        selected_ap_count = 0;
        return;
    }

    selected_ap_count = count;

    for (int i = 0; i < count; i++) {
        selected_aps[i] = scanned_aps[indices[i]];
    }

    selected_ap = selected_aps[0];

    printf("Selected %d Access Points:\n", count);
    TERMINAL_VIEW_ADD_TEXT("Selected %d Access Points:\n", count);

    for (int i = 0; i < count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(selected_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        printf("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");

        TERMINAL_VIEW_ADD_TEXT("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");
    }

    printf("Multiple APs selected successfully. Primary AP: %s\n", 
           (char*)selected_ap.ssid);
    TERMINAL_VIEW_ADD_TEXT("Multiple APs selected successfully.\n");
}

void wifi_manager_get_selected_aps(wifi_ap_record_t **aps, int *count) {
    if (aps != NULL) {
        *aps = selected_aps;
    }
    if (count != NULL) {
        *count = selected_ap_count;
    }
}

// Select station - delegated to station_scan module
void wifi_manager_select_station(int index) {
    station_scan_select(index);
}


#define MAX_PAYLOAD 64
#define UDP_PORT 6677
#define VIS_DISCOVERY_PORT 6678
#define VIS_DISCOVERY_PAYLOAD "GHOSTESP_RAVE_DISCOVER_V1"
#define VIS_RECV_TIMEOUT_MS 250
#define VIS_DISCOVERY_INTERVAL_US 1000000ULL
#define TRACK_NAME_LEN 32
#define ARTIST_NAME_LEN 32
#define NUM_BARS 15

void screen_music_visualizer_task(void *pvParameters) {
    (void)pvParameters;
    char rx_buffer[128];
    uint8_t amplitudes[NUM_BARS];

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    struct sockaddr_in helper_discovery_addr;
    helper_discovery_addr.sin_family = AF_INET;
    helper_discovery_addr.sin_port = htons(VIS_DISCOVERY_PORT);
    helper_discovery_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    visualizer_stop_requested = false;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }

    visualizer_socket = sock;

    printf("Socket created\n");

    struct timeval recv_timeout = {
        .tv_sec = 0,
        .tv_usec = VIS_RECV_TIMEOUT_MS * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        visualizer_socket = -1;
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }

    printf("Socket bound, port %d\n", UDP_PORT);

    int discover_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discover_sock >= 0) {
        int broadcast_enable = 1;
        setsockopt(discover_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
    }
    int64_t last_discovery_us = 0;

    while (!visualizer_stop_requested) {
        int64_t now_us = esp_timer_get_time();
        if (discover_sock >= 0 && (now_us - last_discovery_us) >= VIS_DISCOVERY_INTERVAL_US) {
            sendto(discover_sock,
                   VIS_DISCOVERY_PAYLOAD,
                   strlen(VIS_DISCOVERY_PAYLOAD),
                   0,
                   (struct sockaddr *)&helper_discovery_addr,
                   sizeof(helper_discovery_addr));
            last_discovery_us = now_us;
        }

        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            if (visualizer_stop_requested) {
                break;
            }

            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT || err == EINTR) {
                continue;
            }

            if (err == ENETDOWN || err == ENETUNREACH || err == EHOSTUNREACH ||
                err == ENOTCONN || err == EADDRNOTAVAIL) {
                vTaskDelay(pdMS_TO_TICKS(80));
                continue;
            }

            if (err == EBADF || err == ENOTSOCK) {
                printf("recvfrom socket invalid: errno %d\n", err);
                break;
            }

            printf("recvfrom transient error: errno %d\n", err);
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        rx_buffer[len] = '\0';

        if (len >= TRACK_NAME_LEN + ARTIST_NAME_LEN + NUM_BARS) {
            memcpy(amplitudes, rx_buffer + TRACK_NAME_LEN + ARTIST_NAME_LEN, NUM_BARS);

#if defined(CONFIG_WITH_SCREEN) || defined(WITH_SCREEN)
            music_visualizer_view_update(amplitudes, "LIVE INPUT", "Desktop Audio (Wi-Fi)");
#endif
        } else {
            printf("Received packet of unexpected size\n");
        }
    }

    if (sock != -1) {
        printf("Shutting down socket and restarting...\n");
        shutdown(sock, 0);
        close(sock);
    }
    if (discover_sock >= 0) {
        close(discover_sock);
    }

    visualizer_socket = -1;
    visualizer_stop_requested = false;
    VisualizerHandle = NULL;
    vTaskDelete(NULL);
}
void animate_led_based_on_amplitude(void *pvParameters) {
    (void)pvParameters;
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = addr_family;
    dest_addr.sin_port = htons(UDP_PORT);

    visualizer_stop_requested = false;

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }
    visualizer_socket = sock;
    printf("Socket created\n");

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        visualizer_socket = -1;
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }
    printf("Socket bound, port %d\n", UDP_PORT);

    float amplitude = 0.0f;
    float last_amplitude = 0.0f;
    float smoothing_factor = 0.1f;
    int hue = 0;
    
    uint32_t last_error_time = 0;
    const uint32_t error_rate_limit_ms = 5000;

    while (!visualizer_stop_requested) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len > 0) {
            rx_buffer[len] = '\0';
            inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
            printf("Received %d bytes from %s: %s\n", len, addr_str, rx_buffer);

            amplitude = atof(rx_buffer);
            amplitude = fmaxf(0.0f, fminf(amplitude, 1.0f)); // Clamp between 0.0 and 1.0

            // Smooth amplitude to avoid sudden changes (optional)
            amplitude =
                (smoothing_factor * amplitude) + ((1.0f - smoothing_factor) * last_amplitude);
            last_amplitude = amplitude;
        } else {
            // Gradually decrease amplitude when no data is received
            amplitude = last_amplitude * 0.9f; // Adjust decay rate as needed
            last_amplitude = amplitude;
        }

        // Ensure amplitude doesn't go below zero
        amplitude = fmaxf(0.0f, amplitude);

        hue = (int)(amplitude * 360) % 360;

        float h = hue / 60.0f;
        float s = 1.0f;
        float v = amplitude;

        int i = (int)h % 6;
        float f = h - (int)h;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);

        float r = 0.0f, g = 0.0f, b = 0.0f;
        switch (i) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        case 5:
            r = v;
            g = p;
            b = q;
            break;
        }

        uint8_t red = (uint8_t)(r * 255);
        uint8_t green = (uint8_t)(g * 255);
        uint8_t blue = (uint8_t)(b * 255);

        esp_err_t ret = rgb_manager_set_color(&rgb_manager, 0, red, green, blue, false);
        if (ret != ESP_OK) {
            printf("Failed to set color\n");
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    if (sock != -1) {
        printf("Shutting down socket...\n");
        shutdown(sock, 0);
        close(sock);
    }

    visualizer_socket = -1;
    visualizer_stop_requested = false;
    VisualizerHandle = NULL;
    vTaskDelete(NULL);
}

#define START_HOST 1
#define END_HOST 254
#define SCAN_TIMEOUT_MS 100
#define HOST_TIMEOUT_MS 100
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_OPEN_PORTS 64

uint16_t calculate_checksum(uint16_t *addr, int len) {
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}


void rgb_visualizer_server_task(void *pvParameters) {
    char rx_buffer[MAX_PAYLOAD];
    char addr_str[128];
    int addr_family;
    int ip_protocol;

    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(UDP_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            printf("Unable to create socket: errno %d\n", errno);
            break;
        }
        printf("Socket created\n");

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            printf("Socket unable to bind: errno %d\n", errno);
        }
        printf("Socket bound, port %d\n", UDP_PORT);

        while (1) {
            printf("Waiting for data\n");
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                printf("recvfrom failed: errno %d\n", errno);
                break;
            } else {
                // Data received
                rx_buffer[len] = 0; // Null-terminate

                // Process the received data
                uint8_t *amplitudes = (uint8_t *)rx_buffer;
                size_t num_bars = len;
                update_led_visualizer(amplitudes, num_bars, false);
            }
        }

        if (sock != -1) {
            printf("Shutting down socket and restarting...\n");
            shutdown(sock, 0);
            close(sock);
        }
    }

    vTaskDelete(NULL);
}

static void wifi_manager_print_ap_entry_formatted(uint16_t idx, const wifi_ap_record_t *rec, bool include_security) {
    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden((uint8_t *)rec->ssid, sanitized_ssid, sizeof(sanitized_ssid));

    // lookup vendor using oui database
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             rec->bssid[0], rec->bssid[1], rec->bssid[2],
             rec->bssid[3], rec->bssid[4], rec->bssid[5]);
    char vendor[64] = {0};
    bool has_vendor = ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));

    printf("[%u] SSID: %s,\n"
           "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
           "     RSSI: %d,\n"
           "     Channel: %d,\n",
           idx,
           sanitized_ssid,
           rec->bssid[0], rec->bssid[1], rec->bssid[2], rec->bssid[3], rec->bssid[4], rec->bssid[5],
           rec->rssi,
           rec->primary);
    TERMINAL_VIEW_ADD_TEXT("[%u] SSID: %s,\n"
                           "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
                           "     RSSI: %d,\n"
                           "     Channel: %d,\n",
                           idx,
                           sanitized_ssid,
                           rec->bssid[0], rec->bssid[1], rec->bssid[2], rec->bssid[3], rec->bssid[4], rec->bssid[5],
                           rec->rssi,
                           rec->primary);

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (include_security) {
        int ch = rec->primary;
        const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
        printf("     Band: %s,\n", band_str);
        TERMINAL_VIEW_ADD_TEXT("     Band: %s,\n", band_str);

        const char *auth_str = "Unknown";
        const char *pmf_str = NULL;
        switch (rec->authmode) {
            case WIFI_AUTH_OPEN: auth_str = "Open"; break;
            case WIFI_AUTH_WEP: auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: auth_str = "WPA2-Enterprise"; break;
            case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; pmf_str = "Required"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/WPA3"; pmf_str = "Required (WPA3)"; break;
            case WIFI_AUTH_WAPI_PSK: auth_str = "WAPI"; break;
            case WIFI_AUTH_WPA3_ENTERPRISE: auth_str = "WPA3-Enterprise"; pmf_str = "Required"; break;
            default: auth_str = "Unknown"; break;
        }
        if (pmf_str) {
            printf("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
            TERMINAL_VIEW_ADD_TEXT("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
        } else {
            printf("     Security: %s\n", auth_str);
            TERMINAL_VIEW_ADD_TEXT("     Security: %s\n", auth_str);
        }
    }
#endif

    if (has_vendor) {
        printf("     Vendor: %s\n", vendor);
        TERMINAL_VIEW_ADD_TEXT("     Vendor: %s\n", vendor);
    }
}
void wifi_manager_print_scan_results_with_oui() {
    if (scanned_aps == NULL) {
        glog("AP information not available\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "ap_scan", "txt") == ESP_OK);

    uint16_t limit = ap_count;

    if (saving) {
        scan_file_printf(&sf, "--- AP Scan Results (%u APs) ---\n", limit);
    }

    for (uint16_t i = 0; i < limit; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5]);
        char vendor[64] = {0};
        bool has_vendor = ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));

        glog("[%u] SSID: %s,\n"
             "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
             "     RSSI: %d,\n"
             "     Channel: %d,\n",
             i, sanitized_ssid, 
             scanned_aps[i].bssid[0], scanned_aps[i].bssid[1],
             scanned_aps[i].bssid[2], scanned_aps[i].bssid[3],
             scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
             scanned_aps[i].rssi,
             scanned_aps[i].primary);

        if (saving) {
            scan_file_printf(&sf, "[%u] SSID: %s, BSSID: %s, RSSI: %d, CH: %d",
                             i, sanitized_ssid, mac_str,
                             scanned_aps[i].rssi, scanned_aps[i].primary);
        }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        {
            int ch = scanned_aps[i].primary;
            const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
            glog("     Band: %s,\n", band_str);
            
            const char *auth_str = "Unknown";
            const char *pmf_str = NULL;
            
            switch (scanned_aps[i].authmode) {
                case WIFI_AUTH_OPEN:
                    auth_str = "Open";
                    break;
                case WIFI_AUTH_WEP:
                    auth_str = "WEP";
                    break;
                case WIFI_AUTH_WPA_PSK:
                    auth_str = "WPA";
                    break;
                case WIFI_AUTH_WPA2_PSK:
                    auth_str = "WPA2";
                    break;
                case WIFI_AUTH_WPA_WPA2_PSK:
                    auth_str = "WPA/WPA2";
                    break;
                case WIFI_AUTH_WPA2_ENTERPRISE:
                    auth_str = "WPA2-Enterprise";
                    break;
                case WIFI_AUTH_WPA3_PSK:
                    auth_str = "WPA3";
                    pmf_str = "Required";
                    break;
                case WIFI_AUTH_WPA2_WPA3_PSK:
                    auth_str = "WPA2/WPA3";
                    pmf_str = "Required (WPA3)";
                    break;
                case WIFI_AUTH_WAPI_PSK:
                    auth_str = "WAPI";
                    break;
                case WIFI_AUTH_WPA3_ENTERPRISE:
                    auth_str = "WPA3-Enterprise";
                    pmf_str = "Required";
                    break;
                default:
                    auth_str = "Unknown";
                    break;
            }
            
            if (pmf_str) {
                glog("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
            } else {
                glog("     Security: %s\n", auth_str);
            }
            if (saving) {
                scan_file_printf(&sf, ", Band: %s, Security: %s", band_str, auth_str);
                if (pmf_str) scan_file_printf(&sf, ", PMF: %s", pmf_str);
            }
        }
#endif
        if (has_vendor) {
            glog("     Vendor: %s\n", vendor);
            if (saving) scan_file_printf(&sf, ", Vendor: %s", vendor);
        }
        if (saving) scan_file_printf(&sf, "\n");
    }

    if (saving) scan_file_close(&sf);
}

static void live_ap_channel_hop_timer_callback(void *arg) {
    if (!live_ap_hopping_active) return;
    live_ap_channel_index = (live_ap_channel_index + 1) % live_ap_channels_len;
    esp_wifi_set_channel(live_ap_channels[live_ap_channel_index], WIFI_SECOND_CHAN_NONE);
}

static esp_err_t start_live_ap_channel_hopping(void) {
    if (live_ap_channel_hop_timer != NULL) {
        esp_timer_stop(live_ap_channel_hop_timer);
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
    }
    live_ap_channel_index = 0;
    esp_wifi_set_channel(live_ap_channels[live_ap_channel_index], WIFI_SECOND_CHAN_NONE);
    esp_timer_create_args_t timer_args = {
        .callback = live_ap_channel_hop_timer_callback,
        .name = "live_ap_hop"
    };
    esp_err_t err = esp_timer_create(&timer_args, &live_ap_channel_hop_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(live_ap_channel_hop_timer, WIRESHARK_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
        return err;
    }
    live_ap_hopping_active = true;
    return ESP_OK;
}

static void stop_live_ap_channel_hopping(void) {
    if (live_ap_channel_hop_timer) {
        esp_timer_stop(live_ap_channel_hop_timer);
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
    }
    live_ap_hopping_active = false;
}

static bool bssid_already_listed(const uint8_t *bssid) {
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, bssid, 6) == 0) return true;
    }
    return false;
}
static void live_ap_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 36) return;
    const uint8_t *payload = pkt->payload;
    uint8_t frame_subtype = (payload[0] & 0xF0) >> 4;
    if (frame_subtype != 0x08 && frame_subtype != 0x05) return;

    const wifi_ieee80211_packet_t *ipkt = (const wifi_ieee80211_packet_t *)payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(hdr_copy));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;
    const uint8_t *bssid = hdr->addr3;

    if (bssid_already_listed(bssid)) return;

    int idx = 36;
    char ssid[33] = {0};
    while (idx + 1 < pkt->rx_ctrl.sig_len) {
        uint8_t id = payload[idx];
        uint8_t ie_len = payload[idx + 1];
        if (idx + 2 + ie_len > pkt->rx_ctrl.sig_len) break;
        if (id == 0 && ie_len <= 32) {
            memcpy(ssid, &payload[idx + 2], ie_len);
            ssid[ie_len] = '\0';
            break;
        }
        idx += 2 + ie_len;
    }

    if (ssid[0] == '\0') {
        strncpy(ssid, "<hidden>", sizeof(ssid));
    }

    char sanitized[33];
    sanitize_ssid_and_check_hidden((uint8_t *)ssid, sanitized, sizeof(sanitized));

    // derive security from IEs
    bool has_wpa = false;
    bool has_wpa2 = false;
    bool has_wpa3 = false;
    // capability info privacy bit for WEP detection
    if (pkt->rx_ctrl.sig_len >= 36) {
        uint16_t cap = (uint16_t)payload[34] | ((uint16_t)payload[35] << 8);
        // iterate IEs to find RSN/WPA
        int ie = 36;
        while (ie + 1 < pkt->rx_ctrl.sig_len) {
            uint8_t eid = payload[ie];
            uint8_t elen = payload[ie + 1];
            if (ie + 2 + elen > pkt->rx_ctrl.sig_len) break;
            if (eid == 48 /* RSN */ && elen >= 2) {
                int off = ie + 2;
                if (off + 2 <= ie + 2 + elen) {
                    off += 2; // version
                }
                if (off + 4 <= ie + 2 + elen) {
                    off += 4; // group cipher suite
                }
                if (off + 2 <= ie + 2 + elen) {
                    uint16_t pairwise_count = payload[off] | (payload[off + 1] << 8);
                    off += 2 + 4 * pairwise_count;
                }
                if (off + 2 <= ie + 2 + elen) {
                    uint16_t akm_count = payload[off] | (payload[off + 1] << 8);
                    off += 2;
                    for (uint16_t a = 0; a < akm_count; a++) {
                        if (off + 4 > ie + 2 + elen) break;
                        // OUI 00:0F:AC
                        uint8_t oui0 = payload[off + 0];
                        uint8_t oui1 = payload[off + 1];
                        uint8_t oui2 = payload[off + 2];
                        uint8_t type = payload[off + 3];
                        if (oui0 == 0x00 && oui1 == 0x0F && oui2 == 0xAC) {
                            if (type == 2) has_wpa2 = true;      // PSK
                            if (type == 8) has_wpa3 = true;      // SAE
                        }
                        off += 4;
                    }
                }
            } else if (eid == 221 /* Vendor */ && elen >= 4) {
                // WPA (00:50:F2, type 1)
                if (payload[ie + 2] == 0x00 && payload[ie + 3] == 0x50 && payload[ie + 4] == 0xF2 && payload[ie + 5] == 0x01) {
                    has_wpa = true;
                }
            }
            ie += 2 + elen;
        }
    }

    if (scanned_aps == NULL) {
        scanned_aps = calloc(MAX_SCANNED_APS, sizeof(wifi_ap_record_t));
        ap_count = 0;
    }
    if (scanned_aps && ap_count < MAX_SCANNED_APS) {
        wifi_ap_record_t *rec = &scanned_aps[ap_count++];
        memset(rec, 0, sizeof(*rec));
        memcpy(rec->bssid, bssid, 6);
        strncpy((char *)rec->ssid, sanitized, sizeof(rec->ssid));
        rec->rssi = pkt->rx_ctrl.rssi;
        rec->primary = pkt->rx_ctrl.channel;
        // map to closest auth mode
        if (has_wpa3 && has_wpa2) rec->authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        else if (has_wpa3) rec->authmode = WIFI_AUTH_WPA3_PSK;
        else if (has_wpa2) rec->authmode = WIFI_AUTH_WPA2_PSK;
        else if (has_wpa) rec->authmode = WIFI_AUTH_WPA_PSK;
        else {
            // check WEP via privacy bit
            uint16_t cap = (uint16_t)payload[34] | ((uint16_t)payload[35] << 8);
            if (cap & 0x0010) rec->authmode = WIFI_AUTH_WEP; else rec->authmode = WIFI_AUTH_OPEN;
        }
        char ap_payload[96];
        snprintf(ap_payload, sizeof(ap_payload), "%02x:%02x:%02x:%02x:%02x:%02x|%d|%d",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
            rec->primary, rec->rssi);
        ghostscript_emit_event("wifi_ap_found", ap_payload);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - last_live_print_ms < 100) return;
    last_live_print_ms = now_ms;

    while (live_last_printed_index < ap_count) {
        uint16_t idx = live_last_printed_index;
        wifi_ap_record_t *rec = &scanned_aps[idx];
        wifi_manager_print_ap_entry_formatted(idx, rec, true);
        live_last_printed_index++;
    }
}

void wifi_manager_start_live_ap_scan(void) {
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (scanned_aps) { free(scanned_aps); scanned_aps = NULL; }
    ap_count = 0;
    live_last_printed_index = 0;
    last_live_print_ms = 0;
    wifi_manager_start_monitor_mode(live_ap_scan_callback);
    start_live_ap_channel_hopping();
    printf("Live AP scan started. Type 'stopscan' to stop.\n");
    TERMINAL_VIEW_ADD_TEXT("Live AP scan started.\n");
}

void wifi_manager_start_ip_lookup() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK || ap_info.rssi == 0) {
        printf("Not connected to an Access Point.\n");
        return;
    }

    g_mdns_result_count = 0;
    bool store_results = (g_mdns_results != NULL);
    bool was_running = g_mdns_scan_running;
    g_mdns_scan_running = true;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) ==
        ESP_OK) {
        printf("Connected. Proceeding with IP lookup...\n");

        for (int s = 0; s < NUM_SERVICES; s++) {
            if (!g_mdns_scan_running) break;

            int retries = 0;
            mdns_result_t *mdnsresult = NULL;

            while (retries < 5 && mdnsresult == NULL && g_mdns_scan_running) {
                mdns_query_ptr(services[s].query, "_tcp", 2000, 30, &mdnsresult);
                if (mdnsresult == NULL) {
                    retries++;
                    printf("Retrying mDNS query for service: %s (Attempt %d)\n",
                           services[s].query, retries);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }

            if (mdnsresult != NULL) {
                printf("mDNS query succeeded for service: %s\n", services[s].query);

                mdns_result_t *current_result = mdnsresult;
                while (current_result != NULL) {
                    char ip_str[INET_ADDRSTRLEN] = {0};
                    mdns_ip_addr_t *addr_item = current_result->addr;
                    bool has_v4 = false;
                    while (addr_item != NULL) {
                        if (addr_item->addr.type == IPADDR_TYPE_V4) {
                            inet_ntop(AF_INET, &addr_item->addr.u_addr.ip4, ip_str, INET_ADDRSTRLEN);
                            has_v4 = true;
                            break;
                        }
                        addr_item = addr_item->next;
                    }
                    if (!has_v4) {
                        strncpy(ip_str, "0.0.0.0", sizeof(ip_str));
                    }

                    if (store_results && g_mdns_result_count < MDNS_MAX_DEVICES) {
                        bool duplicate = false;
                        for (int d = 0; d < g_mdns_result_count; d++) {
                            if (strcmp(g_mdns_results[d].ip, ip_str) == 0) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (!duplicate) {
                            mdns_device_t *dev = &g_mdns_results[g_mdns_result_count];
                            memset(dev, 0, sizeof(mdns_device_t));
                            if (current_result->hostname) {
                                strncpy(dev->hostname, current_result->hostname, sizeof(dev->hostname) - 1);
                            }
                            strncpy(dev->ip, ip_str, sizeof(dev->ip) - 1);
                            dev->port = current_result->port;
                            strncpy(dev->service_type, services[s].type, sizeof(dev->service_type) - 1);
                            g_mdns_result_count++;
                        }
                    }

                    printf("Device at: %s\n", ip_str);
                    printf("  Name: %s\n", current_result->hostname);
                    printf("  Type: %s\n", services[s].type);
                    printf("  Port: %u\n", current_result->port);

                    current_result = current_result->next;
                }

                mdns_query_results_free(mdnsresult);
            } else {
                printf("Failed to find devices for service: %s after %d retries\n",
                       services[s].query, retries);
            }
        }
    } else {
        printf("Can't get network interface info.\n");
    }

    printf("IP Scan Done. Found %d devices.\n", g_mdns_result_count);
    if (!was_running) {
        g_mdns_scan_running = false;
    }
}

static void wifi_manager_ip_lookup_task(void *pvParameters) {
    (void)pvParameters;
    wifi_manager_start_ip_lookup();
    g_mdns_scan_done = true;
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_ip_lookup_async(void) {
    if (g_mdns_scan_running) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_manager_ip_lookup_clear();
    g_mdns_results = malloc(sizeof(mdns_device_t) * MDNS_MAX_DEVICES);
    if (!g_mdns_results) {
        return ESP_ERR_NO_MEM;
    }
    memset(g_mdns_results, 0, sizeof(mdns_device_t) * MDNS_MAX_DEVICES);
    g_mdns_scan_running = true;
    g_mdns_scan_done = false;
    BaseType_t ret = xTaskCreate(wifi_manager_ip_lookup_task, "mdns_scan", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        g_mdns_scan_running = false;
        free(g_mdns_results);
        g_mdns_results = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool wifi_manager_ip_lookup_check_done(void) {
    return g_mdns_scan_done;
}

void wifi_manager_ip_lookup_finish_async(void) {
    g_mdns_scan_running = false;
}

bool wifi_manager_ip_lookup_is_running(void) {
    return g_mdns_scan_running && !g_mdns_scan_done;
}

int wifi_manager_ip_lookup_get_count(void) {
    return g_mdns_result_count;
}

const mdns_device_t* wifi_manager_ip_lookup_get_device(int index) {
    if (index < 0 || index >= g_mdns_result_count) {
        return NULL;
    }
    return &g_mdns_results[index];
}

void wifi_manager_ip_lookup_clear(void) {
    g_mdns_result_count = 0;
    if (g_mdns_results) {
        free(g_mdns_results);
        g_mdns_results = NULL;
    }
}
void wifi_manager_connect_wifi(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        printf("No SSID provided\n");
        TERMINAL_VIEW_ADD_TEXT("No SSID provided\n");
        status_display_show_status("WiFi No SSID");
        return;
    }

    wifi_reconnect_reset();

    if (!wifi_ctrl_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "connect: wifi ctrl mutex lock failed");
        TERMINAL_VIEW_ADD_TEXT("WiFi busy, try again\n");
        status_display_show_status("WiFi Busy");
        return;
    }

    printf("Connecting to WiFi: %s\n", ssid);
    TERMINAL_VIEW_ADD_TEXT("Connecting to WiFi: %s\n", ssid);
    status_display_show_status("WiFi Connecting...");
    wifi_connect_cancel_requested = false;

    wifi_ap_record_t current_ap = {0};
    if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK &&
        strncmp((const char *)current_ap.ssid, ssid, sizeof(current_ap.ssid)) == 0) {
        printf("Already connected to %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("Already connected to %s\n", ssid);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        status_display_show_status("WiFi Connected");
        wifi_ctrl_unlock();
        return;
    }
    
    wifi_config_t wifi_config = {0};
    
    // Copy SSID and password safely
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    
    // Set auth mode - use WPA_WPA2_PSK for better compatibility with modern routers
    if (strlen(password) > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    
    // Enable scan method for better AP selection
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    
    // Ensure clean start state
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_CONNECTING_BIT);
    
    // Set the connecting bit BEFORE any WiFi operations
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTING_BIT);

    // Match the init policy: keep the AP half only when the SoftAP is enabled,
    // otherwise connect STA-only so the AP interface's internal RAM stays freed
    // during normal connected operation (this is the state Cloud Store runs in).
    wifi_mode_t connect_mode = settings_get_ap_enabled(&G_Settings) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(connect_mode);
    if (err != ESP_OK) {
        printf("Failed to set WiFi mode: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to set WiFi mode\n");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);
        status_display_show_status("WiFi Mode Fail");
        wifi_ctrl_unlock();
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to configure STA: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to configure WiFi\n");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);
        status_display_show_status("WiFi Config Fail");
        wifi_ctrl_unlock();
        return;
    }

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect: esp_wifi_disconnect returned %s", esp_err_to_name(err));
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        printf("Failed to start WiFi: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to start WiFi\n");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);
        status_display_show_status("WiFi Start Fail");
        wifi_ctrl_unlock();
        return;
    }

    wifi_ctrl_unlock();

    vTaskDelay(pdMS_TO_TICKS(150));

    int retry_count = 0;
    const int max_retries = 5;  // Reduced retry count for cleaner logs
    bool connected = false;

    while (retry_count < max_retries && !connected) {
        if (wifi_connect_cancel_requested) {
            TERMINAL_VIEW_ADD_TEXT("WiFi connection cancelled\n");
            printf("WiFi connection cancelled\n");
            break;
        }

        if (retry_count > 0) {
            printf("Retry attempt %d/%d...\n", retry_count, max_retries);
            TERMINAL_VIEW_ADD_TEXT("Retry attempt %d/%d...\n", retry_count, max_retries);
        }
        
        esp_err_t ret = esp_wifi_connect();
        if (ret == ESP_ERR_WIFI_CONN) {
            ret = ESP_OK; // Already connecting, handled elsewhere
        } else if (ret == ESP_ERR_WIFI_NOT_STARTED) {
            esp_err_t start_err = esp_wifi_start();
            if (start_err == ESP_OK || start_err == ESP_ERR_WIFI_CONN) {
                vTaskDelay(pdMS_TO_TICKS(150));
                ret = esp_wifi_connect();
                if (ret == ESP_ERR_WIFI_CONN) {
                    ret = ESP_OK;
                }
            }
        }

        if (ret == ESP_OK) {
            EventBits_t bits = 0;
            const TickType_t wait_slice = pdMS_TO_TICKS(250);
            const TickType_t wait_total = pdMS_TO_TICKS(10000);
            TickType_t waited = 0;

            while (!wifi_connect_cancel_requested && waited < wait_total) {
                bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           wait_slice);
                if (bits & WIFI_CONNECTED_BIT) {
                    break;
                }
                waited += wait_slice;
            }

            if (wifi_connect_cancel_requested) {
                TERMINAL_VIEW_ADD_TEXT("WiFi connection cancelled\n");
                printf("WiFi connection cancelled\n");
                break;
            }
            
            if (bits & WIFI_CONNECTED_BIT) {
                connected = true;
                printf("Successfully connected to %s\n", ssid);
                TERMINAL_VIEW_ADD_TEXT("Successfully connected to %s\n", ssid);
                break;
            }
        } else {
            printf("Connection initiation failed (error: %d)\n", ret);
            TERMINAL_VIEW_ADD_TEXT("Connection initiation failed (error: %d)\n", ret);
        }

        if (!connected) {
            esp_wifi_disconnect();
            retry_count++;
            if (retry_count < max_retries) {
                TERMINAL_VIEW_ADD_TEXT("Retrying connection (%d/%d)...\n", retry_count, max_retries);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
    }

    // Clear the connecting bit as we're done with the manual connection attempt
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);

    if (!connected && !wifi_connect_cancel_requested) {
        TERMINAL_VIEW_ADD_TEXT("Failed to connect to %s after %d attempts\n", ssid, max_retries);
        printf("Failed to connect to %s after %d attempts\n", ssid, max_retries);
        esp_wifi_disconnect();
    }

    wifi_connect_cancel_requested = false;
}

// Function to provide access to the last scan results
void wifi_manager_get_scan_results_data(uint16_t *count, wifi_ap_record_t **aps) {
    *count = ap_count;
    *aps = scanned_aps;
}

esp_err_t wifi_manager_start_scan_with_time(int seconds) {
    ap_manager_stop_services();

    // Mark a timed scan as active so the auto-reconnect timer defers instead of
    // reconfiguring STA mid-scan (which aborts the scan -> 0 results). This path
    // calls esp_wifi_scan_start() directly and never touches the ap_scan module,
    // so ap_scan_is_running() alone wouldn't cover it.
    wifi_timed_scan_active = true;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        printf("Failed to set WiFi mode for timed scan: %s\n", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        printf("Failed to start WiFi for timed scan: %s\n", esp_err_to_name(err));
        goto cleanup;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    rgb_manager_set_color(&rgb_manager, -1, 50, 255, 50, false);

    printf("WiFi Scan started\n");
    printf("Please wait %d Seconds...\n", seconds);
    TERMINAL_VIEW_ADD_TEXT("WiFi Scan started\n");
    TERMINAL_VIEW_ADD_TEXT("Please wait %d Seconds...\n", seconds);

    err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        printf("WiFi scan failed to start: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));

    wifi_manager_stop_scan();
    err = esp_wifi_stop();
    if (err != ESP_OK) {
        printf("Failed to stop WiFi after timed scan: %s\n", esp_err_to_name(err));
        goto cleanup;
    }

cleanup:
    wifi_timed_scan_active = false;
    ap_manager_start_services();
    return err;
}

// Station scan channel hopping functions moved to station_scan.c module

// Wireshark Capture Channel Hopping Callback
static void wireshark_channel_hop_timer_callback(void *arg) {
    if (!wireshark_hopping_active) return;
    wireshark_channel_index = (wireshark_channel_index + 1) % wireshark_channels_count;
    uint8_t channel = wireshark_channels[wireshark_channel_index];
    
    // determine if 5ghz or 2.4ghz
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    
    #if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (channel > 14) {
        // 5ghz channel - use ht40
        second = WIFI_SECOND_CHAN_ABOVE;
    }
    #endif
    
    esp_wifi_set_channel(channel, second);
}

static bool callback_uses_selected_ap_capture_plan(wifi_promiscuous_cb_t_t callback) {
    return callback == wifi_probe_scan_callback ||
           callback == wifi_deauth_scan_callback ||
           callback == wifi_beacon_scan_callback ||
           callback == wifi_raw_scan_callback ||
           callback == wifi_eapol_scan_callback ||
           callback == wifi_pwn_scan_callback ||
           callback == wifi_wps_detection_callback;
}

static void apply_selected_ap_capture_channel_plan(wifi_promiscuous_cb_t_t callback) {
    if (!callback_uses_selected_ap_capture_plan(callback)) {
        return;
    }

    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (live_ap_hopping_active) {
        stop_live_ap_channel_hopping();
    }
    if (wireshark_hopping_active) {
        wifi_manager_stop_wireshark_channel_hop();
    }

    const char *cap_name = "CAPTURE";
    if (callback == wifi_probe_scan_callback) cap_name = "PROBE";
    else if (callback == wifi_deauth_scan_callback) cap_name = "DEAUTH";
    else if (callback == wifi_beacon_scan_callback) cap_name = "BEACON";
    else if (callback == wifi_raw_scan_callback) cap_name = "RAW";
    else if (callback == wifi_eapol_scan_callback) cap_name = "EAPOL";
    else if (callback == wifi_pwn_scan_callback) cap_name = "PWN";
    else if (callback == wifi_wps_detection_callback) cap_name = "WPS";

    if (selected_ap_count <= 0 || selected_aps == NULL) {
        printf("%s: no AP selected, channel hopping disabled\n", cap_name);
        return;
    }

    uint8_t unique_channels[50] = {0};
    int unique_count = 0;
    for (int i = 0; i < selected_ap_count && unique_count < (int)(sizeof(unique_channels) / sizeof(unique_channels[0])); i++) {
        uint8_t channel = selected_aps[i].primary;
        if (channel == 0) {
            continue;
        }

        bool seen = false;
        for (int j = 0; j < unique_count; j++) {
            if (unique_channels[j] == channel) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unique_channels[unique_count++] = channel;
        }
    }

    if (unique_count <= 0) {
        printf("%s: selected APs have no valid channel, channel hopping disabled\n", cap_name);
        return;
    }

    if (unique_count == 1) {
        esp_err_t ch_err = esp_wifi_set_channel(unique_channels[0], WIFI_SECOND_CHAN_NONE);
        if (ch_err == ESP_OK) {
            printf("%s: locked to channel %d\n", cap_name, unique_channels[0]);
        } else {
            printf("%s: failed to set channel %d: %s\n", cap_name, unique_channels[0], esp_err_to_name(ch_err));
        }
        return;
    }

    memcpy(wireshark_channels, unique_channels, (size_t)unique_count * sizeof(uint8_t));
    wireshark_channels_count = (size_t)unique_count;
    wireshark_channel_index = 0;

    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (wireshark_channels[0] > 14) {
        second = WIFI_SECOND_CHAN_ABOVE;
    }
#endif
    esp_wifi_set_channel(wireshark_channels[0], second);

    esp_timer_create_args_t timer_args = {
        .callback = wireshark_channel_hop_timer_callback,
        .name = "wireshark_hop"
    };

    esp_err_t err = esp_timer_create(&timer_args, &wireshark_channel_hop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create selected AP capture hop timer");
        return;
    }

    err = esp_timer_start_periodic(wireshark_channel_hop_timer, WIRESHARK_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start selected AP capture hop timer");
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
        return;
    }

    wireshark_hopping_active = true;
    printf("%s: hopping selected AP channels (%d)", cap_name, unique_count);
    for (int i = 0; i < unique_count; i++) {
        printf("%s%d", (i == 0) ? ": " : ",", unique_channels[i]);
    }
    printf("\n");
}

esp_err_t wifi_manager_start_wireshark_channel_list(const uint8_t *channels, size_t count) {
    if (!channels || count == 0 || count > sizeof(wireshark_channels)) return ESP_ERR_INVALID_ARG;

    uint8_t unique[sizeof(wireshark_channels)] = {0};
    size_t unique_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (channels[i] < 1 || channels[i] > MAX_WIFI_CHANNEL) return ESP_ERR_INVALID_ARG;
        bool seen = false;
        for (size_t j = 0; j < unique_count; j++) {
            if (unique[j] == channels[i]) {
                seen = true;
                break;
            }
        }
        if (!seen) unique[unique_count++] = channels[i];
    }
    if (unique_count == 1) return wifi_manager_set_wireshark_fixed_channel(unique[0]);

    if (wireshark_channel_hop_timer != NULL) {
        esp_timer_stop(wireshark_channel_hop_timer);
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
    }
    wireshark_hopping_active = false;

    memcpy(wireshark_channels, unique, unique_count);
    wireshark_channels_count = unique_count;
    wireshark_channel_index = 0;

    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (wireshark_channels[0] > 14) second = WIFI_SECOND_CHAN_ABOVE;
#endif
    esp_err_t err = esp_wifi_set_channel(wireshark_channels[0], second);
    if (err != ESP_OK) return err;

    esp_timer_create_args_t timer_args = {
        .callback = wireshark_channel_hop_timer_callback,
        .name = "wireshark_hop"
    };
    err = esp_timer_create(&timer_args, &wireshark_channel_hop_timer);
    if (err != ESP_OK) return err;

    err = esp_timer_start_periodic(wireshark_channel_hop_timer, WIRESHARK_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
        return err;
    }

    wireshark_hopping_active = true;
    return ESP_OK;
}

void wifi_manager_start_wireshark_channel_hop(void) {
    uint8_t channels[sizeof(wireshark_channels)] = {0};

    // build country-appropriate channel list
    size_t count = wifi_channels_build_country_list(channels, sizeof(channels));
    if (count == 0) {
        ESP_LOGE(TAG, "No channels available for Wireshark hopping");
        return;
    }
    esp_err_t err = wifi_manager_start_wireshark_channel_list(channels, count);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to start Wireshark channel hopping: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "Wireshark Channel Hopping Started (%d channels, 150ms interval)", count);
}

void wifi_manager_stop_wireshark_channel_hop(void) {
    if (wireshark_channel_hop_timer) {
        esp_timer_stop(wireshark_channel_hop_timer);
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
        wireshark_hopping_active = false;
        ESP_LOGI(TAG, "Wireshark Channel Hopping Stopped.");
    }
}

esp_err_t wifi_manager_set_wireshark_fixed_channel(uint8_t channel) {
    // Validate channel range based on target
    uint8_t max_channel = MAX_WIFI_CHANNEL;

    if (channel < 1 || channel > max_channel) {
        ESP_LOGE(TAG, "Invalid channel %d. Must be between 1 and %d", channel, max_channel);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop any existing channel hopping
    wifi_manager_stop_wireshark_channel_hop();

    // Set the fixed channel
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set channel %d: %s", channel, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Wireshark capture locked to channel %d", channel);
    return ESP_OK;
}

esp_err_t wifi_manager_set_capture_channel_lock(uint8_t channel) {
    // Validate channel range based on target
    uint8_t max_channel = MAX_WIFI_CHANNEL;

    if (channel < 1 || channel > max_channel) {
        ESP_LOGE(TAG, "Invalid capture channel %d. Must be between 1 and %d", channel, max_channel);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop any active scan/capture channel hopping first so monitor mode stays locked.
    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (live_ap_hopping_active) {
        stop_live_ap_channel_hopping();
    }
    wifi_manager_stop_wireshark_channel_hop();
    if (airspace_monitor_is_active()) {
        airspace_monitor_stop();
    }

    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock capture to channel %d: %s", channel, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Capture locked to channel %d", channel);
    return ESP_OK;
}

// Start station scan - delegated to station_scan module
void wifi_manager_start_station_scan() {
    station_scan_start();
}

// Print combined AP/Station scan results in ASCII chart
void wifi_manager_scanall_chart() {
    if (ap_count == 0) {
        printf("No APs found during scan.\n");
        TERMINAL_VIEW_ADD_TEXT("No APs found during scan.\n");
        return;
    }

    printf("\n--- Combined AP and Station Scan Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("\n--- Combined AP and Station Scan Results ---\n\n");

    const char* ap_header_top =    "┌──────────────────────────────────┬───────────────────┬──────┬───────────┐";
    const char* ap_header_mid =    "│ SSID                             │ BSSID             │ Chan │ Company   │";
    const char* ap_header_bottom = "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_format =        "│ %-32.32s │ %02X:%02X:%02X:%02X:%02X:%02X │ %-4d │ %-9.9s │";
    const char* ap_separator =     "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_footer =        "└──────────────────────────────────┴───────────────────┴──────┴───────────┘";
    const char* sta_format =       "│   -> STA: %02X:%02X:%02X:%02X:%02X:%02X                                             │"; // Formatted station line


    // Print Header Once
    printf("%s\n", ap_header_top);
    printf("%s\n", ap_header_mid);
    printf("%s\n", ap_header_bottom);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_top);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_mid);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_bottom);


    for (uint16_t i = 0; i < ap_count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        // lookup vendor using oui database
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5]);
        char vendor[64] = {0};
        if (!ouis_lookup_vendor(mac_str, vendor, sizeof(vendor))) {
            strncpy(vendor, "Unknown", sizeof(vendor) - 1);
        }

        // Print AP details line
        char ap_details_line[200];
        snprintf(ap_details_line, sizeof(ap_details_line), ap_format, sanitized_ssid,
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
                 scanned_aps[i].primary, vendor);
        printf("%s\n", ap_details_line);
        TERMINAL_VIEW_ADD_TEXT("%s\n", ap_details_line);

        bool station_found_for_ap = false;
        // Find and print associated stations for this AP
        for (int j = 0; j < station_count; j++) {
            if (memcmp(station_ap_list[j].ap_bssid, scanned_aps[i].bssid, 6) == 0) {
                // lookup vendor for station mac
                char sta_mac_str[18];
                snprintf(sta_mac_str, sizeof(sta_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         station_ap_list[j].station_mac[0], station_ap_list[j].station_mac[1],
                         station_ap_list[j].station_mac[2], station_ap_list[j].station_mac[3],
                         station_ap_list[j].station_mac[4], station_ap_list[j].station_mac[5]);
                char sta_vendor[64] = {0};
                bool has_sta_vendor = ouis_lookup_vendor(sta_mac_str, sta_vendor, sizeof(sta_vendor));

                // Print station MAC using the new format
                char sta_details_line[150];
                if (has_sta_vendor) {
                    snprintf(sta_details_line, sizeof(sta_details_line), "%s (%s)",
                             sta_mac_str, sta_vendor);
                } else {
                    snprintf(sta_details_line, sizeof(sta_details_line), "%s",
                             sta_mac_str);
                }
                printf("    STA: %s\n", sta_details_line);
                TERMINAL_VIEW_ADD_TEXT("    STA: %s\n", sta_details_line);
                station_found_for_ap = true;
            }
        }

        (void)station_found_for_ap;

        // Print separator line below the AP (and its stations) if it's not the last AP
        if (i < ap_count - 1) {
            printf("%s\n", ap_separator);
            TERMINAL_VIEW_ADD_TEXT("%s\n", ap_separator);
        }
    }

    // Print Footer Once
    printf("%s\n", ap_footer);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_footer);

    printf("\n--- End of Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("--- End of Results ---\n\n");
}


// Helper function to sanitize SSID and handle hidden networks
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
    char temp_ssid[33];
    memcpy(temp_ssid, input_ssid, 32);
    temp_ssid[32] = '\0';

    if (strlen(temp_ssid) == 0) {
        snprintf(output_buffer, buffer_size, "(Hidden)");
    } else {
        int len = strlen(temp_ssid);
        int out_idx = 0;
        for (int k = 0; k < len && out_idx < buffer_size - 1; k++) {
            char c = temp_ssid[k];
            output_buffer[out_idx++] = (c >= 32 && c <= 126) ? c : '.';
        }
        output_buffer[out_idx] = '\0';
    }
}

void wifi_manager_set_html_from_uart(void) {
    use_html_buffer = true;
    if (html_buffer == NULL) {
#if CONFIG_SPIRAM
        html_buffer = (char*)heap_caps_malloc(MAX_HTML_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (html_buffer == NULL) {
            ESP_LOGW(TAG, "PSRAM html_buffer failed, trying internal RAM");
            html_buffer = (char*)malloc(MAX_HTML_BUFFER_SIZE);
        }
#else
        html_buffer = (char*)malloc(MAX_HTML_BUFFER_SIZE);
#endif
        if (html_buffer == NULL) {
            printf("Failed to allocate HTML buffer\n");
            use_html_buffer = false;
            log_heap_status(TAG, "html_buffer_alloc_fail");
            return;
        }
        log_heap_status(TAG, "html_buffer_alloc_ok");
    }
    html_buffer_size = 0;
    printf("HTML buffer mode enabled, ready to receive HTML content\n");
}

void wifi_manager_store_html_chunk(const char* data, size_t len, bool is_final) {
    if (!use_html_buffer || html_buffer == NULL) {
        return;
    }
    
    if (html_buffer_size + len >= MAX_HTML_BUFFER_SIZE) {
        printf("HTML buffer overflow, truncating content\n");
        len = MAX_HTML_BUFFER_SIZE - html_buffer_size - 1;
    }
    
    if (len > 0) {
        memcpy(html_buffer + html_buffer_size, data, len);
        html_buffer_size += len;
    }
    
    if (is_final) {
        html_buffer[html_buffer_size] = '\0';
        printf("HTML content stored in buffer (%zu bytes)\n", html_buffer_size);
        ESP_LOGI(TAG, "HTML capture completed: buffer=%p, size=%zu, use_html_buffer=%s", 
                 html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");
    }
}

void wifi_manager_clear_html_buffer(void) {
    ESP_LOGI(TAG, "Clearing HTML buffer - current state: buffer=%p, size=%zu, use_html_buffer=%s", 
             html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");
    
    use_html_buffer = false;
    if (html_buffer != NULL) {
        free(html_buffer);
        html_buffer = NULL;
    }
    html_buffer_size = 0;
    printf("HTML buffer cleared and disabled\n");
    ESP_LOGI(TAG, "HTML buffer cleared successfully");
}


// rssi tracking for selected ap and sta
static volatile bool ap_tracking_active = false;
static volatile bool sta_tracking_active = false;
static int8_t tracking_last_rssi = 0;
static int8_t tracking_min_rssi = 0;
static int8_t tracking_max_rssi = -127;
static int64_t tracking_last_rx_us = 0; // timestamp of last matched packet (signal freshness)

static void wifi_track_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(hdr_copy));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;
    
    int8_t rssi = pkt->rx_ctrl.rssi;
    bool match = false;
    
    if (ap_tracking_active && strlen((const char *)selected_ap.ssid) > 0) {
        // track ap by bssid (addr2 for beacons)
        if (memcmp(hdr->addr2, selected_ap.bssid, 6) == 0) {
            match = true;
        }
    }
    
    if (sta_tracking_active && station_selected) {
        // track station by mac address (addr2 for frames from sta)
        if (memcmp(hdr->addr2, selected_station.station_mac, 6) == 0) {
            match = true;
        }
    }
    
    if (!match) return;
    
    int8_t delta = rssi - tracking_last_rssi;
    
    if (rssi > tracking_max_rssi) tracking_max_rssi = rssi;
    if (rssi < tracking_min_rssi) tracking_min_rssi = rssi;
    
    const char *direction = "";
    if (delta > 5) direction = " ↑ CLOSER";
    else if (delta < -5) direction = " ↓ FARTHER";
    
    int bars = 0;
    if (rssi > -50) bars = 5;
    else if (rssi > -60) bars = 4;
    else if (rssi > -70) bars = 3;
    else if (rssi > -80) bars = 2;
    else if (rssi > -90) bars = 1;
    
    char bar_str[8] = "";
    for (int i = 0; i < bars; i++) {
        strcat(bar_str, "#");
    }
    
    glog("%s %d dBm (min:%d max:%d)%s\n", bar_str, rssi, tracking_min_rssi, tracking_max_rssi, direction);
    tracking_last_rssi = rssi;
    tracking_last_rx_us = esp_timer_get_time();
}

bool wifi_manager_get_track_status(int8_t *out_rssi, bool *out_fresh) {
    if (!ap_tracking_active && !sta_tracking_active) {
        return false;
    }
    if (out_rssi) {
        *out_rssi = tracking_last_rssi;
    }
    if (out_fresh) {
        int64_t now = esp_timer_get_time();
        // Consider the reading "live" if a matching packet arrived recently.
        *out_fresh = (tracking_last_rx_us != 0) && ((now - tracking_last_rx_us) < 1500000);
    }
    return true;
}

void wifi_manager_track_ap(void) {
    if (strlen((const char *)selected_ap.ssid) == 0) {
        glog("no ap selected. use 'select -a <index>' first.\n");
        return;
    }
    
    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden(selected_ap.ssid, sanitized_ssid, sizeof(sanitized_ssid));
    
    glog("=== tracking ap: %s ===\n", sanitized_ssid);
    glog("bssid: %02x:%02x:%02x:%02x:%02x:%02x\n",
         selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2],
         selected_ap.bssid[3], selected_ap.bssid[4], selected_ap.bssid[5]);
    glog("channel: %d\n", selected_ap.primary);
    glog("move closer to increase signal. type 'stop' to end.\n\n");
    
    tracking_last_rssi = selected_ap.rssi;
    tracking_min_rssi = selected_ap.rssi;
    tracking_max_rssi = selected_ap.rssi;
    tracking_last_rx_us = esp_timer_get_time();
    ap_tracking_active = true;
    sta_tracking_active = false;
    
    // set channel to ap's channel
    esp_wifi_set_channel(selected_ap.primary, WIFI_SECOND_CHAN_NONE);
    
    status_display_show_status("Track AP");
    wifi_manager_start_monitor_mode(wifi_track_callback);
}

void wifi_manager_track_sta(void) {
    if (!station_selected) {
        glog("no station selected. use 'select -s <index>' first.\n");
        return;
    }
    
    glog("=== tracking sta ===\n");
    glog("station: %02x:%02x:%02x:%02x:%02x:%02x\n",
         selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2],
         selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5]);
    glog("ap: %02x:%02x:%02x:%02x:%02x:%02x\n",
         selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2],
         selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    glog("move closer to increase signal. type 'stop' to end.\n\n");
    
    // find the channel for this station's ap
    int channel = 1;
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, selected_station.ap_bssid, 6) == 0) {
            channel = scanned_aps[i].primary;
            break;
        }
    }
    
    tracking_last_rssi = -100;
    tracking_min_rssi = -100;
    tracking_max_rssi = -127;
    tracking_last_rx_us = 0; // no station packet seen yet
    ap_tracking_active = false;
    sta_tracking_active = true;
    
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    status_display_show_status("Track STA");
    wifi_manager_start_monitor_mode(wifi_track_callback);
}

void wifi_manager_stop_tracking(void) {
    if (ap_tracking_active || sta_tracking_active) {
        ap_tracking_active = false;
        sta_tracking_active = false;
        wifi_manager_stop_monitor_mode();
        glog("tracking stopped.\n");
        status_display_show_status("Track Stopped");
    }
}
