// network_constants.c
// Common network port definitions and related utilities

#include "core/network_constants.h"
#include <string.h>
#include <stdbool.h>

// Common TCP ports for scanning with service names
// Sorted by port number for binary search optimization
const uint16_t COMMON_PORTS[] = {
    7,     // echo
    20,    // ftp-data
    21,    // ftp
    22,    // ssh
    23,    // telnet
    25,    // smtp
    53,    // dns
    69,    // tftp (udp mostly)
    80,    // http
    88,    // kerberos
    110,   // pop3
    111,   // rpcbind
    119,   // nntp
    123,   // ntp (udp mostly)
    135,   // msrpc
    137,   // netbios-ns
    138,   // netbios-dgm
    139,   // netbios-ssn
    143,   // imap
    161,   // snmp (udp mostly)
    162,   // snmp-trap (udp mostly)
    389,   // ldap
    443,   // https
    445,   // smb
    465,   // smtps
    500,   // ike (udp mostly)
    502,   // modbus
    512,   // exec
    513,   // login
    514,   // syslog/shell (udp mostly)
    515,   // lpd
    587,   // smtp-submission
    593,   // rpc over http
    631,   // ipp
    636,   // ldaps
    646,   // ldp
    873,   // rsync
    902,   // vmware-server
    989,   // ftps-data
    990,   // ftps
    993,   // imaps
    995,   // pop3s
    1080,  // socks
    1099,  // rmi
    1433,  // mssql
    1434,  // mssql-browser (udp mostly)
    1494,  // citrix-ica
    1521,  // oracle-db
    1701,  // l2tp (udp mostly)
    1720,  // h323
    1723,  // pptp
    1883,  // mqtt
    1900,  // ssdp (udp mostly)
    2049,  // nfs
    2082,  // cpanel
    2083,  // cpanel-ssl
    2086,  // whm
    2087,  // whm-ssl
    2095,  // webmail
    2096,  // webmail-ssl
    2222,  // ssh-alt
    2375,  // docker
    2376,  // docker-tls
    2377,  // docker-swarm
    2379,  // etcd
    2380,  // etcd-peer
    2381,  // etcd-alt
    2480,  // oracle-web
    25565, // minecraft
    27017, // mongodb
    27018, // mongodb-shard
    27019, // mongodb-config
    28017, // mongodb-http
    3000,  // dev-http
    3001,  // dev-http-alt
    3128,  // squid-proxy
    32400, // plex
    3260,  // iscsi
    3306,  // mysql
    3389,  // rdp
    3478,  // stun (udp mostly)
    3689,  // daap
    4369,  // epmd
    4444,  // tcp-alt
    4500,  // ipsec-nat-t (udp mostly)
    4789,  // vxlan (udp mostly)
    4848,  // glassfish-admin
    5000,  // http-alt/upnp
    5001,  // http-alt
    5004,  // rtp (udp mostly)
    5005,  // rtp (udp mostly)
    5060,  // sip
    5061,  // sips
    5222,  // xmpp
    5223,  // xmpp-ssl/apns
    5357,  // wsdapi
    5432,  // postgresql
    5555,  // android-adb
    5601,  // kibana
    5671,  // amqp-tls
    5672,  // amqp
    5683,  // coap (udp mostly)
    5900,  // vnc
    5901,  // vnc-1
    5902,  // vnc-2
    5984,  // couchdb
    5985,  // winrm
    5986,  // winrm-https
    6000,  // x11
    6379,  // redis
    6667,  // irc
    7001,  // websphere
    7199,  // cassandra-intra
    8000,  // http-alt
    8008,  // http-alt
    8080,  // http-proxy
    8081,  // http-alt
    8082,  // http-alt
    8083,  // http-alt
    8086,  // influxdb
    8088,  // http-alt
    8123,  // home-assistant
    8161,  // activemq
    8181,  // http-alt
    8200,  // upnp-minidlna
    8222,  // vmware
    8333,  // bitcoin
    8443,  // https-alt
    8500,  // consul
    8530,  // wsus
    8554,  // rtsp-alt
    8883,  // mqtt-tls
    8888,  // http-alt
    9000,  // sonarqube/php-fpm
    9042,  // cassandra-cql
    9080,  // http-alt
    9090,  // http-alt
    9091,  // transmission
    9092,  // kafka
    9100,  // printer
    9200,  // elasticsearch
    9300,  // elasticsearch-node
    9418,  // git
    9443,  // https-alt
    10000, // webmin
    11211, // memcached
    15672, // rabbitmq-mgmt
    51820, // wireguard
    55443  // http-alt
};

