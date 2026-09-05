// aerial_detector_manager.c
//
// opendroneid message decoding portions derived from:
// opendroneid-core-c (https://github.com/opendroneid/opendroneid-core-c)
// Licensed under Apache License 2.0
//
// detection approach informed by:
// nyanBOX by jbohack (https://github.com/jbohack/nyanBOX)
// Copyright (c) 2025 jbohack, Licensed under MIT
//
#include "managers/aerial_detector_manager.h"
#include "managers/ble_manager.h"
#include "core/glog.h"
#include "core/network_constants.h"   // is_dji_oui() + full DJI OUI table
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#endif

static const char *TAG = "AerialDetector";

// opendroneid message constants (astm f3411 standard)
#define ODID_MESSAGE_SIZE 25
#define ODID_ID_SIZE 20
#define ODID_STR_SIZE 23

// opendroneid message types
typedef enum {
    ODID_MSG_BASIC_ID = 0,
    ODID_MSG_LOCATION = 1,
    ODID_MSG_AUTH = 2,
    ODID_MSG_SELF_ID = 3,
    ODID_MSG_SYSTEM = 4,
    ODID_MSG_OPERATOR_ID = 5,
    ODID_MSG_PACKED = 0xF,
} odid_message_type_t;

// opendroneid id types
typedef enum {
    ODID_IDTYPE_NONE = 0,
    ODID_IDTYPE_SERIAL_NUMBER = 1,
    ODID_IDTYPE_CAA_REGISTRATION_ID = 2,
    ODID_IDTYPE_UTM_ASSIGNED_UUID = 3,
    ODID_IDTYPE_SPECIFIC_SESSION_ID = 4,
} odid_id_type_t;

// opendroneid ua types
typedef enum {
    ODID_UATYPE_NONE = 0,
    ODID_UATYPE_AEROPLANE = 1,
    ODID_UATYPE_HELICOPTER_OR_MULTIROTOR = 2,
    ODID_UATYPE_GYROPLANE = 3,
    ODID_UATYPE_HYBRID_LIFT = 4,
    ODID_UATYPE_ORNITHOPTER = 5,
    ODID_UATYPE_GLIDER = 6,
    ODID_UATYPE_KITE = 7,
    ODID_UATYPE_FREE_BALLOON = 8,
    ODID_UATYPE_CAPTIVE_BALLOON = 9,
    ODID_UATYPE_AIRSHIP = 10,
    ODID_UATYPE_FREE_FALL_PARACHUTE = 11,
    ODID_UATYPE_ROCKET = 12,
    ODID_UATYPE_TETHERED_POWERED_AIRCRAFT = 13,
    ODID_UATYPE_GROUND_OBSTACLE = 14,
    ODID_UATYPE_OTHER = 15,
} odid_ua_type_t;

// opendroneid status
typedef enum {
    ODID_STATUS_UNDECLARED = 0,
    ODID_STATUS_GROUND = 1,
    ODID_STATUS_AIRBORNE = 2,
    ODID_STATUS_EMERGENCY = 3,
    ODID_STATUS_SYSTEM_FAILURE = 4,
} odid_status_t;

// dji droneid identifiers (proprietary protocol)
// NOTE: drone *hardware* OUIs live in one place only — core/network_constants.c
// (DJI_OUIS[]/is_dji_oui() for DJI's 15 registered prefixes, and the broader
// SURVEIL_OUIS[]/drone_oui_lookup() covering every UAV vendor). Don't re-declare
// them here; the pair that used to live at this spot was stale and one of them
// (5C:E8:83) is actually Huawei, which flagged ordinary Huawei gear as a drone.
//
// This is the DJI DroneID *payload* signature, which is what a flying DJI drone
// actually broadcasts: an 802.11 vendor-specific IE (id 0xDD) carrying OUI
// 26:37:12, then vendor_type/unk1/unk2/subcommand. subcommand 0x10 = flight
// telemetry + location, 0x11 = flight purpose / operator-entered info.
// (Layout per Kismet's dot11_ie_221_dji_droneid parser.) Matching on this rather
// than the transmitter MAC is what catches drones that randomize their MAC or
// transmit from a radio-chipset OUI instead of a DJI-owned one.
#define DJI_DRONEID_IE_OUI_0 0x26
#define DJI_DRONEID_IE_OUI_1 0x37
#define DJI_DRONEID_IE_OUI_2 0x12
#define DJI_DRONEID_SUBCMD_FLIGHT_REG     0x10
#define DJI_DRONEID_SUBCMD_FLIGHT_PURPOSE 0x11
#define DJI_BLE_SERVICE_UUID 0xFFE0      // dji characteristic service

// wifi network patterns from manufacturers (validated 2024)
static const char *drone_ssid_patterns[] = {
    "DJI-",
    "Mavic",
    "Phantom",
    "Inspire",
    "Matrice",
    "ANAFI",           // parrot
    "Bebop",           // parrot
    "EVO-",            // autel
    "Autel",           // autel
    "SKYDIO",          // skydio
    "Skydio",          // skydio
    "AR.Drone",        // parrot
    "Aerodome",        // flock safety drone-as-first-responder program
    "Anzu",            // anzu robotics (flock's DFR hardware partner)
    "FPV-",            // generic fpv
    "DroneLink",       // generic
    NULL
};

// telemetry protocol identifiers (mavlink, frsky sport, crsf)
#define MAVLINK_STX_V1 0xFE
#define MAVLINK_STX_V2 0xFD
#define FRSKY_SPORT_START 0x7E
#define CRSF_SYNC_BYTE 0xC8

// nan (neighbor aware networking) destination for opendroneid wifi
static const uint8_t nan_dest_mac[6] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};

// device storage
static AerialDevice *devices = NULL;
static int device_count = 0;
static int device_capacity = 0;
static SemaphoreHandle_t device_mutex = NULL;

// scanning state
static bool is_scanning = false;
static bool wifi_scan_phase = false;
static bool ble_scan_phase = false;
static esp_timer_handle_t scan_timer = NULL;
static esp_timer_handle_t phase_timer = NULL;
static esp_timer_handle_t channel_hop_timer = NULL;
static uint32_t wifi_scan_duration_ms = 8000;
static uint32_t ble_scan_duration_ms = 8000;

// channel hopping
static uint8_t current_channel_index = 0;
static uint8_t allowed_channels[50];  // max channels across both bands
static uint8_t allowed_channel_count = 0;
static uint32_t channel_hop_interval_ms = 300;  // 300ms per channel

// detection feature flags
static bool enable_opendroneid = true;
static bool enable_dji = true;
static bool enable_networks = true;
static bool enable_telemetry = false;  // requires uart monitoring

// Diagnostics (opt-in, `aerialdiag on`). Detection is otherwise silent, which
// makes "my drone isn't showing" impossible to tell apart from "nothing was
// heard". When on, the sniffer reports every unique transmitter OUI it hears
// plus each DJI OUI / DroneID-IE hit, and each phase prints a frame tally.
// Rate-limited and de-duplicated so it can't flood the serial/LVGL path.
static bool diag_enabled = false;
#define DIAG_OUI_MAX 32
static uint8_t diag_seen_ouis[DIAG_OUI_MAX][3];
static uint8_t diag_seen_oui_count = 0;
static uint32_t diag_frames = 0, diag_mgmt = 0, diag_dji_oui_hits = 0, diag_droneid_hits = 0;
static uint32_t diag_last_log_ms = 0;

