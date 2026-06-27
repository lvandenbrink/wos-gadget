/*
 * ESP.c - ESP32 AT command driver for WOS Gadget
 *
 * === Sequence of operations per measurement cycle ===
 *
 * First boot only — ESP_PROGRAM_INIT:
 *   resetESP() powers on the ESP32 and waits for the "ready" banner.
 *   AT_INIT runs: disable echo, set RF power, init WiFi driver,
 *     enable auto-connect (saved to ESP32 flash), set station mode, disable MUX.
 *   If "WIFI CONNECTED" was seen during init → mode = ESP_PROGRAM_SEND.
 *   Otherwise                                → mode = ESP_PROGRAM_SET_CONN (explicit CWJAP).
 *
 * Every measurement cycle — ESP_PROGRAM_SEND:
 *   After sensors finish, main.c sets espHandle.startSend = true.
 *   AT_SEND runs: disable echo, join WiFi (CWJAP handles "already connected"),
 *     configure MQTT user, connect to broker, publish raw message, send payload,
 *     disconnect cleanly.
 *   On success  → done = true.
 *   On max retries/timeouts → done = true  (main loop sleeps and retries next cycle).
 *
 * AP config mode — ESP_PROGRAM_CONFIG_AP (user button held):
 *   Starts a soft-AP "Omgevingsmonitor_Config" with a web server for WiFi reconfiguration.
 */

#include "ESP.h"
#include <string.h>
#include <stdio.h>
#include "EEProm.h"
#include "Config.h"
#include "microphone.h"
#include "PowerUtils.h"
#include "RealTimeClock.h"
#include "sen5x.h"
#include "statusCheck.h"
#include "main.h"
#include "usart.h"
#include <stdint.h>
#include "setLED.h"

// ── UART / DMA ────────────────────────────────────────────────────────────────

static UART_HandleTypeDef *EspUart = NULL;
extern DMA_HandleTypeDef hdma_usart4_rx;

static uint8_t RxBuffer[ESP_MAX_BUFFER_SIZE];
static volatile bool RxComplete = false;

// RX parse state — reset via resetRxState() on every ESP power-cycle so stale
// DMA positions from the previous cycle don't corrupt the new session.
static uint16_t rxOldPos  = 0;
static char     rxLine[128];
static uint16_t rxLinePos = 0;

// ── Module state ──────────────────────────────────────────────────────────────

bool    EspTurnedOn    = false;
static bool    setTime        = true;
static uint32_t uid[3];
static uint32_t txStartTick;         // timestamp when current send sequence began
static uint8_t  oldEspState   = 255;

// ── Sensor data links (set via initVariableLink) ──────────────────────────────

ESPHandler  *ESPHandle = NULL;
SensorType2 *HTLink    = NULL;
SensorType1 *VOCLink   = NULL;
SensorType1 *DBLink    = NULL;
SensorType3 *SensLink  = NULL;

float batteryCharge = 0.0f;
float solarCharge   = 0.0f;

// ── Configuration ─────────────────────────────────────────────────────────────

WifiConfig Credentials;

MqttConfig MqttCredentials = {
    .broker   = "192.168.1.21",
    .port     = 1883,
    .clientId = "wos-gadget",
    .topic    = "sensor/climate/wos",
    .username = "wos-client",
    .password = ""
};

// ── AT command sequences ──────────────────────────────────────────────────────

// First-boot initialisation.
static AT_Commands AT_INIT[] = {
    AT_WAKEUP, AT_SET_RFPOWER, AT_CWINIT, AT_CWAUTOCONN, AT_CWMODE1, AT_CIPMUX
};

// Normal send: join WiFi first, then MQTT publish.
// AT_CWJAP handles the "already connected" case gracefully.
static AT_Commands AT_SEND[] = {
    AT_WAKEUP, AT_CWJAP, AT_MQTTUSERCFG, AT_MQTTCONN, AT_MQTTPUB, AT_SENDDATA, AT_MQTTCLEAN
};

// Explicit WiFi join (when INIT didn't get a connection).
static AT_Commands AT_WIFI_CONNECT[] = {
    AT_WAKEUP, AT_CWINIT, AT_CWMODE3, AT_CWAUTOCONN, AT_CWJAP, AT_CIPMUX
};

