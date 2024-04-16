#include <string.h>
#include <inttypes.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "nvs_flash.h"
// ---------------------------------------------------------------------
//추가된 라이브러리
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h>
#include "esp_task_wdt.h"
#include "esp_sntp.h"
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
// ---------------------------------------------------------------------

//콘솔 입력을 위해 추가
#include <stdio.h>
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "mcnl_packet.h"
// ---------------------------------------------------------------------

//For Web socket
#include "esp_websocket_client.h"
#include "esp_system.h"
#include "protocol_examples_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <cJSON.h>
// ---------------------------------------------------------------------

// *************** Constants ***************
#define RX_SIZE (1500) //송신 data의 크기
#define TX_SIZE (1460) //수신 data의 크기
//#define Throughput_Packet_SIZE (1024) // 패킷 사이즈
#define CONFIG_CONSOLE_MAX_COMMAND_LINE_LENGTH 10//입력받을 값 최대 길이설정
#define MAX_SEND_HISTORY 10  // 최근 몇 번의 send 작업을 추적할 것인지 설정

// For forwarding
#define SERVER_IP   "192.168.68.101"
#define SERVER_PORT 30303
static const char *TAG = "websocket";
#define NO_DATA_TIMEOUT_SEC 10

// ---------------------------------------------------------------------

// *************** Var definitions ***************
static const char *MESH_TAG = "mesh_main";
static const uint8_t MESH_ID[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
static uint8_t tx_buf[TX_SIZE] = {0,};
static uint8_t rx_buf[RX_SIZE] = {0,};
static bool is_running = true;
static bool is_mesh_connected = false;
static bool is_tx_latency_started = false;
static bool is_tx_throughput_started = false;
static int mesh_layer = -1;
static mesh_addr_t mesh_parent_addr;
static esp_netif_t *netif_sta = NULL;

static TimerHandle_t shutdown_signal_timer;
static SemaphoreHandle_t shutdown_sema;

int flow_id = 0;
long tx_time_history[MAX_SEND_HISTORY];  // 최근 send 작업의 타임스탬프를 저장하는 배열

// ---------------------------------------------------------------------

// *************** Function declarations ***************
void print_routing_table(mesh_addr_t *route_table, int route_table_size);
void esp_mesh_receive_handler(void *arg);
void esp_mesh_forward_handler(mcnl_packet_t *data, mesh_addr_t *from);
void esp_mesh_send_perf_latency_request(void *arg);
void esp_mesh_handle_perf_latency_request(mcnl_packet_t *request, mesh_addr_t *from);
void esp_mesh_handle_perf_latency_response(mcnl_packet_t *request, mesh_addr_t *from);
void esp_mesh_send_perf_throughput_request(void *arg);
void esp_mesh_handle_perf_throughput_request(mcnl_packet_t *request, mesh_addr_t *from);
void esp_mesh_handle_perf_throughput_response(mcnl_packet_t *request, mesh_addr_t *from);
esp_err_t esp_mesh_latency_start(int dst_idx);
esp_err_t esp_mesh_throughput_start(int dst_idx);
void app_main(void);

// ---------------------------------------------------------------------

// *************** Function definitions ***************

void print_routing_table(mesh_addr_t *route_table, int route_table_size) 
{
    ESP_LOGI(MESH_TAG, "ROUTE_TABLE_SIZE=%d", route_table_size);
    for (int i = 0; i < route_table_size; i++) {
        ESP_LOGI(MESH_TAG, "ROUTE_TABLE[%d]:" MACSTR, i, MAC2STR(route_table[i].addr));
    }
}

// void storeMAC(const char *cmac)
// {
//     const esp_websocket_client_config_t websocket_cfg = {
//     };

//     websocket_cfg.uri = CONFIG_WEBSOCKET_URI;

//     esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
//     esp_websocket_client_start(client);

//     size_t cmac_len = strlen(cmac);
//     esp_websocket_client_send_text(client, cmac, cmac_len, portMAX_DELAY);

//     esp_websocket_client_stop(client);
//     esp_websocket_client_destroy(client);
// }


static void log_error_if_nonzero(const char *message, int error_code) {
    
    if (error_code != 0) { ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);} }