const size_t NUM_PORTS = sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]);

// Common UDP ports for scanning
const uint16_t UDP_COMMON_PORTS[] = {
    53,    // dns
    67,    // dhcp-server
    68,    // dhcp-client
    69,    // tftp
    123,   // ntp
    137,   // netbios-ns
    161,   // snmp
    162,   // snmp-trap
    1900,  // ssdp
    500,   // ike
    514,   // syslog
    520,   // rip
    5353,  // mdns
    5683   // coap
};

const size_t NUM_UDP_PORTS = sizeof(UDP_COMMON_PORTS) / sizeof(UDP_COMMON_PORTS[0]);

// Service name lookup table for TCP ports
typedef struct {
    uint16_t port;
    const char *service;
} port_service_t;

static const port_service_t tcp_services[] = {
    {7, "echo"}, {20, "ftp-data"}, {21, "ftp"}, {22, "ssh"}, {23, "telnet"},
    {25, "smtp"}, {53, "dns"}, {69, "tftp"}, {80, "http"}, {88, "kerberos"},
    {110, "pop3"}, {111, "rpcbind"}, {119, "nntp"}, {123, "ntp"}, {135, "msrpc"},
    {137, "netbios-ns"}, {138, "netbios-dgm"}, {139, "netbios-ssn"}, {143, "imap"},
    {161, "snmp"}, {162, "snmp-trap"}, {389, "ldap"}, {443, "https"}, {445, "smb"},
    {465, "smtps"}, {500, "ike"}, {502, "modbus"}, {512, "exec"}, {513, "login"},
    {514, "syslog"}, {515, "lpd"}, {587, "smtp-submission"}, {593, "rpc-http"},
    {631, "ipp"}, {636, "ldaps"}, {646, "ldp"}, {873, "rsync"}, {902, "vmware"},
    {989, "ftps-data"}, {990, "ftps"}, {993, "imaps"}, {995, "pop3s"},
    {1080, "socks"}, {1099, "rmi"}, {1433, "mssql"}, {1434, "mssql-browser"},
    {1494, "citrix"}, {1521, "oracle"}, {1701, "l2tp"}, {1720, "h323"},
    {1723, "pptp"}, {1883, "mqtt"}, {1900, "ssdp"}, {2049, "nfs"},
    {2082, "cpanel"}, {2083, "cpanel-ssl"}, {2086, "whm"}, {2087, "whm-ssl"},
    {2095, "webmail"}, {2096, "webmail-ssl"}, {2222, "ssh-alt"},
    {2375, "docker"}, {2376, "docker-tls"}, {2377, "docker-swarm"},
    {2379, "etcd"}, {2380, "etcd-peer"}, {2381, "etcd-alt"}, {2480, "oracle-web"},
    {25565, "minecraft"}, {27017, "mongodb"}, {27018, "mongodb-shard"},
    {27019, "mongodb-config"}, {28017, "mongodb-http"}, {3000, "dev-http"},
    {3001, "dev-http-alt"}, {3128, "squid"}, {32400, "plex"}, {3260, "iscsi"},
    {3306, "mysql"}, {3389, "rdp"}, {3478, "stun"}, {3689, "daap"},
    {4369, "epmd"}, {4444, "tcp-alt"}, {4500, "ipsec-nat-t"},
    {4789, "vxlan"}, {4848, "glassfish"}, {5000, "http-alt"}, {5001, "http-alt"},
    {5004, "rtp"}, {5005, "rtp"}, {5060, "sip"}, {5061, "sips"},
    {5222, "xmpp"}, {5223, "xmpp-ssl"}, {5357, "wsdapi"}, {5432, "postgresql"},
    {5555, "adb"}, {5601, "kibana"}, {5671, "amqp-tls"}, {5672, "amqp"},
    {5683, "coap"}, {5900, "vnc"}, {5901, "vnc-1"}, {5902, "vnc-2"},
    {5984, "couchdb"}, {5985, "winrm"}, {5986, "winrm-https"}, {6000, "x11"},
    {6379, "redis"}, {6667, "irc"}, {7001, "websphere"}, {7199, "cassandra"},
    {8000, "http-alt"}, {8008, "http-alt"}, {8080, "http-proxy"},
    {8081, "http-alt"}, {8082, "http-alt"}, {8083, "http-alt"},
    {8086, "influxdb"}, {8088, "http-alt"}, {8123, "home-assistant"},
    {8161, "activemq"}, {8181, "http-alt"}, {8200, "minidlna"},
    {8222, "vmware"}, {8333, "bitcoin"}, {8443, "https-alt"},
    {8500, "consul"}, {8530, "wsus"}, {8554, "rtsp-alt"},
    {8883, "mqtt-tls"}, {8888, "http-alt"}, {9000, "php-fpm"},
    {9042, "cassandra"}, {9080, "http-alt"}, {9090, "http-alt"},
    {9091, "transmission"}, {9092, "kafka"}, {9100, "printer"},
    {9200, "elasticsearch"}, {9300, "elasticsearch"}, {9418, "git"},
    {9443, "https-alt"}, {10000, "webmin"}, {11211, "memcached"},
    {15672, "rabbitmq"}, {51820, "wireguard"}, {55443, "http-alt"}
};