// AP reconfiguration (user-triggered via button).
static AT_Commands AT_WIFI_CONFIG[] = {
    AT_WAKEUP, AT_CWMODE3, AT_CWSAP, AT_CIPMUX, AT_WEBSERVER
};

// NTP time synchronisation.
static AT_Commands AT_SNTP[] = {
    AT_WAKEUP, AT_CIPSNTPCFG, AT_CIPSNTPTIME, AT_CIPSNTPINTV
};

// ── AT state machine variables ────────────────────────────────────────────────

static char        message[1152];
static AT_Commands ATCommandArray[10];

uint8_t ATCounter   = 0;
static uint8_t errorcntr   = 0;
static uint8_t timeoutcntr = 0;
static uint32_t ESPTimeStamp    = 0;
static uint32_t ESPNTPTimeStamp = 0;
static uint8_t  retry           = 0;

static AT_Expectation ATExpectation = RECEIVE_EXPECTATION_OK;
static AT_Commands    ATCommand     = AT_WAKEUP;
static AT_Mode        Mode;

// ── Public API ────────────────────────────────────────────────────────────────

void forceNTPupdate(void) {
    ESPNTPTimeStamp = 0;
}

void initVariableLink(SensorType2 *HT, SensorType1 *VOC, SensorType1 *DB, SensorType3 *Sens) {
    HTLink   = HT;
    DBLink   = DB;
    VOCLink  = VOC;
    SensLink = Sens;
}

void setCharges(void) {
    batteryCharge = ReadBatteryVoltage();
    solarCharge   = ReadSolarVoltage();
}

void getWifiCred(void) {
    ReadUint8ArrayEEprom(SSIDStartAddr,     Credentials.SSID,     SSIDAddrMaxSize);
    ReadUint8ArrayEEprom(PasswordStartAddr, Credentials.Password, PasswordAddrMaxSize);
    Info("WiFi SSID: %s", Credentials.SSID);
}

bool PM25Active(void) {
    uint8_t  cfg[IdSize];
    uint32_t sum = 0;
    ReadUint8ArrayEEprom(PM2ConfigAddr, cfg, IdSize);
    for (uint8_t i = 0; i < IdSize; i++) sum += cfg[i];
    return sum != 0;
}

void DisableESP(void) {
    EspTurnedOn = false;
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port,        ESP32_EN_Pin,        GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ESP32_BOOT_GPIO_Port,      ESP32_BOOT_Pin,      GPIO_PIN_RESET);
    HAL_GPIO_WritePin(Wireless_PSU_EN_GPIO_Port, Wireless_PSU_EN_Pin, GPIO_PIN_RESET);
}

void initESPHandler(ESPHandler *ESPHand) {
    ESPHandle = ESPHand;
    ESPHandle->state = ESP_STATE_INIT;
    uid[0] = HAL_GetUIDw0();
    uid[1] = HAL_GetUIDw1();
    uid[2] = HAL_GetUIDw2();
}

void initUart(UART_HandleTypeDef *espUart) {
    EspUart = espUart;
}

// ── UART send / receive ───────────────────────────────────────────────────────

static bool ESP_Send(const char *command) {
    Debug("ESP_Send: %s", command);
    HAL_StatusTypeDef status = HAL_UART_Transmit_DMA(EspUart, (const uint8_t *)command,
                                                      strlen(command));
    if (status != HAL_OK) {
        Error("HAL_UART_Transmit_DMA failed");
        return false;
    }
    return true;
}

static bool ESP_Receive(uint8_t *buf, uint16_t len) {
    RxComplete = false;
    HAL_StatusTypeDef status = HAL_UART_Receive_DMA(EspUart, buf, len);
    if (status != HAL_OK) {
        Error("HAL_UART_Receive_DMA failed, code: %d", EspUart->ErrorCode);
        if (status & HAL_UART_ERROR_NE) {
            // Noise error — reinitialise UART
            HAL_UART_AbortReceive(EspUart);
            HAL_Delay(10);
            HAL_UART_DeInit(EspUart);
            HAL_Delay(10);
            MX_USART4_UART_Init();
        }
        return false;
    }
    return true;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart == EspUart && huart->ErrorCode != 4) {
        Debug("UART error, code %d", huart->ErrorCode);
    }
}

