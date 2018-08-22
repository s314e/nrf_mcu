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

#include "tinyRF.h"
#include "uesb_error_codes.h"
#include "nrf_gpio.h"
#include <string.h>


static uesb_event_handler_t     m_event_handler;

// RF parameters
static uesb_config_t            m_config_local;

// TX FIFO
static uesb_payload_t           m_tx_fifo_payload[UESB_CORE_TX_FIFO_SIZE];
static uesb_payload_tx_fifo_t   m_tx_fifo;


static  uint8_t                 m_tx_payload_buffer[UESB_CORE_MAX_PAYLOAD_LENGTH + 2];

// Run time variables
static volatile uint32_t        m_interrupt_flags       = 0;
static uint32_t                 m_pid                   = 0;
static volatile uint32_t        m_retransmits_remaining;
static volatile uint32_t        m_last_tx_attempts;
static volatile uint8_t         m_last_rx_packet_pid = UESB_PID_RESET_VALUE;
static volatile uint32_t        m_last_rx_packet_crc = 0xFFFFFFFF;


static uesb_payload_t           *current_payload;


// Constant parameters
#define                         RX_WAIT_FOR_ACK_TIMEOUT_US_2MBPS   48   // Smallest reliable value - 43
#define                         RX_WAIT_FOR_ACK_TIMEOUT_US_1MBPS   64   // Smallest reliable value - 59
#define                         RX_WAIT_FOR_ACK_TIMEOUT_US_250KBPS 250

// Macros
#define                         DISABLE_RF_IRQ      NVIC_DisableIRQ(RADIO_IRQn)
#define                         ENABLE_RF_IRQ       NVIC_EnableIRQ(RADIO_IRQn)

#define                         RADIO_SHORTS_COMMON ( RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk | \
													  RADIO_SHORTS_ADDRESS_RSSISTART_Msk | RADIO_SHORTS_DISABLED_RSSISTOP_Msk )

// These function pointers are changed dynamically, depending on protocol configuration and state
static void (*on_radio_disabled)(void) = 0;
static void (*on_radio_end)(void) = 0;
static void (*update_rf_payload_format)(uint32_t payload_length) = 0;

// The following functions are assigned to the function pointers above
static void on_radio_disabled_esb_dpl_tx_noack(void);

static void update_rf_payload_format_esb_dpl(uint32_t payload_length)
{
#if(UESB_CORE_MAX_PAYLOAD_LENGTH <= 32)
	NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) | (6 << RADIO_PCNF0_LFLEN_Pos) | (3 << RADIO_PCNF0_S1LEN_Pos);
#else
	NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) | (8 << RADIO_PCNF0_LFLEN_Pos) | (3 << RADIO_PCNF0_S1LEN_Pos);
#endif
	NRF_RADIO->PCNF1 = (RADIO_PCNF1_WHITEEN_Disabled        << RADIO_PCNF1_WHITEEN_Pos) |
					   (RADIO_PCNF1_ENDIAN_Big              << RADIO_PCNF1_ENDIAN_Pos)  |
					   ((m_config_local.rf_addr_length - 1) << RADIO_PCNF1_BALEN_Pos)   |
					   (0                                   << RADIO_PCNF1_STATLEN_Pos) |
					   (UESB_CORE_MAX_PAYLOAD_LENGTH        << RADIO_PCNF1_MAXLEN_Pos);
}


// Function that swaps the bits within each byte in a uint32. Used to convert from nRF24L type addressing to nRF51 type addressing
static uint32_t bytewise_bit_swap(uint32_t inp)
{
	inp = (inp & 0xF0F0F0F0) >> 4 | (inp & 0x0F0F0F0F) << 4;
	inp = (inp & 0xCCCCCCCC) >> 2 | (inp & 0x33333333) << 2;
	return (inp & 0xAAAAAAAA) >> 1 | (inp & 0x55555555) << 1;
}

static void update_radio_parameters()
{
	// Protocol
	update_rf_payload_format = update_rf_payload_format_esb_dpl;
	// TX power
	NRF_RADIO->TXPOWER   = m_config_local.tx_output_power   << RADIO_TXPOWER_TXPOWER_Pos;

	// RF bitrate
	NRF_RADIO->MODE      = m_config_local.bitrate           << RADIO_MODE_MODE_Pos;
	

	// CRC configuration
	NRF_RADIO->CRCCNF    = m_config_local.crc               << RADIO_CRCCNF_LEN_Pos;
	NRF_RADIO->CRCINIT = 0xFFFFUL;      // Initial value
	NRF_RADIO->CRCPOLY = 0x11021UL;     // CRC poly: x^16+x^12^x^5+1
	

	// Packet format
	update_rf_payload_format(m_config_local.payload_length);

	// Radio address config
	NRF_RADIO->PREFIX0 = bytewise_bit_swap(m_config_local.rx_address_p3 << 24 | m_config_local.rx_address_p2 << 16 | m_config_local.rx_address_p1[0] << 8 | m_config_local.rx_address_p0[0]);
	NRF_RADIO->PREFIX1 = bytewise_bit_swap(m_config_local.rx_address_p7 << 24 | m_config_local.rx_address_p6 << 16 | m_config_local.rx_address_p5 << 8 | m_config_local.rx_address_p4);
	NRF_RADIO->BASE0   = bytewise_bit_swap(m_config_local.rx_address_p0[1] << 24 | m_config_local.rx_address_p0[2] << 16 | m_config_local.rx_address_p0[3] << 8 | m_config_local.rx_address_p0[4]);
	NRF_RADIO->BASE1   = bytewise_bit_swap(m_config_local.rx_address_p1[1] << 24 | m_config_local.rx_address_p1[2] << 16 | m_config_local.rx_address_p1[3] << 8 | m_config_local.rx_address_p1[4]);
}