// Raw DroneID vendor-IE capture: one full hex dump per distinct subcommand
// value seen (0x10 flight_reg_info, 0x11 flight_purpose, and anything else --
// undocumented ones included, since this is meant to build real ground truth
// rather than only confirm what the decoder already expects). This is what
// makes the decoder's field offsets (currently just "per Kismet's parser",
// unverified against any of our own hardware) checkable against a real DJI
// airframe, and the OUI match it hangs off is model-agnostic -- it captures
// from whatever DJI drone is flying, not one specific model.
static bool diag_seen_subcmd[256];

// callback
static AerialDetectorCallback user_callback = NULL;
static void *user_callback_data = NULL;

// tracking
static char tracked_mac[AERIAL_MAC_STR_LEN] = {0};
static esp_timer_handle_t track_timer = NULL;
static uint32_t track_last_seen_ms = 0;


// forward declarations
static void wifi_sniffer_callback(void *buf, wifi_promiscuous_pkt_type_t type);
static void phase_switch_callback(void *arg);
static void channel_hop_callback(void *arg);
static void build_allowed_channels_list(void);
static AerialDevice* find_or_create_device(const uint8_t *mac);
#ifndef CONFIG_IDF_TARGET_ESP32S2
static void diag_dump_dji_ble(const char *mac, const uint8_t *adv, int adv_len);
#endif
static void decode_opendroneid_message(AerialDevice *device, const uint8_t *data, size_t len);
static void decode_dji_message(AerialDevice *device, const uint8_t *data, size_t len);
static void check_drone_network(AerialDevice *device, const char *ssid);
static void notify_callback(AerialDevice *device);
static void start_wifi_phase(void);
static void start_ble_phase(void);
static void stop_wifi_phase(void);
static void stop_ble_phase(void);
static void track_timer_callback(void *arg);
static void start_track_timer(void);
static void stop_track_timer(void);

void aerial_detector_init(void) {
    if (device_mutex == NULL) {
        device_mutex = xSemaphoreCreateMutex();
    }

    // stop_scan retains the buffer now, so free any prior allocation here rather
    // than just dropping the pointer (avoids a leak when re-init'd per session).
    if (devices) {
        free(devices);
    }
    devices = NULL;
    device_count = 0;
    device_capacity = 0;
    is_scanning = false;
    wifi_scan_phase = false;
    ble_scan_phase = false;

    build_allowed_channels_list();
    
    ESP_LOGI(TAG, "aerial detector initialized with %d channels", allowed_channel_count);
}

void aerial_detector_deinit(void) {
    aerial_detector_stop_scan();
    
    if (devices) {
        free(devices);
        devices = NULL;
        device_count = 0;
        device_capacity = 0;
    }
    
    if (device_mutex) {
        vSemaphoreDelete(device_mutex);
        device_mutex = NULL;
    }
    
    ESP_LOGI(TAG, "aerial detector deinitialized");
}

static void build_allowed_channels_list(void) {
    allowed_channel_count = 0;

    // Always scan the FULL 2.4GHz band (channels 1-14). Non-overlapping 1/6/11
    // first so the most common drone channels are hit early in each hop cycle;
    // the rest follow. We deliberately ignore the WiFi country's channel count for
    // 2.4GHz here — this is passive RX for drone detection, and start_wifi_phase
    // sets a permissive country so esp_wifi_set_channel() accepts 12-14.
    for (size_t i = 0; i < WIFI_CHANNELS_2GHZ_ORDER_COUNT && allowed_channel_count < 45; i++) {
        allowed_channels[allowed_channel_count++] = WIFI_CHANNELS_2GHZ_ORDER[i];
    }

    #ifdef CONFIG_IDF_TARGET_ESP32C5
    // 5GHz band (C5 only) is region-dependent; pick channels from the country.
    wifi_country_t country;
    if (esp_wifi_get_country(&country) != ESP_OK) {
        // default: unii-1 only (most permissive worldwide)
        uint8_t default_5ghz[] = {36, 40, 44, 48};
        for (int i = 0; i < (int)sizeof(default_5ghz) && allowed_channel_count < 50; i++) {
            allowed_channels[allowed_channel_count++] = default_5ghz[i];
        }
        ESP_LOGI(TAG, "aerial channels: %d (2.4GHz 1-14 + default 5GHz)", allowed_channel_count);
        return;
    }
    // 5ghz band support for esp32-c5
    // add channels based on country code
    // unii-1 (5.15-5.25 ghz): channels 36, 40, 44, 48
    // unii-2a (5.25-5.35 ghz): channels 52, 56, 60, 64
    // unii-2c (5.47-5.725 ghz): channels 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144
    // unii-3 (5.725-5.85 ghz): channels 149, 153, 157, 161, 165
    
    if (strcmp(country.cc, "US") == 0 || strcmp(country.cc, "CA") == 0) {
        // north america: all bands allowed
        uint8_t us_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
        for (int i = 0; i < sizeof(us_5ghz) && allowed_channel_count < 50; i++) {
            allowed_channels[allowed_channel_count++] = us_5ghz[i];
        }
    } else if (strcmp(country.cc, "JP") == 0) {
        // japan: all bands with restrictions
        uint8_t jp_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140};
        for (int i = 0; i < sizeof(jp_5ghz) && allowed_channel_count < 50; i++) {
            allowed_channels[allowed_channel_count++] = jp_5ghz[i];
        }
    } else if (strcmp(country.cc, "CN") == 0) {
        // china: limited 5ghz
        uint8_t cn_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 149, 153, 157, 161, 165};
        for (int i = 0; i < sizeof(cn_5ghz) && allowed_channel_count < 50; i++) {
            allowed_channels[allowed_channel_count++] = cn_5ghz[i];
        }
    } else if (strcmp(country.cc, "EU") == 0 || strcmp(country.cc, "GB") == 0 || 
               strcmp(country.cc, "DE") == 0 || strcmp(country.cc, "FR") == 0) {
        // europe: unii-1 and unii-2
        uint8_t eu_5ghz[] = {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140};
        for (int i = 0; i < sizeof(eu_5ghz) && allowed_channel_count < 50; i++) {
            allowed_channels[allowed_channel_count++] = eu_5ghz[i];
        }
    } else {
        // default: unii-1 only (most permissive worldwide)
        uint8_t default_5ghz[] = {36, 40, 44, 48};
        for (int i = 0; i < sizeof(default_5ghz) && allowed_channel_count < 50; i++) {
            allowed_channels[allowed_channel_count++] = default_5ghz[i];
        }
    }
    #endif

    ESP_LOGI(TAG, "aerial detector: scanning %d channels (full 2.4GHz 1-14)", allowed_channel_count);
}

static void channel_hop_callback(void *arg) {
    if (!wifi_scan_phase) return;
    
    // move to next channel
    current_channel_index = (current_channel_index + 1) % allowed_channel_count;
    uint8_t channel = allowed_channels[current_channel_index];
    
    // determine if 5ghz or 2.4ghz
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    
    #ifdef CONFIG_IDF_TARGET_ESP32C5
    if (channel > 14) {
        // 5ghz channel - use ht40
        // for 5ghz, channels are spaced 4 apart, so we can use ht40
        second = WIFI_SECOND_CHAN_ABOVE;
    }
    #endif
    
    esp_wifi_set_channel(channel, second);
    ESP_LOGD(TAG, "hopped to channel %d", channel);
}