// ── DMA buffer management ─────────────────────────────────────────────────────

void clearDMABuffer(void) {
    memset(RxBuffer, '\0', ESP_MAX_BUFFER_SIZE);
}

// Reset all RX parse state. Must be called before every ESP power-on so the
// DMA circular-buffer position is correct for the fresh session.
void resetRxState(void) {
    rxOldPos  = 0;
    rxLinePos = 0;
    memset(rxLine, 0, sizeof(rxLine));
    clearDMABuffer();
}

Receive_Status DMA_ProcessBuffer(uint8_t expectation) {
    uint16_t pos = ESP_MAX_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart4_rx);
    if (pos >= ESP_MAX_BUFFER_SIZE) pos = ESP_MAX_BUFFER_SIZE - 1;

    if (pos == rxOldPos) {
        if (retry > ESP_WIFI_WAIT_RESPONSE_TIME_FACTOR) {
            retry = 0;
            return RECEIVE_STATUS_TIMEOUT;
        }
        retry++;
        ESPTimeStamp = HAL_GetTick() + ESP_WIFI_RETRY_TIME;
        return RECEIVE_STATUS_RETRY;
    }

    retry = 0;

    // Log received bytes as printable text
    char logBuf[128];
    uint16_t logLen = 0;
    for (uint16_t i = rxOldPos; i != pos; i = (i + 1) % ESP_MAX_BUFFER_SIZE) {
        char ch = RxBuffer[i];
        if (ch >= ' ' && logLen < sizeof(logBuf) - 1)
            logBuf[logLen++] = ch;
        else if (logLen < sizeof(logBuf) - 3) {
            logBuf[logLen++] = '\\';
            logBuf[logLen++] = (ch == '\n') ? 'n' : (ch == '\r') ? 'r' : '?';
        }
    }
    logBuf[logLen] = '\0';
    Info("ESP rx: '%s'", logBuf);

    // Parse byte-by-byte into lines, check each complete line
    Receive_Status status = RECEIVE_STATUS_INCOMPLETE;
    for (uint16_t i = rxOldPos; i != pos; i = (i + 1) % ESP_MAX_BUFFER_SIZE) {
        char ch = RxBuffer[i];
        bool eol = (ch == '\n' || ch == '\r');

        if (ch >= ' ' && rxLinePos < sizeof(rxLine) - 1) {
            rxLine[rxLinePos++] = ch;
            if (ch == '>') eol = true; // ">" prompt is its own end-of-line
        }

        if (eol && rxLinePos > 0) {
            rxLine[rxLinePos] = '\0';
            rxLinePos = 0;
            Info("ESP line: '%s'", rxLine);

            switch (expectation) {
            case RECEIVE_EXPECTATION_OK:
                if (strstr(rxLine, AT_RESPONSE_OK))           status = RECEIVE_STATUS_OK;
                break;
            case RECEIVE_EXPECTATION_READY:
                if (strstr(rxLine, AT_RESPONSE_READY))        status = RECEIVE_STATUS_READY;
                break;
            case RECEIVE_EXPECTATION_START:
                if (strstr(rxLine, AT_RESPONSE_START))        status = RECEIVE_STATUS_START;
                break;
            case RECEIVE_EXPECTATION_TIME:
                if (strstr(rxLine, AT_RESPONSE_TIME_UPDATED)) status = RECEIVE_STATUS_TIME;
                break;
            }

            if (strstr(rxLine, AT_RESPONSE_ERROR) || strstr(rxLine, AT_RESPONSE_FAIL))
                status = RECEIVE_STATUS_ERROR;

            if (strstr(rxLine, AT_RESPONSE_WIFI))
                ESPHandle->connectionMade = true;

            if (ATCommand == AT_CIPSNTPTIME) {
                char *timeReply = strstr(rxLine, AT_RESPONSE_CIPSNTPTIME);
                if (timeReply) ParseTime(timeReply);
            }
        }
    }

    rxOldPos = pos;
    return status;
}

bool ATCompare(Receive_Status received, AT_Expectation expected) {
    switch (expected) {
    case RECEIVE_EXPECTATION_OK:    return received == RECEIVE_STATUS_OK;
    case RECEIVE_EXPECTATION_READY: return received == RECEIVE_STATUS_READY;
    case RECEIVE_EXPECTATION_START: return received == RECEIVE_STATUS_START;
    case RECEIVE_EXPECTATION_TIME:  return received == RECEIVE_STATUS_TIME;
    default:                        return false;
    }
}