static const port_service_t udp_services[] = {
    {53, "dns"}, {67, "dhcp-server"}, {68, "dhcp-client"}, {69, "tftp"},
    {123, "ntp"}, {137, "netbios-ns"}, {161, "snmp"}, {162, "snmp-trap"},
    {1900, "ssdp"}, {500, "ike"}, {514, "syslog"}, {520, "rip"},
    {5353, "mdns"}, {5683, "coap"}
};

static const size_t NUM_TCP_SERVICES = sizeof(tcp_services) / sizeof(tcp_services[0]);
static const size_t NUM_UDP_SERVICES = sizeof(udp_services) / sizeof(udp_services[0]);

// Binary search helper for sorted port arrays
static bool binary_search_port(const uint16_t *arr, size_t n, uint16_t port) {
    size_t left = 0, right = n;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (arr[mid] == port) {
            return true;
        } else if (arr[mid] < port) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return false;
}

bool is_common_tcp_port(uint16_t port) {
    return binary_search_port(COMMON_PORTS, NUM_PORTS, port);
}

bool is_common_udp_port(uint16_t port) {
    return binary_search_port(UDP_COMMON_PORTS, NUM_UDP_PORTS, port);
}

const char* get_tcp_port_service(uint16_t port) {
    for (size_t i = 0; i < NUM_TCP_SERVICES; i++) {
        if (tcp_services[i].port == port) {
            return tcp_services[i].service;
        }
    }
    return NULL;
}

const char* get_udp_port_service(uint16_t port) {
    for (size_t i = 0; i < NUM_UDP_SERVICES; i++) {
        if (udp_services[i].port == port) {
            return udp_services[i].service;
        }
    }
    return NULL;
}

// ============================================================================
// WIFI CHANNEL DEFINITIONS
// ============================================================================

// WiFi channels for 2.4 GHz only (most ESP32 variants)
const uint8_t LIVE_AP_CHANNELS_2GHZ[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
};
const size_t LIVE_AP_CHANNELS_2GHZ_COUNT = sizeof(LIVE_AP_CHANNELS_2GHZ) / sizeof(LIVE_AP_CHANNELS_2GHZ[0]);

// WiFi channels for 2.4 GHz + 5 GHz (ESP32C5/C6 only)
const uint8_t LIVE_AP_CHANNELS_DUAL[] = {
    // 2.4 GHz channels
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    // 5 GHz channels
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165
};
const size_t LIVE_AP_CHANNELS_DUAL_COUNT = sizeof(LIVE_AP_CHANNELS_DUAL) / sizeof(LIVE_AP_CHANNELS_DUAL[0]);

// Full 2.4 GHz band in scan-priority order: non-overlapping 1/6/11 first, then
// the rest, covering channels 1-14. Shared by the drone scan (aerial_detector)
// and the WiFi AP/station scans so all cover the same channels in the same order.
const uint8_t WIFI_CHANNELS_2GHZ_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 14};
const size_t WIFI_CHANNELS_2GHZ_ORDER_COUNT =
    sizeof(WIFI_CHANNELS_2GHZ_ORDER) / sizeof(WIFI_CHANNELS_2GHZ_ORDER[0]);

