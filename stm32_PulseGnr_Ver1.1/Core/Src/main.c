#include "stm32f4xx_hal.h"
#include "stm32f4xx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_CHANNELS 4                  // Maximum pulse channels
#define CMD_BUFFER_SIZE 128            // UART command buffer size
#define TIM_CLK_MHZ 80                 // Timer clock frequency in MHz
#define FACTOR 10                      // Timer resolution multiplier
#define MISSED_TRIGGER_INTERVAL_MS 1   // 1s delay before firing

char uart_buffer[CMD_BUFFER_SIZE];     // Buffer for incoming UART commands
volatile uint8_t uart_ready = 0;       // Flag set when UART command is ready
uint32_t width[MAX_CHANNELS] = {100, 100, 100, 100}; // Pulse widths per channel (us)
uint32_t delay[MAX_CHANNELS] = {1, 1, 1, 1};         // Delays per channel (us)
uint32_t phase_delay_us = 1000;        // Delay between full pulse cycles
volatile uint32_t max_total_period = 0;// Longest delay + width time
volatile uint8_t run_sequence = 0;     // Flag to indicate sequence should run
volatile uint8_t config_updated = 0;   // Flag to reconfigure timer setup
volatile uint8_t trigger_updated = 0;  // Flag to indicate trigger has been armed
volatile uint8_t is_busy = 0;          // Flag to indicate system is executing pulses
uint32_t last_missed_trigger_time = 0; // Flag for trigger time


UART_HandleTypeDef huart2;             // UART2 handle
uint8_t uart_rx_byte;                  // Most recent received UART byte
volatile uint16_t uart_rx_index = 0;   // Index into UART buffer

TIM_HandleTypeDef htim4;

// Function prototypes
void SystemClock_Config(void);
void GPIO_Init(void);
void TIM_Init(void);
void TIM_Master_Init(TIM_TypeDef *TIMx, uint32_t delay_us, uint32_t width_us, uint8_t channel);
void Start_Pulse_Sequence(void);
void Wait_For_Pulse_Completion(uint32_t duration_us);
void Process_Command(char *cmd);
void Parse_Pulse_Parameters(char* buffer);
uint32_t Convert_To_Microseconds(uint32_t value, char unit);
void Send_Response(const char* response);
void MX_USART2_UART_Init(void);
uint32_t max_pulse_period(uint32_t *delay, uint32_t *width, int len);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);
void EXTI0_IRQHandler(void);
void DWT_Init(void);
void DWT_Delay_us(uint32_t us);
void USART2_IRQHandler(void) {
    HAL_UART_IRQHandler(&huart2);
}

int main(void)
{
    // Basic STM32 HAL initialization
    __enable_irq();
    HAL_Init();
    SystemClock_Config();

    // Peripheral initialization
    GPIO_Init();
    MX_USART2_UART_Init();
    TIM_Init();
    DWT_Init();  // Enable cycle counter-based microsecond delay

    // Start UART interrupt-based receiving
    HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1);
    Send_Response("SYSTEM_READY");

    // Determine initial max pulse duration
    max_total_period = max_pulse_period(delay, width, MAX_CHANNELS);

    while (1) {
        if (uart_ready) {
            Process_Command(uart_buffer);
            uart_ready = 0;
            HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1);
            max_total_period = max_pulse_period(delay, width, MAX_CHANNELS);
        }

        // Reconfigure timers if config updated
        if (config_updated) {
            TIM_Init();
            config_updated = 0;
        }

        // Run pulse sequence if allowed and no external trigger override
        if (run_sequence && !trigger_updated) {
            Start_Pulse_Sequence();
            Wait_For_Pulse_Completion(max_total_period);
            DWT_Delay_us(phase_delay_us);
            is_busy = 0;
        }
    }
}

// UART receive interrupt handler — assembles line commands
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART2) {
        if (uart_rx_byte == '\n' || uart_rx_byte == '\r') {
            if (uart_rx_index > 0) {
                uart_buffer[uart_rx_index] = '\0';
                uart_ready = 1;
                uart_rx_index = 0;
            }
        } else if (uart_rx_index < CMD_BUFFER_SIZE - 1) {
            uart_buffer[uart_rx_index++] = uart_rx_byte;
        }
        HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1);
    }
}