// ── MQTT payload builder ──────────────────────────────────────────────────────

uint16_t CreateMessage(void) {
    setCharges();

    memset(message, 0, sizeof(message));
    size_t pos = 0;
    size_t cap = sizeof(message) - 1;  // keep one byte for null terminator

#define APPEND(...) pos += snprintf(message + pos, cap - pos, __VA_ARGS__)

    APPEND("{");
    APPEND("\"temperature\":%.2f,", HTLink->measurementValue1);
    APPEND("\"humidity\":%.1f,",    HTLink->measurementValue2);
    APPEND("\"sound\":%.2f,",       DBLink->measurementValue);
    APPEND("\"battery\":%.2f,",     batteryCharge);
    APPEND("\"solar\":%.2f,",       solarCharge);
    APPEND("\"voc\":%d",            (uint16_t)VOCLink->measurementValue);

    if (SensLink->active) {
        if (PM25Active())
            APPEND(",\"PM2.5\":%.2f", SensLink->measurementValue1 / 10.0f);
        else
            APPEND(",\"PM10\":%.2f",  SensLink->measurementValue2 / 10.0f);
        APPEND(",\"NOx\":%.2f", SensLink->measurementValue4 / 10.0f);
    }

    APPEND("}");
#undef APPEND

    Info("MQTT payload (%u bytes): %s", (uint16_t)pos, message);
    return (uint16_t)pos;
}

// ── ESP32 hardware control ────────────────────────────────────────────────────

void resetESP(void) {
    // Power-cycle the wireless PSU, then toggle EN to boot the ESP32.
    HAL_GPIO_WritePin(Wireless_PSU_EN_GPIO_Port, Wireless_PSU_EN_Pin, GPIO_PIN_RESET);
    HAL_Delay(50);
    HAL_GPIO_WritePin(Wireless_PSU_EN_GPIO_Port, Wireless_PSU_EN_Pin, GPIO_PIN_SET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port,   ESP32_EN_Pin,   GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(ESP32_BOOT_GPIO_Port, ESP32_BOOT_Pin, 1);
    HAL_Delay(10);
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port,   ESP32_EN_Pin,   GPIO_PIN_SET);
}

// Used when BOOT0 is held at startup to enter ESP32 programming mode.
void StartProg(void) {
    HAL_Delay(100);
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port,   ESP32_EN_Pin,   GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(ESP32_BOOT_GPIO_Port, ESP32_BOOT_Pin, GPIO_PIN_RESET);
    HAL_Delay(500);
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port,   ESP32_EN_Pin,   GPIO_PIN_SET);
    HAL_Delay(500);
    HAL_GPIO_WritePin(ESP32_BOOT_GPIO_Port, ESP32_BOOT_Pin, GPIO_PIN_SET);
    HAL_Delay(40);
}

// ── AT command helpers ────────────────────────────────────────────────────────

static bool PollAwake(void)   { return ESP_Send("ATE0\r\n"); }
static bool RFPower(void)     { return ESP_Send("AT+RFPOWER=70\r\n"); }
static bool ATRestore(void)   { return ESP_Send("AT+RESTORE\r\n"); }
static bool CWINIT(void)      { return ESP_Send("AT+CWINIT=1\r\n"); }
static bool CWMODE1(void)     { return ESP_Send("AT+CWMODE=1\r\n"); }
static bool CWMODE2(void)     { return ESP_Send("AT+CWMODE=2\r\n"); }
static bool CWMODE3(void)     { return ESP_Send("AT+CWMODE=3\r\n"); }
static bool CWAUTOCONN(void)  { return ESP_Send("AT+CWAUTOCONN=1\r\n"); }
static bool CWSTATE(void)     { return ESP_Send("AT+CWSTATE?\r\n"); }
static bool CWSAP(void)       { return ESP_Send("AT+CWSAP=\"Omgevingsmonitor_Config\",\"\",11,0,1\r\n"); }
static bool CIPMUX(void)      { return ESP_Send("AT+CIPMUX=0\r\n"); }
static bool WEBSERVER(void)   { return ESP_Send("AT+WEBSERVER=1,80,60\r\n"); }