static void shutdown_signaler(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "No data received for %d seconds, signaling shutdown", NO_DATA_TIMEOUT_SEC); 
    xSemaphoreGive(shutdown_sema); // reboot issue from here?
}

#if CONFIG_WEBSOCKET_URI_FROM_STDIN
static void get_string(char *line, size_t size)
{
    int count = 0;
    while (count < size) {
        int c = fgetc(stdin);
        if (c == '\n') {
            line[count] = '\0';
            break;
        } else if (c > 0 && c < 127) {
            line[count] = c;
            ++count;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

#endif /* CONFIG_WEBSOCKET_URI_FROM_STDIN */

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {

    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        log_error_if_nonzero("HTTP status code",  data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", data->error_handle.esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  data->error_handle.esp_transport_sock_errno);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
        if (data->op_code == 0x08 && data->data_len == 2) {
            ESP_LOGW(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
        } else {
            ESP_LOGW(TAG, "Received=%.*s\n\n", data->data_len, (char *)data->data_ptr);
        }

        // If received data contains json structure it succeed to parse
        cJSON *root = cJSON_Parse(data->data_ptr);
        if (root) {
            for (int i = 0 ; i < cJSON_GetArraySize(root) ; i++) {
                cJSON *elem = cJSON_GetArrayItem(root, i);
                cJSON *id = cJSON_GetObjectItem(elem, "id");
                cJSON *name = cJSON_GetObjectItem(elem, "name");
                ESP_LOGW(TAG, "Json={'id': '%s', 'name': '%s'}", id->valuestring, name->valuestring);
            }
            cJSON_Delete(root);
        }

        ESP_LOGW(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);

        xTimerReset(shutdown_signal_timer, portMAX_DELAY);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        log_error_if_nonzero("HTTP status code",  data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", data->error_handle.esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  data->error_handle.esp_transport_sock_errno);
        }
        break;
    }
}

static void websocket_app_start(void)
{
    esp_websocket_client_config_t websocket_cfg = {};

    shutdown_signal_timer = xTimerCreate("Websocket shutdown timer", NO_DATA_TIMEOUT_SEC * 1000 / portTICK_PERIOD_MS, pdFALSE, NULL, shutdown_signaler);
    shutdown_sema = xSemaphoreCreateBinary();

#if CONFIG_WEBSOCKET_URI_FROM_STDIN
    char line[128];

    ESP_LOGI(TAG, "Please enter uri of websocket endpoint");
    get_string(line, sizeof(line));

    websocket_cfg.uri = line;
    ESP_LOGI(TAG, "Endpoint uri: %s\n", line);

#else
    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;
#endif /* CONFIG_WEBSOCKET_URI_FROM_STDIN */

#if CONFIG_WS_OVER_TLS_MUTUAL_AUTH
    /* Configuring client certificates for mutual authentification */
    extern const char cacert_start[] asm("_binary_ca_cert_pem_start"); // CA certificate
    extern const char cert_start[] asm("_binary_client_cert_pem_start"); // Client certificate
    extern const char cert_end[]   asm("_binary_client_cert_pem_end");
    extern const char key_start[] asm("_binary_client_key_pem_start"); // Client private key
    extern const char key_end[]   asm("_binary_client_key_pem_end");

    websocket_cfg.cert_pem = cacert_start;
    websocket_cfg.client_cert = cert_start;
    websocket_cfg.client_cert_len = cert_end - cert_start;
    websocket_cfg.client_key = key_start;
    websocket_cfg.client_key_len = key_end - key_start;
#elif CONFIG_WS_OVER_TLS_SERVER_AUTH
    extern const char cacert_start[] asm("_binary_ca_certificate_public_domain_pem_start"); // CA cert of wss://echo.websocket.event, modify it if using another server
    websocket_cfg.cert_pem = cacert_start;
#endif

#if CONFIG_WS_OVER_TLS_SKIP_COMMON_NAME_CHECK
    websocket_cfg.skip_cert_common_name_check = true;
#endif

    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);
    xTimerStart(shutdown_signal_timer, portMAX_DELAY);
    char data[32];
    int i = 0;
    while (i < 1) {
        if (esp_websocket_client_is_connected(client)) {
            int len = sprintf(data, "HELLO TEST PLEASE %04d", i++);
            ESP_LOGI(TAG, "Sending %s", data);
            esp_websocket_client_send_text(client, data, len, portMAX_DELAY);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }


    // maybe fix here

    ESP_LOGI(TAG, "Sending fragmented message");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    //memset(data, 'a', sizeof(data));
    //esp_websocket_client_send_text_partial(client, data, sizeof(data), portMAX_DELAY);
    //memset(data, 'b', sizeof(data));
    
    esp_websocket_client_send_cont_msg(client, data, sizeof(data), portMAX_DELAY);
    esp_websocket_client_send_fin(client, portMAX_DELAY);

    xSemaphoreTake(shutdown_sema, portMAX_DELAY);
    esp_websocket_client_close(client, portMAX_DELAY);
    ESP_LOGI(TAG, "Websocket Stopped");
    esp_websocket_client_destroy(client);

}

// -------------------------------------------------------------------------

void esp_mesh_receive_handler(void *arg)
{
    int flag = 0;
    esp_err_t err;
    mesh_addr_t from;
    mesh_data_t mesh_pkt;
    mcnl_packet_t *recv_data;
    
    mesh_pkt.data = rx_buf;
    is_running = true;

    while (is_running) {
        mesh_pkt.size = RX_SIZE;

        // Packet receive 
        err = esp_mesh_recv(&from, &mesh_pkt, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK || !mesh_pkt.size) {
            ESP_LOGE(MESH_TAG, "err:0x%x, size:%d", err, mesh_pkt.size);
            continue;
        }
        
        // Get type of mcnl_packet and handle the received packet 
        recv_data = (mcnl_packet_t*) mesh_pkt.data;
        switch (recv_data->type) {
            case MCNL_PACKET_TYPE_PERF_LATENCY_REQUEST:
                esp_mesh_handle_perf_latency_request(recv_data, &from);
                break; 
            case MCNL_PACKET_TYPE_PERF_LATENCY_RESPONSE: 
                esp_mesh_handle_perf_latency_response(recv_data, &from);
                break;
            case MCNL_PACKET_TYPE_PERF_THROUGHPUT_REQUEST: 
                esp_mesh_handle_perf_throughput_request(recv_data, &from);
                break;
            case MCNL_PACKET_TYPE_PERF_THROUGHPUT_RESPONSE: 
                esp_mesh_handle_perf_throughput_response(recv_data, &from);
                break;
            case MCNL_PACKET_TYPE_FORWARD: 
                if (esp_mesh_is_root())
                    esp_mesh_forward_handler(recv_data, &from);
                break;
            default: 
                ESP_LOGI(MESH_TAG, "Unknown mcnl_packet type:%d ", (int)recv_data->type);
                break;
        }
    }

    vTaskDelete(NULL);
}

void esp_mesh_forward_handler(mcnl_packet_t *data, mesh_addr_t *from) 
{
    static bool is_connected_to_server = false; 
    static int sock;
    int err;

    ESP_LOGI(MESH_TAG, "[Rx][FORWARD]: Src=" MACSTR, MAC2STR(from->addr));

    if (!is_connected_to_server) {
        // Creat a socket 
        sock =  socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(MESH_TAG, "Unable to create socket: errno %d", errno);
            return; 
        }
        ESP_LOGI(MESH_TAG, "Socket created, connecting to %s:%d", SERVER_IP, SERVER_PORT);

        // Specify the server details 
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        server_addr.sin_addr.s_addr = inet_addr(SERVER_IP); // Change this to the server's IP address

        // Connect to the server
        err = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (err != 0) {
            ESP_LOGE(MESH_TAG, "Socket unable to connect: errno %d", errno);
            return;
        }
        ESP_LOGI(MESH_TAG, "Successfully connected");

        is_connected_to_server = true; 
    }

    if (is_connected_to_server) {
        err = send(sock, data->payload, MCNL_PACKET_PAYLOAD_LEN_SHORT, 0);
        if (err < 0) {
            ESP_LOGE(MESH_TAG, "Error occurred during sending: errno %d", errno);
            return;
        }
    }
}

void esp_mesh_send_perf_latency_request(void *arg)
{
    int tx_cnt = 0;
    int dst_idx = *((int*)(arg));
    int route_table_size = 0;
    long cur_time = 0;
    struct timeval tx_time;   
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE] ={0};
    mesh_data_t mesh_pkt; 
    mcnl_packet_t *data = (mcnl_packet_t*) tx_buf;  
    esp_err_t err; 

    esp_mesh_get_routing_table((mesh_addr_t *)&route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
    if (dst_idx < 0 || dst_idx >= route_table_size) {
        ESP_LOGE(MESH_TAG, "Dest. index exceeds the range of routig table: dst_idx=%d, route_table_size=%d", dst_idx, route_table_size);
        is_tx_latency_started = false;
        vTaskDelete(NULL);
        return;
    }

    // Initialize mesh data and mcnl packet  
    mesh_pkt.size = MCNL_PACKET_HEADER_LEN + MCNL_PACKET_PAYLOAD_LEN_SHORT;
    mesh_pkt.proto = MESH_PROTO_BIN;
    mesh_pkt.tos = MESH_TOS_P2P;
    mesh_pkt.data = (uint8_t*)data; 

    data->type = MCNL_PACKET_TYPE_PERF_LATENCY_REQUEST;
    data->flow_id = flow_id++;
    memset(data->payload, 0xab, MCNL_PACKET_PAYLOAD_LEN_SHORT); 

    is_running = true;
    while (is_running && tx_cnt <= 120)
    {
        // If this node is not root 
        // if (!esp_mesh_is_root()) {
        //     ESP_LOGI(MESH_TAG, "layer:%d, rtableSize:%d, %s", mesh_layer,
        //              esp_mesh_get_routing_table_size(),
        //              (is_mesh_connected && esp_mesh_is_root()) ? "ROOT" : is_mesh_connected ? "NODE": "DISCONNECT");
        //     vTaskDelay(10 * 1000 / portTICK_PERIOD_MS);
        //     continue;
        // }

        // Get routing table and print it
        esp_mesh_get_routing_table((mesh_addr_t *)&route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
        if (tx_cnt % 10 == 0) {
            print_routing_table(route_table, route_table_size); 
        }

        // Set sequence number 
        data->seq = tx_cnt; 

        // Set TX time to measure RTT  
        gettimeofday(&tx_time, NULL);
        cur_time = (long)tx_time.tv_sec * 1000 + (long)tx_time.tv_usec / 1000;
        tx_time_history[tx_cnt % MAX_SEND_HISTORY] = cur_time;

        // Send mesh_packet to the destination (leaf node)
        //err = esp_mesh_send(&route_table[dst_idx], &mesh_pkt, MESH_DATA_P2P, NULL, 0); 
        if (esp_mesh_is_root()) {
            err = esp_mesh_send(&route_table[dst_idx], &mesh_pkt, MESH_DATA_P2P, NULL, 0); 
        } else {
            data->type = MCNL_PACKET_TYPE_FORWARD;
            data->payload[0] = tx_cnt;
            err = esp_mesh_send(NULL, &mesh_pkt, MESH_DATA_P2P, NULL, 0); 

            // mesh_pkt.tos = MESH_TOS_P2P;

            // mesh_addr_t ip_addr; 
            // err = esp_netif_str_to_ip4(SERVER_IP, &(ip_addr.mip.ip4));
            // if (err != ESP_OK) {
            //     ESP_LOGE(MESH_TAG, "Failed to convert IP address!: %s", esp_err_to_name(err));
            // }
            // ip_addr.mip.port = SERVER_PORT;
            // ESP_LOGI(MESH_TAG, "[Tx][Ext IP]");
            // err = esp_mesh_send(&ip_addr, &mesh_pkt, MESH_DATA_TODS, NULL, 0); 
            // ESP_LOGI(MESH_TAG, "[Tx][Ext IP OK!!!]");
        }

        if (err != ESP_OK) {
            ESP_LOGE(MESH_TAG, "Failed to request packet to destination : %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(MESH_TAG, "[Tx][LATENCY_REQUEST] Dst=" MACSTR ", Seq=%d, time=%ldms]", 
                MAC2STR(route_table[dst_idx].addr), tx_cnt, cur_time);
        }

        /* if route_table_size is less than 10, add delay to avoid watchdog in this task.*/
        if (route_table_size < 10) {
            vTaskDelay(1 * 1000 / portTICK_PERIOD_MS);
        }

        tx_cnt++;
    }
    is_tx_latency_started = false;
    vTaskDelete(NULL);
}

void esp_mesh_handle_perf_latency_request(mcnl_packet_t *request, mesh_addr_t *from)
{
    mesh_data_t mesh_pkt; 
    mcnl_packet_t *data = (mcnl_packet_t*) tx_buf;  
    esp_err_t err;

    ESP_LOGI(MESH_TAG, "[Rx][LATENCY_REQUEST]: Src=" MACSTR ", Seq=%d", 
            MAC2STR(from->addr), request->seq);

    // Initialize mesh data and mcnl packet  
    mesh_pkt.size = MCNL_PACKET_HEADER_LEN + MCNL_PACKET_PAYLOAD_LEN_SHORT;
    mesh_pkt.proto = MESH_PROTO_BIN;
    mesh_pkt.tos = MESH_TOS_P2P;
    mesh_pkt.data = (uint8_t*)data; 

    data->type = MCNL_PACKET_TYPE_PERF_LATENCY_RESPONSE;
    data->flow_id = request->flow_id;
    data->seq = request->seq;
    memset(data->payload, 0xcd, MCNL_PACKET_PAYLOAD_LEN_SHORT);

    // Send LATENCY_RESPONSE 
    err = esp_mesh_send(from, &mesh_pkt, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(MESH_TAG, "Failed to response packet to destination : %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(MESH_TAG, "[Tx][LATENCY_RESPONSE]: Dst=" MACSTR ", Seq=%d", 
            MAC2STR(from->addr), request->seq);
    }      
}

void esp_mesh_handle_perf_latency_response(mcnl_packet_t *response, mesh_addr_t *from)
{
    long cur_time = 0; 
    long rtt = 0; 
    struct timeval rx_time;

    // Calculate RTT 
    gettimeofday(&rx_time, NULL);
    cur_time = (long)rx_time.tv_sec * 1000 + (long)rx_time.tv_usec / 1000;
    rtt = cur_time - tx_time_history[response->seq % MAX_SEND_HISTORY];
    
    ESP_LOGI(MESH_TAG, "[Rx][LATENCY_RESPONSE]: Src=" MACSTR ", Seq=%d, RTT=%ldms", 
            MAC2STR(from->addr), response->seq, rtt);
}

void esp_mesh_send_perf_throughput_request(void *arg)
{
    int tx_cnt = 0;
    int dst_idx = *((int*)(arg));
    int route_table_size = 0;
    bool first = false;
    double elapsed_time = 0;
    struct timeval start_time, end_time;   
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE] ={0};
    mesh_data_t mesh_pkt; 
    mcnl_packet_t *data = (mcnl_packet_t*) tx_buf;  
    esp_err_t err;

    esp_mesh_get_routing_table((mesh_addr_t *)&route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
    if (dst_idx < 0 || dst_idx >= route_table_size) {
        ESP_LOGE(MESH_TAG, "Dest. index exceeds the range of routig table: dst_idx=%d, route_table_size=%d", dst_idx, route_table_size);
        is_tx_throughput_started = false;
        vTaskDelete(NULL);
        return;
    }

    // Initialize mesh data and mcnl packet  
    mesh_pkt.size = MCNL_PACKET_HEADER_LEN + MCNL_PACKET_PAYLOAD_LEN_LONG;
    mesh_pkt.proto = MESH_PROTO_BIN;
    mesh_pkt.tos = MESH_TOS_P2P;
    mesh_pkt.data = (uint8_t*)data; 

    data->type = MCNL_PACKET_TYPE_PERF_THROUGHPUT_REQUEST;
    data->flow_id = flow_id++;
    memset(data->payload, 0xab, MCNL_PACKET_PAYLOAD_LEN_LONG); 
    
    // Get a routing table and print it 
    esp_mesh_get_routing_table((mesh_addr_t *)&route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
    print_routing_table(route_table, route_table_size); 

    while (tx_cnt < PERF_THROUGHPUT_NUM_PACKETS) {
        // Set sequence number 
        data->seq = tx_cnt++;

        // Send mesh_packet to the destination (leaf node)
        err = esp_mesh_send(&route_table[dst_idx], &mesh_pkt, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(MESH_TAG, "Send error: tx_cnt=%d, err=%d", tx_cnt, err);
            break; 
        } else {
            if (tx_cnt % 50 == 0) {
                ESP_LOGI(MESH_TAG, "[Tx][THROUGHPUT_REQUEST] Dst=" MACSTR ", Seq=%d, Size=%d]", 
                    MAC2STR(route_table[dst_idx].addr), tx_cnt, mesh_pkt.size);
            }
        }

        // Set start time
        if (!first) {
            first = true;
            gettimeofday(&start_time, NULL); 
        }
    }

    // Set end time and finish it 
    gettimeofday(&end_time, NULL); 
    elapsed_time = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    ESP_LOGI(MESH_TAG, "[Tx][THROUGHPUT_REQUEST] Finish! Dst=" MACSTR ", NumData=%d, Total Size=%d, ElapsedTime=%.4f]", 
            MAC2STR(route_table[dst_idx].addr), tx_cnt+1, mesh_pkt.size*(tx_cnt+1), elapsed_time);

    is_tx_throughput_started = false;
    vTaskDelete(NULL);
}

void esp_mesh_handle_perf_throughput_request(mcnl_packet_t *request, mesh_addr_t *from)
{
    static int cur_flow_id = -1; 
    static int rx_cnt;
    static int last_recv_seq; 
    static struct timeval start_time, end_time;
    static float throughput = 0.0;   // Mbps
    static float packet_loss_rate = 0.0; 
    float elapsed_time; // second
    bool last = false;
    esp_err_t err;

    // Initialize when first packet of flow id is received 
    if (cur_flow_id != request->flow_id) {
        cur_flow_id = request->flow_id;
        rx_cnt = 0;
        last_recv_seq = -1;
        throughput = 0.0; 
        packet_loss_rate = 0.0;
        gettimeofday(&start_time, NULL);
    }

    rx_cnt++; 
    if (last_recv_seq < request->seq)
        last_recv_seq = request->seq;

    if (request->seq+1 >= PERF_THROUGHPUT_NUM_PACKETS) 
        last = true;

    // Calculate throughput and packet loss rate 
    if (request->seq % 10 == 0 || last) {
        gettimeofday(&end_time, NULL);
        elapsed_time = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
        throughput = (rx_cnt * MCNL_PACKET_PAYLOAD_LEN_LONG * 8.0) / elapsed_time / 1000000.0; // Mbps
        packet_loss_rate = 1.0 - (float)rx_cnt / (float)(last_recv_seq+1); 
    }

    // Print result
    if (request->seq % 50 == 0 || last) {
        ESP_LOGI(MESH_TAG, "[Rx][THROUGHPUT_REQUEST]: Src=" MACSTR ", Seq=%d, RxCnt=%d, Throughput=%.4fMbps, PacketLossRate=%.4f", 
                MAC2STR(from->addr), request->seq, rx_cnt, throughput, packet_loss_rate);
    }
    
    // Send THROUGHPUT_RESPONSE to root
    if (last) {
        mesh_data_t mesh_pkt; 
        mcnl_packet_t *data = (mcnl_packet_t*) tx_buf;  

        mesh_pkt.size = MCNL_PACKET_HEADER_LEN + MCNL_PACKET_PAYLOAD_LEN_SHORT;
        mesh_pkt.proto = MESH_PROTO_BIN;
        mesh_pkt.tos = MESH_TOS_P2P;
        mesh_pkt.data = (uint8_t*)data; 

        data->type = MCNL_PACKET_TYPE_PERF_THROUGHPUT_RESPONSE;
        data->flow_id = request->flow_id;
        data->seq = request->seq + 1;
        memcpy(&data->payload[0], &throughput, sizeof(float));      // throughput
        memcpy(&data->payload[4], &packet_loss_rate, sizeof(int));  // packet loss rate 

        err = esp_mesh_send(from, &mesh_pkt, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(MESH_TAG, "Failed to response packet to destination : %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(MESH_TAG, "[Tx][THROUGHPUT_RESPONSE]: Dst=" MACSTR ", Throughput=%.4f, PacketLoss=%.4f", 
                MAC2STR(from->addr), throughput, packet_loss_rate);
        }   
    }
}

void esp_mesh_handle_perf_throughput_response(mcnl_packet_t *response, mesh_addr_t *from)
{
    float throughput;
    float packet_loss_rate; 

    memcpy(&throughput, &response->payload[0], sizeof(float));
    memcpy(&packet_loss_rate, &response->payload[4], sizeof(float));

    ESP_LOGI(MESH_TAG, "[Rx][THROUGHPUT_RESPONSE]: Src=" MACSTR ", Throughput=%.4f, PacketLoss=%.4f", 
            MAC2STR(from->addr), throughput, packet_loss_rate);
}


static int latency_command(int argc, char **argv)
{
    int dst_idx = atoi(argv[1]);

    if (!is_tx_latency_started) {
        is_tx_latency_started = true;
        ESP_LOGI(MESH_TAG, "Starting Latency Measurement");

        xTaskCreate(esp_mesh_send_perf_latency_request, "MPTX_LATENCY", 3072, (void*)&dst_idx, 5, NULL);
        //xTaskCreate(esp_mesh_p2p_rx_main_latency, "MPTX_LATENCY", 3072, NULL, 5, NULL);
    } else {
        ESP_LOGI(MESH_TAG, "Already Started Latency Measurement!\n");
    }

    return 0;
}

static int throughput_command(int argc, char **argv)
{
    int dst_idx = atoi(argv[1]);

    if (!is_tx_throughput_started) {
        is_tx_throughput_started = true;
        ESP_LOGI(MESH_TAG, "Starting Throughput Measurement\n");

        xTaskCreate(esp_mesh_send_perf_throughput_request, "MPTX_THROUGHPUT", 3072, (void*)&dst_idx, 5, NULL);
    } else {
        ESP_LOGI(MESH_TAG, "Already Started Throughput Measurement!\n");
    }

    return 0;
}

static int route_table_command(int argc, char **argv)
{
    int route_table_size;
    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE] ={0};

    // Get a routing table and print it 
    esp_mesh_get_routing_table((mesh_addr_t *)&route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
    print_routing_table(route_table, route_table_size); 

    return 0;
}

static int reStart(int argc, char **argv)
{
    app_main();
    return 0;
}

esp_err_t esp_mesh_rx_handler_start(void)
{
    static bool is_rx_handler_started = false;
    if (!is_rx_handler_started) {
        is_rx_handler_started = true;
        ESP_LOGI(MESH_TAG, "RX Task Started!");

        xTaskCreate(esp_mesh_receive_handler, "MPTX_LATENCY", 3072, NULL, 5, NULL);
    }
    return ESP_OK;
}


void mesh_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint16_t last_layer = 0;

    switch (event_id){
    case MESH_EVENT_STARTED:
    {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:" MACSTR "", MAC2STR(id.addr));
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED:
    {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED:
    {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, " MACSTR "",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
                 websocket_app_start();
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED:
    {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, " MACSTR "",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD:
    {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE:
    {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND:
    {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;

    case MESH_EVENT_PARENT_CONNECTED:
    {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:" MACSTR "%s, ID:" MACSTR ", duty:%d",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" : (mesh_layer == 2) ? "<layer2>" : "",
                 MAC2STR(id.addr), connected->duty);
        last_layer = mesh_layer;
        is_mesh_connected = true;
        //storeMAC("Hello");
        

        if (esp_mesh_is_root())
        {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_dhcpc_start(netif_sta);
        }
        // Yunmin 
        esp_mesh_rx_handler_start();
    }
    break;

    case MESH_EVENT_PARENT_DISCONNECTED:
    {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        is_mesh_connected = false;
        //mesh_disconnected_indicator();
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE:
    {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" : (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
        //mesh_connected_indicator(mesh_layer);
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS:
    {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:" MACSTR "",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED:
    {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:" MACSTR "",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED:
    {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ:
    {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:" MACSTR "",
                 switch_req->reason,
                 MAC2STR(switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK:
    {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:" MACSTR "", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE:
    {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED:
    {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD:
    {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>" MACSTR ", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH:
    {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE:
    {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE:
    {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION:
    {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK:
    {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:" MACSTR "",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH:
    {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, " MACSTR "",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    case MESH_EVENT_PS_PARENT_DUTY:
    {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_PARENT_DUTY>duty:%d", ps_duty->duty);
    }
    break;
    case MESH_EVENT_PS_CHILD_DUTY:
    {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_CHILD_DUTY>cidx:%d, " MACSTR ", duty:%d", ps_duty->child_connected.aid - 1,
                 MAC2STR(ps_duty->child_connected.mac), ps_duty->duty);
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));
    //선택옵션을 보일 수 있도록 코드를 추가하였다.
    ESP_LOGI(MESH_TAG, "Select an option:");
    ESP_LOGI(MESH_TAG, "1. Latency Measurement");
    ESP_LOGI(MESH_TAG, "2. Throughput Measurement");
    ESP_LOGI(MESH_TAG, "3. Routing Table");
    ESP_LOGI(MESH_TAG, "0. reStart");

    // if (esp_mesh_is_root()) {
    //     ESP_LOGI(MESH_TAG, "Forward Task Started!");
    //     xTaskCreate(esp_mesh_forward_handler, "MPTX_FORWARD", 3072, NULL, 5, NULL);
    // }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    //기존 코드 사이에 선택옵션에 대한 입력을 받을 수 있도록 코드를 추가
    // ------------------------------------------------------------------------------------
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    repl_config.prompt = ">";
    repl_config.max_cmdline_length = CONFIG_CONSOLE_MAX_COMMAND_LINE_LENGTH;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    //.command 중 하나를 입력하면 각 해당하는 .func이 동작한다.
    const esp_console_cmd_t latency_cmd = {
        .command = "1",
        .help = "Start latency",
        .hint = NULL,
        .func = &latency_command,
    };

    const esp_console_cmd_t throughput_cmd = {
        .command = "2",
        .help = "Start throughtput",
        .hint = NULL,
        .func = &throughput_command,
    };

    const esp_console_cmd_t route_table_cmd = {
        .command = "3",
        .help = "Route Table",
        .hint = NULL,
        .func = &route_table_command,
    };

    const esp_console_cmd_t start_cmd = {
        .command = "0",
        .help = "reStart",
        .hint = NULL,
        .func = &reStart,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&latency_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&throughput_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&route_table_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_cmd));

    // ------------------------------------------------------------------------------------

    /*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /*  create network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));
    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    /*  set mesh topology */
    ESP_ERROR_CHECK(esp_mesh_set_topology(CONFIG_MESH_TOPOLOGY));
    /*  set mesh max layer according to the topology */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(128));
#ifdef CONFIG_MESH_ENABLE_PS
    /* Enable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_enable_ps());

    /* better to increase the associate expired time, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(60));
    /* better to increase the announce interval to avoid too much management traffic, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_announce_interval(600, 3300));
#else
    /* Disable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
#endif
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
    /* router */
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *)&cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *)&cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
           strlen(CONFIG_MESH_ROUTER_PASSWD));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *)&cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());

#ifdef CONFIG_MESH_ENABLE_PS
    /* set the device active duty cycle. (default:10, MESH_PS_DEVICE_DUTY_REQUEST) */
    ESP_ERROR_CHECK(esp_mesh_set_active_duty_cycle(CONFIG_MESH_PS_DEV_DUTY, CONFIG_MESH_PS_DEV_DUTY_TYPE));
    /* set the network active duty cycle. (default:10, -1, MESH_PS_NETWORK_DUTY_APPLIED_ENTIRE) */
    ESP_ERROR_CHECK(esp_mesh_set_network_duty_cycle(CONFIG_MESH_PS_NWK_DUTY, CONFIG_MESH_PS_NWK_DUTY_DURATION, CONFIG_MESH_PS_NWK_DUTY_RULE));
#endif
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s<%d>%s, ps:%d", esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)" : "(tree)", esp_mesh_is_ps_enabled());
}