static void start_wifi_phase(void) {
    wifi_scan_phase = true;
    ble_scan_phase = false;
    
    glog("Phase 1: WiFi Scan\n");
    
    // wifi already running, just enable promiscuous mode
    wifi_mode_t current_mode;
    esp_err_t ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }

    // Permit the full 2.4GHz range for scanning: some regions (e.g. US nchan=11)
    // otherwise make esp_wifi_set_channel() reject ch 12-14. Manual policy over
    // channels 1-14 so every channel we hop is actually tunable for RX.
    wifi_country_t scan_country = { .cc = "JP", .schan = 1, .nchan = 14,
                                    .policy = WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&scan_country);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_callback);
    
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
    };
    esp_wifi_set_promiscuous_filter(&filter);
    
    // Resume hopping where the previous phase left off. A single DRONES phase
    // (~2.5s at 300ms/hop) only visits ~8 channels; resetting to index 0 every
    // phase meant channels past that (e.g. 2.4GHz ch 8-13 once a country adds the
    // overlapping channels) were never scanned. Resuming lets successive phases
    // eventually cover every channel so a drone isn't missed for being "late" in
    // the list.
    if (allowed_channel_count > 0) {
        if (current_channel_index >= allowed_channel_count) current_channel_index = 0;
        esp_wifi_set_channel(allowed_channels[current_channel_index], WIFI_SECOND_CHAN_NONE);
    } else {
        esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);  // fallback
    }
    
    // start channel hopping timer
    esp_timer_create_args_t timer_args = {
        .callback = channel_hop_callback,
        .name = "aerial_channel_hop"
    };
    esp_timer_create(&timer_args, &channel_hop_timer);
    esp_timer_start_periodic(channel_hop_timer, channel_hop_interval_ms * 1000);
    
    ESP_LOGI(TAG, "wifi scan phase started (hopping %d channels @ %lums intervals)", 
             allowed_channel_count, channel_hop_interval_ms);
}

static void stop_wifi_phase(void) {
    if (!wifi_scan_phase) return;
    
    // stop channel hopping
    if (channel_hop_timer) {
        esp_timer_stop(channel_hop_timer);
        esp_timer_delete(channel_hop_timer);
        channel_hop_timer = NULL;
    }
    
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    wifi_scan_phase = false;

    if (diag_enabled) {
        // The decisive line: zero mgmt frames means we never heard the drone at
        // all (wrong channel, or it isn't emitting 802.11); frames but no hits
        // means it's transmitting something without a DJI signature.
        glog("[AERIAL] phase: %lu frames (%lu mgmt), DJI-OUI %lu, DroneID %lu, devices %d\n",
             (unsigned long)diag_frames, (unsigned long)diag_mgmt,
             (unsigned long)diag_dji_oui_hits, (unsigned long)diag_droneid_hits,
             aerial_detector_get_device_count());
        diag_frames = diag_mgmt = 0;
    }

    ESP_LOGI(TAG, "wifi scan phase stopped");
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
static void aerial_ble_data_handler(struct ble_gap_event *event, size_t len) {
    if (!event || !ble_scan_phase) return;
    
    // extract advertisement data
    if (event->type != BLE_GAP_EVENT_DISC) return;
    
    uint8_t *addr = event->disc.addr.val;
    int8_t rssi = event->disc.rssi;
    const uint8_t *adv_data = event->disc.data;
    size_t adv_len = event->disc.length_data;
    
    AerialDevice *device = NULL;
    bool detected = false;
    bool is_new_device = false;

    #define ENSURE_DEVICE()                                      \
        do {                                                     \
            if (!device) {                                       \
                device = find_or_create_device(addr);            \
                if (!device) return;                             \
                device->rssi = rssi;                             \
                device->last_seen_ms = esp_timer_get_time() / 1000; \
                is_new_device = (device->type == AERIAL_TYPE_UNKNOWN); \
            }                                                    \
        } while (0)
    
    // scan through ad structures looking for opendroneid or dji
    size_t i = 0;
    while (i < adv_len) {
        uint8_t field_len = adv_data[i];
        if (field_len == 0 || i + field_len >= adv_len) break;
        
        uint8_t field_type = adv_data[i + 1];
        
        // check for 16-bit service data (0x16)
        if (field_type == 0x16 && field_len >= 4) {
            uint16_t service_uuid = adv_data[i + 2] | (adv_data[i + 3] << 8);
            
            // opendroneid service uuid 0xFFFA
            if (enable_opendroneid && service_uuid == 0xFFFA && field_len >= 2 + ODID_MESSAGE_SIZE) {
                ENSURE_DEVICE();
                AerialDeviceType old_type = device->type;
                device->type = AERIAL_TYPE_REMOTEID_BLE;
                // skip length, type, uuid (4 bytes) and optional counter (1 byte) = 5 bytes
                decode_opendroneid_message(device, &adv_data[i + 5], ODID_MESSAGE_SIZE);
                detected = true;
                
                // only log if this is newly detected
                if (old_type == AERIAL_TYPE_UNKNOWN) {
                    glog("Drone: %s,\n", device->device_id);
                    glog("     MAC: %s,\n", device->mac);
                    glog("     RSSI: %d dBm,\n", rssi);
                    glog("     Type: RemoteID BLE,\n");
                    glog("     Status: %s,\n", aerial_detector_get_status_string(device->status));
                    
                    if (device->has_location) {
                        glog("     Location: %.6f, %.6f,\n", device->latitude, device->longitude);
                        if (device->altitude > -1000.0f) {
                            glog("     Altitude: %.1f m,\n", device->altitude);
                        }
                        if (device->speed_horizontal < 255.0f) {
                            glog("     Speed: %.1f m/s,\n", device->speed_horizontal);
                        }
                        if (device->direction <= 360.0f) {
                            glog("     Direction: %.0f deg,\n", device->direction);
                        }
                    }
                    
                    if (device->description[0] != '\0' && strcmp(device->description, "N/A") != 0) {
                        glog("     Description: %s,\n", device->description);
                    }
                    
                    if (device->has_operator_location) {
                        glog("     Operator: %.6f, %.6f,\n", 
                             device->operator_latitude, device->operator_longitude);
                    }
                    glog("\n");
                }
            }
            // dji service uuid 0xFFE0
            else if (enable_dji && service_uuid == 0xFFE0 && is_new_device) {
                ENSURE_DEVICE();
                device->type = AERIAL_TYPE_DJI_BLE;
                snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "DJI");
                decode_dji_message(device, adv_data, adv_len);
                detected = true;
                if (diag_enabled) diag_dump_dji_ble(device->mac, adv_data, (int)adv_len);
                glog("DJI Device,\n");
                glog("     MAC: %s,\n", device->mac);
                glog("     RSSI: %d dBm,\n", rssi);
                glog("\n");
            }
        }
        
        i += field_len + 1;
    }
    
    if (detected && device) {
        notify_callback(device);
    }
    
    #undef ENSURE_DEVICE
}
#else
// Stub for ESP32-S2 (no NimBLE)
static void aerial_ble_data_handler(void *event, size_t len) {
    (void)event;
    (void)len;
}
#endif

