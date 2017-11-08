/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     drivers_sx1272
 * @{
 * @file
 * @brief       Semtech SX1272 driver
 *
 * @author      Eugeny Ponomarev <ep@unwds.com>
 * @}
 */
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "debug.h"

#include "periph/gpio.h"
#include "periph/spi.h"

#include "rtctimers-millis.h"
#include "thread.h"

#include "sx1272.h"
#include "include/sx1272_regs_fsk.h"
#include "include/sx1272_regs_lora.h"

/**
 * Radio registers definition
 */
typedef struct {
    sx1272_radio_modems_t modem;
    uint8_t addr;
    uint8_t value;

} sx1272_radio_registers_t;

/*
 * Private functions prototypes
 */

/**
 * @brief Resets the SX1272
 */
void sx1272_reset(sx1272_t *dev);

/**
 * @brief Writes the buffer contents to the SX1272 FIFO
 *
 * @param [IN] buffer Buffer containing data to be put on the FIFO.
 * @param [IN] size Number of bytes to be written to the FIFO
 */
void sx1272_write_fifo(sx1272_t *dev, uint8_t *buffer, uint8_t size);

/**
 * @brief Reads the contents of the SX1272 FIFO
 *
 * @param [OUT] buffer Buffer where to copy the FIFO read data.
 * @param [IN] size Number of bytes to be read from the FIFO
 */
void sx1272_read_fifo(sx1272_t *dev, uint8_t *buffer, uint8_t size);

/**
 * @brief Sets the SX1272 operating mode
 *
 * @param [IN] op_mode New operating mode
 */
void sx1272_set_op_mode(sx1272_t *dev, uint8_t op_mode);

/*
 * Private global constants
 */

/**
 * Constant values need to compute the RSSI value
 */
#define RSSI_OFFSET                                 -139

static void sx1272_set_status(sx1272_t *dev, sx1272_radio_state_t state)
{
    dev->settings.state = state;
}

/**
 * @brief SX1272 DIO interrupt handlers initialization
 */

static void sx1272_on_dio0_isr(void *arg);
static void sx1272_on_dio1_isr(void *arg);
static void sx1272_on_dio2_isr(void *arg);
static void sx1272_on_dio3_isr(void *arg);

static void _init_isrs(sx1272_t *dev)
{
    if (dev->dio0_pin != GPIO_UNDEF ) {
        gpio_init_int(dev->dio0_pin, GPIO_IN, GPIO_RISING, sx1272_on_dio0_isr, dev);
    }
    
    if (dev->dio1_pin != GPIO_UNDEF ) {
        gpio_init_int(dev->dio1_pin, GPIO_IN, GPIO_RISING, sx1272_on_dio1_isr, dev);
    }
    
    if (dev->dio2_pin != GPIO_UNDEF ) {
        gpio_init_int(dev->dio2_pin, GPIO_IN, GPIO_RISING, sx1272_on_dio2_isr, dev);
    }
    
    if (dev->dio3_pin != GPIO_UNDEF ) {
        gpio_init_int(dev->dio3_pin, GPIO_IN, GPIO_RISING, sx1272_on_dio3_isr, dev);
    }
    
    if (dev->dio4_pin != GPIO_UNDEF ) {
        gpio_init_int(dev->dio4_pin, GPIO_IN, GPIO_RISING, sx1272_on_dio4_isr, dev);
    }
    
    if (dev->dio5_pin != GPIO_UNDEF ) {
        gpio_init_int(dev->dio5_pin, GPIO_IN, GPIO_RISING, sx1272_on_dio5_isr, dev);
    }
}

static inline void send_event(sx1272_t *dev, sx1272_event_type_t event_type)
{
    if (dev->sx1272_event_cb != NULL) {
        dev->sx1272_event_cb(dev, event_type);
    }
}

/**
 * @brief Timeout timers internal routines
 */
static void _on_tx_timeout(void *arg)
{
    sx1272_t *dev = (sx1272_t *) arg;

    /* TX timeout. Send event message to the application's thread */
    send_event(dev, SX1272_TX_TIMEOUT);
}

static void _on_rx_timeout(void *arg)
{
    sx1272_t *dev = (sx1272_t *) arg;

    /* RX timeout. Send event message to the application's thread */
    send_event(dev, SX1272_RX_TIMEOUT);
}

/**
 * @brief Sets timers callbacks and arguments
 */
static void _init_timers(sx1272_t *dev)
{
    dev->_internal.tx_timeout_timer.arg = dev;
    dev->_internal.tx_timeout_timer.callback = _on_tx_timeout;

    dev->_internal.rx_timeout_timer.arg = dev;
    dev->_internal.rx_timeout_timer.callback = _on_rx_timeout;
}

static int _init_peripherals(sx1272_t *dev)
{
    int res;

    /* Setup SPI for SX1272 */
    spi_acquire(dev->spi);
    res = spi_init_master(dev->spi, SPI_CONF_FIRST_RISING, SPI_SPEED_1MHZ);
    spi_release(dev->spi);

    if (res < 0) {
        printf("sx1272: error initializing SPI_%i device (code %i)\n",
        		dev->spi, res);
        return 0;
    }

    res = gpio_init(dev->nss_pin, GPIO_OUT);
    if (res < 0) {
        printf("sx1272: error initializing GPIO_%ld as CS line (code %i)\n",
               (long)dev->nss_pin, res);
        return 0;
    }
    
    gpio_init(dev->rfswitch_pin, GPIO_OUT);
    gpio_set(dev->rfswitch_pin);

    gpio_set(dev->nss_pin);

    return 1;
}

