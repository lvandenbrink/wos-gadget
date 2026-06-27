/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2024 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "i2s.h"
#include "iwdg.h"
#include "usart.h"
#include "rtc.h"
#include "tim.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "microphone.h"
#include "I2CSensors.h"
#include "utils.h"
#include "measurement.h"
#include "globals.h"
#include "ESP.h"
#include "PowerUtils.h"
#include "usbd_cdc_if.h"
#include "statusCheck.h"
#include "RealTimeClock.h"
#include "sound_measurement.h"
#include "print_functions.h"
#include "sen5x.h"
#include "setLED.h"
#include "sgp40.h"
#include "wsenHIDS.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum {
    GADGET_BATTERY_CHECK,
    GADGET_START_MEASUREMENTS,
    GADGET_CHECK_MEASUREMENTS_DONE,
    GADGET_CONFIG_MODE,
    GADGET_SEND_MEASUREMENTS,
    GADGET_GO_TO_SLEEP,
    GADGET_REINIT_UART,
}gadgetState;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

// Sleep intervals (seconds). USB sends every 5 min; battery every 15/30 min.
#define SLEEP_USB           300   // 5 minutes — USB powered
#define SLEEP_BATTERY_GOOD  900   // 15 minutes — battery >= 3.7 V
#define SLEEP_BATTERY_LOW  1800   // 30 minutes — battery 3.5–3.7 V
#define SLEEP_BATTERY_CRIT  600   // 10 minutes — battery critical (very low activity)

// Hard deadline for the entire send phase (ESP32 boot + WiFi + MQTT).
// 90 s on battery, 120 s on USB (warm connection more likely on USB).
#define SEND_TIMEOUT_BATTERY_MS  90000
#define SEND_TIMEOUT_USB_MS     120000

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
bool ESP_Programming = false;
static bool priorUSBpluggedIn = false;

uint8_t u1_rx_buff[16];                        // rxbuffer for serial logger
uint8_t RxData[UART_CDC_DMABUFFERSIZE] = {0};  // rx buffer for USB
uint16_t IndexRxData = 0;
uint32_t LastRxTime  = 0;
uint16_t size        = 0;

extern DMA_HandleTypeDef hdma_spi2_rx;

ESPHandler espHandle = {
    .done          = false,
    .ready         = false,
    .resume        = false,
    .startSend     = false,
    .connectionMade = false,
    .configAP      = false,
    .mode          = ESP_PROGRAM_INIT
};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */


void petDog(){
  if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
    {
      Error_Handler();
    }
}

