#include "mcp23017.h"
#include "esp8266/ets_sys.h"
#include "gpio.h"

/* Definitions for shared state declared in header */
int output = 0;
int last_input = 0;
// MCP23017 7-bit base address is 0x20 (0b0100 000). If A2/A1/A0 are
// tied low on the board, the device address is 0x20. Adjust here if your
// address pins are different.
const uint8_t address = 0x20;


int mcp23017_setup(bool mirroring, bool polarity) {
    int result;
	result = setup_bank_configuration(MCP23017_IOCONA, mirroring, polarity);
	if (result != 0){
		garage_control_blink_debug_led(GARAGE_LED_FAST, 5);
		return result;
	}
	result = setup_bank_configuration(MCP23017_IOCONB, mirroring, polarity);
	if (result != 0){
		garage_control_blink_debug_led(GARAGE_LED_FAST, 5);
		return result;
	}
	garage_control_blink_debug_led(GARAGE_LED_SLOW, 5);
	return result;
}

int setup_bank_configuration(int reg, bool mirroring, bool polarity) {
	int ioConValue = 0;
	set_bit(&ioConValue, MCP23017_IOCON_BANK_BIT, false);
	set_bit(&ioConValue, MCP23017_IOCON_MIRROR_BIT, mirroring);
	set_bit(&ioConValue, MCP23017_IOCON_SEQOP_BIT, false);
	set_bit(&ioConValue, MCP23017_IOCON_DISSLW_BIT, false);
	set_bit(&ioConValue, MCP23017_IOCON_HAEN_BIT, false);
	set_bit(&ioConValue, MCP23017_IOCON_ODR_BIT, false);
	set_bit(&ioConValue, MCP23017_IOCON_INTPOL_BIT, polarity);
	return write_register(reg, ioConValue);
}

int update_and_get_input_values(){
	int result = 0; // TODO: implement read_dual_registers(MCP23017_GPIOA) to read both ports
	if (result >= 0) {
		last_input = result;
		return 0;
	}
	return -1;
}

void set_output_bit_for_pin(int pin, bool set){
	set_bit(&output, pin, set);
}

int flush_output(){
    return write_dual_registers(MCP23017_GPIOA, output); //includes MCP23017_GPIOB
}

int write_dual_registers(uint8_t reg, int value) {
	uint8_t low_byte = (uint8_t)(value & 0xff);
	uint8_t high_byte = (uint8_t)((value >> 8) & 0xff);

	// Use bit-banged master API provided by the framework
	i2c_master_start();
	i2c_master_writeByte((uint8_t)((address << 1) | WRITE_BIT));
	if (!i2c_master_checkAck()) { i2c_master_stop(); return -1; }
	i2c_master_writeByte(reg);
	if (!i2c_master_checkAck()) { i2c_master_stop(); return -1; }
	i2c_master_writeByte(low_byte);
	if (!i2c_master_checkAck()) { i2c_master_stop(); return -1; }
	i2c_master_writeByte(high_byte);
	if (!i2c_master_checkAck()) { i2c_master_stop(); return -1; }
	i2c_master_stop();
	return 0;
}

int write_register(uint8_t reg, uint8_t value) {
	i2c_master_start();
	i2c_master_writeByte((uint8_t)((address << 1) | WRITE_BIT));
	if (!i2c_master_checkAck()) { 
		i2c_master_stop(); 
		LOGF("mcp23017: write_register failed to address device\n");
		return -1; 
	}
	i2c_master_writeByte(reg);
	if (!i2c_master_checkAck()) { 
		i2c_master_stop(); 
		LOGF("mcp23017: write_register failed to write register\n");
		return -1; 
	}
	i2c_master_writeByte(value);
	if (!i2c_master_checkAck()) { 
		i2c_master_stop();
		LOGF("mcp23017: write_register failed to write value\n");
		return -1; 
	}
	i2c_master_stop();
	return 0;
}

