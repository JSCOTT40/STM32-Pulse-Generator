# STM32F4 Four-Channel Pulse Generator — Design Document


## 1. Purpose

This document describes the design of an STM32F4-based four-channel pulse generator. The firmware generates precisely timed output pulses using hardware timers, supports UART-based configuration, and can operate using either internal software triggering or an external hardware trigger.

The system is designed for applications where multiple pulse channels must be generated with configurable delay, pulse width, and phase timing.

<img width="1080" height="1876" alt="image" src="https://github.com/user-attachments/assets/df9872d8-6961-40d9-adc2-ce1fbad781e8" />

---

## 2. System Overview

On startup, the firmware:

1. Configures the STM32F4 system clock to 80 MHz.
2. Initializes GPIO pins for timer outputs, UART communication, and external triggering.
3. Configures four hardware timers in one-pulse PWM mode.
4. Enables USART2 for command input and status responses.
5. Enables the Cortex-M4 DWT cycle counter for accurate microsecond delays.

The firmware accepts ASCII commands over UART to configure:

* Pulse width
* Pulse delay
* Inter-cycle phase delay
* Internal start/stop mode
* External trigger arming

Each output channel is driven by a separate timer. Timers are configured in one-pulse mode, meaning each timer produces exactly one pulse per trigger event.

---

## 3. Hardware Resources

### 3.1 Main Components

The hardware system consists of:

* STM32F4 microcontroller
* CP2102 USB-to-TTL UART converter
* Aluminum enclosure
* BNC connectors
* Timer output wiring
* External trigger input wiring

USART2 is used for communication between the front-end software and the STM32 firmware.

---

## 4. GPIO Pin Mapping

| Pin | Peripheral / Signal | Firmware Use             |
| --- | ------------------- | ------------------------ |
| PA0 | EXTI0               | External trigger input   |
| PA1 | TIM5_CH2            | Pulse output channel 4   |
| PA2 | USART2_TX           | UART transmit to CP2102  |
| PA3 | USART2_RX           | UART receive from CP2102 |
| PA5 | TIM2_CH1            | Pulse output channel 2   |
| PA6 | TIM3_CH1            | Pulse output channel 3   |
| PE9 | TIM1_CH1            | Pulse output channel 1   |

---

## 5. Timer Selection

Four independent timer channels are used:

| Channel | Timer | Timer Channel | GPIO Pin | Notes                                               |
| ------- | ----- | ------------- | -------- | --------------------------------------------------- |
| CH1     | TIM1  | CH1           | PE9      | Advanced-control timer; requires main output enable |
| CH2     | TIM2  | CH1           | PA5      | 32-bit general-purpose timer                        |
| CH3     | TIM3  | CH1           | PA6      | 16-bit general-purpose timer                        |
| CH4     | TIM5  | CH2           | PA1      | 32-bit timer; CH2 used due to pin availability      |

TIM1, TIM2, TIM3, and TIM5 were selected because they provide suitable hardware PWM output capability while fitting the available STM32F4 pin mappings.

---

## 6. Timing Design

### 6.1 Timer Clock

The firmware configures the system clock to 80 MHz.

APB1 is prescaled to 40 MHz, but STM32 timers on APB1 receive a doubled timer clock when the APB prescaler is not 1. Therefore, the APB1 timers still run from an 80 MHz timer clock.

TIM1 on APB2 also runs at 80 MHz.

### 6.2 Timer Resolution

The firmware uses a timer resolution of:

```text
10 timer ticks = 1 microsecond
```

This gives a timer frequency of:

```text
10 MHz
```

With an 80 MHz timer clock, the prescaler is calculated as:

```text
PSC = (TIM_CLK_MHZ / FACTOR) - 1
PSC = (80 / 10) - 1
PSC = 7
```

Since STM32 timer prescalers are zero-based, a prescaler value of `7` divides the timer clock by `8`.

Result:

