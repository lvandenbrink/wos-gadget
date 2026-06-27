/*
 * ESP.h - ESP32 AT command driver interface
 */

#ifndef INC_ESP_H_
#define INC_ESP_H_

#include <stdbool.h>
#include <stdint.h>
#include "stm32l0xx_hal.h"
#include "utils.h"
#include "gpio.h"
#include "measurement.h"
#include "PC_Config.h"
#include "statusCheck.h"

// ── Timing constants (ms) ─────────────────────────────────────────────────────

#define ESP_MAX_BUFFER_SIZE              256
#define ESP_TX_BUFFER_SIZE               512
#define ESP_START_UP_TIME                700
#define ESP_RESPONSE_TIME                10
#define ESP_RESPONSE_LONG                50
#define ESP_WIFI_INIT_TIME               1000
#define ESP_WIFI_RETRY_TIME              750
#define ESP_WIFI_WAIT_RESPONSE_TIME_FACTOR 3
#define ESP_UNTIL_NEXT_SEND              10000
#define ESP_UNTIL_NEXT_NTP               75398223  // ~24 hours
#define ESP_MAX_RETRANSMITIONS           3

// ── AT response strings ───────────────────────────────────────────────────────

#define AT_RESPONSE_OK             "OK"
#define AT_RESPONSE_ERROR          "ERROR"
#define AT_RESPONSE_FAIL           "FAIL"
#define AT_RESPONSE_READY          "ready"
#define AT_RESPONSE_START          ">"
#define AT_RESPONSE_WIFI           "WIFI CONNECTED"
#define AT_RESPONSE_TIME_UPDATED   "+TIME_UPDATED"
#define AT_RESPONSE_CIPSNTPTIME    "+CIPSNTPTIME:"

// ── Enumerations ──────────────────────────────────────────────────────────────

typedef enum {
    AT_MODE_INIT,
    AT_MODE_CONFIG,
    AT_MODE_SEND,
    AT_MODE_RECONFIG,
    AT_MODE_GETTIME,
} AT_Mode;

typedef enum {
    RECEIVE_STATUS_OK,
    RECEIVE_STATUS_ERROR,
    RECEIVE_STATUS_READY,
    RECEIVE_STATUS_INCOMPLETE,
    RECEIVE_STATUS_RETRY,
    RECEIVE_STATUS_START,
    RECEIVE_STATUS_TIMEOUT,
    RECEIVE_STATUS_TIME,
} Receive_Status;

typedef enum {
    RECEIVE_EXPECTATION_OK,
    RECEIVE_EXPECTATION_READY,
    RECEIVE_EXPECTATION_START,
    RECEIVE_EXPECTATION_TIME,
} AT_Expectation;

typedef enum {
    AT_WAKEUP,
    AT_SET_RFPOWER,
    AT_RESTORE,
    AT_CWINIT,
    AT_CWMODE1,
    AT_CWMODE2,
    AT_CWAUTOCONN,
    AT_CWJAP,
    AT_CWSTATE,
    AT_CWMODE3,
    AT_CWSAP,
    AT_CIPMUX,
    AT_WEBSERVER,
    AT_SENDDATA,
    AT_MQTTUSERCFG,
    AT_MQTTCONN,
    AT_MQTTPUB,
    AT_MQTTCLEAN,
    AT_CIPSNTPCFG,
    AT_CIPSNTPTIME,
    AT_CIPSNTPINTV,
    AT_END,
} AT_Commands;

typedef enum {
    ESP_STATE_IDLE,
    ESP_STATE_INIT,
    ESP_STATE_WAIT_FOR_REPLY,
    ESP_STATE_SEND,
    ESP_STATE_NEXT_AT,
    ESP_STATE_MODE_SELECT,
    ESP_STATE_DEINIT,
    ESP_STATE_WAIT_AWAKE,
    ESP_STATE_CONFIG,
} ESP_States;

// ── Structs ───────────────────────────────────────────────────────────────────

typedef struct {
    char SSID[32];
    char Password[64];
} WifiConfig;

typedef struct {
    char     broker[64];
    uint16_t port;
    char     clientId[32];
    char     topic[64];
    char     username[32];
    char     password[32];
} MqttConfig;

// ── Public functions ──────────────────────────────────────────────────────────

ESP_States ESP_Upkeep(void);
void       initESPHandler(ESPHandler *ESPHand);
void       initUart(UART_HandleTypeDef *espUart);
void       DisableESP(void);
void       getWifiCred(void);
void       initVariableLink(SensorType2 *HT, SensorType1 *VOC, SensorType1 *DB, SensorType3 *Sens);
void       forceNTPupdate(void);
void       resetRxState(void);
void       clearDMABuffer(void);
bool       ATCompare(Receive_Status received, AT_Expectation expected);

// Debug string helpers
const char *ESPStateToString(uint8_t state);
const char *ATCommandToString(AT_Commands cmd);
const char *ATModeToString(AT_Mode mode);
const char *ATExpectationToString(AT_Expectation exp);

#endif /* INC_ESP_H_ */