// ============================================================================
// OUI (ORGANIZATIONALLY UNIQUE IDENTIFIER) DEFINITIONS
// ============================================================================

// WiFi Pineapple OUIs for detection
const uint8_t PINEAPPLE_OUIS[][3] = {
    {0x00, 0x13, 0x37},  // WiFi Pineapple
};
const size_t PINEAPPLE_OUI_COUNT = sizeof(PINEAPPLE_OUIS) / sizeof(PINEAPPLE_OUIS[0]);

// DJI drone OUIs for detection.
// All 10 OUIs IEEE-registered to "SZ DJI TECHNOLOGY CO.,LTD" (verified against the
// IEEE registry / maclookup.app, Aug 2026). DJI assigns OUIs per-company, not per
// product line, so these cover the Mini, Air, Mavic (and every other) DJI series.
// (The former 0x5C,0xE8,0x83 entry was actually Huawei — removed to avoid flagging
// common Huawei gear as a drone.)
const uint8_t DJI_OUIS[][3] = {
    {0x04, 0xA8, 0x5A},  // SZ DJI Technology
    {0x0C, 0x9A, 0xE6},  // SZ DJI Technology
    {0x34, 0xD2, 0x62},  // SZ DJI Technology
    {0x48, 0x1C, 0xB9},  // SZ DJI Technology
    {0x4C, 0x43, 0xF6},  // SZ DJI Technology
    {0x58, 0xB8, 0x58},  // SZ DJI Technology
    {0x60, 0x60, 0x1F},  // SZ DJI Technology
    {0x88, 0x29, 0x85},  // SZ DJI Technology
    {0x8C, 0x58, 0x23},  // SZ DJI Technology
    {0xE4, 0x7A, 0x2C},  // SZ DJI Technology
    // Additional DJI corporate entities in the IEEE registry (verified 2026-08):
    {0x20, 0x1F, 0x55},  // DJI Osmo Technology
    {0x34, 0x91, 0xF0},  // DJI Baiwang Technology
    {0x9C, 0x5A, 0x8A},  // DJI Baiwang Technology
    {0xEC, 0x72, 0xF7},  // DJI Baiwang Technology
    {0xF8, 0x40, 0x68},  // SZ DJI Ronin Technology
};
const size_t DJI_OUI_COUNT = sizeof(DJI_OUIS) / sizeof(DJI_OUIS[0]);

