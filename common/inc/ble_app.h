#ifndef _BLE_APP_
#define _BLE_APP_

#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "softdevice_handler.h"
#include "app_timer.h"
#include "custom_ble_services.h"

//#define SOFT_DEVICE_ENABLED

#ifdef SOFT_DEVICE_ENABLED
	#define BLE_INIT_INI_MODULES() ble_init_modules();
#else
	#define BLE_INIT_INI_MODULES() 
#endif


void nrf_ble_stack_init(void);

void nrf_ble_evt_dispatch( ble_evt_t * p_ble_evt);

void on_ble_evt(ble_evt_t * p_ble_evt);

void advertising_init(void);

void on_conn_params_evt(ble_conn_params_evt_t * p_evt);

void services_init(void);;

void on_adv_evt(ble_adv_evt_t  ble_adv_evt);

void gap_params_init(void);

void ble_device_manager_init(bool erase_bonds);

void conn_params_init(void);

void ble_send(uint8_t *p_string, uint8_t length);


void ble_init_modules(void);

#endif