static void start_ble_phase(void) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
    wifi_scan_phase = false;
    ble_scan_phase = true;
    
    glog("Phase 2: BLE Scan\n");
    
    // initialize ble FIRST (suspends wifi automatically)
    // NOTE: ble_init() clears all handlers, so we must register AFTER init!
    ble_init();
    
    // register our handler after ble_init cleared handlers
    ble_register_handler(aerial_ble_data_handler);
    
    // start scanning
    ble_start_scanning();
    
    ESP_LOGI(TAG, "ble scan phase started");
#else
    ESP_LOGW(TAG, "ble not supported on esp32-s2");
#endif
}

static void stop_ble_phase(void) {
    if (!ble_scan_phase) return;
    
#ifndef CONFIG_IDF_TARGET_ESP32S2
    // unregister our handler
    ble_unregister_handler(aerial_ble_data_handler);
    
    // stop ble (restores wifi automatically via ble_resume_networking)
    ble_deinit();
    
    ESP_LOGI(TAG, "ble scan phase stopped (wifi auto-restored)");
#endif
    
    ble_scan_phase = false;
}

static void phase_switch_callback(void *arg) {
    if (!is_scanning) return;
    
    if (wifi_scan_phase) {
        // switch from wifi to ble
        stop_wifi_phase();
        start_ble_phase();
        
        // schedule next phase or end
        if (ble_scan_duration_ms > 0) {
            esp_timer_start_once(phase_timer, ble_scan_duration_ms * 1000);
        }
    } else if (ble_scan_phase) {
        // ble phase done, stop everything
        stop_ble_phase();
        aerial_detector_stop_scan();
    }
}

static void track_timer_callback(void *arg) {
    (void)arg;
    AerialDevice *dev = aerial_detector_get_tracked_device();
    if (!dev || dev->type == AERIAL_TYPE_UNKNOWN) return;
    
    uint32_t age_sec = (esp_timer_get_time() / 1000 - dev->last_seen_ms) / 1000;
    if (dev->has_location) {
        glog("AerialTrack %s RSSI=%ddBm age=%lus loc=%.5f,%.5f alt=%.1fm\n",
             dev->mac, dev->rssi, age_sec,
             dev->latitude, dev->longitude,
             (dev->altitude > -1000.0f) ? dev->altitude : 0.0f);
    } else {
        glog("AerialTrack %s RSSI=%ddBm age=%lus\n",
             dev->mac, dev->rssi, age_sec);
    }
}

static void start_track_timer(void) {
    if (track_timer) return;
    
    esp_timer_create_args_t targs = {
        .callback = track_timer_callback,
        .name = "aerial_track"
    };
    if (esp_timer_create(&targs, &track_timer) == ESP_OK) {
        esp_timer_start_periodic(track_timer, 500000); // 500ms
    } else {
        track_timer = NULL;
    }
}

static void stop_track_timer(void) {
    if (track_timer) {
        esp_timer_stop(track_timer);
        esp_timer_delete(track_timer);
        track_timer = NULL;
    }
}


