#ifndef NETWORK_CONSTANTS_H
#define NETWORK_CONSTANTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// PORT DEFINITIONS
// ============================================================================

// Common TCP ports for scanning
// Sorted by port number for binary search optimization
extern const uint16_t COMMON_PORTS[];
extern const size_t NUM_PORTS;

// Common UDP ports for scanning
extern const uint16_t UDP_COMMON_PORTS[];
extern const size_t NUM_UDP_PORTS;

// Helper function to check if a TCP port is in the common ports list
bool is_common_tcp_port(uint16_t port);

// Helper function to check if a UDP port is in the common ports list
bool is_common_udp_port(uint16_t port);

// Get the service name for a TCP port (returns NULL if unknown)
const char* get_tcp_port_service(uint16_t port);

// Get the service name for a UDP port (returns NULL if unknown)
const char* get_udp_port_service(uint16_t port);

// Get a user-friendly service description for a port (for port scan analysis)
// Returns NULL if no description is available
const char* get_port_service_description(uint16_t port);

// ============================================================================
// PORT CATEGORY DETECTION (for device type analysis)
// ============================================================================

// Check if a port is commonly used for web services
bool is_web_port(uint16_t port);

// Check if a port is commonly used for database services
bool is_database_port(uint16_t port);

// Check if a port is commonly used for file sharing services
bool is_file_sharing_port(uint16_t port);

// ============================================================================
// WIFI CHANNEL DEFINITIONS
// ============================================================================

// WiFi channels for live AP scanning (2.4 GHz only for most ESP32 variants)
extern const uint8_t LIVE_AP_CHANNELS_2GHZ[];
extern const size_t LIVE_AP_CHANNELS_2GHZ_COUNT;

// WiFi channels for 2.4 GHz + 5 GHz (ESP32C5/C6 only)
extern const uint8_t LIVE_AP_CHANNELS_DUAL[];
extern const size_t LIVE_AP_CHANNELS_DUAL_COUNT;

// Full 2.4 GHz band (channels 1-14) in scan-priority order: the non-overlapping
// channels 1/6/11 first, then the rest. Shared by the drone scan and the WiFi
// AP/station scans so they all cover the same channels in the same order.
extern const uint8_t WIFI_CHANNELS_2GHZ_ORDER[];
extern const size_t WIFI_CHANNELS_2GHZ_ORDER_COUNT;

// ============================================================================
// OUI (ORGANIZATIONALLY UNIQUE IDENTIFIER) DEFINITIONS
// ============================================================================

// WiFi Pineapple OUIs for detection
extern const uint8_t PINEAPPLE_OUIS[][3];
extern const size_t PINEAPPLE_OUI_COUNT;

// DJI drone OUIs for detection
extern const uint8_t DJI_OUIS[][3];
extern const size_t DJI_OUI_COUNT;

// Check if a MAC address matches a known Pineapple OUI
bool is_pineapple_oui(const uint8_t *mac);

// Check if a MAC address matches a known DJI OUI
bool is_dji_oui(const uint8_t *mac);

// ----------------------------------------------------------------------------
// Surveillance + drone vendor OUI table
// ----------------------------------------------------------------------------
// Curated from the authoritative IEEE MA-L registry (standards-oui.ieee.org),
// matched on exact organization names. The embedded vendor DB (core/ouis.bin) is
// a subset that contains none of these vendors, so runtime name lookup cannot
// identify them -- this table is the only source. Never add an OUI that has not
// been verified against the registry: a wrong entry turns ordinary hardware into
// a false "camera"/"drone" alert.
typedef enum {
    SURV_TIER_AMBIENT = 0,  // ordinary IP cameras -- common, informational (amber)
    SURV_TIER_TARGETED,     // ALPR / bodycam / cloud-surveillance platforms (red)
    SURV_TIER_DRONE,        // UAV manufacturers
} surveil_tier_t;

typedef struct {
    uint32_t oui;      // packed 24-bit OUI, 0xAABBCC
    uint8_t  vendor;   // index into SURVEIL_VENDOR_NAMES
    uint8_t  tier;     // surveil_tier_t
} surveil_oui_t;

extern const surveil_oui_t SURVEIL_OUIS[];      // sorted by oui (binary search)
extern const size_t        SURVEIL_OUI_COUNT;
extern const char *const   SURVEIL_VENDOR_NAMES[];
extern const size_t        SURVEIL_VENDOR_COUNT;

// Look up a MAC's first 3 bytes. On a hit fills vendor name / tier and returns
// true. Safe to call from an ISR-context sniffer: no allocation, no blocking.
bool surveil_oui_lookup(const uint8_t *mac, const char **vendor_out, uint8_t *tier_out);

// Convenience wrapper: true only for SURV_TIER_DRONE entries (any UAV vendor,
// not just DJI). Complements is_dji_oui().
bool drone_oui_lookup(const uint8_t *mac, const char **vendor_out);

// ============================================================================
// SPECIAL MAC ADDRESSES
// ============================================================================

// NAN (Neighbor Aware Networking) destination MAC for OpenDroneID WiFi
extern const uint8_t NAN_DEST_MAC[6];

// ============================================================================
// FRAME CONSTANTS
// ============================================================================

// Minimum RSSI threshold for packet processing
#define MIN_RSSI_THRESHOLD -90

// Maximum WiFi channel based on target
#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif

// Minimum 802.11 header size
#define MIN_PACKET_LENGTH 24

// Maximum Information Element length
#define MAX_IE_LEN 255

#endif // NETWORK_CONSTANTS_H