```text
80 MHz / 8 = 10 MHz
```

Therefore:

```text
1 timer tick = 0.1 microseconds
10 timer ticks = 1 microsecond
```

This makes pulse timing calculations simple and gives adequate microsecond-level precision.

---

## 7. Pulse Generation Method

The firmware uses:

* One-pulse mode
* PWM Mode 2
* Capture/compare register for pulse delay
* Auto-reload register for pulse end time

### 7.1 Pulse Timing

Each pulse is defined by two values:

```text
delay_us
width_us
```

These are converted into timer ticks:

```text
delay_ticks = delay_us * 10
width_ticks = width_us * 10
```

The timer registers are configured as:

```text
CCR = delay_ticks
ARR = delay_ticks + width_ticks
```

### 7.2 PWM Mode 2 Behavior

PWM Mode 2 is used so that the output becomes active when the counter reaches the compare value, then stays active until the counter reaches the auto-reload value.

This allows the pulse to start after a configurable delay without requiring extra software timing logic.

### 7.3 Why One-Pulse Mode?

One-pulse mode automatically stops the timer after one pulse cycle. This avoids needing software to manually stop the timer after each pulse and improves timing consistency.

---

## 8. UART Command Interface

The firmware receives commands through USART2 using interrupt-driven UART reception.

UART configuration:

| Setting      | Value  |
| ------------ | ------ |
| Baud rate    | 115200 |
| Data bits    | 8      |
| Stop bits    | 1      |
| Parity       | None   |
| Flow control | None   |
| Oversampling | 16     |

The firmware supports simple ASCII commands such as:

```text
START
STOP
ARM
SET ...
```

### 8.1 Command Modes

| Command | Behavior                                     |
| ------- | -------------------------------------------- |
| START   | Begins internally triggered pulse generation |
| STOP    | Stops pulse generation                       |
| ARM     | Arms the firmware for an external trigger    |
| SET     | Updates timing parameters                    |

The parser uses simple substring matching to extract parameters. This keeps the firmware simple, though malformed commands could potentially be matched unintentionally.

---

## 9. Interrupt Model

The firmware uses two main interrupt sources:

| Interrupt        | Purpose                               |
| ---------------- | ------------------------------------- |
| USART2 interrupt | Receives UART command bytes           |
| EXTI0 interrupt  | Handles external trigger input on PA0 |

### 9.1 UART Receive Interrupt

UART reception is handled one byte at a time using:

```c
HAL_UART_Receive_IT(...)
```

When a byte is received, `HAL_UART_RxCpltCallback` is called. Incoming characters are stored in a command buffer until a newline or carriage return is received.

Once a complete command is received, the firmware marks it as ready for processing in the main loop.

### 9.2 External Trigger Interrupt

The external trigger is connected to PA0 using EXTI0. When triggered, the EXTI interrupt path starts the pulse sequence if the system is armed and not already busy.

---

## 10. Firmware State Flags

The firmware uses volatile flags to safely communicate between interrupt context and the main loop.

| Flag              | Purpose                                         |
| ----------------- | ----------------------------------------------- |
| `run_sequence`    | Indicates whether pulse generation should run   |
| `config_updated`  | Indicates that timing configuration has changed |
| `trigger_updated` | Indicates that external trigger mode is active  |
| `is_busy`         | Prevents overlapping pulse sequences            |

Timing arrays such as `width[]` and `delay[]` are updated in the main context to reduce race-condition risk.

---

## 11. Main Firmware Flow

The `main` function performs all system initialization and then enters the main control loop.

Startup sequence:

1. Enable HAL and interrupts.
2. Configure the system clock.
3. Initialize GPIO pins.
4. Initialize USART2.
5. Initialize timers.
6. Enable the DWT cycle counter.
7. Start UART receive interrupt.
8. Send a `SYSTEM_READY` response.

After initialization, the main loop waits for received UART commands and updates system behavior accordingly.

