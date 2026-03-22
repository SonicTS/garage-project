#pragma once

#include "esp_system.h"
#include "i2c_master.h"
#include "debug_led.h"
#include "log.h"

// If the framework's i2c master header hardcodes different pins, allow
// overriding here. Default to common ESP8266 pins 4(SDA) and 5(SCL) if
// the framework did not define them correctly.

#define I2C_SDA_GPIO 4
#define I2C_SCL_GPIO 5

#define MCP23017_ALL_PINS_OUT 0x0000
#define MCP23017_ALL_PINS_ON  0x0000
#define MCP23017_ALL_PINS_OFF 0xFFFF

#define WRITE_BIT                           0 /*!< I2C master write */
#define READ_BIT                            1  /*!< I2C master read */
#define ACK_CHECK_EN                        0x1              /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS                       0x0              /*!< I2C master will not check ack from slave */
#define ACK_VAL                             0x0              /*!< I2C ack value */
#define NACK_VAL                            0x1              /*!< I2C nack value */
#define LAST_NACK_VAL                       0x2              /*!< I2C last_nack value */

#define MCP23017_IODIRA 0x00 //Direction of data I/O (bits set as: 1 = input, 0 = output)
#define MCP23017_IODIRB 0x01 //Direction of data I/O (bits set as: 1 = input, 0 = output)
#define MCP23017_GPINTENA 0x04 //Interrupt on change
#define MCP23017_GPINTENB 0x05 //Interrupt on change
#define MCP23017_INTCONA 0x08 //Interrupt on change control register
#define MCP23017_INTCONB 0x09 //Interrupt on change control register
#define MCP23017_GPPUA 0x0C //PullUp set internal pull up for input pins
#define MCP23017_GPPUB 0x0D //PullUp set internal pull up for input pins
#define MCP23017_GPIOA 0x12 //Port Register - write modifies latch
#define MCP23017_GPIOB 0x13 //Port Register - write modifies latch

#define MCP23017_IOCONA 0x0A //IO Configuration - BANK/MIRROR/SLEW/INTPOL
#define MCP23017_IOCONB 0x0B //IO Configuration - BANK/MIRROR/SLEW/INTPOL
#define MCP23017_INTFA 0x0E //Interrupt Flag
#define MCP23017_INTFB 0x0F //Interrupt Flag
#define MCP23017_INTCAPA 0x10 //Interrupt Capture
#define MCP23017_INTCAPB 0x11 //Interrupt Capture
#define MCP23017_IOCON_BANK_BIT 7
#define MCP23017_IOCON_MIRROR_BIT 6
#define MCP23017_IOCON_SEQOP_BIT 5
#define MCP23017_IOCON_DISSLW_BIT 4
#define MCP23017_IOCON_HAEN_BIT 3
#define MCP23017_IOCON_ODR_BIT 2
#define MCP23017_IOCON_INTPOL_BIT 1

static inline void set_bit(int *value, int bit, bool set) {
	if (!value) return;
	if (bit >= 0 && bit <= 15) {
		if (set) {
			*value |= (1 << bit);
		} else {
			*value &= ~(1 << bit);
		}
	}
}

static inline bool is_bit_set(int value, int bit) {
	if (bit >= 0 && bit <= 15) {
		return (bool) (0x1 & (value >> bit));
	}
	return false;
}

/**
* Configure with a IOCON (I/O Expander configuration register)
*
* @param mirroring true if you want the INT pins to be internally connected, allows you to save IO lines needed for detecting interrupts
* @param polarity the polarity of the interrupt, true = active-high, false = active-low
* @return PICO_ERROR_NONE or PICO_ERROR_GENERIC
*/
int mcp23017_setup(bool mirroring, bool polarity);

int setup_bank_configuration(int reg, bool mirroring, bool polarity);

/**
* Stores and returns the last input state in the class for later interrogation with
* get_last_input_pin_value or get_last_input_pin_values
* @return the pin values or PICO_ERROR_NONE or PICO_ERROR_GENERIC
*/
int update_and_get_input_values();


/**
* Sets an individual pin's within the internal state, this must be flushed to take effect
* @param pin the pin 0-15
* @param set true = on, false = off
*/
void set_output_bit_for_pin(int pin, bool set);

/**
* Flushes the internal output state to the device
* @return PICO_ERROR_NONE or PICO_ERROR_GENERIC
*/
int flush_output();

int write_register(uint8_t reg, uint8_t value);

int write_dual_registers(uint8_t reg, int value);

/**
 * Sets the IO direction for each pin
 * @param direction '1' bits input, '0' bits output
 * @return PICO_ERROR_NONE or PICO_ERROR_GENERIC
 */
int set_io_direction(int direction);

int set_all_output_bits(int all_bits);

int mcp23017_init();


// Shared state variables (defined in the C file)
extern int output;
extern int last_input;

// inline const int BASE_I2C_ADDR_MCP23017 = 0b0100;
// inline const int A0 = 0;
// inline const int A1 = 0;
// inline const int A2 = 0;
