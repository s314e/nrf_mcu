/* Copyright (c) 2014 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include "nrf.h"
#include "tinyRFRX.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "bbn_board.h"
#include "simple_uart.h"


//static uesb_payload_t rx_payload;

extern uint8_t led_state;
int main(void)
{
    
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    while(NRF_CLOCK->EVENTS_HFCLKSTARTED == 0);

    nrf_gpio_range_cfg_output(8, 31);
    
      tinyrx_setup_rx();
//    simple_uart_config(  RTS_PIN,
//                         TX_PIN, 
//                         CTS_PIN,
//                         RX_PIN, 
//                        false);
//      simple_uart_putstring("nrf init\n");
	  nrf_gpio_pin_set(LED_BLUE);
    while (true)
    {   

        if( led_state){
        
            led_state=0;
            nrf_gpio_pin_toggle(LED_GREEN);
            nrf_gpio_pin_set(LED_RED);
            //printf( get_rssi());
					nrf_delay_ms(1000);
        }else{
            nrf_delay_ms(1000);
            nrf_gpio_pin_set(LED_GREEN);
            nrf_gpio_pin_toggle(LED_RED);
            nrf_delay_ms(1000);
                    
        }
  //      simple_uart_putstring("nrf loop\n");  

    }
}