---

## 12. Important Functions

### `SystemClock_Config`

Configures the STM32F4 system clock to 80 MHz using the HSI oscillator and PLL.

It also sets the bus prescalers so that timer peripherals receive the expected 80 MHz timer clock.

---

### `GPIO_Init`

Enables GPIO ports and configures:

* Timer output pins
* USART pins
* External trigger input on PA0
* EXTI interrupt settings
* NVIC priority for the external trigger interrupt

---

### `MX_USART2_UART_Init`

Initializes USART2 for communication with the front-end interface.

USART2 is configured for 115200 baud, 8N1 serial communication with interrupt-driven receive enabled.

---

### `HAL_UART_RxCpltCallback`

Handles incoming UART bytes.

The callback appends received characters to a buffer until a newline or carriage return is received. Once a full command is available, the main loop processes it.

---

### Timer Initialization Functions

Each timer initialization function configures one timer for pulse output.

The common setup includes:

* Prescaler for 10 MHz timer frequency
* PWM Mode 2
* One-pulse mode
* Output compare configuration
* GPIO alternate function mapping

TIM1 also requires enabling its main output through the BDTR register because it is an advanced-control timer.

---

### `Send_Response`

Sends response strings back over UART using:

```c
HAL_UART_Transmit(...)
```

The firmware uses blocking transmit with `HAL_MAX_DELAY`. This guarantees the response is sent before execution continues, but it can block the CPU while transmitting.

---

## 13. Design Trade-Offs

### 13.1 Timer-Based Pulse Generation

Using hardware timers improves timing accuracy compared to software-only pulse generation.

**Advantages**

* Accurate timing
* Low software overhead
* Clean pulse generation
* No need for manual GPIO toggling

**Trade-offs**

* Timer and pin mappings are less flexible
* Some timers are only 16-bit
* TIM1 requires additional advanced-timer configuration

---

### 13.2 DWT Microsecond Delays

The DWT cycle counter provides precise blocking delays.

**Advantages**

* Very accurate
* Simple implementation
* Useful for short timing gaps

**Trade-offs**

* Blocks the CPU
* Not power efficient
* Depends on a correct `SystemCoreClock` value

---

### 13.3 Text-Based UART Protocol

A simple ASCII protocol makes the firmware easy to test from a serial terminal or front-end application.

**Advantages**

* Human-readable
* Easy to debug
* Simple to implement

**Trade-offs**

* More parsing logic than binary commands
* Substring matching can accept malformed commands
* Blocking UART responses can delay other work

---

## 14. Limitations

Current limitations include:

* UART responses use blocking transmit calls.
* Command parsing is simple and not fully strict.
* DWT delays block the CPU.
* Timer resolution assumes an 80 MHz timer clock.
* 16-bit timers limit the maximum pulse duration compared to 32-bit timers.
* External trigger handling should avoid doing too much work directly inside interrupt context.

---

## 15. Possible Improvements

Future improvements could include:

* A stricter command parser.
* Non-blocking UART transmit using DMA or interrupt-based TX.
* Moving more EXTI work out of the interrupt handler.
* Adding command validation and error codes.
* Supporting configurable timer resolution.
* Adding EEPROM or flash storage for saved pulse configurations.
* Adding a checksum or framed protocol for more reliable communication.
* Adding a formal front-end command schema.

---

## 16. Summary

This firmware implements a four-channel STM32F4 pulse generator using hardware timers, UART control, and optional external triggering.

The main design idea is to offload pulse timing to STM32 hardware timers. Each timer runs at 10 MHz, giving 0.1 microsecond tick resolution and simple microsecond timing calculations. One-pulse mode and PWM Mode 2 are used so each timer can produce a delayed pulse with minimal software overhead.

The design favors simplicity, accuracy, and ease of debugging while leaving room for future improvements in command parsing, interrupt handling, and non-blocking communication.