static void initialize_fifos()
{
	m_tx_fifo.entry_point = 0;
	m_tx_fifo.exit_point  = 0;
	m_tx_fifo.count       = 0;
	for(int i = 0; i < UESB_CORE_TX_FIFO_SIZE; i++)
	{
		m_tx_fifo.payload_ptr[i] = &m_tx_fifo_payload[i];
	}

}

static void tx_fifo_remove_last()
{
	if(m_tx_fifo.count > 0)
	{
		DISABLE_RF_IRQ;
		m_tx_fifo.count--;
		m_tx_fifo.exit_point++;
		if(m_tx_fifo.exit_point >= UESB_CORE_TX_FIFO_SIZE) m_tx_fifo.exit_point = 0;
		ENABLE_RF_IRQ;
	}
}


static void ppi_init()
{
	NRF_PPI->CH[UESB_PPI_TIMER_START].EEP = (uint32_t)&NRF_RADIO->EVENTS_READY;
	NRF_PPI->CH[UESB_PPI_TIMER_START].TEP = (uint32_t)&UESB_SYS_TIMER->TASKS_START;
	NRF_PPI->CH[UESB_PPI_TIMER_STOP].EEP =  (uint32_t)&NRF_RADIO->EVENTS_ADDRESS;
	NRF_PPI->CH[UESB_PPI_TIMER_STOP].TEP =  (uint32_t)&UESB_SYS_TIMER->TASKS_STOP;
	NRF_PPI->CH[UESB_PPI_RX_TIMEOUT].EEP = (uint32_t)&UESB_SYS_TIMER->EVENTS_COMPARE[0];
	NRF_PPI->CH[UESB_PPI_RX_TIMEOUT].TEP = (uint32_t)&NRF_RADIO->TASKS_DISABLE;
	NRF_PPI->CH[UESB_PPI_TX_START].EEP = (uint32_t)&UESB_SYS_TIMER->EVENTS_COMPARE[1];
	NRF_PPI->CH[UESB_PPI_TX_START].TEP = (uint32_t)&NRF_RADIO->TASKS_TXEN;
}

uint32_t uesb_init(uesb_config_t *parameters)
{
	m_event_handler = parameters->event_handler;
	memcpy(&m_config_local, parameters, sizeof(uesb_config_t));

	m_interrupt_flags    = 0;
	m_pid                = 0;
	m_last_rx_packet_pid = 0xFF;
	m_last_rx_packet_crc = 0xFFFFFFFF;

	update_radio_parameters();
	initialize_fifos();	
	ppi_init();

	NVIC_SetPriority(RADIO_IRQn, m_config_local.radio_irq_priority & 0x03);


	return UESB_SUCCESS;
}

uint32_t uesb_disable(void)
{
	NRF_PPI->CHENCLR = (1 << UESB_PPI_TIMER_START) | (1 << UESB_PPI_TIMER_STOP) | (1 << UESB_PPI_RX_TIMEOUT) | (1 << UESB_PPI_TX_START);
	return UESB_SUCCESS;
}

static void start_tx_transaction()
{
	// Prepare the payload
	current_payload = m_tx_fifo.payload_ptr[m_tx_fifo.exit_point];
	m_pid = (m_pid + 1) % 4;
	//ack = current_payload->noack == 0 || m_config_local.dynamic_ack_enabled == 0;

	m_tx_payload_buffer[0] = current_payload->length;
	m_tx_payload_buffer[1] = 0x01;//m_pid << 1 | ((ack == 0 && m_config_local.dynamic_ack_enabled) ? 0x00 : 0x01);
	memcpy(&m_tx_payload_buffer[2], current_payload->data, current_payload->length);

	NRF_RADIO->SHORTS   = RADIO_SHORTS_COMMON;
	NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
	on_radio_disabled   = on_radio_disabled_esb_dpl_tx_noack;
	

	NRF_RADIO->TXADDRESS = current_payload->pipe;
	NRF_RADIO->RXADDRESSES = 1 << current_payload->pipe;

	NRF_RADIO->FREQUENCY = m_config_local.rf_channel;

	NRF_RADIO->PACKETPTR = (uint32_t)m_tx_payload_buffer;

	NVIC_ClearPendingIRQ(RADIO_IRQn);
	NVIC_EnableIRQ(RADIO_IRQn);

	NRF_RADIO->EVENTS_ADDRESS = NRF_RADIO->EVENTS_PAYLOAD = NRF_RADIO->EVENTS_DISABLED = 0;
	
	NRF_RADIO->TASKS_TXEN  = 1;
}