bool is_pineapple_oui(const uint8_t *mac) {
    if (mac == NULL) return false;
    for (size_t i = 0; i < PINEAPPLE_OUI_COUNT; i++) {
        if (memcmp(mac, PINEAPPLE_OUIS[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

bool is_dji_oui(const uint8_t *mac) {
    if (mac == NULL) return false;
    for (size_t i = 0; i < DJI_OUI_COUNT; i++) {
        if (memcmp(mac, DJI_OUIS[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// SPECIAL MAC ADDRESSES
// ============================================================================

// NAN (Neighbor Aware Networking) destination MAC for OpenDroneID WiFi
const uint8_t NAN_DEST_MAC[6] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};

// ============================================================================
// SERVICE DESCRIPTION LOOKUP (for port scan analysis)
// ============================================================================

// Service description table with user-friendly names
typedef struct {
    uint16_t port;
    const char *description;
} port_description_t;

static const port_description_t service_descriptions[] = {
    {20, "FTP Server (data)"},
    {21, "FTP Server"},
    {22, "SSH Server"},
    {23, "Telnet Server"},
    {80, "Web Server"},
    {139, "Windows File Share/Domain Controller"},
    {443, "Web Server (HTTPS)"},
    {445, "Windows File Share/Domain Controller"},
    {1521, "Oracle Database"},
    {1883, "IoT Device (MQTT)"},
    {2222, "SSH Server"},
    {2082, "Web Hosting Control Panel"},
    {2083, "Web Hosting Control Panel"},
    {2086, "Web Hosting Control Panel"},
    {2087, "Web Hosting Control Panel"},
    {3306, "MySQL Database"},
    {3389, "Windows Remote Desktop"},
    {5432, "PostgreSQL Database"},
    {5900, "VNC Remote Access"},
    {5901, "VNC Remote Access"},
    {5902, "VNC Remote Access"},
    {6379, "Redis Server"},
    {8080, "Web Server"},
    {8443, "Web Server"},
    {8883, "IoT Device (MQTT)"},
    {9100, "Network Printer"},
    {27017, "MongoDB Database"},
    {32400, "Plex Media Server"}
};

static const size_t NUM_SERVICE_DESCRIPTIONS = sizeof(service_descriptions) / sizeof(service_descriptions[0]);

const char* get_port_service_description(uint16_t port) {
    for (size_t i = 0; i < NUM_SERVICE_DESCRIPTIONS; i++) {
        if (service_descriptions[i].port == port) {
            return service_descriptions[i].description;
        }
    }
    return NULL;
}

// ============================================================================
// PORT CATEGORY DETECTION (for device type analysis)
// ============================================================================

bool is_web_port(uint16_t port) {
    return port == 80 || port == 443 || port == 8080 || port == 8443;
}

bool is_database_port(uint16_t port) {
    return port == 3306 || port == 5432 || port == 1521 || port == 27017;
}

bool is_file_sharing_port(uint16_t port) {
    return port == 445 || port == 139;
}

// ----------------------------------------------------------------------------
// Surveillance + drone vendor OUIs (see header for the rules)
// ----------------------------------------------------------------------------
// Generated from the IEEE MA-L registry (standards-oui.ieee.org/oui/oui.csv) by
// exact organization-name match, 213 entries across 33 vendors. Deliberately
// EXCLUDED to avoid false positives: Ubiquiti (its ~48 OUIs are overwhelmingly
// WiFi APs, not cameras), Sony's ~86 consumer OUIs (only the 5 security-camera
// prefixes are listed), Bosch's automotive/thermotech arms, and Motorola
// Mobility phones. Autel Robotics, Yuneec, Anduril, BRINC, Hubsan, Walkera and
// EHang have NO IEEE registration, so they are absent rather than guessed at.
const char *const SURVEIL_VENDOR_NAMES[] = {"Flock Safety", "Axon", "Verkada", "Avigilon", "Genetec", "Hikvision", "Dahua", "Axis Comm", "Hanwha", "Bosch Sec", "Uniview", "Vivotek", "Sony", "i-PRO", "Arecont", "ACTi", "Amcrest", "Reolink", "Lorex", "Arlo", "Wyze", "Ring", "Mobotix", "GeoVision", "Digital Watchdog", "Honeywell Vid", "FLIR", "DJI", "Parrot", "Skydio", "Teal Drones", "Freefly", "PowerVision"};
const size_t SURVEIL_VENDOR_COUNT = sizeof(SURVEIL_VENDOR_NAMES) / sizeof(SURVEIL_VENDOR_NAMES[0]);

const surveil_oui_t SURVEIL_OUIS[] = {
    {0x000131, 9, 0},
    {0x00014A, 12, 0},
    {0x0002D1, 11, 0},
    {0x0003C5, 22, 0},
    {0x000463, 9, 0},
    {0x000918, 8, 0},
    {0x000A13, 25, 0},
    {0x000F7C, 15, 0},
    {0x001017, 9, 0},
    {0x00121C, 28, 2},
    {0x001356, 26, 0},
    {0x0013A9, 12, 0},
    {0x0013E2, 23, 0},
    {0x001885, 3, 1},
    {0x001A07, 14, 0},
    {0x001A80, 12, 0},
    {0x001B86, 9, 0},
    {0x001BD8, 26, 0},
    {0x001F54, 18, 0},
    {0x001F55, 25, 0},
    {0x0025DF, 1, 1},
    {0x00267E, 28, 2},
    {0x004040, 21, 0},
    {0x00407F, 26, 0},
    {0x00408C, 7, 0},
    {0x00651E, 16, 0},
    {0x00B463, 21, 0},
    {0x00BC99, 5, 0},
    {0x00BF15, 4, 1},
    {0x00E0F2, 19, 0},
    {0x040312, 5, 0},
    {0x04A85A, 27, 2},
    {0x04EECD, 5, 0},
    {0x083BC1, 5, 0},
    {0x085411, 5, 0},
    {0x08A189, 5, 0},
    {0x08CC81, 5, 0},
    {0x08EDED, 6, 0},
    {0x0C75D2, 5, 0},
    {0x0C9AE6, 27, 2},
    {0x0CBF15, 4, 1},
    {0x1012FB, 5, 0},
    {0x14A78B, 6, 0},
    {0x14BA88, 10, 0},
    {0x1868CB, 5, 0},
    {0x187F88, 21, 0},
    {0x188025, 5, 0},
    {0x201F55, 27, 2},
    {0x202C05, 6, 0},
    {0x240F9B, 5, 0},
    {0x2428FD, 5, 0},
    {0x242BD6, 21, 0},
    {0x2432AE, 5, 0},
    {0x244845, 5, 0},
    {0x24526A, 6, 0},
    {0x24B105, 5, 0},
    {0x2857BE, 5, 0},
    {0x2CA59C, 5, 0},
    {0x2CAA8E, 20, 0},
    {0x30DDAA, 6, 0},
    {0x30F028, 9, 0},
    {0x30F9ED, 12, 0},
    {0x340962, 5, 0},
    {0x343EA4, 21, 0},
    {0x3491F0, 27, 2},
    {0x34D262, 27, 2},
    {0x381D14, 29, 2},
    {0x38AF29, 6, 0},
    {0x3C1BF8, 5, 0},
    {0x3CE36B, 6, 0},
    {0x3CEF8C, 6, 0},
    {0x407AA4, 6, 0},
    {0x40ACBF, 5, 0},
    {0x40B570, 5, 0},
    {0x4419B6, 5, 0},
    {0x4447CC, 5, 0},
    {0x44A642, 5, 0},
    {0x44B423, 8, 0},
    {0x481CB9, 27, 2},
    {0x486264, 19, 0},
    {0x48785B, 5, 0},
    {0x48EA63, 10, 0},
    {0x4C11BF, 6, 0},
    {0x4C1F86, 5, 0},
    {0x4C43F6, 27, 2},
    {0x4C62DF, 5, 0},
    {0x4C99E8, 6, 0},
    {0x4CBD8F, 5, 0},
    {0x4CF5DC, 5, 0},
    {0x50E467, 21, 0},
    {0x50E538, 5, 0},
    {0x547D40, 32, 2},
    {0x548C81, 5, 0},
    {0x54C415, 5, 0},
    {0x54E019, 21, 0},
    {0x5803FB, 5, 0},
    {0x5850ED, 5, 0},
    {0x58B858, 27, 2},
    {0x5C345B, 5, 0},
    {0x5C475E, 21, 0},
    {0x5CF51A, 6, 0},
    {0x60601F, 27, 2},
    {0x649A63, 21, 0},
    {0x64DB8B, 5, 0},
    {0x64FD29, 6, 0},
    {0x686DBC, 5, 0},
    {0x6C1C71, 6, 0},
    {0x6CF17E, 10, 0},
    {0x701AD5, 3, 1},
    {0x743FC2, 5, 0},
    {0x74C929, 6, 0},
    {0x7C78B2, 20, 0},
    {0x80482C, 20, 0},
    {0x80489F, 5, 0},
    {0x807C62, 5, 0},
    {0x80BEAF, 5, 0},
    {0x80F5AE, 5, 0},
    {0x849459, 5, 0},
    {0x849A40, 5, 0},
    {0x88263F, 10, 0},
    {0x882985, 27, 2},
    {0x88DE39, 5, 0},
    {0x8C22D2, 5, 0},
    {0x8C5823, 27, 2},
    {0x8CE748, 5, 0},
    {0x8CE9B4, 6, 0},
    {0x9002A9, 6, 0},
    {0x9003B7, 28, 2},
    {0x903AE6, 28, 2},
    {0x90486C, 21, 0},
    {0x94E1AC, 5, 0},
    {0x988B0A, 5, 0},
    {0x989DE5, 5, 0},
    {0x98DF82, 5, 0},
    {0x98F112, 5, 0},
    {0x98F9CC, 6, 0},
    {0x9C1463, 6, 0},
    {0x9C5A8A, 27, 2},
    {0x9C7613, 21, 0},
    {0x9C8ECD, 16, 0},
    {0xA0143D, 28, 2},
    {0xA06032, 16, 0},
    {0xA0BD1D, 6, 0},
    {0xA0FF0C, 5, 0},
    {0xA41162, 19, 0},
    {0xA41437, 5, 0},
    {0xA42902, 5, 0},
    {0xA44BD9, 5, 0},
    {0xA4A459, 5, 0},
    {0xA4D5C2, 5, 0},
    {0xA8CA87, 6, 0},
    {0xA8DC5A, 24, 0},
    {0xAC9FC3, 21, 0},
    {0xACB92F, 5, 0},
    {0xACCB51, 5, 0},
    {0xACCC8E, 7, 0},
    {0xB009DA, 21, 0},
    {0xB030C8, 30, 2},
    {0xB0FF0D, 5, 0},
    {0xB41E52, 0, 1},
    {0xB44C3B, 6, 0},
    {0xB4A382, 5, 0},
    {0xB8A44F, 7, 0},
    {0xBC2978, 5, 0},
    {0xBC325F, 6, 0},
    {0xBC5E33, 5, 0},
    {0xBC9B5E, 5, 0},
    {0xBCAD28, 5, 0},
    {0xBCBAC2, 5, 0},
    {0xC0395A, 6, 0},
    {0xC0517E, 5, 0},
    {0xC056E3, 5, 0},
    {0xC06DED, 5, 0},
    {0xC42F90, 5, 0},
    {0xC47905, 10, 0},
    {0xC4AAC4, 6, 0},
    {0xC4DBAD, 21, 0},
    {0xC8A702, 5, 0},
    {0xCC13F3, 5, 0},
    {0xCC3BFB, 21, 0},
    {0xD03F27, 20, 0},
    {0xD42DC5, 13, 0},
    {0xD4430E, 6, 0},
    {0xD4E853, 5, 0},
    {0xDC07F8, 5, 0},
    {0xDCD26A, 5, 0},
    {0xE02EFE, 6, 0},
    {0xE0508B, 6, 0},
    {0xE0A700, 2, 1},
    {0xE0BAAD, 5, 0},
    {0xE0CA3C, 5, 0},
    {0xE0DF13, 5, 0},
    {0xE4246C, 6, 0},
    {0xE43022, 8, 0},
    {0xE47A2C, 27, 2},
    {0xE4D58B, 5, 0},
    {0xE82725, 7, 0},
    {0xE8A0ED, 5, 0},
    {0xEC715E, 31, 2},
    {0xEC71DB, 17, 0},
    {0xEC72F7, 27, 2},
    {0xECA971, 5, 0},
    {0xECC89C, 5, 0},
    {0xF0C88B, 20, 0},
    {0xF4B1C2, 6, 0},
    {0xF84068, 27, 2},
    {0xF84DFC, 5, 0},
    {0xF8CE07, 6, 0},
    {0xFC5F49, 6, 0},
    {0xFC9C98, 19, 0},
    {0xFC9FFD, 5, 0},
    {0xFCB69D, 6, 0},
    {0xFCF152, 12, 0},
};
const size_t SURVEIL_OUI_COUNT = sizeof(SURVEIL_OUIS) / sizeof(SURVEIL_OUIS[0]);

bool surveil_oui_lookup(const uint8_t *mac, const char **vendor_out, uint8_t *tier_out) {
    if (mac == NULL) return false;
    uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    size_t lo = 0, hi = SURVEIL_OUI_COUNT;          // table is sorted by oui
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        uint32_t v = SURVEIL_OUIS[mid].oui;
        if (v == key) {
            if (vendor_out) *vendor_out = SURVEIL_VENDOR_NAMES[SURVEIL_OUIS[mid].vendor];
            if (tier_out) *tier_out = SURVEIL_OUIS[mid].tier;
            return true;
        }
        if (v < key) lo = mid + 1;
        else         hi = mid;
    }
    return false;
}

bool drone_oui_lookup(const uint8_t *mac, const char **vendor_out) {
    const char *vendor = NULL;
    uint8_t tier = 0;
    if (!surveil_oui_lookup(mac, &vendor, &tier)) return false;
    if (tier != SURV_TIER_DRONE) return false;
    if (vendor_out) *vendor_out = vendor;
    return true;
}