#define AT_CMD_BUF_LEN 150
static char atCmdBuf[AT_CMD_BUF_LEN];

static bool CWJAP(void) {
    getWifiCred();
    if (Credentials.SSID[0] == 0) {
        // No credentials stored — send a no-op (ATE0) so the state machine
        // gets an OK and advances to the next step normally.
        Info("No WiFi credentials configured");
        return ESP_Send("ATE0\r\n");
    }
    snprintf(atCmdBuf, sizeof(atCmdBuf), "AT+CWJAP=\"%s\",\"%s\"\r\n",
             Credentials.SSID, Credentials.Password);
    return ESP_Send(atCmdBuf);
}

static bool SENDDATA(void) { return ESP_Send(message); }

static bool MQTTUSERCFG(void) {
    snprintf(atCmdBuf, sizeof(atCmdBuf),
             "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
             MqttCredentials.clientId, MqttCredentials.username, MqttCredentials.password);
    return ESP_Send(atCmdBuf);
}

static bool MQTTCONN(void) {
    snprintf(atCmdBuf, sizeof(atCmdBuf),
             "AT+MQTTCONN=0,\"%s\",%d,0\r\n",
             MqttCredentials.broker, MqttCredentials.port);
    return ESP_Send(atCmdBuf);
}

static bool MQTTPUB(void) {
    uint16_t len = CreateMessage();
    Info("Publishing to '%s' (%u bytes, retain=1)", MqttCredentials.topic, len);
    snprintf(atCmdBuf, sizeof(atCmdBuf),
             "AT+MQTTPUBRAW=0,\"%s\",%u,0,1\r\n", MqttCredentials.topic, len);
    return ESP_Send(atCmdBuf);
}

static bool MQTTCLEAN(void) { return ESP_Send("AT+MQTTCLEAN=0\r\n"); }

static bool CIPSNTPCFG(void) {
    bool ok = ESP_Send("AT+CIPSNTPCFG=1,100,\"nl.pool.ntp.org\",\"time.google.com\",\"time.windows.com\"\r\n");
    if (ok) HAL_Delay(1000);
    return ok;
}

static bool CIPSNTPTIME(void) { return ESP_Send("AT+CIPSNTPTIME?\r\n"); }
static bool CIPSNTPINTV(void) { return ESP_Send("AT+CIPSNTPINTV=14400\r\n"); }

// ── AT_Send dispatcher ────────────────────────────────────────────────────────

bool AT_Send(AT_Commands cmd) {
    bool sent = false;
    switch (cmd) {
    case AT_WAKEUP:
        if (TimestampIsReached(ESPTimeStamp)) {
            sent = PollAwake();
            ESPTimeStamp = HAL_GetTick() + ESP_WIFI_INIT_TIME;
        }
        break;
    case AT_SET_RFPOWER:
        sent = RFPower();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_RESTORE:
        sent = ATRestore();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_LONG;
        break;
    case AT_CWINIT:
        sent = CWINIT();
        ESPTimeStamp = HAL_GetTick() + ESP_WIFI_INIT_TIME;
        break;
    case AT_CWSTATE:
        sent = CWSTATE();
        ESPTimeStamp = HAL_GetTick() + ESP_WIFI_INIT_TIME;
        break;
    case AT_CWMODE1:
        sent = CWMODE1();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_CWMODE2:
        sent = CWMODE2();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_CWAUTOCONN:
        sent = CWAUTOCONN();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_CWJAP:
        sent = CWJAP();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_LONG;
        break;
    case AT_CWMODE3:
        sent = CWMODE3();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_CWSAP:
        sent = CWSAP();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_CIPMUX:
        sent = CIPMUX();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_WEBSERVER:
        sent = WEBSERVER();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_MQTTUSERCFG:
        sent = MQTTUSERCFG();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_LONG;
        break;
    case AT_MQTTCONN:
        if (ESPHandle->startSend) {
            sent = MQTTCONN();
            ESPTimeStamp = HAL_GetTick() + ESP_WIFI_INIT_TIME;
        }
        break;
    case AT_MQTTPUB:
        sent = MQTTPUB();
        ESPTimeStamp = HAL_GetTick() + ESP_WIFI_INIT_TIME;
        break;
    case AT_MQTTCLEAN:
        sent = MQTTCLEAN();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_LONG;
        break;
    case AT_SENDDATA:
        sent = SENDDATA();
        ESPTimeStamp = HAL_GetTick() + ESP_WIFI_INIT_TIME;
        break;
    case AT_CIPSNTPCFG:
        sent = CIPSNTPCFG();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_CIPSNTPTIME:
        sent = CIPSNTPTIME();
        ESPTimeStamp = HAL_GetTick() + ESP_WIFI_INIT_TIME;
        break;
    case AT_CIPSNTPINTV:
        sent = CIPSNTPINTV();
        ESPTimeStamp = HAL_GetTick() + ESP_RESPONSE_TIME;
        break;
    case AT_END:
        break;
    }
    return sent;
}