void ESP_Programming_Read_Remaining_DMA()
{
    // ESP programmer section

    if (LastRxTime != 0 && ESP_Programming)
    {
        if ((LastRxTime + 100) < HAL_GetTick()) // 120
        {
            HAL_UART_DMAPause(&hlpuart1);
            size = __HAL_DMA_GET_COUNTER(hlpuart1.hdmarx);
            if (size > (UART_CDC_DMABUFFERSIZE / 2))
            {
                size = UART_CDC_DMABUFFERSIZE - size;
            }
            else
            {
                size = (UART_CDC_DMABUFFERSIZE / 2) - size;
            }
            if (size > 0)
            {
                CDC_Transmit_FS(&RxData[IndexRxData], size);
                LastRxTime = 0;
                IndexRxData += size;
            }
            HAL_UART_DMAResume(&hlpuart1);
        }
    }
}

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_I2S2_Init();
  MX_USART1_UART_Init();
  MX_I2C2_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_USART4_UART_Init();
  MX_ADC_Init();
  MX_USB_DEVICE_Init();
  MX_RTC_Init();
  MX_LPUART1_UART_Init();
  MX_TIM6_Init();
  MX_IWDG_Init();
  /* USER CODE BEGIN 2 */
    /*
     * === Boot sequence ===
     *
     * 1. Peripheral init (HAL-generated above).
     * 2. GPIO_InitPWMLEDs — configure PWM channels for the three RGB LEDs.
     * 3. If BOOT0 is held: enter ESP32 passthrough programming mode (UART ↔ USB).
     * 4. soundInit — configure I2S DMA for the microphone.
     * 5. Device_Init — initialise I2C bus and all connected sensors.
     * 6. initESPHandler / initUart — prepare ESP32 driver state.
     *
     * === Main loop states ===
     *
     * BATTERY_CHECK        Read battery and solar voltage; select sleep interval.
     * START_MEASUREMENTS   Reset sensor state; kick off measurement upkeep.
     * CHECK_MEASUREMENTS_DONE  Poll sensors every loop tick until all are done
     *                          (or 45 s timeout elapses).
     * SEND_MEASUREMENTS    Drive ESP_Upkeep() until done=true or send timeout.
     * GO_TO_SLEEP          Power down ESP32, deinit UART, enter STOP mode.
     * REINIT_UART          Re-init UART after wake; loop back to BATTERY_CHECK.
     */

    GPIO_InitPWMLEDs(&htim2, &htim3);
    if (UserButton_Pressed()) {
        EnableESPProg();
        ESP_Programming = true;
    }
    SetVerboseLevel(VERBOSE_ALL);
    BinaryReleaseInfo();
    HAL_UART_Receive_IT(&huart1, u1_rx_buff, 1);
    InitClock(&hrtc);

    if (!soundInit(&hdma_spi2_rx, &hi2s2, &htim6, DMA1_Channel4_5_6_7_IRQn)) {
        errorHandler(__func__, __LINE__, __FILE__);
    }
    Device_Init(&hi2c1, &hi2s2, &hadc);
    priorUSBpluggedIn = !Check_USB_PowerOn();
    initESPHandler(&espHandle);
    initUart(&huart4);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    while (1)
    {
        static gadgetState    state       = GADGET_BATTERY_CHECK;
        static uint16_t       sleepTime   = SLEEP_BATTERY_GOOD;
        static Battery_Status lastCharge  = BATTERY_GOOD;

        // Long-press user button → enter AP config mode
        if (processButtonPressed() && state != GADGET_CONFIG_MODE) {
            Debug("User button: entering AP config mode");
            espHandle.mode     = ESP_PROGRAM_CONFIG_AP;
            espHandle.configAP = true;
            state = GADGET_CONFIG_MODE;
        }

        petDog();

        switch (state) {

        // ── Check battery / select sleep interval ─────────────────────────────
        case GADGET_BATTERY_CHECK:
            lastCharge = Battery_Upkeep();
            switch (lastCharge) {
            case USB_PLUGGED_IN:
                sleepTime = SLEEP_USB;
                state = GADGET_START_MEASUREMENTS;
                break;
            case BATTERY_GOOD:
                sleepTime = SLEEP_BATTERY_GOOD;
                state = GADGET_START_MEASUREMENTS;
                break;
            case BATTERY_LOW:
                sleepTime = SLEEP_BATTERY_LOW;
                state = GADGET_START_MEASUREMENTS;
                break;
            case BATTERY_CRITICAL:
                sleepTime = SLEEP_BATTERY_CRIT;
                state = GADGET_GO_TO_SLEEP;
                break;
            }
            break;

        // ── Start all sensor measurements ────────────────────────────────────
        case GADGET_START_MEASUREMENTS:
            measurementReset();
            measurementUpkeep();
            state = GADGET_CHECK_MEASUREMENTS_DONE;
            break;

        // ── Poll until all sensors are done (timeout handled inside upkeep) ──
        case GADGET_CHECK_MEASUREMENTS_DONE:
            if (measurementUpkeep()) {
                uint32_t timeout = (lastCharge == USB_PLUGGED_IN)
                                 ? SEND_TIMEOUT_USB_MS
                                 : SEND_TIMEOUT_BATTERY_MS;
                espHandle.startSend    = true;
                espHandle.timeOutStamp = HAL_GetTick() + timeout;
                state = GADGET_SEND_MEASUREMENTS;
            }
            break;

        // ── Drive ESP_Upkeep until done or timeout ───────────────────────────
        case GADGET_SEND_MEASUREMENTS:
            ESP_Upkeep();
            HAL_GPIO_WritePin(MCU_LED_C_B_GPIO_Port, MCU_LED_C_B_Pin, false);

            if (espHandle.done) {
                espHandle.startSend = false;
                state = GADGET_GO_TO_SLEEP;
            } else if (TimestampIsReached(espHandle.timeOutStamp)) {
                Error("Send timeout — going to sleep");
                HAL_GPIO_WritePin(MCU_LED_C_R_GPIO_Port, MCU_LED_C_R_Pin, false);
                HAL_Delay(500);
                HAL_GPIO_WritePin(MCU_LED_C_R_GPIO_Port, MCU_LED_C_R_Pin, true);
                espHandle.startSend = false;
                state = GADGET_GO_TO_SLEEP;
            }
            break;

        // ── AP config mode: blink LED, process USB config commands ───────────
        case GADGET_CONFIG_MODE:
            ESP_Upkeep();
            HAL_GPIO_WritePin(MCU_LED_C_R_GPIO_Port, MCU_LED_C_R_Pin, true);
            HAL_Delay(200);
            HAL_GPIO_WritePin(MCU_LED_C_R_GPIO_Port, MCU_LED_C_R_Pin, false);
            HAL_Delay(200);
            break;

        // ── Power down and enter STOP mode ───────────────────────────────────
        case GADGET_GO_TO_SLEEP:
            DisableESP();
            HAL_UART_DeInit(&huart4);
            espHandle.done = false;
            LEDSOff();
            Debug("Sleeping for %u s", sleepTime);
            watchdogStopMode(sleepTime);
            state = GADGET_REINIT_UART;
            break;

        // ── Re-init UART after wake, then repeat ─────────────────────────────
        case GADGET_REINIT_UART:
            MX_USART4_UART_Init();
            initUart(&huart4);
            measurementReset();
            state = GADGET_BATTERY_CHECK;
            break;
        }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_MEDIUMHIGH);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_LSE|RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1|RCC_PERIPHCLK_LPUART1
                              |RCC_PERIPHCLK_I2C1|RCC_PERIPHCLK_RTC
                              |RCC_PERIPHCLK_USB;
  PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
  PeriphClkInit.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_PCLK1;
  PeriphClkInit.I2c1ClockSelection = RCC_I2C1CLKSOURCE_PCLK1;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
  PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// Provide a print interface for print_functions.
void printString(const char *str, uint16_t length)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)str, length, 0xFFFF);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1)
    {
        NVIC_SystemReset();
    }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
