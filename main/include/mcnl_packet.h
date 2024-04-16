#include "esp_wifi_types.h"

#define MCNL_PACKET_HEADER_LEN              4       // 4 bytes (type, flow_id, seq. except for payload) 
#define MCNL_PACKET_DEFAULT_PAYLOAD_LEN     1024    // 1024 bytes 
#define MCNL_PACKET_PAYLOAD_LEN_SHORT       30      // 30 bytes
#define MCNL_PACKET_PAYLOAD_LEN_LONG        1024    // 1024 bytes

#define MCNL_PACKET_TYPE_PERF_LATENCY_REQUEST       0
#define MCNL_PACKET_TYPE_PERF_LATENCY_RESPONSE      1
#define MCNL_PACKET_TYPE_PERF_THROUGHPUT_REQUEST    2
#define MCNL_PACKET_TYPE_PERF_THROUGHPUT_RESPONSE   3
#define MCNL_PACKET_TYPE_FORWARD                    4 

#define PERF_THROUGHPUT_NUM_PACKETS 640


typedef struct {
    uint8_t  type; 
    uint8_t  flow_id; 
    uint16_t seq; 
    uint8_t  payload[MCNL_PACKET_DEFAULT_PAYLOAD_LEN];
} mcnl_packet_t;



