/*
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <sys/time.h>
#include <openbmc/ipmi.h>
#include <openbmc/ipmb.h>
#include <openbmc/pal.h>

#define BTN_MAX_SAMPLES   200
#define BTN_POWER_OFF     40
#define MAX_NUM_SLOTS 1
#define HB_SLEEP_TIME (5 * 60)
#define HB_TIMESTAMP_COUNT (60 * 60 / HB_SLEEP_TIME)

#define LED_ON 1
#define LED_OFF 0

#define ID_LED_ON  1
#define ID_LED_OFF 0

#define LED_ON_TIME_IDENTIFY 200
#define LED_OFF_TIME_IDENTIFY 200

#define LED_ON_TIME_HEALTH 900
#define LED_OFF_TIME_HEALTH 100

#define LED_ON_TIME_BMC_SELECT 500
#define LED_OFF_TIME_BMC_SELECT 500

#define IOM_TYPE5 1
#define IOM_TYPE7 2
#define PATH_HEARTBEAT_HEALTH "/tmp/heartbeat_health"
#define BMC_RMT_HB_TIMEOUT_COUNT  1800
#define SCC_LOC_HB_TIMEOUT_COUNT  1800
#define SCC_RMT_HB_TIMEOUT_COUNT  1800
#define BMC_RMT_HB_RPM_LIMIT  0
#define SCC_LOC_HB_RPM_LIMIT  0
#define SCC_RMT_HB_RPM_LIMIT  0

uint8_t g_sync_led[MAX_NUM_SLOTS+1] = {0x0};
unsigned char g_err_code[ERROR_CODE_NUM];

int
write_cache(const char *device, uint8_t value) {
  FILE *fp;
  int rc;

  fp = fopen(device, "w");
  if (!fp) {
    int err = errno;
    return err;
  }

  rc = fprintf(fp, "%d", value);
  fclose(fp);

  if (rc < 0) {
    return ENOENT;
  } else {
    return 0;
  }
}

// Thread for monitoring debug card hotswap
static void *
debug_card_handler() {
  int curr = -1;
  int prev = -1;
  uint8_t prsnt;
  uint8_t pos ;
  uint8_t prev_pos = -1;
  uint8_t lpc;
  int i, ret;
  uint8_t UART_OUT = 1;
  int CURT = 0;

  while (1) {
    // Check if debug card present or not
    #if 1
    ret = pal_is_debug_card_prsnt(&prsnt);
    if (ret) {
      goto debug_card_out;
    }
    #endif
    curr = 1;

    // If Debug Card is present
    if (prsnt) {
      ret = pal_get_hand_sw(&pos);
      if (ret) {
        goto debug_card_out;
      }
      if(pos != prev_pos)
      {
         CURT++;
         prev_pos = pos;
         if( (CURT%2) == 0)
	     {
		    UART_OUT = !UART_OUT;
		    ret = pal_switch_uart_mux(UART_OUT);
	     }
	  }

      // Enable POST code based on hand switch
      if (UART_OUT == HAND_SW_BMC) {
        // For BMC, there is no need to have POST specific code
        goto debug_card_done;
      }

      // Make sure the server at selected position is present
      ret = pal_is_fru_prsnt(1, &prsnt);
      if (ret || !prsnt) {
        goto debug_card_done;
      }
      // Enable POST codes for all slots
      ret = pal_post_enable(1);
      if (ret) {
        goto debug_card_out;
      }

      // Get last post code and display it
      ret = pal_post_get_last(FRU_SLOT1, &lpc);
      if (ret) {
        goto debug_card_out;
      }
      ret = pal_post_handle(FRU_SLOT1, lpc);
      if (ret) {
        goto debug_card_out;
      }
    }

debug_card_done:
    prev = curr;
    prev_pos = pos;
debug_card_out:
    if (prsnt)
      msleep(500);
    else
      sleep(1);
  }
}

// Thread to monitor the hand switch
static void *
usb_handler() {
  int curr = -1;
  int prev = -1;
  int ret;
  uint8_t pos;
  uint8_t prsnt;
  uint8_t lpc;

  while (1) {
    // Get the current hand switch position
    ret = pal_get_hand_sw(&pos);
    if (ret) {
      goto hand_sw_out;
    }
    curr = pos;
    if (curr == prev) {
      // No state change, continue;
      goto hand_sw_out;
    }

    // Switch USB Mux to selected server
    ret = pal_switch_usb_mux(pos);
    if (ret) {
      goto hand_sw_out;
    }

    prev = curr;
hand_sw_out:
    sleep(1);
    continue;
  }
}

// Thread to monitor Reset Button and propagate to selected server
static void *
rst_btn_handler() {
  int ret;
  uint8_t pos;
  int i;
  uint8_t btn;

  while (1) {
    // Check if reset button is pressed
    ret = pal_get_rst_btn(&btn);
    if (ret || !btn) {
      goto rst_btn_out;
    }

    // Pass the reset button to the selected slot
    syslog(LOG_WARNING, "Reset button pressed\n");
    ret = pal_set_rst_btn(FRU_SLOT1, 0);
    if (ret) {
      goto rst_btn_out;
    }

    // Wait for the button to be released
    for (i = 0; i < BTN_MAX_SAMPLES; i++) {
      ret = pal_get_rst_btn(&btn);
      if (ret || btn) {
        msleep(100);
        continue;
      }
      pal_update_ts_sled();
      syslog(LOG_WARNING, "Reset button released\n");
      syslog(LOG_CRIT, "Reset Button pressed for FRU: %d\n", pos);
      ret = pal_set_rst_btn(FRU_SLOT1, 1);
      goto rst_btn_out;
    }

    // handle error case
    if (i == BTN_MAX_SAMPLES) {
      pal_update_ts_sled();
      syslog(LOG_WARNING, "Reset button seems to stuck for long time\n");
      goto rst_btn_out;
    }
rst_btn_out:
    msleep(100);
  }
}

// Thread to handle Power Button and power on/off the selected server
static void *
pwr_btn_handler() {
  int ret;
  uint8_t pos, btn, cmd;
  int i;
  uint8_t power;

  while (1) {
    // Check if power button is pressed
    ret = pal_get_pwr_btn(&btn);
    if (ret || !btn) {
      goto pwr_btn_out;
    }

    syslog(LOG_WARNING, "power button pressed\n");
    // Wait for the button to be released
    for (i = 0; i < BTN_POWER_OFF; i++) {
      ret = pal_get_pwr_btn(&btn);
      if (ret || btn ) {
        msleep(100);
        continue;
      }
      syslog(LOG_WARNING, "power button released\n");
      break;
    }


    // Get the current power state (power on vs. power off)
    ret = pal_get_server_power(FRU_SLOT1, &power);
    if (ret) {
      goto pwr_btn_out;
    }

    // Set power command should reverse of current power state
    cmd = !power;

    // To determine long button press
    if (i >= BTN_POWER_OFF) {
      pal_update_ts_sled();
      syslog(LOG_CRIT, "Power Button Long Press for FRU: %d\n", pos);
    } else {

      // If current power state is ON and it is not a long press,
      // the power off should be Graceful Shutdown
      if (power == SERVER_POWER_ON)
        cmd = SERVER_GRACEFUL_SHUTDOWN;

      pal_update_ts_sled();
      syslog(LOG_CRIT, "Power Button Press for FRU: %d\n", pos);
    }

    // Reverse the power state of the given server
    ret = pal_set_server_power(FRU_SLOT1, cmd);
pwr_btn_out:
    msleep(100);
  }
}

// Thread to monitor SLED Cycles by using time stamp
static void *
ts_handler() {
  int count = 0;
  struct timespec ts;
  struct timespec mts;
  char tstr[64] = {0};
  char buf[128] = {0};
  uint8_t por = 0;
  uint8_t time_init = 0;
  long time_sled_on;
  long time_sled_off;

  // Read the last timestamp from KV storage
  pal_get_key_value("timestamp_sled", tstr);
  time_sled_off = (long) strtoul(tstr, NULL, 10);

  // If this reset is due to Power-On-Reset, we detected SLED power OFF event
  if (pal_is_bmc_por()) {
    ctime_r(&time_sled_off, buf);
    syslog(LOG_CRIT, "SLED Powered OFF at %s", buf);
  }

  while (1) {

    // Make sure the time is initialized properly
    // Since there is no battery backup, the time could be reset to build time
    if (time_init == 0) {
      // Read current time
      clock_gettime(CLOCK_REALTIME, &ts);

      if (ts.tv_sec < time_sled_off) {
        sleep(1);
        continue;
      }

      // If current time is more than the stored time, the date is correct
      time_init = 1;
      // Need to log SLED ON event, if this is Power-On-Reset
      if (pal_is_bmc_por()) {
        // Get uptime
        clock_gettime(CLOCK_MONOTONIC, &mts);
        // To find out when SLED was on, subtract the uptime from current time
        time_sled_on = ts.tv_sec - mts.tv_sec;

        ctime_r(&time_sled_on, buf);
        // Log an event if this is Power-On-Reset
        syslog(LOG_CRIT, "SLED Powered ON at %s", buf);
      }
    }

    // Store timestamp every one hour to keep track of SLED power
    if (count++ == HB_TIMESTAMP_COUNT) {
      pal_update_ts_sled();
      count = 0;
    }

    sleep(HB_SLEEP_TIME);
  }
}

// Thread to handle LED state of the server at given slot
static void *
led_handler(void *num) {
  int ret;
  uint8_t prsnt;
  uint8_t power;
  uint8_t pos;
  uint8_t led_blink;
  uint8_t ident = 0;
  int led_on_time, led_off_time;
  char identify[16] = {0};
  char tstr[64] = {0};
  int power_led_on_time = 500;
  int power_led_off_time = 500;
  uint8_t hlth = 0;

  uint8_t slot = (*(int*) num) + 1;

#ifdef DEBUG
  syslog(LOG_INFO, "led_handler for slot %d\n", slot);
#endif

  while (1) {
    // Check if this LED is managed by sync_led thread
    if (g_sync_led[slot]) {
      sleep(1);
      continue;
    }

    // Get power status for this slot
    ret = pal_get_server_power(slot, &power);
    if (ret) {
      sleep(1);
      continue;
    }

    // Get health status for this slot
    ret = pal_get_fru_health(slot, &hlth);
    if (ret) {
      sleep(1);
      continue;
    }

    led_blink = 0;  // solid on/off

    //If no identify: Set LEDs based on power and hlth status
    if (!led_blink) {
		pal_set_led(slot, LED_ON);
      goto led_handler_out;
    }

    // Set blink rate
    if (power == SERVER_POWER_ON) {
      led_on_time = 900;
      led_off_time = 100;
    } else {
      led_on_time = 100;
      led_off_time = 900;
    }

    // Start blinking the LED
    if (hlth == FRU_STATUS_GOOD) {
      pal_set_led(slot, LED_ON);
    }

    msleep(led_on_time);

    if (hlth == FRU_STATUS_GOOD) {
      pal_set_led(slot, LED_OFF);
    }

    msleep(led_off_time);
led_handler_out:
    msleep(100);
  }
}

// Thread to handle LED state of the SLED
static void *
led_sync_handler() {
  int ret;
  uint8_t pos;
  uint8_t ident = 0;
  char identify[16] = {0};
  char tstr[64] = {0};
  char id_arr[5] = {0};
  uint8_t slot;

#ifdef DEBUG
  syslog(LOG_INFO, "led_handler for slot %d\n", slot);
#endif
  pal_set_led(slot, LED_ON);
  
  while (1) {
    // Check if slot needs to be identified
    ident = 0;
    for (slot = 1; slot <= MAX_NUM_SLOTS; slot++)  {
      id_arr[slot] = 0x0;
      sprintf(tstr, "identify_slot%d", slot);
      memset(identify, 0x0, 16);
      ret = pal_get_key_value(tstr, identify);
      if (ret == 0 && !strcmp(identify, "on")) {
        id_arr[slot] = 0x1;
        ident = 1;
      }
    }

    // Handle BMC select condition when no slot is being identified
    if (ident == 1) {
      // Start blinking Blue LED
      for (slot = 1; slot <= MAX_NUM_SLOTS; slot++) {
        g_sync_led[slot] = 1;
        pal_set_led(slot, LED_ON);
      }

      msleep(LED_ON_TIME_BMC_SELECT);

      for (slot = 1; slot <= MAX_NUM_SLOTS; slot++) {
        pal_set_led(slot, LED_OFF);
      }

      msleep(LED_OFF_TIME_BMC_SELECT);
      continue;
    }
    for (slot = 1; slot <= MAX_NUM_SLOTS; slot++) {
      g_sync_led[slot] = 0;
    }
    msleep(200);
  }
}

// Thread for handling the Enclosure LED
static void *
encl_led_handler() {
  uint8_t slot1_hlth;
  uint8_t iom_hlth;
  uint8_t dpb_hlth;
  uint8_t scc_hlth;
  uint8_t nic_hlth;
  int ret;
  int i;

  // Initial error code
  memset(g_err_code, 0, sizeof(unsigned char) * ERROR_CODE_NUM);
  pal_fault_led(ID_LED_OFF, 0);
  
  while (1) {
    // Get health status for all the fru and then update the ENCL_LED status
    ret = pal_get_fru_health(FRU_SLOT1, &slot1_hlth);
    if (ret) {
      pal_err_code_enable(0xF7);
    }
    else {
      if (!slot1_hlth) {
        pal_err_code_enable(0xF7);
      }
      else {
        pal_err_code_disable(0xF7);
      }
    }

    ret = pal_get_fru_health(FRU_IOM, &iom_hlth);
    if (ret) {
      pal_err_code_enable(0xF8);
    }
    else {
      if (!iom_hlth) {
        pal_err_code_enable(0xF8);
      }
      else {
        pal_err_code_disable(0xF8);
      }
    }

    ret = pal_get_fru_health(FRU_DPB, &dpb_hlth);
    if (ret) {
      pal_err_code_enable(0xF9);
    }
    else {
      if (!dpb_hlth) {
        pal_err_code_enable(0xF9);
      }
      else {
        pal_err_code_disable(0xF9);
      }
    }

    ret = pal_get_fru_health(FRU_SCC, &scc_hlth);
    if (ret) {
      pal_err_code_enable(0xFA);
    }
    else {
      if (!scc_hlth) {
        pal_err_code_enable(0xFA);
      }
      else {
        pal_err_code_disable(0xFA);
      }
    }

    ret = pal_get_fru_health(FRU_NIC, &nic_hlth);
    if (ret) {
      pal_err_code_enable(0xFB);
    }
    else {
      if (!nic_hlth) {
        pal_err_code_enable(0xFB);
      }
      else {
        pal_err_code_disable(0xFB);
      }
    }

    if(pal_sum_error_code() == 1) {   // error occur
      //pal_fault_led(ID_LED_ON, 0);
    }
    else {
      //pal_fault_led(ID_LED_OFF, 0);
    }
    sleep(1);
  }
}

// Thread for handling the BMC and SCC heartbeat stauts
static void *
hb_mon_handler() {
  int bmc_rmt_hb_value = -1;
  int scc_loc_hb_value = -1;
  int scc_rmt_hb_value = -1;
  int count_bmc_rmt = 0;
  int count_scc_loc = 0;
  int count_scc_rmt = 0;
  int curr_bmc_rmt_status = -1;
  int curr_scc_loc_status = -1;
  int curr_scc_rmt_status = -1;
  int prev_bmc_rmt_status = -1;
  int prev_scc_loc_status = -1;
  int prev_scc_rmt_status = -1;
  uint8_t iom_type = 0;
  uint8_t scc_rmt_type = 0;
  uint8_t hb_health = 0;

  // Get remote SCC and IOM type to identify IOM is M.2 or IOC solution
  iom_type = pal_get_iom_type();
  scc_rmt_type = ((pal_get_sku() >> 6) & 0x1);
  // Type 5
  if (scc_rmt_type == 0) {
    if (iom_type == IOM_TYPE7) {
      syslog(LOG_CRIT, "The chassis type is type V, the IOM type is IOC solution. The IOM does not match in this chassis. Default monitor type V HB.");
    }
    else if (iom_type != IOM_TYPE5) {
      syslog(LOG_CRIT, "The chassis type is type V, the IOM type is unable to identify. Default monitor type V HB.");
    }
    iom_type = IOM_TYPE5;
    hb_health = (1 << 5) | (1 << 4);
  }
  // Type 7
  else {
    if (iom_type == IOM_TYPE5) {
      syslog(LOG_CRIT, "The chassis type is type VII, the IOM type is M.2 solution. The IOM does not match in this chassis. Default monitor type VII HB.");
    }
    else if (iom_type != IOM_TYPE7) {
      syslog(LOG_CRIT, "The chassis type is type VII, the IOM type is unable to identify. Default monitor type VII HB");
    }
    iom_type = IOM_TYPE7;
    hb_health = (1 << 4) | (1 << 3);
  }

  // Update heartbeat present bits [5:3] = {BMC_RMT, SCC_LOC, SCC_RMT}
  write_cache(PATH_HEARTBEAT_HEALTH, hb_health);

  while(1) {
    // Heartbeat health bits [2:0] = {BMC_RMT, SCC_LOC, SCC_RMT}
    // Diagnosis flow:
    //   1. Get heartbeat
    //   2. Timeout detect: heartbeat has no response continuous 3 minutes. 1800 = 3 * 60 * 1000 / 100
    //   3. Cache heartbeat health in /tmp
    //   4. Update heartbeat status

    // BMC remote heartbeat
    if (iom_type == IOM_TYPE5) {
      // Get heartbeat
      bmc_rmt_hb_value = pal_get_bmc_rmt_hb();
      if (bmc_rmt_hb_value <= BMC_RMT_HB_RPM_LIMIT) {
        count_bmc_rmt++;
      }
      else {
        count_bmc_rmt = 0;
      }
      // Timeout Detect
      if (count_bmc_rmt > BMC_RMT_HB_TIMEOUT_COUNT) {
        curr_bmc_rmt_status = 0;
      }
      else {
        curr_bmc_rmt_status = 1;
      }
      // Cache heartbeat health in /tmp
      if (curr_bmc_rmt_status == 0) {
        hb_health = hb_health | (1 << 2);
        if (curr_bmc_rmt_status != prev_bmc_rmt_status) {
          syslog(LOG_CRIT, "BMC remote heartbeat is abnormal");
        }
        pal_err_code_enable(0xFC);
      }
      else {
        hb_health = hb_health & (~(1 << 2));
        pal_err_code_disable(0xFC);
      }
      // Update heartbeat status
      prev_bmc_rmt_status = curr_bmc_rmt_status;
    }

    // SCC local heartbeat
    if ((iom_type == IOM_TYPE5) || (iom_type == IOM_TYPE7)) {
      scc_loc_hb_value = pal_get_scc_loc_hb();
      if (scc_loc_hb_value <= SCC_LOC_HB_RPM_LIMIT) {
        count_scc_loc++;
      }
      else {
        count_scc_loc = 0;
      }
      if (count_scc_loc > SCC_LOC_HB_TIMEOUT_COUNT) {
        curr_scc_loc_status = 0;
      }
      else {
        curr_scc_loc_status = 1;
      }
      if (curr_scc_loc_status == 0) {
        hb_health = hb_health | (1 << 1);
        if (curr_scc_loc_status != prev_scc_loc_status) {
          syslog(LOG_CRIT, "SCC local heartbeat is abnormal");
        }
        pal_err_code_enable(0xFD);
      }
      else {
        hb_health = hb_health & (~(1 << 1));
        pal_err_code_disable(0xFD);
      }
      prev_scc_loc_status = curr_scc_loc_status;
    }

    // SCC remote heartbeat
    if (iom_type == IOM_TYPE7) {
      scc_rmt_hb_value = pal_get_scc_rmt_hb();
      if (scc_rmt_hb_value <= SCC_RMT_HB_RPM_LIMIT) {
        count_scc_rmt++;
      }
      else {
        count_scc_rmt = 0;
      }
      if (count_scc_rmt > SCC_RMT_HB_TIMEOUT_COUNT) {
        curr_scc_rmt_status = 0;
      }
      else {
        curr_scc_rmt_status = 1;
      }
      if (curr_scc_rmt_status == 0) {
        hb_health = hb_health | (1 << 0);
        if (curr_scc_rmt_status != prev_scc_rmt_status) {
          syslog(LOG_CRIT, "SCC remote heartbeat is abnormal");
        }
        pal_err_code_enable(0xFE);
      }
      else {
        hb_health = hb_health & (~(1 << 0));
        pal_err_code_disable(0xFE);
      }
      prev_scc_rmt_status = curr_scc_rmt_status;
    }
    write_cache(PATH_HEARTBEAT_HEALTH, hb_health);

    msleep(100);
  }
}

int
main (int argc, char * const argv[]) {
  pthread_t tid_debug_card;
  pthread_t tid_rst_btn;
  pthread_t tid_pwr_btn;
  pthread_t tid_ts;
  pthread_t tid_sync_led;
  pthread_t tid_leds[MAX_NUM_SLOTS];
  pthread_t tid_encl_led;
  pthread_t tid_hb_mon;
  int i;
  int *ip;
  int rc;
  int pid_file;

  pid_file = open("/var/run/front-paneld.pid", O_CREAT | O_RDWR, 0666);
  rc = flock(pid_file, LOCK_EX | LOCK_NB);
  if(rc) {
    if(EWOULDBLOCK == errno) {
      printf("Another front-paneld instance is running...\n");
      exit(-1);
    }
  } else {
   daemon(0, 1);
   openlog("front-paneld", LOG_CONS, LOG_DAEMON);
  }


  if (pthread_create(&tid_debug_card, NULL, debug_card_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for debug card error\n");
    exit(1);
  }

  if (pthread_create(&tid_rst_btn, NULL, rst_btn_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for reset button error\n");
    exit(1);
  }

  if (pthread_create(&tid_pwr_btn, NULL, pwr_btn_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for power button error\n");
    exit(1);
  }

  if (pthread_create(&tid_ts, NULL, ts_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for time stamp error\n");
    exit(1);
  }

  if (pthread_create(&tid_sync_led, NULL, led_sync_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for led sync error\n");
    exit(1);
  }

  for (i = 0; i < MAX_NUM_SLOTS; i++) {
    ip = malloc(sizeof(int));
    *ip = i;
    if (pthread_create(&tid_leds[i], NULL, led_handler, (void*)ip) < 0) {
      syslog(LOG_WARNING, "pthread_create for led error\n");
      exit(1);
    }
  }

  if (pthread_create(&tid_encl_led, NULL, encl_led_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for encl led error\n");
    exit(1);
  }

  if (pthread_create(&tid_hb_mon, NULL, hb_mon_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for heartbeat error\n");
    exit(1);
  }

  pthread_join(tid_debug_card, NULL);
  pthread_join(tid_rst_btn, NULL);
  pthread_join(tid_pwr_btn, NULL);
  pthread_join(tid_ts, NULL);
  pthread_join(tid_sync_led, NULL);
  for (i = 0;  i < MAX_NUM_SLOTS; i++) {
    pthread_join(tid_leds[i], NULL);
  }
  pthread_join(tid_encl_led, NULL);
  pthread_join(tid_hb_mon, NULL);

  return 0;
}