/*
 * sensor.c
 *
 *  Created on: Feb 8, 2016
 *      Author: petera
 */

#include "system.h"
#include "sensor.h"
#include "app.h"
#include "adxl345_driver.h"
#include "hmc5883l_driver.h"
#include "itg3200_driver.h"
#include "gpio.h"
#include "miniutils.h"
#include "taskq.h"

#define I2C_BUS               (_I2C_BUS(0))
#define I2C_CLK               (400000)

static adxl345_dev acc_dev;
static hmc5883l_dev mag_dev;
static itg3200_dev gyr_dev;
static union {
  bool check_id;
  adxl_status acc_status;
  struct {
    adxl_reading acc;
    hmc_reading mag;
    itg_reading gyr;
  } data;
} result;

static task_mutex i2c_mutex = TASK_MUTEX_INIT;
static task_timer sensor_timer;
static task *sensor_task;
static task *acc_sr_task;
static task *gyr_temp_task;
static volatile bool acc_sr_read_bsy = FALSE;
static volatile bool temp_read_bsy = FALSE;
static volatile enum {
  SENS_IDLE = 0,
  SENS_READ_SR,
  SENS_READ_DATA,
  SENS_READ_TEMP,
  SENS_CONFIG
} state = SENS_IDLE;



static adxl_cfg acc_cfg = {
    .pow_low_power = FALSE,
    .pow_rate = ADXL345_RATE_12_5_LP,
    .pow_link = TRUE,
    .pow_auto_sleep = TRUE,
    .pow_mode = ADXL345_MODE_MEASURE,
    .pow_sleep = FALSE,
    .pow_sleep_rate = ADXL345_SLEEP_RATE_8,

    .tap_ena = ADXL345_XYZ,
    .tap_thresh = 20, //*62.5mg
    .tap_dur = 15, //*625us
    .tap_latent = 80, //*1.25ms
    .tap_window = 200, //*1.25ms
    .tap_suppress = FALSE,

    .act_ac_dc = ADXL345_DC,
    .act_ena = ADXL345_XYZ,
    .act_inact_ena = ADXL345_XYZ,
    .act_thr_act = 18, //*62.5mg
    .act_thr_inact = 21, //*62.5mg
    .act_time_inact = 10, //*1s

    .freefall_thresh = 7, //*62.5mg
    .freefall_time = 40, //*5ms

    .int_ena = ADXL345_INT_SINGLE_TAP|ADXL345_INT_DOUBLE_TAP|ADXL345_INT_ACTIVITY|ADXL345_INT_INACTIVITY|ADXL345_INT_FREE_FALL,
    .int_map = 0b00000000,

    .format_int_inv = FALSE,
    .format_full_res = TRUE,
    .format_justify = TRUE,
    .format_range = ADXL345_RANGE_2G,

    .fifo_mode = ADXL345_FIFO_BYPASS,
    .fifo_trigger = ADXL345_PIN_INT2,
    .fifo_samples = 0,
};

static itg_cfg gyr_cfg = {
    .samplerate_div = 0,
    .pwr_clk = ITG3200_CLK_INTERNAL,
    .pwr_reset = ITG3200_NO_RESET,
    .pwr_sleep = ITG3200_ACTIVE,
    .pwr_stdby_x = ITG3200_STNDBY_X_OFF,
    .pwr_stdby_y = ITG3200_STNDBY_Y_OFF,
    .pwr_stdby_z = ITG3200_STNDBY_Z_OFF,
    .lp_filter_rate = ITG3200_LP_5,
    .int_act = ITG3200_INT_ACTIVE_HI,
    .int_clr = ITG3200_INT_CLR_ANY,
    .int_data = ITG3200_INT_NO_DATA,
    .int_pll = ITG3200_INT_NO_PLL,
    .int_latch_pulse = ITG3200_INT_50US_PULSE,
    .int_odpp = ITG3200_INT_OPENDRAIN,
};


//
// i2c devices callbacks
//