sx1272_init_result_t sx1272_init(sx1272_t *dev)
{
    sx1272_reset(dev);

    /** Do internal initialization routines */
    if (!_init_peripherals(dev)) {
        return SX1272_ERR_SPI;
    }

    _init_isrs(dev);
    _init_timers(dev);

    /* Check presence of SX1272 */
    if (!sx1272_test(dev)) {
        DEBUG("init_radio: test failed");
        return SX1272_ERR_TEST_FAILED;
    }

    /* Set RegOpMode value to the datasheet's default. Actual default after POR is 0x09 */
    sx1272_reg_write(dev, REG_OPMODE, 0x00);

    /* Switch into LoRa mode */
    sx1272_set_modem(dev, SX1272_MODEM_LORA);

    /* Set current frequency */
    sx1272_set_channel(dev, dev->settings.channel);

    /* Create DIO event lines handler */
    kernel_pid_t pid = thread_create((char *) dev->_internal.dio_polling_thread_stack, sizeof(dev->_internal.dio_polling_thread_stack), THREAD_PRIORITY_MAIN - 2,
                                     THREAD_CREATE_STACKTEST, dio_polling_thread, dev,
                                     "sx1272 DIO handler");

    if (pid <= KERNEL_PID_UNDEF) {
        DEBUG("sx1272: creation of DIO handling thread");
        return SX1272_ERR_THREAD;
    }
    dev->_internal.dio_polling_thread_pid = pid;

    return SX1272_INIT_OK;
}

sx1272_radio_state_t sx1272_get_status(sx1272_t *dev)
{
    return dev->settings.state;
}

void sx1272_set_channel(sx1272_t *dev, uint32_t freq)
{
    /* Save current operating mode */
    uint8_t prev_mode = sx1272_reg_read(dev, REG_OPMODE);

    sx1272_set_op_mode(dev, RF_OPMODE_STANDBY);

    freq = (uint32_t)((double) freq / (double) SX1272_FREQ_STEP);

    /* Write frequency settings into chip */
    sx1272_reg_write(dev, REG_FRFMSB, (uint8_t)((freq >> 16) & 0xFF));
    sx1272_reg_write(dev, REG_FRFMID, (uint8_t)((freq >> 8) & 0xFF));
    sx1272_reg_write(dev, REG_FRFLSB, (uint8_t)(freq & 0xFF));

    /* Restore previous operating mode */
    sx1272_reg_write(dev, REG_OPMODE, prev_mode);
}

bool sx1272_test(sx1272_t *dev)
{
    /* Read version number and compare with sx1272 assigned revision */
    uint8_t version = sx1272_reg_read(dev, REG_VERSION);

    if (version != VERSION_SX1272 ) {
        printf("sx1272: test failed, invalid version number: %d\n", version);
        return false;
    }

    return true;
}

bool sx1272_is_channel_free(sx1272_t *dev, uint32_t freq, int16_t rssi_thresh)
{
    int16_t rssi = 0;

    sx1272_set_channel(dev, freq);
    sx1272_set_op_mode(dev, RF_OPMODE_RECEIVER);

    // xtimer_usleep(1000); /* wait 1 millisecond */
    xtimer_spin(xtimer_ticks_from_usec(1000));

    rssi = sx1272_read_rssi(dev);
    sx1272_set_sleep(dev);

    if (rssi > rssi_thresh) {
        return false;
    }

    return true;
}

void sx1272_set_modem(sx1272_t *dev, sx1272_radio_modems_t modem)
{
    dev->settings.modem = modem;

    switch (dev->settings.modem) {
        case SX1272_MODEM_LORA:
            sx1272_set_op_mode(dev, RF_OPMODE_SLEEP);
            sx1272_reg_write(dev,
                             REG_OPMODE,
                             (sx1272_reg_read(dev, REG_OPMODE)
                              & RFLR_OPMODE_LONGRANGEMODE_MASK)
                             | RFLR_OPMODE_LONGRANGEMODE_ON);

            sx1272_reg_write(dev, REG_DIOMAPPING1, 0x00);
            sx1272_reg_write(dev, REG_DIOMAPPING2, 0x10); /* DIO5=ClkOut */
            break;

        case SX1272_MODEM_FSK:
            sx1272_set_op_mode(dev, RF_OPMODE_SLEEP);
            sx1272_reg_write(dev,
                             REG_OPMODE,
                             (sx1272_reg_read(dev, REG_OPMODE)
                              & RFLR_OPMODE_LONGRANGEMODE_MASK)
                             | RFLR_OPMODE_LONGRANGEMODE_OFF);

            sx1272_reg_write(dev, REG_DIOMAPPING1, 0x00);
            //sx1272_reg_write(dev, REG_DIOMAPPING2, 0x20); /* DIO5=mode_ready */
            break;
        default:
            break;
    }
}

#define RXLORA_RXMODE_RSSI_REG_MODEM_CONFIG1 0x0A
#define RXLORA_RXMODE_RSSI_REG_MODEM_CONFIG2 0x70

uint32_t sx1272_random(sx1272_t *dev)
{
    uint8_t i;
    uint32_t rnd = 0;

    sx1272_set_modem(dev, SX1272_MODEM_LORA); /* Set LoRa modem ON */

    /* Disable LoRa modem interrupts */
    sx1272_reg_write(dev, REG_LR_IRQFLAGSMASK, RFLR_IRQFLAGS_RXTIMEOUT |
                     RFLR_IRQFLAGS_RXDONE |
                     RFLR_IRQFLAGS_PAYLOADCRCERROR |
                     RFLR_IRQFLAGS_VALIDHEADER |
                     RFLR_IRQFLAGS_TXDONE |
                     RFLR_IRQFLAGS_CADDONE |
                     RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                     RFLR_IRQFLAGS_CADDETECTED);

    sx1272_set_op_mode(dev, RF_OPMODE_STANDBY);
    sx1272_reg_write(dev, REG_LR_MODEMCONFIG1, RXLORA_RXMODE_RSSI_REG_MODEM_CONFIG1);
    sx1272_reg_write(dev, REG_LR_MODEMCONFIG1, RXLORA_RXMODE_RSSI_REG_MODEM_CONFIG2);

    /* Set radio in continuous reception */
    sx1272_set_op_mode(dev, RF_OPMODE_RECEIVER);

    for (i = 0; i < 32; i++) {
        //xtimer_usleep(1000); /* wait for the chaos */
        xtimer_spin(xtimer_ticks_from_usec(1000));

        /* Non-filtered RSSI value reading. Only takes the LSB value */
        rnd |= ((uint32_t) sx1272_reg_read(dev, REG_LR_RSSIWIDEBAND) & 0x01) << i;
    }

    sx1272_set_sleep(dev);

    return rnd;
}