// Sends a string message over UART (with newline)
void Send_Response(const char *msg) {
    char buffer[CMD_BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s\r\n", msg);
    HAL_UART_Transmit(&huart2, (uint8_t *)buffer, strlen(buffer), HAL_MAX_DELAY);
}

// Compute maximum delay+width combination across all channels
uint32_t max_pulse_period(uint32_t *delay, uint32_t *width, int len) {
    uint32_t max_val = 0;
    for (int i = 0; i < len; i++) {
        uint32_t total = delay[i] + width[i];
        if (total > max_val) {
            max_val = total;
        }
    }
    return max_val;
}

// Starts pulse generation on all four timers
void Start_Pulse_Sequence(void) {
    // Reset timers
    TIM1->CNT = TIM2->CNT = TIM3->CNT = TIM5->CNT = 0;

    // Set one-pulse mode
    TIM1->CR1 = (TIM1->CR1 & ~TIM_CR1_OPM) | TIM_CR1_OPM;
    TIM2->CR1 = (TIM2->CR1 & ~TIM_CR1_OPM) | TIM_CR1_OPM;
    TIM3->CR1 = (TIM3->CR1 & ~TIM_CR1_OPM) | TIM_CR1_OPM;
    TIM5->CR1 = (TIM5->CR1 & ~TIM_CR1_OPM) | TIM_CR1_OPM;

    // Force register update
    TIM1->EGR = TIM2->EGR = TIM3->EGR = TIM5->EGR = TIM_EGR_UG;

    // Start timers
    TIM1->CR1 |= TIM_CR1_CEN;
    TIM2->CR1 |= TIM_CR1_CEN;
    TIM3->CR1 |= TIM_CR1_CEN;
    TIM5->CR1 |= TIM_CR1_CEN;

    is_busy = 1;
}

// Simple busy-wait delay for pulse duration
void Wait_For_Pulse_Completion(uint32_t duration_us) {
    DWT_Delay_us(duration_us);
}

// Configure all timers and their output channels
void TIM_Init(void) {
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM5_CLK_ENABLE();

    TIM_Master_Init(TIM1, delay[0], width[0], 1);
    TIM_Master_Init(TIM2, delay[1], width[1], 1);
    TIM_Master_Init(TIM3, delay[2], width[2], 1);
    TIM_Master_Init(TIM5, delay[3], width[3], 2);
}

// Individual timer channel setup
void TIM_Master_Init(TIM_TypeDef *TIMx, uint32_t delay_us, uint32_t width_us, uint8_t channel) {
    // Convert delay and width to timer ticks
    uint32_t delay_ticks = delay_us * FACTOR;
    uint32_t width_ticks = width_us * FACTOR;
    uint32_t total_period = delay_ticks + width_ticks;

    // Timer prescaler: Scale timer clock down to match desired tick resolution
    uint16_t prescaler = (TIM_CLK_MHZ / FACTOR) - 1;

    // Avoid zero delay, which may cause undefined behavior
    if (delay_ticks == 0) delay_ticks = 1;

    // Configure prescaler and auto-reload register
    TIMx->PSC = prescaler;                 // Set timer prescaler
    TIMx->ARR = total_period - 1;         // Set the auto-reload value (period - 1)

    // Timer mode configuration: enable preload and one-pulse mode
    TIMx->CR1 = TIM_CR1_ARPE | TIM_CR1_OPM;

    // Configure PWM output for the specified channel
    switch (channel) {
        case 1:
            // Clear OC1M bits (Output Compare Mode for channel 1)
            TIMx->CCMR1 &= ~TIM_CCMR1_OC1M;

            // Set PWM Mode 2 (OCx stays high after match), enable preload
            TIMx->CCMR1 |= (7 << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;

            // Set the pulse start time (rising edge at delay_ticks)
            TIMx->CCR1 = delay_ticks;

            // Enable channel 1 output
            TIMx->CCER |= TIM_CCER_CC1E;

            // Set output polarity to active high
            TIMx->CCER &= ~TIM_CCER_CC1P;
            break;

        case 2:
            // Clear OC2M bits (Output Compare Mode for channel 2)
            TIMx->CCMR1 &= ~TIM_CCMR1_OC2M;

            // Set PWM Mode 2, enable preload
            TIMx->CCMR1 |= (7 << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;

            // Set the pulse start time for channel 2
            TIMx->CCR2 = delay_ticks;

            // Enable channel 2 output
            TIMx->CCER |= TIM_CCER_CC2E;

            // Set output polarity to active high
            TIMx->CCER &= ~TIM_CCER_CC2P;
            break;
    }

    // Enable main output for advanced-control timers (e.g., TIM1, TIM8)
    if (TIMx == TIM1 || TIMx == TIM8)
        TIMx->BDTR |= TIM_BDTR_MOE;
    TIMx->EGR = TIM_EGR_UG;     // Force update event to load all registers
    TIMx->CNT = 0;     // Reset counter to start from 0 when enabled
}


// DWT-based microsecond delay
void DWT_Delay_us(uint32_t us) {
    uint32_t cycles = us * (SystemCoreClock / 1000000U);
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles);
}

// Parse incoming command string and act
void Process_Command(char *cmd) {
    char debug_msg[CMD_BUFFER_SIZE + 20];
    snprintf(debug_msg, sizeof(debug_msg), "CMD: %s", cmd);
    Send_Response(debug_msg);

    if (strstr(cmd, "START")) {
        run_sequence = 1;
        trigger_updated = 0;
    } else if (strstr(cmd, "STOP")) {
        run_sequence = 0;
        is_busy = 0;
    } else if (strstr(cmd, "SET:")) {
        Parse_Pulse_Parameters(cmd + 4);
        config_updated = 1;
    } else if (strstr(cmd, "ARM")) {
        trigger_updated = 1;
        run_sequence = 1;
    } else {
        Send_Response("UNKNOWN_CMD");
    }
}

// Parse pulse widths, delays, and phase delay from SET command
void Parse_Pulse_Parameters(char* buffer) {
    char *token;
    char param[8];
    uint32_t value;
    char unit;
    int ch_index;

    for (int i = 1; i <= MAX_CHANNELS; i++) {
        snprintf(param, sizeof(param), "W%d=", i);
        token = strstr(buffer, param);
        if (token && sscanf(token, "W%d=%lu%c", &ch_index, &value, &unit) >= 2) {
            if (ch_index >= 1 && ch_index <= MAX_CHANNELS)
                width[ch_index - 1] = Convert_To_Microseconds(value, unit);
        }

        snprintf(param, sizeof(param), "D%d=", i);
        token = strstr(buffer, param);
        if (token && sscanf(token, "D%d=%lu%c", &ch_index, &value, &unit) >= 2) {
            if (ch_index >= 1 && ch_index <= MAX_CHANNELS)
                delay[ch_index - 1] = Convert_To_Microseconds(value, unit);
        }
    }

    token = strstr(buffer, "PHASE=");
    if (token && sscanf(token, "PHASE=%lu%c", &value, &unit) >= 1)
        phase_delay_us = Convert_To_Microseconds(value, unit);
}

// Convert unit suffix to microseconds
uint32_t Convert_To_Microseconds(uint32_t value, char unit) {
    switch (unit) {
        case 's': return value * 1000000U;
        case 'm': return value * 1000U;
        default:  return value;
    }
}

// UART setup for 115200 baud
void MX_USART2_UART_Init(void) {
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK)
        while (1);
    HAL_NVIC_SetPriority(USART2_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

// GPIO Initialization
void GPIO_Init(void) {
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

    //TIM5_CH2 - PA1
    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Alternate = GPIO_AF2_TIM5;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // TIM2_CH1 - PA5
    GPIO_InitStruct.Pin = GPIO_PIN_5;
    GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // TIM3_CH1 - PA6
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // TIM1_CH1 - PE9
    GPIO_InitStruct.Pin = GPIO_PIN_9;
    GPIO_InitStruct.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    // EXTI input on PA0 (External Trigger Input)
    GPIO_InitStruct.Pin = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Alternate = 0; // Clear alternate function
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);


    // Configure EXTI interrupt
    HAL_NVIC_SetPriority(EXTI0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

}


void EXTI0_IRQHandler(void) {
    if (__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_0) != RESET) {
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_0);

        if (!run_sequence || !trigger_updated) {
            Send_Response("NOT_ARMED");
            HAL_Delay(100);
            return;
        }
        if (is_busy) {
            uint32_t now = HAL_GetTick(); // Current system time in ms
            if ((now - last_missed_trigger_time) >= MISSED_TRIGGER_INTERVAL_MS)
            {
                Send_Response("MISSED_TRIGGER");
                last_missed_trigger_time = now;
            }
            return;
        }

        Start_Pulse_Sequence();
        Wait_For_Pulse_Completion(max_total_period);
        is_busy = 0;
    }
}

void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // Enable DWT
    DWT->CYCCNT = 0;                                // Reset cycle counter
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;            // Start cycle counter
}



// System Clock Configuration
void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = 8;
    RCC_OscInitStruct.PLL.PLLN = 80;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 7;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                                |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);
}