esp_err_t aerial_detector_start_scan(uint32_t duration_ms) {
    if (is_scanning) {
        ESP_LOGW(TAG, "scan already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!devices) {
        device_capacity = AERIAL_MAX_DEVICES;
        devices = (AerialDevice *)calloc(device_capacity, sizeof(AerialDevice));
        if (!devices) {
            ESP_LOGE(TAG, "failed to allocate device array");
            return ESP_ERR_NO_MEM;
        }
        device_count = 0;
        ESP_LOGI(TAG, "allocated %d device slots", device_capacity);
    }
    
    is_scanning = true;

    // Round-robin always hops at the standard interval, even if a prior Drones-
    // tile focus session left the fast interval in place (see
    // aerial_detector_start_scan_wifi).
    channel_hop_interval_ms = 300;

    // calculate phase durations (split time between wifi and ble)
    wifi_scan_duration_ms = duration_ms / 2;
    ble_scan_duration_ms = duration_ms / 2;
    if (wifi_scan_duration_ms < 5000) wifi_scan_duration_ms = 5000;
    if (ble_scan_duration_ms < 5000) ble_scan_duration_ms = 5000;
    
    // create phase timer
    esp_timer_create_args_t timer_args = {
        .callback = phase_switch_callback,
        .name = "aerial_phase_timer"
    };
    esp_timer_create(&timer_args, &phase_timer);
    
    // start with wifi phase
    // note: wifi should already be running in ghost esp
    // we just enable promiscuous mode
    start_wifi_phase();
    
    // schedule switch to ble phase
    esp_timer_start_once(phase_timer, wifi_scan_duration_ms * 1000);
    
    ESP_LOGI(TAG, "scan started: wifi %lums then ble %lums",
             wifi_scan_duration_ms, ble_scan_duration_ms);
    return ESP_OK;
}

// Continuous WiFi-only drone hunt used by the Drones-tile focus mode.
//
// The normal round-robin DRONES phase does start_scan -> ~2.5s dwell ->
// accumulate -> stop_scan every cycle. stop_scan wipes the device buffer and
// tears down promiscuous mode, and 2.5s at 300ms/hop only visits ~8 of 14
// channels -- so a DJI DroneID beacon sitting on one channel can be missed for
// many cycles (the "30s to acquire" problem). This entry point instead:
//   * stays in the WiFi phase indefinitely (no phase timer -> never switches to
//     BLE, never auto-stops),
//   * hops fast (caller passes ~150ms -> a full 14-channel sweep in ~2s),
//   * leaves the scan running so the caller can refresh the accumulator in place
//     without wiping the aerial device list between refreshes.
// The caller ends it with aerial_detector_stop_scan(). DJI DroneID is a WiFi
// signature, so dropping the BLE half costs no DJI coverage; BLE OpenDroneID
// remote-ID is still covered by the round-robin.
esp_err_t aerial_detector_start_scan_wifi(uint32_t hop_interval_ms) {
    if (is_scanning) {
        ESP_LOGW(TAG, "scan already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (!devices) {
        device_capacity = AERIAL_MAX_DEVICES;
        devices = (AerialDevice *)calloc(device_capacity, sizeof(AerialDevice));
        if (!devices) {
            ESP_LOGE(TAG, "failed to allocate device array");
            return ESP_ERR_NO_MEM;
        }
        device_count = 0;
    }

    if (hop_interval_ms < 50)   hop_interval_ms = 50;    // esp_timer / driver floor
    if (hop_interval_ms > 1000) hop_interval_ms = 1000;
    channel_hop_interval_ms = hop_interval_ms;

    is_scanning = true;
    wifi_scan_duration_ms = 0;   // no phase timer: WiFi phase runs until stop
    ble_scan_duration_ms = 0;

    start_wifi_phase();

    ESP_LOGI(TAG, "wifi-only drone focus scan started (hop %lums, %d channels)",
             channel_hop_interval_ms, allowed_channel_count);
    return ESP_OK;
}

esp_err_t aerial_detector_stop_scan(void) {
    if (!is_scanning) {
        return ESP_OK;
    }
    
    is_scanning = false;
    
    if (phase_timer) {
        esp_timer_stop(phase_timer);
        esp_timer_delete(phase_timer);
        phase_timer = NULL;
    }
    
    if (scan_timer) {
        esp_timer_stop(scan_timer);
        esp_timer_delete(scan_timer);
        scan_timer = NULL;
    }
    
    stop_wifi_phase();
    stop_ble_phase();
    
    // Keep the device buffer allocated across stop/start (it is freed only in
    // aerial_detector_deinit). Freeing it here races the WiFi/BLE decode
    // callbacks, which write through a device pointer returned by
    // find_or_create_device *after* releasing device_mutex — a write-to-freed
    // heap that aborts under the dashboard's rapid start/stop cycling. Reset the
    // count under the mutex instead so the buffer stays valid for late callbacks.
    if (devices && xSemaphoreTake(device_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(devices, 0, device_capacity * sizeof(AerialDevice));
        device_count = 0;
        xSemaphoreGive(device_mutex);
    }

    ESP_LOGI(TAG, "scan stopped");
    glog("Scan Complete\n");
    return ESP_OK;
}

bool aerial_detector_is_scanning(void) {
    return is_scanning;
}

// Report a transmitter OUI once per session (diag only). Tells us what the drone
// is actually sending if neither the OUI nor the DroneID signature matched.
static void diag_note_oui(const uint8_t *mac, int channel, int rssi) {
    for (int i = 0; i < diag_seen_oui_count; i++) {
        if (memcmp(diag_seen_ouis[i], mac, 3) == 0) return;
    }
    if (diag_seen_oui_count >= DIAG_OUI_MAX) return;
    memcpy(diag_seen_ouis[diag_seen_oui_count++], mac, 3);
    glog("[AERIAL] OUI %02X:%02X:%02X  ch%d  %ddBm%s\n", mac[0], mac[1], mac[2],
         channel, rssi, is_dji_oui(mac) ? "  <-- DJI" : "");
}

// Renders up to 100 bytes of `buf` as "XX XX XX ..." into `out` (caller-sized,
// used at 3*100+1 below). Shared by every raw-capture dump so ground truth for
// a new DJI model/firmware always comes out in the same copy-pasteable form.
static void diag_hex(const uint8_t *buf, int len, char *out, size_t outsz) {
    int hexlen = 0;
    if (len > 100) len = 100;
    for (int b = 0; b < len && hexlen + 3 < (int)outsz; b++) {
        hexlen += snprintf(out + hexlen, outsz - hexlen, "%02X ", buf[b]);
    }
}

// One full hex dump per distinct DJI DroneID subcommand value, the first time
// each is seen while diagnostics are on. `ie` points at the IE's type byte
// (0xDD), `ie_total_len` is type+len+payload bytes to print (already bounds-
// clamped by the caller against the frame buffer).
static void diag_dump_dji_ie(uint8_t subcommand, const char *mac, const uint8_t *ie,
                              int ie_total_len) {
    if (diag_seen_subcmd[subcommand]) return;
    diag_seen_subcmd[subcommand] = true;
    // static, not stack-local: this runs inside the promiscuous WiFi RX
    // callback's call chain, a context that's bitten this codebase before on
    // tight stack margins -- no reason to add ~300 bytes of frame there when
    // this is diagnostic-only, off by default, and already single-shot.
    static char hex[3 * 100 + 1];
    diag_hex(ie, ie_total_len, hex, sizeof(hex));
    glog("[AERIAL] RAW WiFi IE subcmd 0x%02X (%d bytes) from %s: %s\n",
         subcommand, ie_total_len, mac, hex);
}

// Same idea for the BLE DJI service (0xFFE0) path, which carries the whole
// advertisement rather than a subcommand-framed 802.11 IE -- one dump total
// (BLE ads don't carry a subcommand byte to key on) per diag session. No BLE
// hardware on ESP32-S2, so guard it like every other BLE-only symbol here to
// avoid an unused-function warning on that target.
#ifndef CONFIG_IDF_TARGET_ESP32S2
static bool diag_seen_dji_ble = false;
static void diag_dump_dji_ble(const char *mac, const uint8_t *adv, int adv_len) {
    if (diag_seen_dji_ble) return;
    diag_seen_dji_ble = true;
    static char hex[3 * 100 + 1];   // see diag_dump_dji_ie: static to spare the stack
    diag_hex(adv, adv_len, hex, sizeof(hex));
    glog("[AERIAL] RAW BLE adv (%d bytes) from %s: %s\n", adv_len, mac, hex);
}
#endif

static void wifi_sniffer_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!enable_opendroneid && !enable_dji && !enable_networks) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 40) return;

    uint8_t *src_mac = &payload[10];

    if (diag_enabled) {
        diag_frames++;
        uint8_t ftype = payload[0] & 0xFC;
        if (ftype == 0x80 || ftype == 0x50) diag_mgmt++;
        // Rate-limit the per-OUI lines so a busy band can't flood serial.
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now - diag_last_log_ms) >= 300) {
            diag_last_log_ms = now;
            diag_note_oui(src_mac, pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi);
        }
    }
    AerialDevice *device = NULL;
    bool detected = false;

    // lazy-create device only when we have a real detection
    #define ENSURE_DEVICE()                          \
        do {                                         \
            if (!device) {                           \
                device = find_or_create_device(src_mac); \
                if (!device) return;                 \
                device->rssi = pkt->rx_ctrl.rssi;    \
                device->channel = pkt->rx_ctrl.channel; \
                device->last_seen_ms = esp_timer_get_time() / 1000; \
            }                                        \
        } while (0)
    
    // check for opendroneid nan frames
    if (enable_opendroneid && memcmp(&payload[4], nan_dest_mac, 6) == 0) {
        for (int offset = 26; offset < len - ODID_MESSAGE_SIZE; offset++) {
            ENSURE_DEVICE();
            decode_opendroneid_message(device, &payload[offset], ODID_MESSAGE_SIZE);
            detected = true;
        }
    }
    
    // check for opendroneid in vendor specific ies (beacon frames)
    if (enable_opendroneid && payload[0] == 0x80) {
        int offset = 36;
        while (offset < len - 6) {
            uint8_t ie_type = payload[offset];
            uint8_t ie_len = payload[offset + 1];
            
            if (ie_type == 0xDD && ie_len >= 4) {
                // check for opendroneid oui (0x90 0x3A 0xE6 or 0xFA 0x0B 0xBC)
                if ((payload[offset + 2] == 0x90 && payload[offset + 3] == 0x3A && payload[offset + 4] == 0xE6) ||
                    (payload[offset + 2] == 0xFA && payload[offset + 3] == 0x0B && payload[offset + 4] == 0xBC)) {
                    ENSURE_DEVICE();
                    decode_opendroneid_message(device, &payload[offset + 7], ODID_MESSAGE_SIZE);
                    detected = true;
                }
            }
            
            offset += ie_len + 2;
            if (offset >= len) break;
        }
    }
    
    // DJI DroneID vendor IE (the signature a flying DJI drone actually emits).
    // Matched on payload, not MAC, so it still catches a drone that randomizes
    // its transmitter MAC or transmits from a radio-chipset OUI. Beacons (0x80)
    // and probe responses (0x50) both carry it.
    if (enable_dji && (payload[0] == 0x80 || payload[0] == 0x50)) {
        int offset = 36;
        while (offset + 1 < len) {
            uint8_t ie_type = payload[offset];
            uint8_t ie_len = payload[offset + 1];

            // Need the 3-byte OUI plus vendor_type/unk1/unk2/subcommand behind it.
            if (ie_type == 0xDD && ie_len >= 7 && (offset + 8) < len &&
                payload[offset + 2] == DJI_DRONEID_IE_OUI_0 &&
                payload[offset + 3] == DJI_DRONEID_IE_OUI_1 &&
                payload[offset + 4] == DJI_DRONEID_IE_OUI_2) {
                uint8_t subcommand = payload[offset + 8];
                ENSURE_DEVICE();
                device->type = AERIAL_TYPE_DJI_WIFI;
                snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "DJI");
                detected = true;
                if (diag_enabled) {
                    int ie_total = (int)ie_len + 2;             // type + len + payload
                    int avail = len - offset;                    // bytes actually in this frame
                    if (ie_total > avail) ie_total = avail;
                    diag_dump_dji_ie(subcommand, device->mac, &payload[offset], ie_total);
                }
                // flight_reg_info (0x10) telemetry sits after the 4-byte vendor
                // header (vendor_type/unk1/unk2/subcommand): payload[offset+9..].
                if (subcommand == DJI_DRONEID_SUBCMD_FLIGHT_REG &&
                    ie_len >= 7 + 53 && (offset + 9 + 53) <= len) {
                    const uint8_t *fri = &payload[offset + 9];
                    int fri_len = (int)ie_len - 7;
                    decode_dji_message(device, fri, (size_t)fri_len);
                    if (diag_enabled) {
                        // Ground truth: the user confirms these against the armed
                        // drone before we trust the on-screen numbers.
                        glog("[AERIAL] DroneID v%u %s: aircraft %s %.6f,%.6f  operator %s %.6f,%.6f\n",
                             fri[0], device->mac,
                             device->has_location ? "@" : "(none)",
                             device->latitude, device->longitude,
                             device->has_operator_location ? "@" : "(none)",
                             device->operator_latitude, device->operator_longitude);
                    }
                }
                if (diag_enabled) {
                    diag_droneid_hits++;
                    glog("[AERIAL] DroneID IE from %s ch%d %ddBm subcmd 0x%02X\n",
                         device->mac, pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi, subcommand);
                }
                break;
            }

            if (ie_len == 0 && ie_type == 0) break;   // padding / malformed
            offset += ie_len + 2;
        }
    }

    // check for dji specific patterns — match the transmitter MAC against the
    // full set of DJI-registered OUIs (Mini/Air/Mavic/etc.), not a hardcoded pair.
    // Transmitter-MAC match against every UAV vendor in the IEEE-derived table
    // (DJI's 15 prefixes plus Parrot, Skydio, Teal, Freefly, PowerVision), so a
    // non-DJI drone is identified and named rather than ignored.
    const char *drone_vendor = NULL;
    if (enable_dji && drone_oui_lookup(src_mac, &drone_vendor)) {
        ENSURE_DEVICE();
        device->type = AERIAL_TYPE_DJI_WIFI;
        snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "%s", drone_vendor);
        // Telemetry is decoded from the DroneID IE above, not from a bare OUI
        // match (which carries no flight_reg_info payload).
        detected = true;
        if (diag_enabled) {
            diag_dji_oui_hits++;
            glog("[AERIAL] drone OUI hit %s (%s) ch%d %ddBm\n", device->mac,
                 drone_vendor, pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi);
        }
    }
    
    // check for drone network ssids (beacon frames)
    if (enable_networks && payload[0] == 0x80) {
        int offset = 36;
        while (offset < len - 2) {
            uint8_t ie_type = payload[offset];
            uint8_t ie_len = payload[offset + 1];
            
            if (ie_type == 0 && ie_len > 0 && ie_len < 33) {  // ssid ie
                char ssid[33];
                memcpy(ssid, &payload[offset + 2], ie_len);
                ssid[ie_len] = '\0';
                ENSURE_DEVICE();
                check_drone_network(device, ssid);
                if (device->type == AERIAL_TYPE_DRONE_NETWORK) {
                    detected = true;
                }
                break;
            }
            
            offset += ie_len + 2;
            if (offset >= len) break;
        }
    }
    
    if (detected && device) {
        notify_callback(device);
    }

    #undef ENSURE_DEVICE
}


