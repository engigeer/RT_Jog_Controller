/**
 * Example program for basic use of pico as an I2C peripheral (previously known as I2C slave)
 * 
 * This example allows the pico to act as a 256byte RAM
 * 
 * Author: Graham Smith (graham@smithg.co.uk)
 */


// Usage:
//
// When writing data to the pico the first data byte updates the current address to be used when writing or reading from the RAM
// Subsequent data bytes contain data that is written to the ram at the current address and following locations (current address auto increments)
//
// When reading data from the pico the first data byte returned will be from the ram storage located at current address
// Subsequent bytes will be returned from the following ram locations (again current address auto increments)
//
// N.B. if the current address reaches 255, it will autoincrement to 0 after next read / write

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"

#include "pico/stdio.h"
#include "pico/time.h"
#include <tusb.h>

#include <i2c_fifo.h>
#include <i2c_slave.h>

#include <math.h>

#include "i2c_jogger.h"


#define SHOWRAM 1
#define TWOWAY 1


#define TICK_TIMER_PERIOD 10
#define ROLLOVER_DELAY_PERIOD 7


// define I2C addresses to be used for this peripheral
static const uint I2C_SLAVE_ADDRESS = 0x49;
static const uint I2C_BAUDRATE = 100000; // 100 kHz

// GPIO pins to use for I2C SLAVE
static const uint I2C_SLAVE_SDA_PIN = 12;
static const uint I2C_SLAVE_SCL_PIN = 13;

// RPI Pico

int led_update_counter = 0;
int status_update_counter = 0;
int onboard_led_count = 0;

int command_error = 0;

// ram_addr is the current address to be used when writing / reading the RAM
// N.B. the address auto increments, as stored in 8 bit value it automatically rolls round when reaches 255

static struct
{
    uint8_t mem[256];
    uint8_t mem_address;
    bool mem_address_written;
} context;

char buf[8];

uint8_t ram_addr = 0;
uint8_t status_addr = 0;
uint8_t key_pressed = 0;
uint8_t key_character = '\0';

uint8_t jog_toggle_pressed = 0;
uint8_t reset_pressed = 0;
uint8_t unlock_pressed = 0;

uint8_t keysent = 0;

machine_status_packet_t *packet = (machine_status_packet_t*) context.mem;
machine_status_packet_t prev_packet;
machine_status_packet_t *previous_packet = &prev_packet;

char *ram_ptr = (char*) &context.mem[0];
int character_sent;

// Our handler is called from the I2C ISR, so it must complete quickly. Blocking calls /
// printing to stdio may interfere with interrupt handling.
static void i2c_slave_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
    case I2C_SLAVE_RECEIVE: // master has written some data
        if (!context.mem_address_written) {
            // writes always start with the memory address
            context.mem_address = i2c_read_byte(i2c);
            //context.mem_address = 0x01;
            context.mem_address_written = true;
        } else {
            // save into memory
            context.mem[context.mem_address] = i2c_read_byte(i2c);
            context.mem_address++;
        }
        break;
    case I2C_SLAVE_REQUEST: // master is requesting data
        // load from memory
        i2c_write_byte(i2c, context.mem[context.mem_address]);
        context.mem_address++;
        break;
    case I2C_SLAVE_FINISH: // master has signalled Stop / Restart
        context.mem_address_written = false;
        break;
    default:
        break;
    }
}

static void setup_slave() {
    gpio_init(I2C_SLAVE_SDA_PIN);
    gpio_set_function(I2C_SLAVE_SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SLAVE_SDA_PIN);

    gpio_init(I2C_SLAVE_SCL_PIN);
    gpio_set_function(I2C_SLAVE_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SLAVE_SCL_PIN);

    i2c_init(i2c0, I2C_BAUDRATE);
    // configure I2C0 for slave mode
    i2c_slave_init(i2c0, I2C_SLAVE_ADDRESS, &i2c_slave_handler);
}

volatile bool timer_fired = false;

uint8_t keypad_sendchar (uint8_t character, bool clearpin, bool update_status) {
  //maybe use key_pressed variable to avoid spamming?
  int timeout = I2C_TIMEOUT_VALUE;

  command_error = 0;

  gpio_put(ONBOARD_LED, 0);

  while (context.mem_address_written != false && context.mem_address>0);

  context.mem[0] = character;
  context.mem_address = 0;
  //gpio_put(KPSTR_PIN, false);
  sleep_us(1000);
  gpio_put(KPSTR_PIN, false);
  //sleep_us(100);
  while (context.mem_address == 0 && timeout){
    sleep_us(1);      
    timeout = timeout - 1;}

  if(!timeout)
    command_error = 1;
  //sleep_ms(2);
  if (clearpin){
    //sleep_ms(5);
    gpio_put(KPSTR_PIN, true);
  }
  gpio_put(ONBOARD_LED, 1);
  return true;
};