// ── ESP upkeep state machine ──────────────────────────────────────────────────

ESP_States ESP_Upkeep(void) {
    static Receive_Status ATReceived = RECEIVE_STATUS_INCOMPLETE;

    if (ESPHandle->state != oldEspState && GetVerboseLevel() == VERBOSE_ALL) {
        oldEspState = ESPHandle->state;
        Debug("ESP state=%-14s cmd=%-14s mode=%-10s exp=%s",
              ESPStateToString(ESPHandle->state), ATCommandToString(ATCommand),
              ATModeToString(Mode), ATExpectationToString(ATExpectation));
    }

    switch (ESPHandle->state) {

    // ── IDLE: wait for a trigger ──────────────────────────────────────────────
    case ESP_STATE_IDLE:
        if (ESPHandle->configAP) {
            ESPHandle->state = ESP_STATE_CONFIG;
        } else if (!ESPHandle->done) {
            ESPHandle->state = ESP_STATE_INIT;
            EspTurnedOn = false;
        }
        break;

    // ── INIT: power on ESP32 and start listening ──────────────────────────────
    case ESP_STATE_INIT:
        if (!EspTurnedOn) {
            resetRxState();  // clear stale DMA position from previous cycle
            resetESP();
            ESPTimeStamp = HAL_GetTick() + ESP_START_UP_TIME;
            EspTurnedOn  = true;
            ESPHandle->ready = true;
        }
        if (ESP_Receive(RxBuffer, ESP_MAX_BUFFER_SIZE))
            ESPHandle->state = ESP_STATE_WAIT_AWAKE;
        break;

    // ── WAIT_AWAKE: wait for the "ready" boot banner ──────────────────────────
    case ESP_STATE_WAIT_AWAKE:
        ATReceived = DMA_ProcessBuffer(RECEIVE_EXPECTATION_READY);
        if (ATCompare(ATReceived, RECEIVE_EXPECTATION_READY))
            ESPHandle->state = ESP_STATE_MODE_SELECT;
        break;

    // ── MODE_SELECT: load the command sequence for the current program ────────
    case ESP_STATE_MODE_SELECT:
        memset(ATCommandArray, AT_END, sizeof(ATCommandArray));
        ATCounter     = 0;
        ATExpectation = RECEIVE_EXPECTATION_OK;

        switch (ESPHandle->mode) {
        case ESP_PROGRAM_INIT:
            memcpy(ATCommandArray, AT_INIT,         sizeof(AT_INIT));
            Mode = AT_MODE_INIT;
            break;
        case ESP_PROGRAM_SET_CONN:
            memcpy(ATCommandArray, AT_WIFI_CONNECT, sizeof(AT_WIFI_CONNECT));
            Mode = AT_MODE_CONFIG;
            break;
        case ESP_PROGRAM_SEND:
            memcpy(ATCommandArray, AT_SEND,         sizeof(AT_SEND));
            Mode = AT_MODE_SEND;
            txStartTick = HAL_GetTick();
            break;
        case ESP_PROGRAM_CONFIG_AP:
            memcpy(ATCommandArray, AT_WIFI_CONFIG,  sizeof(AT_WIFI_CONFIG));
            Mode = AT_MODE_RECONFIG;
            break;
        case ESP_PROGRAM_RTC:
            memcpy(ATCommandArray, AT_SNTP,         sizeof(AT_SNTP));
            Mode = AT_MODE_GETTIME;
            txStartTick = HAL_GetTick();
            break;
        default:
            Error("Unknown ESP program mode %d", ESPHandle->mode);
            ESPHandle->state = ESP_STATE_DEINIT;
            return ESPHandle->state;
        }

        ATCommand = ATCommandArray[ATCounter];
        ESPHandle->state = ESP_STATE_SEND;
        break;

    // ── SEND: dispatch the current AT command ─────────────────────────────────
    case ESP_STATE_SEND:
        if (AT_Send(ATCommand))
            ESPHandle->state = ESP_STATE_WAIT_FOR_REPLY;
        break;

    // ── WAIT_FOR_REPLY: parse the response ───────────────────────────────────
    case ESP_STATE_WAIT_FOR_REPLY:
        if (!TimestampIsReached(ESPTimeStamp)) break;

        ATReceived = DMA_ProcessBuffer(ATExpectation);

        if (ATReceived == RECEIVE_STATUS_INCOMPLETE) {
            // Still receiving — check again after a short delay
            ESPTimeStamp = HAL_GetTick() + 10;
        } else if (ATReceived == RECEIVE_STATUS_ERROR) {
            if (ATCommand == AT_CWJAP) ErrorBlinkWiFi();
            errorcntr++;
            if (errorcntr >= ESP_MAX_RETRANSMITIONS) {
                clearDMABuffer();
                Error("Max retransmits, aborting after %lu ms", HAL_GetTick() - txStartTick);
                errorcntr = 0;
                ESPHandle->state = ESP_STATE_DEINIT;
            } else {
                ESPHandle->state = ESP_STATE_SEND;
            }
        } else if (ATReceived == RECEIVE_STATUS_TIMEOUT) {
            timeoutcntr++;
            Error("AT timeout (cmd=%s)", ATCommandToString(ATCommand));
            if (timeoutcntr >= ESP_MAX_RETRANSMITIONS) {
                clearDMABuffer();
                Error("Max timeouts, aborting after %lu ms", HAL_GetTick() - txStartTick);
                timeoutcntr = 0;
                ESPHandle->state = ESP_STATE_DEINIT;
            } else {
                ESPHandle->state = ESP_STATE_SEND;
            }
        } else if (ATCompare(ATReceived, ATExpectation)) {
            ESPHandle->state = ESP_STATE_NEXT_AT;
        }
        break;

    // ── NEXT_AT: advance to the next command in the sequence ─────────────────
    case ESP_STATE_NEXT_AT:
        ATCounter++;
        ATCommand   = ATCommandArray[ATCounter];
        errorcntr   = 0;
        timeoutcntr = 0;

        // Pick the right expectation for the incoming command
        if      (ATCommand == AT_RESTORE)     ATExpectation = RECEIVE_EXPECTATION_READY;
        else if (ATCommand == AT_MQTTPUB)     ATExpectation = RECEIVE_EXPECTATION_START;
        else if (ATCommand == AT_CIPSNTPCFG)  ATExpectation = RECEIVE_EXPECTATION_TIME;
        else                                  ATExpectation = RECEIVE_EXPECTATION_OK;

        if (ATCommand != AT_END) {
            ESPHandle->state = ESP_STATE_SEND;
            break;
        }

        // ── Sequence complete ─────────────────────────────────────────────────
        if (ESPHandle->mode == ESP_PROGRAM_INIT ||
            ESPHandle->mode == ESP_PROGRAM_SET_CONN) {
            if (!ESPHandle->connectionMade) {
                ErrorBlinkWiFi();
                if (ESPHandle->mode == ESP_PROGRAM_SET_CONN) ESPHandle->done = true;
                ESPHandle->mode = ESP_PROGRAM_SET_CONN;
            } else {
                ESPHandle->mode = ESP_PROGRAM_SEND;
            }
            ESPHandle->state = ESP_STATE_IDLE;

        } else if (ESPHandle->mode == ESP_PROGRAM_SEND) {
            clearDMABuffer();
            Info("MQTT publish done in %lu ms", HAL_GetTick() - txStartTick);
            ResetdBAmax();
            showTime();
            ESPHandle->done  = true;
            ESPHandle->state = ESP_STATE_IDLE;

        } else if (ESPHandle->mode == ESP_PROGRAM_RTC) {
            setTime = false;
            ESPNTPTimeStamp = HAL_GetTick() + ESP_UNTIL_NEXT_NTP;
            Info("NTP synced. Next sync in ~24 h (tick %lu)", ESPNTPTimeStamp);
            clearDMABuffer();
            ESPHandle->mode  = ESP_PROGRAM_SEND;
            ESPHandle->state = ESP_STATE_IDLE;

        } else {
            ESPHandle->state = ESP_STATE_IDLE;
        }
        break;

    // ── DEINIT: sequence failed — let the main loop proceed to sleep ──────────
    case ESP_STATE_DEINIT:
        ESPHandle->done  = true;
        ESPHandle->state = ESP_STATE_IDLE;
        break;

    // ── CONFIG: AP config mode — process USB commands until reset ─────────────
    case ESP_STATE_CONFIG:
        Process_PC_Config(GetUsbRxPointer());
        break;

    default:
        Error("Unexpected ESP state %d", ESPHandle->state);
        ESPHandle->state = ESP_STATE_INIT;
        break;
    }

    return ESPHandle->state;
}