void scan_i2c_bus() {
	LOGF("Scanning I2C bus for devices...\n");
	for (uint8_t addr = 1; addr < 127; ++addr) {
		i2c_master_start();
		i2c_master_writeByte((addr << 1) | WRITE_BIT);
		if (i2c_master_checkAck()) {
			LOGF("Found device at address 0x%02X\n", addr);
		}
		i2c_master_stop();
	}
}
int set_io_direction(int direction) {
	return write_dual_registers(MCP23017_IODIRA, direction); //inc MCP23017_IODIRB
}

int set_all_output_bits(int all_bits){
	output = all_bits;
	return write_dual_registers(MCP23017_GPIOA, all_bits); //inc MCP23017_GPIOB
}

uint8 m_nLastSDA;
uint8 m_nLastSCL;

void i2c_master_setDC(uint8 SDA, uint8 SCL)
{
    SDA	&= 0x01;
    SCL	&= 0x01;
    m_nLastSDA = SDA;
    m_nLastSCL = SCL;
    ETS_INTR_LOCK();
    if ((0 == SDA) && (0 == SCL)) {
        I2C_MASTER_SDA_LOW_SCL_LOW();
    } else if ((0 == SDA) && (1 == SCL)) {
        I2C_MASTER_SDA_LOW_SCL_HIGH();
    } else if ((1 == SDA) && (0 == SCL)) {
        I2C_MASTER_SDA_HIGH_SCL_LOW();
    } else {
        I2C_MASTER_SDA_HIGH_SCL_HIGH();
    }
    ETS_INTR_UNLOCK();
}

int mcp23017_init() {
	// Initialize the framework's bit-banged i2c master (GPIO-controlled)
	ETS_INTR_LOCK();

    PIN_FUNC_SELECT(I2C_MASTER_SDA_MUX, I2C_MASTER_SDA_FUNC);
    PIN_FUNC_SELECT(I2C_MASTER_SCL_MUX, I2C_MASTER_SCL_FUNC);

    GPIO_REG_WRITE(GPIO_PIN_ADDR(GPIO_ID_PIN(I2C_SDA_GPIO)), GPIO_REG_READ(GPIO_PIN_ADDR(GPIO_ID_PIN(I2C_SDA_GPIO))) | GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_ENABLE)); //open drain;
    GPIO_REG_WRITE(GPIO_ENABLE_ADDRESS, GPIO_REG_READ(GPIO_ENABLE_ADDRESS) | (1 << I2C_SDA_GPIO));
    GPIO_REG_WRITE(GPIO_PIN_ADDR(GPIO_ID_PIN(I2C_SCL_GPIO)), GPIO_REG_READ(GPIO_PIN_ADDR(GPIO_ID_PIN(I2C_SCL_GPIO))) | GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_ENABLE)); //open drain;
    GPIO_REG_WRITE(GPIO_ENABLE_ADDRESS, GPIO_REG_READ(GPIO_ENABLE_ADDRESS) | (1 << I2C_SCL_GPIO));

    I2C_MASTER_SDA_HIGH_SCL_HIGH();

	os_delay_us(1000);

	printf("GPIO5=%d\n", GPIO_INPUT_GET(5));

//    ETS_GPIO_INTR_ENABLE() ;
    ETS_INTR_UNLOCK();
	uint8 i;

    i2c_master_setDC(1, 0);
    i2c_master_wait(5);

    // when SCL = 0, toggle SDA to clear up
    i2c_master_setDC(0, 0) ;
    i2c_master_wait(5);
    i2c_master_setDC(1, 0) ;
    i2c_master_wait(5);

    // set data_cnt to max value
    for (i = 0; i < 28; i++) {
        i2c_master_setDC(1, 0);
        i2c_master_wait(5);	// sda 1, scl 0
        i2c_master_setDC(1, 1);
        i2c_master_wait(5);	// sda 1, scl 1
    }

    // reset all
    i2c_master_stop();
	scan_i2c_bus(); // Optional: scan bus to verify device presence
	return 0;
}