bool tick_timer_callback(struct repeating_timer *t) {
    if (onboard_led_count == 0){
    gpio_put(ONBOARD_LED, !gpio_get_out_level(ONBOARD_LED));                // toggle the LED
    onboard_led_count = 20;  //thi ssets the heartbeat
    }else{
    onboard_led_count = onboard_led_count - 1;
    }

    status_update_counter = status_update_counter - 1;
    if (status_update_counter < 0){
      status_update_counter = 0;
    }
    
    return true;
}

int main() {

  stdio_init_all();

  gpio_init(KPSTR_PIN);
  gpio_set_dir(KPSTR_PIN, GPIO_OUT);

  gpio_init(RUNBUTTON);
  gpio_set_dir(RUNBUTTON, GPIO_IN);
  gpio_set_pulls(RUNBUTTON,true,false);

  gpio_init(ONBOARD_LED);
  gpio_set_dir(ONBOARD_LED, GPIO_OUT);
  gpio_put(ONBOARD_LED, 1);
  sleep_ms(250);
  gpio_put(ONBOARD_LED, 0);
  sleep_ms(250);
  gpio_put(ONBOARD_LED, 1);

  struct repeating_timer timer;
  add_repeating_timer_ms(TICK_TIMER_PERIOD, tick_timer_callback, NULL, &timer);

// Setup I2C0 as slave (peripheral)
setup_slave();
packet->status_code = Status_UserException; // ADD STATUS FOR CONTROLLER DISCONNECTED?
    
    // Main loop handles the buttons, everything else handled in interrupts
    while (true) {

        jog_toggle_pressed = true;

        if( packet->machine_state != previous_packet->machine_state ||
            packet->feed_override != previous_packet->feed_override ||
            packet->spindle_override != previous_packet->spindle_override||
            packet->jog_mode.value != previous_packet->jog_mode.value ||
            packet->coordinate.x != previous_packet->coordinate.x ||
            packet->coordinate.y != previous_packet->coordinate.y ||
            packet->coordinate.z != previous_packet->coordinate.z ||
            packet->coordinate.a != previous_packet->coordinate.a ||                  
            packet->current_wcs != previous_packet->current_wcs ||
            packet->jog_stepsize != previous_packet->jog_stepsize ||
            packet->feed_rate != previous_packet->feed_rate ||
            packet->spindle_rpm != previous_packet->spindle_rpm ||
            packet->jog_mode.modifier != previous_packet->jog_mode.modifier
            ){          
        }

        if (packet->status_code != Status_UserException){
          if(context.mem_address >= offsetof(machine_status_packet_t, msgtype))          
            {}//update_neopixels();
        }

        //BUTTON READING ***********************************************************************                               
        if (!gpio_get(RUNBUTTON)){
          if(!jog_toggle_pressed){             
          //key_character = CMD_FEED_HOLD ;
          //keypad_sendchar(key_character, 1, 1);
          }
          while(!gpio_get(RUNBUTTON))
            sleep_us(100);
        } else {
            gpio_put(KPSTR_PIN, true); //make sure stobe is clear when no button is pressed.
          if (status_update_counter < 1){
            status_update_counter = STATUS_REQUEST_PERIOD;
          }
        }        

//SINGLE BUTTON PRESSES ***********************************************************************
//Alternate functions ***********************************************************************
        if (jog_toggle_pressed){  //Pure modifier button.          
          if (true){//keyis still helddown, check alternate keys.
            if (!gpio_get(RUNBUTTON)){
              reset_pressed = 1;
              //unlock_pressed = 1;
            }                                                                                                                 
          }//close jog toggle pressed.
        }//close jog button pressed statement
//Single functions ***********************************************************************
        if (reset_pressed){
          if (!gpio_get(RUNBUTTON)){}//button is still pressed, do nothing
          else{
            key_character = RESET;
            keypad_sendchar (key_character, 1, 1);
            gpio_put(ONBOARD_LED,1);
            reset_pressed = 0;
            sleep_ms(10);
            packet->machine_state = MachineState_Other;
            packet->status_code = Status_Reset;
            sleep_ms(500);            
        }}
        if (unlock_pressed){
          if (!gpio_get(RUNBUTTON)){}//button is still pressed, do nothing
          else{
            key_character = UNLOCK;
            keypad_sendchar (key_character, 1, 1);
            gpio_put(ONBOARD_LED,1);
            unlock_pressed = 0;
            sleep_ms(10);
        }}                                                                                                                                                 
    }//close main while loop
    return 0;
}