static void acc_cb_irq(adxl345_dev *dev, adxl_state s, int res) {
  switch (state) {
  case SENS_READ_DATA:
    if (res != I2C_OK) {
      DBG(D_APP, D_DEBUG, "sens read acc data err: %i\n", res);
      state = SENS_IDLE;
      TASK_mutex_unlock(&i2c_mutex);
    } else {
      res = hmc_read(&mag_dev, &result.data.mag);
      if (res != I2C_OK) {
        DBG(D_APP, D_DEBUG, "sens read mag data call err: %i\n", res);
        state = SENS_IDLE;
        TASK_mutex_unlock(&i2c_mutex);
      }
    }
    break;
  case SENS_READ_SR:
    acc_sr_read_bsy = FALSE;
    state = SENS_IDLE;
    TASK_mutex_unlock(&i2c_mutex);
    if (res != I2C_OK)
      DBG(D_APP, D_WARN, "sens read sr err: %i\n", res);
    else
      DBG(D_APP, D_DEBUG, "sens adxl state:\n"
          "  int raw       : %08b\n"
          "  int dataready : %i\n"
          "  int activity  : %i\n"
          "  int inactivity: %i\n"
          "  int sgl tap   : %i\n"
          "  int dbl tap   : %i\n"
          "  int freefall  : %i\n"
          "  int overrun   : %i\n"
          "  int watermark : %i\n"
          "  acttapsleep   : %08b\n"
          "  act x y z     : %i %i %i\n"
          "  tap x y z     : %i %i %i\n"
          "  sleep         : %i\n"
          "  fifo trigger  : %i\n"
          "  entries       : %i\n"
          ,
          result.acc_status.int_src,
          (result.acc_status.int_src & ADXL345_INT_DATA_READY) != 0,
          (result.acc_status.int_src & ADXL345_INT_ACTIVITY) != 0,
          (result.acc_status.int_src & ADXL345_INT_INACTIVITY) != 0,
          (result.acc_status.int_src & ADXL345_INT_SINGLE_TAP) != 0,
          (result.acc_status.int_src & ADXL345_INT_DOUBLE_TAP) != 0,
          (result.acc_status.int_src & ADXL345_INT_FREE_FALL) != 0,
          (result.acc_status.int_src & ADXL345_INT_OVERRUN) != 0,
          (result.acc_status.int_src & ADXL345_INT_WATERMARK) != 0,
          result.acc_status.act_tap_status,
          result.acc_status.act_tap_status.act_x,
          result.acc_status.act_tap_status.act_y,
          result.acc_status.act_tap_status.act_z,
          result.acc_status.act_tap_status.tap_x,
          result.acc_status.act_tap_status.tap_y,
          result.acc_status.act_tap_status.tap_z,
          result.acc_status.act_tap_status.asleep,
          result.acc_status.fifo_status.fifo_trig,
          result.acc_status.fifo_status.entries
          );
    break;
  case SENS_CONFIG:
    state = SENS_IDLE;
    break;
  default:
    ASSERT(FALSE);
    break;
  }
}

static void mag_cb_irq(hmc5883l_dev *dev, hmc_state s, int res) {
  switch (state) {
  case SENS_READ_DATA:
    if (res != I2C_OK) {
      DBG(D_APP, D_WARN, "sens read mag data err: %i\n", res);
      state = SENS_IDLE;
      TASK_mutex_unlock(&i2c_mutex);
    } else {
      res = itg_read_data(&gyr_dev, &result.data.gyr);
      if (res != I2C_OK) {
        DBG(D_APP, D_WARN, "sens read gyr data call err: %i\n", res);
        state = SENS_IDLE;
        TASK_mutex_unlock(&i2c_mutex);
      }
    }
    break;
  case SENS_CONFIG:
    state = SENS_IDLE;
    break;
  default:
    ASSERT(FALSE);
    break;
  }
}

static void gyr_cb_irq(itg3200_dev *dev, itg_state s, int res) {
  switch (state) {
  case SENS_READ_TEMP:
  case SENS_READ_DATA:
    TASK_mutex_unlock(&i2c_mutex);
    if (res != I2C_OK) {
      DBG(D_APP, D_WARN, "sens read mag data err: %i\n", res);
    } else {
      if (state == SENS_READ_TEMP) {
        temp_read_bsy = FALSE;
        float ftemp = (float)result.data.gyr.temp / 280.0 + 82;
        DBG(D_APP, D_DEBUG, "sensor temp:%i (%i.%i°C)\n", result.data.gyr.temp, (int)(ftemp), (int)((ftemp - (int)ftemp)* 10.0));
        print("sensor temp:%i (%i.%i°C)\n", result.data.gyr.temp, (int)(ftemp), (int)((ftemp - (int)ftemp)* 10.0));
      } else {
        DBG(D_APP, D_DEBUG, "sensor data acc:%04x %04x %04x mag:%04x %04x %04x gyr:%04x %04x %04x\n",
            result.data.acc.x, result.data.acc.y, result.data.acc.z,
            result.data.mag.x, result.data.mag.y, result.data.mag.z,
            result.data.gyr.x, result.data.gyr.y, result.data.gyr.z);
      }
    }
    state = SENS_IDLE;
    break;
  case SENS_CONFIG:
    state = SENS_IDLE;
    break;
  default:
    ASSERT(FALSE);
    break;
  }

}

//
// accelerometer interrupt pin
//

static void acc_pin_irq(gpio_pin pin) {
  DBG(D_APP, D_DEBUG, "sens acc pin irq\n");
  if (!acc_sr_read_bsy) {
    acc_sr_read_bsy = TRUE;
    TASK_run(acc_sr_task, 0, NULL);
  }
}

//
// sensor task functions
//

static void sensor_read(u32_t a, void *p) {
  if (!TASK_mutex_lock(&i2c_mutex)) return;
  state = SENS_READ_DATA;
  int res = adxl_read_data(&acc_dev, &result.data.acc);
  if (res != I2C_OK) {
    DBG(D_APP, D_WARN, "sens read acc data call err: %i\n", res);
    state = SENS_IDLE;
    TASK_mutex_unlock(&i2c_mutex);
  }
}