static uint32_t write_tx_payload(uesb_payload_t *payload, bool noack) // ~50us @ 61 bytes SB
{
	if(m_tx_fifo.count >= UESB_CORE_TX_FIFO_SIZE) return UESB_ERROR_TX_FIFO_FULL;

	DISABLE_RF_IRQ;
	
	memcpy(m_tx_fifo.payload_ptr[m_tx_fifo.entry_point], payload, sizeof(uesb_payload_t));
	

	m_tx_fifo.entry_point++;
	if(m_tx_fifo.entry_point >= UESB_CORE_TX_FIFO_SIZE) m_tx_fifo.entry_point = 0;
	
	m_tx_fifo.count++;
	
	ENABLE_RF_IRQ;

	
	start_tx_transaction();
	
	return UESB_SUCCESS;
}

uint32_t uesb_write_tx_payload(uesb_payload_t *payload)
{
	return write_tx_payload(payload, false);
}


uint32_t uesb_start_tx()
{
	if(m_tx_fifo.count == 0) return UESB_ERROR_TX_FIFO_EMPTY;
	start_tx_transaction();
	return UESB_SUCCESS;
}


uint32_t uesb_flush_tx(void)
{
	DISABLE_RF_IRQ;
	m_tx_fifo.count = 0;
	m_tx_fifo.entry_point = m_tx_fifo.exit_point = 0;
	ENABLE_RF_IRQ;
	return UESB_SUCCESS;
}



uint32_t uesb_get_clear_interrupts(uint32_t *interrupts)
{
	DISABLE_RF_IRQ;
	*interrupts = m_interrupt_flags;
	m_interrupt_flags = 0;
	ENABLE_RF_IRQ;
	return UESB_SUCCESS;
}





static void on_radio_disabled_esb_dpl_tx_noack()
{
	m_interrupt_flags |= UESB_INT_TX_SUCCESS_MSK;
	tx_fifo_remove_last();

	if(m_tx_fifo.count == 0)
	{
	if(m_event_handler != 0) m_event_handler();
	}
	else
	{
		if(m_event_handler != 0) m_event_handler();
		start_tx_transaction();
	}
}


uint32_t uesb_set_address(uesb_address_type_t address, const uint8_t *data_ptr)
{
	switch(address)
	{
		case UESB_ADDRESS_PIPE0:
			memcpy(m_config_local.rx_address_p0, data_ptr, m_config_local.rf_addr_length);
			break;
		case UESB_ADDRESS_PIPE1:
			memcpy(m_config_local.rx_address_p1, data_ptr, m_config_local.rf_addr_length);
			break;
		case UESB_ADDRESS_PIPE2:
			m_config_local.rx_address_p2 = *data_ptr;
			break;
		case UESB_ADDRESS_PIPE3:
			m_config_local.rx_address_p3 = *data_ptr;
			break;
		case UESB_ADDRESS_PIPE4:
			m_config_local.rx_address_p4 = *data_ptr;
			break;
		case UESB_ADDRESS_PIPE5:
			m_config_local.rx_address_p5 = *data_ptr;
			break;
		case UESB_ADDRESS_PIPE6:
			m_config_local.rx_address_p6 = *data_ptr;
			break;
		case UESB_ADDRESS_PIPE7:
			m_config_local.rx_address_p7 = *data_ptr;
			break;
		default:
			return UESB_ERROR_INVALID_PARAMETERS;
	}
	update_radio_parameters();
	return UESB_SUCCESS;
}


void RADIO_IRQHandler()
{
	if(NRF_RADIO->EVENTS_READY && (NRF_RADIO->INTENSET & RADIO_INTENSET_READY_Msk))
	{
		NRF_RADIO->EVENTS_READY = 0;

	}

	if(NRF_RADIO->EVENTS_END && (NRF_RADIO->INTENSET & RADIO_INTENSET_END_Msk))
	{
		NRF_RADIO->EVENTS_END = 0;



		// Call the correct on_radio_end function, depending on the current protocol state
		if(on_radio_end)
		{
			on_radio_end();
		}
	}

	if(NRF_RADIO->EVENTS_DISABLED && (NRF_RADIO->INTENSET & RADIO_INTENSET_DISABLED_Msk))
	{
		NRF_RADIO->EVENTS_DISABLED = 0;

	  
		// Call the correct on_radio_disable function, depending on the current protocol state
		if(on_radio_disabled)
		{
			on_radio_disabled();
		}
	}

}