static AerialDevice* find_or_create_device(const uint8_t *mac) {
    char mac_str[AERIAL_MAC_STR_LEN];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    if (!devices) {
        return NULL;
    }
    
    if (xSemaphoreTake(device_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return NULL;
    }

    // Reject callbacks that fire after the scan stopped (the buffer is retained
    // across stop, so without this a late packet would re-populate the count).
    if (!is_scanning) {
        xSemaphoreGive(device_mutex);
        return NULL;
    }

    if (tracked_mac[0] != '\0' && strcmp(mac_str, tracked_mac) != 0) {
        xSemaphoreGive(device_mutex);
        return NULL;
    }
    
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].mac, mac_str) == 0) {
            xSemaphoreGive(device_mutex);
            return &devices[i];
        }
    }
    
    if (device_count >= device_capacity) {
        xSemaphoreGive(device_mutex);
        return NULL;
    }
    
    AerialDevice *device = &devices[device_count++];
    memset(device, 0, sizeof(AerialDevice));
    strlcpy(device->mac, mac_str, sizeof(device->mac));
    strlcpy(device->device_id, "Unknown", sizeof(device->device_id));
    strlcpy(device->operator_id, "N/A", sizeof(device->operator_id));
    strlcpy(device->description, "N/A", sizeof(device->description));
    device->type = AERIAL_TYPE_UNKNOWN;
    device->status = AERIAL_STATUS_UNKNOWN;
    device->altitude = -1000.0f;
    device->operator_altitude = -1000.0f;
    
    xSemaphoreGive(device_mutex);
    return device;
}