static inline uint8_t sx1272_get_pa_select(uint32_t channel)
{
    if (channel < SX1272_RF_MID_BAND_THRESH) {
        return RF_PACONFIG_PASELECT_PABOOST;
    }
    else {
        return RF_PACONFIG_PASELECT_RFO;
    }
}

static void setup_power_amplifier(sx1272_t *dev, sx1272_lora_settings_t *settings)
{
    uint8_t pa_config = 0;
    uint8_t pa_dac = 0;

    pa_config = sx1272_reg_read(dev, REG_PACONFIG);
    pa_dac = sx1272_reg_read(dev, REG_PADAC);

    pa_config = (pa_config & RF_PACONFIG_PASELECT_MASK) | sx1272_get_pa_select(dev->settings.channel);

    if ((pa_config & RF_PACONFIG_PASELECT_PABOOST) == RF_PACONFIG_PASELECT_PABOOST)
    {
        if (dev->settings.lora.power > 17) {
            pa_dac = (pa_dac & RF_PADAC_20DBM_MASK) | RFLR_PADAC_20DBM_ON;
        } else {
            pa_dac = (pa_dac & RF_PADAC_20DBM_MASK) | RFLR_PADAC_20DBM_OFF;
        }
        if ((pa_dac & RFLR_PADAC_20DBM_ON) == RFLR_PADAC_20DBM_ON) {
            if (dev->settings.lora.power < 5) {
                dev->settings.lora.power = 5;
            }
            if (dev->settings.lora.power > 20) {
                dev->settings.lora.power = 20;
            }
            pa_config = (pa_config & RFLR_PACONFIG_OUTPUTPOWER_MASK) | (uint8_t)((uint16_t)(dev->settings.lora.power - 5) & 0x0F);
        } else {
            if (dev->settings.lora.power < 2) {
                dev->settings.lora.power = 2;
            }
            if (dev->settings.lora.power > 17) {
                dev->settings.lora.power = 17;
            }
            pa_config = (pa_config & RFLR_PACONFIG_OUTPUTPOWER_MASK) | (uint8_t)((uint16_t)(dev->settings.lora.power - 2) & 0x0F);
        }
    } else {
        if (dev->settings.lora.power < -1) {
            dev->settings.lora.power = -1;
        }
        if (dev->settings.lora.power > 14) {
            dev->settings.lora.power = 14;
        }
        pa_config = (pa_config & RFLR_PACONFIG_OUTPUTPOWER_MASK) | (uint8_t)((uint16_t)(dev->settings.lora.power + 1) & 0x0F);
    }

    sx1272_reg_write(dev, REG_PACONFIG, pa_config);
    sx1272_reg_write(dev, REG_PADAC, pa_dac);
}

void sx1272_configure_lora(sx1272_t *dev, sx1272_lora_settings_t *settings)
{
    sx1272_set_modem(dev, SX1272_MODEM_LORA);

    /* Copy LoRa configuration into device structure */
    if (settings != NULL) {
        memcpy(&dev->settings.lora, settings, sizeof(sx1272_lora_settings_t));
    }

    if (((dev->settings.lora.bandwidth == SX1272_BW_125_KHZ) && ((dev->settings.lora.datarate == SX1272_SF11) || (dev->settings.lora.datarate == SX1272_SF12)))
        || ((dev->settings.lora.bandwidth == SX1272_BW_250_KHZ) && (dev->settings.lora.datarate == SX1272_SF12))) {
        dev->settings.lora.low_datarate_optimize = 0x01;
    }
    else {
        dev->settings.lora.low_datarate_optimize = 0x00;
    }

    sx1272_reg_write(dev,
                     REG_LR_MODEMCONFIG1,
                     (sx1272_reg_read(dev, REG_LR_MODEMCONFIG1) &
                      RFLR_MODEMCONFIG1_BW_MASK &
                      RFLR_MODEMCONFIG1_CODINGRATE_MASK &
                      RFLR_MODEMCONFIG1_IMPLICITHEADER_MASK &
                      RFLR_MODEMCONFIG1_RXPAYLOADCRC_MASK & 
                      RFLR_MODEMCONFIG1_LOWDATARATEOPTIMIZE_MASK) |
                      (dev->settings.lora.bandwidth << 6) |
                      (dev->settings.lora.coderate << 3) |
                      (dev->settings.lora.implicit_header << 2 ) |
                      (dev->settings.lora.crc_on << 1) |
                      (dev->settings.lora.low_datarate_optimize));
                     
    sx1272_reg_write(dev, REG_LR_MODEMCONFIG2,
                     (sx1272_reg_read(dev, REG_LR_MODEMCONFIG2) &
                      RFLR_MODEMCONFIG2_SF_MASK &
                      RFLR_MODEMCONFIG2_SYMBTIMEOUTMSB_MASK) | (dev->settings.lora.datarate << 4) |
                      ((dev->settings.lora.rx_timeout >> 8)
                        & ~RFLR_MODEMCONFIG2_SYMBTIMEOUTMSB_MASK));

    sx1272_reg_write(dev, REG_LR_SYMBTIMEOUTLSB,
                     (uint8_t)(dev->settings.lora.rx_timeout & 0xFF));

    sx1272_reg_write(dev, REG_LR_PREAMBLEMSB,
                     (uint8_t)((dev->settings.lora.preamble_len >> 8) & 0xFF));
    sx1272_reg_write(dev, REG_LR_PREAMBLELSB,
                     (uint8_t)(dev->settings.lora.preamble_len & 0xFF));

    if (dev->settings.lora.implicit_header) {
        sx1272_reg_write(dev, REG_LR_PAYLOADLENGTH, dev->settings.lora.payload_len);
    }


    if (dev->settings.lora.freq_hop_on) {
        sx1272_reg_write(dev,
                         REG_LR_PLLHOP,
                         (sx1272_reg_read(dev, REG_LR_PLLHOP)
                          & RFLR_PLLHOP_FASTHOP_MASK) | RFLR_PLLHOP_FASTHOP_ON);
        sx1272_reg_write(dev, REG_LR_HOPPERIOD,
                         dev->settings.lora.hop_period);
    }

    setup_power_amplifier(dev, settings);
    
    /* Enable LNA HF boost, as recommended in AN1200.23 */
    sx1272_reg_write(dev, REG_LR_LNA, (sx1272_reg_read(dev, REG_LR_LNA) & RFLR_LNA_BOOST_MASK) | RFLR_LNA_BOOST_ON);
    
    /* Enable Automatic Gain Control */
    sx1272_reg_write(dev, REG_LR_MODEMCONFIG2, \
                     (sx1272_reg_read(dev, REG_LR_MODEMCONFIG2) & RFLR_MODEMCONFIG2_AGCAUTO_MASK) | RFLR_MODEMCONFIG2_AGCAUTO_ON);
    
    sx1272_reg_write(dev, REG_LR_DETECTOPTIMIZE, RFLR_DETECTIONOPTIMIZE_SF7_TO_SF12);
    sx1272_reg_write(dev, REG_LR_DETECTIONTHRESHOLD, RFLR_DETECTIONTHRESH_SF7_TO_SF12);
}