static void acc_sr_read(u32_t a, void *p) {
  if (!TASK_mutex_lock(&i2c_mutex)) return;
  state = SENS_READ_SR;
  int res = adxl_read_status(&acc_dev, &result.acc_status);
  if (res != I2C_OK) {
    DBG(D_APP, D_WARN, "sens read sr call err: %i\n", res);
    acc_sr_read_bsy = FALSE;
    state = SENS_IDLE;
    TASK_mutex_unlock(&i2c_mutex);
  }
}

static void gyr_temp_read(u32_t a, void *p) {
  if (!TASK_mutex_lock(&i2c_mutex)) return;
  state = SENS_READ_TEMP;
  int res = itg_read_data(&gyr_dev, &result.data.gyr);
  if (res != I2C_OK) {
    DBG(D_APP, D_WARN, "sens read temp call err: %i\n", res);
    state = SENS_IDLE;
    temp_read_bsy = FALSE;
    TASK_mutex_unlock(&i2c_mutex);
  }
}

//
// sensor API
//

void SENS_init(void) {
  int res;
  // setup tasks
  sensor_task = TASK_create(sensor_read, TASK_STATIC);
  ASSERT(sensor_task);
  acc_sr_task = TASK_create(acc_sr_read, TASK_STATIC);
  ASSERT(acc_sr_task);
  gyr_temp_task = TASK_create(gyr_temp_read, TASK_STATIC);
  ASSERT(gyr_temp_task);

  DBG(D_APP, D_DEBUG, "sens open devices\n");
  // open all devices
  adxl_open(&acc_dev, I2C_BUS, I2C_CLK, acc_cb_irq);
  hmc_open(&mag_dev, I2C_BUS, I2C_CLK, mag_cb_irq);
  itg_open(&gyr_dev, I2C_BUS, FALSE, I2C_CLK, gyr_cb_irq);

  // check that devices are online
  DBG(D_APP, D_DEBUG, "sens check all online\n");
  int ok_ids = 0, i;
  for (i = 0; i < 3; i++) {
    state = SENS_CONFIG;
    res = adxl_check_id(&acc_dev, &result.check_id);
    ASSERT(res == I2C_OK);
    while (state == SENS_CONFIG) __WFI();
    if (result.check_id) ok_ids++;

    state = SENS_CONFIG;
    res = hmc_check_id(&mag_dev, &result.check_id);
    ASSERT(res == I2C_OK);
    while (state == SENS_CONFIG) __WFI();
    if (result.check_id) ok_ids++;

    state = SENS_CONFIG;
    res = itg_check_id(&gyr_dev, &result.check_id);
    ASSERT(res == I2C_OK);
    while (state == SENS_CONFIG) __WFI();
    if (result.check_id) ok_ids++;
  }
  ASSERT(ok_ids >= 7);

  DBG(D_APP, D_DEBUG, "sens configure\n");
  // config acc
  state = SENS_CONFIG;
  res = adxl_config(&acc_dev, &acc_cfg);
  ASSERT(res == I2C_OK);
  while (state == SENS_CONFIG) __WFI();

  // config mag
  state = SENS_CONFIG;
  res = hmc_config(&mag_dev,
      hmc5883l_mode_continuous,
      hmc5883l_i2c_speed_normal,
      hmc5883l_gain_1_3,
      hmc5883l_measurement_mode_normal,
      hmc5883l_data_output_35,
      hmc5883l_samples_avg_2
      );
  ASSERT(res == I2C_OK);
  while (state == SENS_CONFIG) __WFI();

  // config gyr
  state = SENS_CONFIG;
  res = itg_config(&gyr_dev, &gyr_cfg);
  ASSERT(res == I2C_OK);
  while (state == SENS_CONFIG) __WFI();

  DBG(D_APP, D_DEBUG, "sens setup pin irq\n");
  // config gpio
  gpio_config(PIN_ACC_INT, CLK_10MHZ, IN, AF0, OPENDRAIN, PULLDOWN);
  gpio_interrupt_config(PIN_ACC_INT, acc_pin_irq, FLANK_UP);
  gpio_interrupt_mask_enable(PIN_ACC_INT, TRUE);

  DBG(D_APP, D_DEBUG, "sens setup ok\n");
}

void SENS_enter_active(void) {
  APP_claim(CLAIM_SEN);
  TASK_start_timer(sensor_task, &sensor_timer, 0, 0, 0, 100, "sensor");
}

void SENS_enter_idle(void) {
  TASK_stop_timer(&sensor_timer);
  hmc_config(&mag_dev,
      hmc5883l_mode_idle,
      hmc5883l_i2c_speed_normal,
      hmc5883l_gain_1_3,
      hmc5883l_measurement_mode_normal,
      hmc5883l_data_output_15,
      hmc5883l_samples_avg_1
      );
  gyr_cfg.pwr_sleep = ITG3200_LOW_POWER;
  itg_config(&gyr_dev, ITG3200_INT_ACTIVE_HI);
}

void SENS_read_temp(void) {
  if (temp_read_bsy) return;
  temp_read_bsy = TRUE;
  TASK_run(gyr_temp_task, 0, NULL);
}