static void decode_opendroneid_message(AerialDevice *device, const uint8_t *data, size_t len) {
    if (len < ODID_MESSAGE_SIZE) return;
    
    uint8_t msg_type = (data[0] >> 4) & 0x0F;
    
    switch (msg_type) {
        case ODID_MSG_BASIC_ID: {
            device->messages_seen |= (1 << 0);
            device->aircraft_type = data[1] & 0x0F;
            // uint8_t id_type = (data[1] >> 4) & 0x0F;
            
            memcpy(device->device_id, &data[2], ODID_ID_SIZE);
            device->device_id[ODID_ID_SIZE] = '\0';
            
            // clean non-printable characters
            for (int i = 0; i < ODID_ID_SIZE; i++) {
                if (device->device_id[i] < 32 || device->device_id[i] > 126) {
                    device->device_id[i] = '\0';
                    break;
                }
            }
            
            if (device->type == AERIAL_TYPE_UNKNOWN) {
                device->type = AERIAL_TYPE_REMOTEID_WIFI;
            }
            break;
        }
        
        case ODID_MSG_LOCATION: {
            device->messages_seen |= (1 << 1);
            device->status = (data[1] >> 4) & 0x0F;
            device->has_location = true;
            
            // decode lat/lon (int32 in 1e-7 degree units)
            int32_t lat = (int32_t)((uint32_t)data[5] | ((uint32_t)data[6] << 8) | 
                                   ((uint32_t)data[7] << 16) | ((uint32_t)data[8] << 24));
            device->latitude = (double)lat / 10000000.0;
            
            int32_t lon = (int32_t)((uint32_t)data[9] | ((uint32_t)data[10] << 8) | 
                                   ((uint32_t)data[11] << 16) | ((uint32_t)data[12] << 24));
            device->longitude = (double)lon / 10000000.0;
            
            // decode altitude (uint16 in 0.5m units, offset -1000m)
            uint16_t alt = (uint16_t)data[15] | ((uint16_t)data[16] << 8);
            if (alt != 0xFFFF) {
                device->altitude = (float)alt * 0.5f - 1000.0f;
            }
            
            // decode speed (uint8 in 0.25 m/s units)
            if (data[3] != 255) {
                device->speed_horizontal = (float)data[3] * 0.25f;
            }
            
            // decode direction
            device->direction = (float)data[2];
            break;
        }
        
        case ODID_MSG_SELF_ID: {
            device->messages_seen |= (1 << 3);
            memcpy(device->description, &data[2], ODID_STR_SIZE);
            device->description[ODID_STR_SIZE] = '\0';
            
            for (int i = 0; i < ODID_STR_SIZE; i++) {
                if (device->description[i] < 32 || device->description[i] > 126) {
                    device->description[i] = '\0';
                    break;
                }
            }
            break;
        }
        
        case ODID_MSG_SYSTEM: {
            device->messages_seen |= (1 << 4);
            device->has_operator_location = true;
            
            int32_t op_lat = (int32_t)((uint32_t)data[2] | ((uint32_t)data[3] << 8) | 
                                      ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24));
            device->operator_latitude = (double)op_lat / 10000000.0;
            
            int32_t op_lon = (int32_t)((uint32_t)data[6] | ((uint32_t)data[7] << 8) | 
                                      ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24));
            device->operator_longitude = (double)op_lon / 10000000.0;
            
            uint16_t op_alt = (uint16_t)data[18] | ((uint16_t)data[19] << 8);
            if (op_alt != 0xFFFF) {
                device->operator_altitude = (float)op_alt * 0.5f - 1000.0f;
            }
            break;
        }
        
        case ODID_MSG_OPERATOR_ID: {
            device->messages_seen |= (1 << 5);
            memcpy(device->operator_id, &data[2], ODID_ID_SIZE);
            device->operator_id[ODID_ID_SIZE] = '\0';
            
            for (int i = 0; i < ODID_ID_SIZE; i++) {
                if (device->operator_id[i] < 32 || device->operator_id[i] > 126) {
                    device->operator_id[i] = '\0';
                    break;
                }
            }
            break;
        }
        
        case ODID_MSG_PACKED: {
            // packed messages contain multiple sub-messages
            if (len > 3 && data[1] == ODID_MESSAGE_SIZE && data[2] > 0) {
                int msg_count = data[2];
                for (int i = 0; i < msg_count && i < 9; i++) {
                    decode_opendroneid_message(device, &data[3 + i * ODID_MESSAGE_SIZE], ODID_MESSAGE_SIZE);
                }
            }
            break;
        }
    }
}

// little-endian readers for the DroneID payload
static int32_t rd_i32le(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static int16_t rd_i16le(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static bool latlon_sane(double lat, double lon) {
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    // Reject the null island: an unset/zeroed field decodes to ~0,0.
    if (lat > -0.0005 && lat < 0.0005 && lon > -0.0005 && lon < 0.0005) return false;
    return true;
}

// Decode the DJI DroneID flight_reg_info payload (subcommand 0x10). `data` points
// at the start of that payload (byte 0 = version), `len` is its length. Layout is
// from Kismet's dot11_ie_221_dji_droneid parser; lat/lon are radians*1e7, so the
// divisor is 174533.0 (= 1e7 / (180/pi)) -- NOT the 1e7 used for ODID degrees.
// Every field is written through a sanity gate so an unknown/encrypted/truncated
// payload yields "not broadcast" rather than fabricated coordinates.
static void decode_dji_message(AerialDevice *device, const uint8_t *data, size_t len) {
    device->type = AERIAL_TYPE_DJI_WIFI;

    // Need bytes through raw_home_lat (offset 49, +4). Below that we can't decode.
    if (len < 53) return;

    // Serial number (16 ASCII @5) -> device_id, printable only.
    char serial[17];
    int sn = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t c = data[5 + i];
        if (c < 32 || c > 126) break;
        serial[sn++] = (char)c;
    }
    serial[sn] = '\0';
    if (sn >= 3) snprintf(device->device_id, sizeof(device->device_id), "%s", serial);

    // Aircraft position: radians*1e7 -> degrees.
    double lon = rd_i32le(&data[21]) / 174533.0;
    double lat = rd_i32le(&data[25]) / 174533.0;
    if (latlon_sane(lat, lon)) {
        device->latitude = lat;
        device->longitude = lon;
        device->altitude = (float)rd_i16le(&data[29]);   // metres
        device->height_agl = (float)rd_i16le(&data[31]); // metres AGL
        device->has_location = true;

        // Ground speed from N/E velocity; scaling is uncertain in the spec, so
        // cap it and drop implausible values rather than show a bogus number.
        double vn = rd_i16le(&data[33]);
        double ve = rd_i16le(&data[35]);
        double spd = sqrt(vn * vn + ve * ve);
        device->speed_horizontal = (spd >= 0.0 && spd <= 120.0) ? (float)spd : -1.0f;
    }

    // Yaw/heading: raw/100 = degrees. This is the aircraft's compass/IMU
    // reading, independent of GPS -- it's present (and changes when the
    // airframe is physically turned) even indoors with no GPS lock, so it
    // must NOT be gated behind latlon_sane() above or it freezes at whatever
    // the last valid-GPS reading happened to be.
    double yaw = rd_i16le(&data[43]) / 100.0;
    if (yaw >= -360.0 && yaw <= 360.0) {
        device->direction = (float)(yaw < 0 ? yaw + 360.0 : yaw);
    }

    // Operator / takeoff (home) point -- the counter-surveillance payoff.
    double hlon = rd_i32le(&data[45]) / 174533.0;
    double hlat = rd_i32le(&data[49]) / 174533.0;
    if (latlon_sane(hlat, hlon)) {
        device->operator_latitude = hlat;
        device->operator_longitude = hlon;
        device->has_operator_location = true;
    }
}

static void check_drone_network(AerialDevice *device, const char *ssid) {
    if (!ssid || strlen(ssid) == 0) return;
    
    for (int i = 0; drone_ssid_patterns[i] != NULL; i++) {
        if (strstr(ssid, drone_ssid_patterns[i]) != NULL) {
            device->type = AERIAL_TYPE_DRONE_NETWORK;
            // ssid can be up to 32 chars, may truncate to fit buffer
            snprintf(device->device_id, sizeof(device->device_id), "%.31s", ssid);
            
            // identify vendor from ssid pattern
            if (strstr(ssid, "DJI") || strstr(ssid, "Mavic") || strstr(ssid, "Phantom") || 
                strstr(ssid, "Inspire") || strstr(ssid, "Matrice")) {
                snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "DJI");
            } else if (strstr(ssid, "ANAFI") || strstr(ssid, "Bebop") || strstr(ssid, "AR.Drone")) {
                snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "Parrot");
            } else if (strstr(ssid, "EVO") || strstr(ssid, "Autel")) {
                snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "Autel");
            } else if (strstr(ssid, "SKYDIO") || strstr(ssid, "Skydio")) {
                snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "Skydio");
            } else if (strstr(ssid, "Aerodome") || strstr(ssid, "Anzu")) {
                snprintf(device->vendor, AERIAL_VENDOR_MAX_LEN, "Flock/Anzu");
            }
            break;
        }
    }
}

