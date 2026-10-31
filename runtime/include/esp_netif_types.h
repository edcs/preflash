#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_event_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_netif_obj esp_netif_t;

// The address is in network byte order, as on the device.
typedef struct {
  uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
  esp_ip4_addr_t ip;
  esp_ip4_addr_t netmask;
  esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

#define esp_ip4_addr1_16(ipaddr) ((((ipaddr)->addr) >> 0) & 0xff)
#define esp_ip4_addr2_16(ipaddr) ((((ipaddr)->addr) >> 8) & 0xff)
#define esp_ip4_addr3_16(ipaddr) ((((ipaddr)->addr) >> 16) & 0xff)
#define esp_ip4_addr4_16(ipaddr) ((((ipaddr)->addr) >> 24) & 0xff)
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(ipaddr)                                                                  \
  (int)esp_ip4_addr1_16(ipaddr), (int)esp_ip4_addr2_16(ipaddr), (int)esp_ip4_addr3_16(ipaddr), \
      (int)esp_ip4_addr4_16(ipaddr)

ESP_EVENT_DECLARE_BASE(IP_EVENT);

typedef enum {
  IP_EVENT_STA_GOT_IP,
  IP_EVENT_STA_LOST_IP,
  IP_EVENT_AP_STAIPASSIGNED,
  IP_EVENT_GOT_IP6,
  IP_EVENT_ETH_GOT_IP,
  IP_EVENT_ETH_LOST_IP,
  IP_EVENT_PPP_GOT_IP,
  IP_EVENT_PPP_LOST_IP,
} ip_event_t;

typedef struct {
  esp_netif_t* esp_netif;
  esp_netif_ip_info_t ip_info;
  bool ip_changed;
} ip_event_got_ip_t;

#ifdef __cplusplus
}
#endif