void sx1272_configure_lora_bw(sx1272_t *dev, sx1272_lora_bandwidth_t bw)
{
    dev->settings.lora.bandwidth = bw;
    sx1272_configure_lora(dev, NULL);
}

void sx1272_configure_lora_sf(sx1272_t *dev, sx1272_lora_spreading_factor_t sf)
{
    dev->settings.lora.datarate = sf;
    sx1272_configure_lora(dev, NULL);
}

void sx1272_configure_lora_cr(sx1272_t *dev, sx1272_lora_coding_rate_t cr)
{
    dev->settings.lora.coderate = cr;
    sx1272_configure_lora(dev, NULL);
}

uint32_t sx1272_get_time_on_air(sx1272_t *dev, sx1272_radio_modems_t modem,
                                uint8_t pkt_len)
{
    uint32_t air_time = 0;

    switch (modem) {
        case SX1272_MODEM_FSK:
            break;

        case SX1272_MODEM_LORA:
        {
            double bw = 0.0;

            /* Note: SX1272 only supports bandwidths 125, 250 and 500 kHz */
            switch (dev->settings.lora.bandwidth) {
                case 0: /* 125 kHz */
                    bw = 125e3;
                    break;
                case 1: /* 250 kHz */
                    bw = 250e3;
                    break;
                case 2: /* 500 kHz */
                    bw = 500e3;
                    break;
            }

            /* Symbol rate : time for one symbol [secs] */
            double rs = bw / (1 << dev->settings.lora.datarate);
            double ts = 1 / rs;

            /* time of preamble */
            double t_preamble = (dev->settings.lora.preamble_len + 4.25) * ts;

            /* Symbol length of payload and time */
            double tmp =
                ceil(
                    (8 * pkt_len - 4 * dev->settings.lora.datarate + 28
                     + 16 * dev->settings.lora.crc_on
                     - (!dev->settings.lora.implicit_header ? 20 : 0))
                    / (double) (4 * dev->settings.lora.datarate
                                - ((dev->settings.lora.low_datarate_optimize
                                    > 0) ? 2 : 0)))
                * (dev->settings.lora.coderate + 4);
            double n_payload = 8 + ((tmp > 0) ? tmp : 0);
            double t_payload = n_payload * ts;

            /* Time on air */
            double t_on_air = t_preamble + t_payload;

            /* return seconds */
            air_time = floor(t_on_air * 1e6 + 0.999);
        }
        break;
    }

    return air_time;
}

void sx1272_send(sx1272_t *dev, uint8_t *buffer, uint8_t size)
{
    switch (dev->settings.modem) {
        case SX1272_MODEM_FSK:
            sx1272_write_fifo(dev, &size, 1);
            sx1272_write_fifo(dev, buffer, size);
            break;

        case SX1272_MODEM_LORA:
        {

            if (dev->settings.lora.iq_inverted) {
                sx1272_reg_write(dev,
                                 REG_LR_INVERTIQ,
                                 ((sx1272_reg_read(dev, REG_LR_INVERTIQ)
                                   & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK)
                                  | RFLR_INVERTIQ_RX_OFF | RFLR_INVERTIQ_TX_ON));
                sx1272_reg_write(dev, REG_LR_INVERTIQ2, RFLR_INVERTIQ2_ON);
            }
            else {
                sx1272_reg_write(dev,
                                 REG_LR_INVERTIQ,
                                 ((sx1272_reg_read(dev, REG_LR_INVERTIQ)
                                   & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK)
                                  | RFLR_INVERTIQ_RX_OFF | RFLR_INVERTIQ_TX_OFF));
                sx1272_reg_write(dev, REG_LR_INVERTIQ2, RFLR_INVERTIQ2_OFF);
            }

            /* Initializes the payload size */
            sx1272_reg_write(dev, REG_LR_PAYLOADLENGTH, size);

            /* Full buffer used for Tx */
            sx1272_reg_write(dev, REG_LR_FIFOTXBASEADDR, 0x00);
            sx1272_reg_write(dev, REG_LR_FIFOADDRPTR, 0x00);

            /* FIFO operations can not take place in Sleep mode
             * So wake up the chip */
            if ((sx1272_reg_read(dev, REG_OPMODE) & ~RF_OPMODE_MASK)
                == RF_OPMODE_SLEEP) {
                sx1272_set_standby(dev);
                //xtimer_usleep(SX1272_RADIO_WAKEUP_TIME); /* wait for chip wake up */
                xtimer_spin(xtimer_ticks_from_usec(SX1272_RADIO_WAKEUP_TIME));
            }

            /* Write payload buffer */
            sx1272_write_fifo(dev, buffer, size);
        }
        break;
    }

    /* Enable TXDONE interrupt */
    sx1272_reg_write(dev, REG_LR_IRQFLAGSMASK,
                     RFLR_IRQFLAGS_RXTIMEOUT |
                     RFLR_IRQFLAGS_RXDONE |
                     RFLR_IRQFLAGS_PAYLOADCRCERROR |
                     RFLR_IRQFLAGS_VALIDHEADER |
                     //RFLR_IRQFLAGS_TXDONE |
                     RFLR_IRQFLAGS_CADDONE |
                     RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                     RFLR_IRQFLAGS_CADDETECTED);

    /* Set TXDONE interrupt to the DIO0 line */
    sx1272_reg_write(dev,
                     REG_DIOMAPPING1,
                     (sx1272_reg_read(dev, REG_DIOMAPPING1)
                      & RFLR_DIOMAPPING1_DIO0_MASK)
                     | RFLR_DIOMAPPING1_DIO0_01);


    /* Start TX timeout timer */
    xtimer_set(&dev->_internal.tx_timeout_timer, dev->settings.lora.tx_timeout);

    /* Put chip into transfer mode */
    sx1272_set_status(dev, SX1272_RF_TX_RUNNING);
    sx1272_set_op_mode(dev, RF_OPMODE_TRANSMITTER);
}