static void notify_callback(AerialDevice *device) {
    if (user_callback) {
        user_callback(device, user_callback_data);
    }
    
    // emit tracker-style update for the tracked device
    if (device && device->is_tracked) {
        track_last_seen_ms = device->last_seen_ms;
        uint32_t age = (esp_timer_get_time() / 1000 - device->last_seen_ms) / 1000;
        if (device->has_location) {
            glog("AerialTrack %s RSSI=%ddBm age=%lus loc=%.5f,%.5f alt=%.1fm\n",
                 device->mac, device->rssi, age,
                 device->latitude, device->longitude,
                 (device->altitude > -1000.0f) ? device->altitude : 0.0f);
        } else {
            glog("AerialTrack %s RSSI=%ddBm age=%lus\n",
                 device->mac, device->rssi, age);
        }
    }
}

int aerial_detector_get_device_count(void) {
    return device_count;
}

AerialDevice* aerial_detector_get_device(int index) {
    if (!devices || index < 0 || index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

AerialDevice* aerial_detector_find_device_by_mac(const char *mac) {
    if (!devices) {
        return NULL;
    }
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].mac, mac) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

void aerial_detector_clear_devices(void) {
    if (!devices) {
        return;
    }
    if (xSemaphoreTake(device_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(devices, 0, device_capacity * sizeof(AerialDevice));
        device_count = 0;
        xSemaphoreGive(device_mutex);
    }
}

void aerial_detector_remove_old_devices(uint32_t max_age_ms) {
    if (!devices) {
        return;
    }
    if (xSemaphoreTake(device_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    uint32_t now = esp_timer_get_time() / 1000;
    int write_idx = 0;
    
    for (int read_idx = 0; read_idx < device_count; read_idx++) {
        if (now - devices[read_idx].last_seen_ms <= max_age_ms) {
            if (write_idx != read_idx) {
                memcpy(&devices[write_idx], &devices[read_idx], sizeof(AerialDevice));
            }
            write_idx++;
        }
    }
    
    device_count = write_idx;
    xSemaphoreGive(device_mutex);
}

void aerial_detector_compact_known_devices(void) {
    if (!devices) {
        return;
    }
    if (xSemaphoreTake(device_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    int write_idx = 0;
    for (int read_idx = 0; read_idx < device_count; read_idx++) {
        if (devices[read_idx].type != AERIAL_TYPE_UNKNOWN) {
            if (write_idx != read_idx) {
                memcpy(&devices[write_idx], &devices[read_idx], sizeof(AerialDevice));
            }
            write_idx++;
        }
    }
    device_count = write_idx;
    xSemaphoreGive(device_mutex);
}

void aerial_detector_set_callback(AerialDetectorCallback callback, void *user_data) {
    user_callback = callback;
    user_callback_data = user_data;
}

const char* aerial_detector_get_type_string(AerialDeviceType type) {
    switch (type) {
        case AERIAL_TYPE_REMOTEID_WIFI: return "RemoteID WiFi";
        case AERIAL_TYPE_REMOTEID_BLE: return "RemoteID BLE";
        case AERIAL_TYPE_DJI_WIFI: return "DJI WiFi";
        case AERIAL_TYPE_DJI_BLE: return "DJI BLE";
        case AERIAL_TYPE_DRONE_NETWORK: return "Drone Network";
        case AERIAL_TYPE_RC_CONTROLLER: return "RC Controller";
        case AERIAL_TYPE_FPV_VIDEO: return "FPV Video";
        case AERIAL_TYPE_TELEMETRY_MAV: return "MAVLink";
        case AERIAL_TYPE_TELEMETRY_SPORT: return "FrSky SPORT";
        case AERIAL_TYPE_TELEMETRY_CRSF: return "CRSF";
        default: return "Unknown";
    }
}

const char* aerial_detector_get_status_string(AerialStatus status) {
    switch (status) {
        case AERIAL_STATUS_GROUND: return "Ground";
        case AERIAL_STATUS_AIRBORNE: return "Airborne";
        case AERIAL_STATUS_EMERGENCY: return "EMERGENCY";
        case AERIAL_STATUS_SYSTEM_FAILURE: return "System Failure";
        default: return "Unknown";
    }
}

esp_err_t aerial_detector_track_device(const char *mac) {
    if (!mac || strlen(mac) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(tracked_mac, mac, AERIAL_MAC_STR_LEN - 1);
    tracked_mac[AERIAL_MAC_STR_LEN - 1] = '\0';
    
    // mark tracked flag on matching device if present
    AerialDevice *dev = aerial_detector_find_device_by_mac(tracked_mac);
    if (dev) {
        dev->is_tracked = true;
    }
    
    ESP_LOGI(TAG, "tracking device: %s", tracked_mac);
    return ESP_OK;
}

esp_err_t aerial_detector_untrack_device(void) {
    if (devices) {
        for (int i = 0; i < device_count; i++) {
            devices[i].is_tracked = false;
        }
    }
    memset(tracked_mac, 0, sizeof(tracked_mac));
    stop_track_timer();
    ESP_LOGI(TAG, "tracking disabled");
    return ESP_OK;
}

AerialDevice* aerial_detector_get_tracked_device(void) {
    if (tracked_mac[0] == '\0') {
        return NULL;
    }
    return aerial_detector_find_device_by_mac(tracked_mac);
}

void aerial_detector_enable_opendroneid(bool enable) {
    enable_opendroneid = enable;
}

void aerial_detector_enable_dji_detection(bool enable) {
    enable_dji = enable;
}

void aerial_detector_enable_network_detection(bool enable) {
    enable_networks = enable;
}

void aerial_detector_set_diagnostics(bool enable) {
    diag_enabled = enable;
    // Fresh tallies + a fresh unique-OUI set each time it's switched on, so a
    // diagnostic run reflects only what was heard during that run.
    diag_seen_oui_count = 0;
    diag_frames = diag_mgmt = diag_dji_oui_hits = diag_droneid_hits = 0;
    diag_last_log_ms = 0;
    memset(diag_seen_subcmd, 0, sizeof(diag_seen_subcmd));
#ifndef CONFIG_IDF_TARGET_ESP32S2
    diag_seen_dji_ble = false;
#endif
}

bool aerial_detector_diagnostics_enabled(void) {
    return diag_enabled;
}

void aerial_detector_enable_telemetry_detection(bool enable) {
    enable_telemetry = enable;
}

void aerial_detector_set_channel_hop_interval(uint32_t interval_ms) {
    if (interval_ms < 100) interval_ms = 100;  // minimum 100ms
    if (interval_ms > 5000) interval_ms = 5000;  // maximum 5s
    
    channel_hop_interval_ms = interval_ms;
    
    // restart timer if currently scanning
    if (wifi_scan_phase && channel_hop_timer) {
        esp_timer_stop(channel_hop_timer);
        esp_timer_start_periodic(channel_hop_timer, channel_hop_interval_ms * 1000);
        ESP_LOGI(TAG, "channel hop interval updated to %lums", channel_hop_interval_ms);
    }
}

uint32_t aerial_detector_get_channel_hop_interval(void) {
    return channel_hop_interval_ms;
}

int aerial_detector_get_channel_count(void) {
    return allowed_channel_count;
}

void aerial_detector_get_channels(uint8_t *channels, int max_count) {
    if (!channels || max_count <= 0) return;
    
    int count = allowed_channel_count < max_count ? allowed_channel_count : max_count;
    memcpy(channels, allowed_channels, count);
}