// ── Debug helpers ─────────────────────────────────────────────────────────────

const char *ESPStateToString(uint8_t state) {
    switch (state) {
    case ESP_STATE_IDLE:           return "IDLE";
    case ESP_STATE_INIT:           return "INIT";
    case ESP_STATE_WAIT_AWAKE:     return "WAIT_AWAKE";
    case ESP_STATE_MODE_SELECT:    return "MODE_SELECT";
    case ESP_STATE_SEND:           return "SEND";
    case ESP_STATE_WAIT_FOR_REPLY: return "WAIT_FOR_REPLY";
    case ESP_STATE_NEXT_AT:        return "NEXT_AT";
    case ESP_STATE_CONFIG:         return "CONFIG";
    case ESP_STATE_DEINIT:         return "DEINIT";
    default:                       return "UNKNOWN";
    }
}

const char *ATCommandToString(AT_Commands cmd) {
    switch (cmd) {
    case AT_WAKEUP:      return "WAKEUP";
    case AT_SET_RFPOWER: return "SET_RFPOWER";
    case AT_RESTORE:     return "RESTORE";
    case AT_CWINIT:      return "CWINIT";
    case AT_CWSTATE:     return "CWSTATE";
    case AT_CWMODE1:     return "CWMODE1";
    case AT_CWMODE2:     return "CWMODE2";
    case AT_CWMODE3:     return "CWMODE3";
    case AT_CWAUTOCONN:  return "CWAUTOCONN";
    case AT_CWJAP:       return "CWJAP";
    case AT_CWSAP:       return "CWSAP";
    case AT_CIPMUX:      return "CIPMUX";
    case AT_WEBSERVER:   return "WEBSERVER";
    case AT_SENDDATA:    return "SENDDATA";
    case AT_MQTTUSERCFG: return "MQTTUSERCFG";
    case AT_MQTTCONN:    return "MQTTCONN";
    case AT_MQTTPUB:     return "MQTTPUB";
    case AT_MQTTCLEAN:   return "MQTTCLEAN";
    case AT_CIPSNTPCFG:  return "CIPSNTPCFG";
    case AT_CIPSNTPTIME: return "CIPSNTPTIME";
    case AT_CIPSNTPINTV: return "CIPSNTPINTV";
    case AT_END:         return "END";
    default:             return "UNKNOWN";
    }
}

const char *ATModeToString(AT_Mode mode) {
    switch (mode) {
    case AT_MODE_INIT:     return "INIT";
    case AT_MODE_CONFIG:   return "CONFIG";
    case AT_MODE_SEND:     return "SEND";
    case AT_MODE_RECONFIG: return "RECONFIG";
    case AT_MODE_GETTIME:  return "GETTIME";
    default:               return "UNKNOWN";
    }
}

const char *ATExpectationToString(AT_Expectation exp) {
    switch (exp) {
    case RECEIVE_EXPECTATION_OK:    return "OK";
    case RECEIVE_EXPECTATION_READY: return "READY";
    case RECEIVE_EXPECTATION_START: return "START";
    case RECEIVE_EXPECTATION_TIME:  return "TIME";
    default:                        return "UNKNOWN";
    }
}