void sx1272_set_sleep(sx1272_t *dev)
{
    /* Disable running timers */
    xtimer_remove(&dev->_internal.tx_timeout_timer);
    xtimer_remove(&dev->_internal.rx_timeout_timer);

    /* Put chip into sleep */
    sx1272_set_op_mode(dev, RF_OPMODE_SLEEP);
    sx1272_set_status(dev,  SX1272_RF_IDLE);
}

void sx1272_set_standby(sx1272_t *dev)
{
    /* Disable running timers */
    xtimer_remove(&dev->_internal.tx_timeout_timer);
    xtimer_remove(&dev->_internal.rx_timeout_timer);

    sx1272_set_op_mode(dev, RF_OPMODE_STANDBY);
    sx1272_set_status(dev,  SX1272_RF_IDLE);
}

void sx1272_set_rx(sx1272_t *dev, uint32_t timeout)
{
    bool rx_continuous = false;

    switch (dev->settings.modem) {
        case SX1272_MODEM_FSK:
            break;

        case SX1272_MODEM_LORA:
        {
            if (dev->settings.lora.iq_inverted) {
                sx1272_reg_write(dev,
                                 REG_LR_INVERTIQ,
                                 ((sx1272_reg_read(dev, REG_LR_INVERTIQ)
                                   & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK)
                                  | RFLR_INVERTIQ_RX_ON | RFLR_INVERTIQ_TX_OFF));
                sx1272_reg_write(dev, REG_LR_INVERTIQ2, RFLR_INVERTIQ2_ON);
            }
            else {
                sx1272_reg_write(dev,
                                 REG_LR_INVERTIQ,
                                 ((sx1272_reg_read(dev, REG_LR_INVERTIQ)
                                   & RFLR_INVERTIQ_TX_MASK & RFLR_INVERTIQ_RX_MASK)
                                  | RFLR_INVERTIQ_RX_OFF | RFLR_INVERTIQ_TX_OFF));
                sx1272_reg_write(dev, REG_LR_INVERTIQ2, RFLR_INVERTIQ2_OFF);
            }

            rx_continuous = dev->settings.lora.rx_continuous;

            /* Setup interrupts */
            if (dev->settings.lora.freq_hop_on) {
                sx1272_reg_write(dev, REG_LR_IRQFLAGSMASK,  //RFLR_IRQFLAGS_RXTIMEOUT |
                                                            //RFLR_IRQFLAGS_RXDONE |
                                                            //RFLR_IRQFLAGS_PAYLOADCRCERROR |
                                 RFLR_IRQFLAGS_VALIDHEADER |
                                 RFLR_IRQFLAGS_TXDONE |
                                 RFLR_IRQFLAGS_CADDONE |
                                 //RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                                 RFLR_IRQFLAGS_CADDETECTED);

                // DIO0=RxDone, DIO2=FhssChangeChannel
                sx1272_reg_write(dev,
                                 REG_DIOMAPPING1,
                                 (sx1272_reg_read(dev, REG_DIOMAPPING1)
                                  & RFLR_DIOMAPPING1_DIO0_MASK
                                  & RFLR_DIOMAPPING1_DIO2_MASK)
                                 | RFLR_DIOMAPPING1_DIO0_00
                                 | RFLR_DIOMAPPING1_DIO2_00);
            }
            else {
                sx1272_reg_write(dev, REG_LR_IRQFLAGSMASK,  //RFLR_IRQFLAGS_RXTIMEOUT |
                                                            //RFLR_IRQFLAGS_RXDONE |
                                                            //RFLR_IRQFLAGS_PAYLOADCRCERROR |
                                 RFLR_IRQFLAGS_VALIDHEADER |
                                 RFLR_IRQFLAGS_TXDONE |
                                 RFLR_IRQFLAGS_CADDONE |
                                 RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL |
                                 RFLR_IRQFLAGS_CADDETECTED);

                // DIO0=RxDone
                sx1272_reg_write(dev,
                                 REG_DIOMAPPING1,
                                 (sx1272_reg_read(dev, REG_DIOMAPPING1)
                                  & RFLR_DIOMAPPING1_DIO0_MASK)
                                 | RFLR_DIOMAPPING1_DIO0_00);
            }

            sx1272_reg_write(dev, REG_LR_FIFORXBASEADDR, 0);
            sx1272_reg_write(dev, REG_LR_FIFOADDRPTR, 0);
        }
        break;
    }

    sx1272_set_status(dev, SX1272_RF_RX_RUNNING);

    if (rx_continuous) {
        sx1272_set_op_mode(dev, RFLR_OPMODE_RECEIVER);
    }
    else {
        if (timeout != 0) {
            xtimer_set(&(dev->_internal.rx_timeout_timer), timeout);
        }

        sx1272_set_op_mode(dev, RFLR_OPMODE_RECEIVER_SINGLE);
    }
}

void sx1272_start_cad(sx1272_t *dev)
{
    switch (dev->settings.modem) {
        case SX1272_MODEM_FSK:
        {

        }
        break;
        case SX1272_MODEM_LORA:
        {
            uint32_t reg = RFLR_IRQFLAGS_RXTIMEOUT |
                           RFLR_IRQFLAGS_RXDONE |
                           RFLR_IRQFLAGS_PAYLOADCRCERROR |
                           RFLR_IRQFLAGS_VALIDHEADER |
                           RFLR_IRQFLAGS_TXDONE |
                           RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL;
                           
            /* mask interrupt for CadDone or CadDetect */
            if (cadmode == SX1272_MODE_CADDONE) {
                reg |= RFLR_IRQFLAGS_CADDETECTED;
            } else {
                reg |= RFLR_IRQFLAGS_CADDONE;
            }
                             
            sx1272_reg_write(dev, REG_LR_IRQFLAGSMASK, reg);

            // DIO3 = CADDone
            // DIO4 = CADDetected
            if (cadmode == SX1272_MODE_CADDONE) {
                sx1272_reg_write(dev,
                                 REG_DIOMAPPING1,
                                 (sx1272_reg_read(dev, REG_DIOMAPPING1)
                                  & RFLR_DIOMAPPING1_DIO3_MASK) | RFLR_DIOMAPPING1_DIO3_00);
            } else {
                sx1272_reg_write(dev,
                                 REG_DIOMAPPING2,
                                 (sx1272_reg_read(dev, REG_DIOMAPPING2)
                                  & RFLR_DIOMAPPING2_DIO4_MASK) | RFLR_DIOMAPPING2_DIO4_00);
            }

            sx1272_set_status(dev,  SX1272_RF_CAD);
            sx1272_set_op_mode(dev, RFLR_OPMODE_CAD);
        }
        break;
        default:
            break;
    }
}

int16_t sx1272_read_rssi(sx1272_t *dev)
{
    int16_t rssi = 0;

    switch (dev->settings.modem) {
        case SX1272_MODEM_FSK:
            rssi = -(sx1272_reg_read(dev, REG_RSSIVALUE) >> 1);
            break;
        case SX1272_MODEM_LORA:
            rssi = RSSI_OFFSET + sx1272_reg_read(dev, REG_LR_RSSIVALUE);
            break;
        default:
            rssi = -1;
            break;
    }

    return rssi;
}

void sx1272_reset(sx1272_t *dev)
{
    /*
     * This reset scheme is complies with 7.2 chapter of the SX1272 datasheet
     *
     * 1. Set NReset pin to HIGH for at least 100 us
     * 2. Set NReset in Hi-Z state
     * 3. Wait at least 5 milliseconds
     */

    if (dev->reset_pin != GPIO_UNDEF) {
        gpio_init(dev->reset_pin, GPIO_OUT);

        /* Set reset pin to 0 */
        gpio_clear(dev->reset_pin);

        /* Wait 1 ms */
        rtctimers_millis_sleep(1);

        /* Put reset pin in High-Z */
        gpio_init(dev->reset_pin, GPIO_OD);

        gpio_set(dev->reset_pin);

        /* Wait 10 ms */
        rtctimers_millis_sleep(10);
    }
}

void sx1272_set_op_mode(sx1272_t *dev, uint8_t op_mode)
{
    static uint8_t op_mode_prev = 0;

    op_mode_prev = sx1272_reg_read(dev, REG_OPMODE) & ~RF_OPMODE_MASK;

    if (op_mode != op_mode_prev) {
        /* Replace previous mode value and setup new mode value */
        sx1272_reg_write(dev, REG_OPMODE, (op_mode_prev & RF_OPMODE_MASK) | op_mode);
    }
	
	if (op_mode == RF_OPMODE_SLEEP) {
        if (dev->dio0_pin != GPIO_UNDEF ) {
            gpio_irq_disable(dev->dio0_pin);
        }
        if (dev->dio1_pin != GPIO_UNDEF ) {
            gpio_irq_disable(dev->dio1_pin);
        }
        if (dev->dio2_pin != GPIO_UNDEF ) {
            gpio_irq_disable(dev->dio2_pin);
        }
        if (dev->dio3_pin != GPIO_UNDEF ) {
            gpio_irq_disable(dev->dio3_pin);
        }
        
        /* disable RF switch power */
        if (dev->rfswitch_pin != GPIO_UNDEF) {
            if (dev->rfswitch_mode == SX1272_RFSWITCH_ACTIVE_LOW) {
                gpio_set(dev->rfswitch_pin);
            } else {
                gpio_clear(dev->rfswitch_pin);
            }
        }
	} else {
        if (dev->dio0_pin != GPIO_UNDEF ) {
            gpio_irq_enable(dev->dio0_pin);
        }
        if (dev->dio1_pin != GPIO_UNDEF ) {
            gpio_irq_enable(dev->dio1_pin);
        }
        if (dev->dio2_pin != GPIO_UNDEF ) {
            gpio_irq_enable(dev->dio2_pin);
        }
        if (dev->dio3_pin != GPIO_UNDEF ) {
            gpio_irq_enable(dev->dio3_pin);
        }
        /* enable RF switch power */
        if (dev->rfswitch_pin != GPIO_UNDEF) {
            if (dev->rfswitch_mode == SX1272_RFSWITCH_ACTIVE_LOW) {
                gpio_clear(dev->rfswitch_pin);
            } else {
                gpio_set(dev->rfswitch_pin);
            }
        }
	}
}

void sx1272_set_max_payload_len(sx1272_t *dev, sx1272_radio_modems_t modem, uint8_t maxlen)
{
    sx1272_set_modem(dev, modem);

    switch (modem) {
        case SX1272_MODEM_FSK:
            break;

        case SX1272_MODEM_LORA:
            sx1272_reg_write(dev, REG_LR_PAYLOADMAXLENGTH, maxlen);
            break;
    }
}

/*
 * SPI Register routines
 */

void sx1272_reg_write(sx1272_t *dev, uint8_t addr, uint8_t data)
{
    sx1272_reg_write_burst(dev, addr, &data, 1);
}

uint8_t sx1272_reg_read(sx1272_t *dev, uint8_t addr)
{
    uint8_t data;

    sx1272_reg_read_burst(dev, addr, &data, 1);

    return data;
}

void sx1272_reg_write_burst(sx1272_t *dev, uint8_t addr, uint8_t *buffer,
                            uint8_t size)
{
    unsigned int cpsr;

    spi_acquire(dev->spi);
    cpsr = irq_disable();

    gpio_clear(dev->nss_pin);
    spi_transfer_regs(dev->spi, addr | 0x80, (char *) buffer, NULL, size);
    gpio_set(dev->nss_pin);

    irq_restore(cpsr);
    spi_release(dev->spi);
}

void sx1272_reg_read_burst(sx1272_t *dev, uint8_t addr, uint8_t *buffer,
                           uint8_t size)
{
    unsigned int cpsr;

    cpsr = irq_disable();

    spi_acquire(dev->spi);

    gpio_clear(dev->nss_pin);
    spi_transfer_regs(dev->spi, addr & 0x7F, NULL, (char *) buffer, size);
    gpio_set(dev->nss_pin);

    spi_release(dev->spi);

    irq_restore(cpsr);
}

void sx1272_write_fifo(sx1272_t *dev, uint8_t *buffer, uint8_t size)
{
    sx1272_reg_write_burst(dev, 0, buffer, size);
}

void sx1272_read_fifo(sx1272_t *dev, uint8_t *buffer, uint8_t size)
{
    sx1272_reg_read_burst(dev, 0, buffer, size);
}

/**
 * IRQ handlers
 */
void sx1272_on_dio0_isr(void *arg)
{
    msg_t msg;

    msg.content.value = 0;
    msg_send_int(&msg, ((sx1272_t *)arg)->_internal.dio_polling_thread_pid);
}

void sx1272_on_dio1_isr(void *arg)
{
    msg_t msg;

    msg.content.value = 1;
    msg_send_int(&msg, ((sx1272_t *)arg)->_internal.dio_polling_thread_pid);
}

void sx1272_on_dio2_isr(void *arg)
{
    msg_t msg;

    msg.content.value = 2;
    msg_send_int(&msg, ((sx1272_t *)arg)->_internal.dio_polling_thread_pid);
}

void sx1272_on_dio3_isr(void *arg)
{
    msg_t msg;

    msg.content.value = 3;
    msg_send_int(&msg, ((sx1272_t *)arg)->_internal.dio_polling_thread_pid);
}

void sx1272_on_dio4_isr(void *arg)
{
    msg_t msg;

    msg.content.value = 4;
    msg_send_int(&msg, ((sx1272_t *)arg)->_internal.dio_polling_thread_pid);
}

void sx1272_on_dio5_isr(void *arg)
{
    msg_t msg;

    msg.content.value = 5;
    msg_send_int(&msg, ((sx1272_t *)arg)->_internal.dio_polling_thread_pid);
}

/* Internal event handlers */

void sx1272_on_dio0(void *arg)
{
    sx1272_t *dev = (sx1272_t *) arg;

    volatile uint8_t irq_flags = 0;

    switch (dev->settings.state) {
        case SX1272_RF_RX_RUNNING:
            switch (dev->settings.modem) {
                case SX1272_MODEM_LORA:
                {
                    int8_t snr = 0;

                    /* Clear IRQ */
                    sx1272_reg_write(dev,  REG_LR_IRQFLAGS, RFLR_IRQFLAGS_RXDONE);

                    irq_flags = sx1272_reg_read(dev,  REG_LR_IRQFLAGS);
                    if ((irq_flags & RFLR_IRQFLAGS_PAYLOADCRCERROR_MASK) == RFLR_IRQFLAGS_PAYLOADCRCERROR) {
                        sx1272_reg_write(dev,  REG_LR_IRQFLAGS, RFLR_IRQFLAGS_PAYLOADCRCERROR); /* Clear IRQ */

                        if (!dev->settings.lora.rx_continuous) {
                            sx1272_set_status(dev,  SX1272_RF_IDLE);
                        }

                        xtimer_remove(&dev->_internal.rx_timeout_timer);

                        send_event(dev, SX1272_RX_ERROR_CRC);

                        break;
                    }

                    sx1272_rx_packet_t *packet = &dev->_internal.last_packet;

                    packet->snr_value = sx1272_reg_read(dev,  REG_LR_PKTSNRVALUE);
                    if (packet->snr_value & 0x80) { /* The SNR is negative */
                        /* Invert and divide by 4 */
                        snr = ((~packet->snr_value + 1) & 0xFF) >> 2;
                        snr = -snr;
                    }
                    else {
                        /* Divide by 4 */
                        snr = (packet->snr_value & 0xFF) >> 2;
                    }

                    int16_t rssi = sx1272_reg_read(dev, REG_LR_PKTRSSIVALUE);
                    if (snr < 0) {
                        packet->rssi_value = RSSI_OFFSET + rssi + ( rssi >> 4 ) + snr;
                    }
                    else {
                        packet->rssi_value = RSSI_OFFSET + rssi + ( rssi >> 4 );
                    }

                    packet->size = sx1272_reg_read(dev, REG_LR_RXNBBYTES);

                    if (!dev->settings.lora.rx_continuous) {
                        sx1272_set_status(dev,  SX1272_RF_IDLE);
                    }

                    xtimer_remove(&dev->_internal.rx_timeout_timer);

                    /* Read the last packet from FIFO */
                    uint8_t last_rx_addr = sx1272_reg_read(dev, REG_LR_FIFORXCURRENTADDR);
                    sx1272_reg_write(dev, REG_LR_FIFOADDRPTR, last_rx_addr);
                    sx1272_read_fifo(dev, (uint8_t *) packet->content, packet->size);

                    /* Notify application about new packet received */
                    send_event(dev, SX1272_RX_DONE);
                }
                break;
                default:
                    break;
            }
            break;
        case SX1272_RF_TX_RUNNING:
            xtimer_remove(&dev->_internal.tx_timeout_timer);                /* Clear TX timeout timer */

            sx1272_reg_write(dev, REG_LR_IRQFLAGS, RFLR_IRQFLAGS_TXDONE);   /* Clear IRQ */
            sx1272_set_status(dev,  SX1272_RF_IDLE);

            send_event(dev, SX1272_TX_DONE);
            break;
        default:
            break;
    }
}

void sx1272_on_dio1(void *arg)
{
    /* Get interrupt context */
    sx1272_t *dev = (sx1272_t *) arg;

    switch (dev->settings.state) {
        case SX1272_RF_RX_RUNNING:
            switch (dev->settings.modem) {
                case SX1272_MODEM_LORA:
                    xtimer_remove(&dev->_internal.rx_timeout_timer);

                    sx1272_set_status(dev,  SX1272_RF_IDLE);

                    send_event(dev, SX1272_RX_TIMEOUT);
                    break;
                default:
                    break;
            }
            break;
        case SX1272_RF_TX_RUNNING:
            break;
        default:
            break;
    }
}

void sx1272_on_dio2(void *arg)
{
    /* Get interrupt context */
    sx1272_t *dev = (sx1272_t *) arg;

    switch (dev->settings.state) {
        case SX1272_RF_RX_RUNNING:
            switch (dev->settings.modem) {
                case SX1272_MODEM_LORA:
                    if (dev->settings.lora.freq_hop_on) {
                        /* Clear IRQ */
                        sx1272_reg_write(dev, REG_LR_IRQFLAGS, RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL);

                        dev->_internal.last_channel = sx1272_reg_read(dev, REG_LR_HOPCHANNEL) & RFLR_HOPCHANNEL_CHANNEL_MASK;
                        send_event(dev, SX1272_FHSS_CHANGE_CHANNEL);
                    }

                    break;
                default:
                    break;
            }
            break;
        case SX1272_RF_TX_RUNNING:
            switch (dev->settings.modem) {
                case SX1272_MODEM_FSK:
                    break;
                case SX1272_MODEM_LORA:
                    if (dev->settings.lora.freq_hop_on) {
                        /* Clear IRQ */
                        sx1272_reg_write(dev, REG_LR_IRQFLAGS, RFLR_IRQFLAGS_FHSSCHANGEDCHANNEL);

                        dev->_internal.last_channel = sx1272_reg_read(dev, REG_LR_HOPCHANNEL) & RFLR_HOPCHANNEL_CHANNEL_MASK;
                        send_event(dev, SX1272_FHSS_CHANGE_CHANNEL);
                    }
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

void sx1272_on_dio3(void *arg)
{
    /* Get interrupt context */
    sx1272_t *dev = (sx1272_t *) arg;

    switch (dev->settings.modem) {
        case SX1272_MODEM_FSK:
            break;
        case SX1272_MODEM_LORA:
            /* Clear IRQ */
            sx1272_reg_write(dev, REG_LR_IRQFLAGS, RFLR_IRQFLAGS_CADDETECTED | RFLR_IRQFLAGS_CADDONE);

            /* Send event message */
            dev->_internal.is_last_cad_success = (sx1272_reg_read(dev, REG_LR_IRQFLAGS) & RFLR_IRQFLAGS_CADDETECTED) == RFLR_IRQFLAGS_CADDETECTED;
            send_event(dev, SX1272_CAD_DONE);
            break;
        default:
            break;
    }
}

/* DIO4 may be used for CadDetect events */
void sx1272_on_dio4(void *arg)
{
    /* Get interrupt context */
    sx1272_t *dev = (sx1272_t *) arg;

    switch (dev->settings.modem) {
        case SX1272_MODEM_FSK:
            break;
        case SX1272_MODEM_LORA:
            /* Clear IRQ */
            sx1272_reg_write(dev, REG_LR_IRQFLAGS, RFLR_IRQFLAGS_CADDETECTED | RFLR_IRQFLAGS_CADDONE);

            /* Send event message */
            dev->_internal.is_last_cad_success = (sx1272_reg_read(dev, REG_LR_IRQFLAGS) & RFLR_IRQFLAGS_CADDETECTED) == RFLR_IRQFLAGS_CADDETECTED;
            send_event(dev, SX1272_CAD_DETECTED);
            break;
        default:
            break;
    }
}

void sx1272_on_dio5(void *arg)
{
    (void) arg;
}

void *dio_polling_thread(void *arg)
{

    DEBUG("sx1272: dio polling thread started");

    sx1272_t *dev = (sx1272_t *) arg;
    msg_t msg_queue[8];
    msg_init_queue(msg_queue, 8);

    msg_t msg;

    while (1) {
        msg_receive(&msg);

        uint32_t v = msg.content.value;
        switch (v) {
            case 0:
                sx1272_on_dio0(dev);
                break;

            case 1:
                sx1272_on_dio1(dev);
                break;

            case 2:
                sx1272_on_dio2(dev);
                break;

            case 3:
                sx1272_on_dio3(dev);
                break;

            case 4:
                sx1272_on_dio4(dev);
                break;

            case 5:
                sx1272_on_dio5(dev);
                break;
        }
    }

    return NULL;
}
