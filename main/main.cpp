/*

  Main module

  # Modified by Kyle T. Gabriel to fix issue with incorrect GPS data for TTNMapper

  Copyright (C) 2018 by Xose Pérez <xose dot perez at gmail dot com>

  Extended by Stefan Westphal <stefan at westphal dot dev> to support web
  configuration

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "main.h"
#include "configuration.h"
#include "rom/rtc.h"
#include "screen.h"
#include "log.h"
#include "gps.h"
#include "ttn.h"
#include "sleep.h"
#include "webserver.h"
#include <Arduino.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>

#include "axp20x.h"

bool packetSent, packetQueued;

#if defined(PAYLOAD_USE_FULL)
// includes number of satellites and accuracy
static uint8_t txBuffer[10];
#elif defined(PAYLOAD_USE_CAYENNE)
// CAYENNE DF
static uint8_t txBuffer[11] = {0x03, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
#endif

bool ssd1306_found = false;
bool axp192_found = false;

AXP20X_Class axp;

bool pmu_irq = false;
String baChStatus = "No charging";

// deep sleep support
RTC_DATA_ATTR int bootCount = 0;
esp_sleep_source_t wakeCause; // the reason we booted this time

// -----------------------------------------------------------------------------
// Application
// -----------------------------------------------------------------------------

/**
 * If we have a valid position send it to the server.
 * @return true if we decided to send.
 */
bool trySend()
{
  packetSent = false;
  // We also wait for altitude being not exactly zero, because the GPS chip generates a bogus 0 alt report when first powered on
  if (0 < gps_hdop() && gps_hdop() < 50 && gps_latitude() != 0 && gps_longitude() != 0 && gps_altitude() != 0)
  {
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "Latitude: %10.6f\n", gps_latitude());
    screen_print(buffer);
    snprintf(buffer, sizeof(buffer), "Longitude: %10.6f\n", gps_longitude());
    screen_print(buffer);
    snprintf(buffer, sizeof(buffer), "Error: %4.2fm\n", gps_hdop());
    screen_print(buffer);

    buildPacket(txBuffer);

    bool confirmed = config.loraConfirmedEvery > 0 ? (ttn_get_count() % config.loraConfirmedEvery == 0) : false;

    packetQueued = true;
    ttn_send(txBuffer, sizeof(txBuffer), config.loraPort, confirmed);
    return true;
  }
  else
  {
    return false;
  }
}

void doDeepSleep(uint64_t msecToWake)
{
  log("Entering deep sleep for %llu seconds\n", msecToWake / 1000);

  // not using wifi yet, but once we are this is needed to shutoff the radio hw
  // esp_wifi_stop();

  screen_off();    // datasheet says this will draw only 10ua
  LMIC_shutdown(); // cleanly shutdown the radio

  if (axp192_found)
  {
    // turn on after initial testing with real hardware
    axp.setPowerOutPut(AXP192_LDO2, AXP202_OFF); // LORA radio
    axp.setPowerOutPut(AXP192_LDO3, AXP202_OFF); // GPS main power
  }

  // FIXME - use an external 10k pulldown so we can leave the RTC peripherals powered off
  // until then we need the following lines
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // Only GPIOs which are have RTC functionality can be used in this bit map: 0,2,4,12-15,25-27,32-39.
  uint64_t gpioMask = (1ULL << BUTTON_PIN);

  // FIXME change polarity so we can wake on ANY_HIGH instead - that would allow us to use all three buttons (instead of just the first)
  gpio_pullup_en((gpio_num_t)BUTTON_PIN);

  esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ALL_LOW);

  esp_sleep_enable_timer_wakeup(msecToWake * 1000ULL); // call expects usecs
  esp_deep_sleep_start();                              // TBD mA sleep current (battery)
}

void sleep()
{
  if (config.sleepBetweenMessages)
  {

    // If the user has a screen, tell them we are about to sleep
    if (ssd1306_found)
    {
      // Show the going to sleep message on the screen
      char buffer[20];
      snprintf(buffer, sizeof(buffer), "Sleeping in %3.1fs\n", (config.sleepDelayMs / 1000.0));
      screen_print(buffer);

      // Wait for config.sleepDelayMs millis to sleep
      delay(config.sleepDelayMs);

      // Turn off screen
      screen_off();
    }

    // Set the user button to wake the board
    sleep_interrupt(BUTTON_PIN, LOW);

    // We sleep for the interval between messages minus the current millis
    // this way we distribute the messages evenly every config.sendIntervalMs millis
    uint32_t sleep_for = (millis() < config.sendIntervalMs) ? config.sendIntervalMs - millis() : config.sendIntervalMs;
    doDeepSleep(sleep_for);
  }
}

void callback(uint8_t message)
{
  if (EV_JOINING == message)
    screen_print("Joining TTN...\n");
  if (EV_JOINED == message)
  {
    screen_print("TTN joined!\n");
  }
  if (EV_JOIN_FAILED == message)
    screen_print("TTN join failed\n");
  if (EV_REJOIN_FAILED == message)
    screen_print("TTN rejoin failed\n");
  if (EV_RESET == message)
    screen_print("Reset TTN connection\n");
  if (EV_LINK_DEAD == message)
    screen_print("TTN link dead\n");
  if (EV_ACK == message)
    screen_print("ACK received\n");
  if (EV_PENDING == message)
    screen_print("Message discarded\n");
  if (EV_QUEUED == message)
    screen_print("Message queued\n");

  // We only want to say 'packetSent' for our packets (not packets needed for joining)
  if (EV_TXCOMPLETE == message && packetQueued)
  {
    screen_print("Message sent\n");
    packetQueued = false;
    packetSent = true;
  }

  if (EV_RESPONSE == message)
  {

    screen_print("[TTN] Response: ");

    size_t len = ttn_response_len();
    uint8_t data[len];
    ttn_response(data, len);

    char buffer[6];
    for (uint8_t i = 0; i < len; i++)
    {
      snprintf(buffer, sizeof(buffer), "%02X", data[i]);
      screen_print(buffer);
    }
    screen_print("\n");
  }
}

void scanI2Cdevice(void)
{
  byte err, addr;
  int nDevices = 0;
  for (addr = 1; addr < 127; addr++)
  {
    Wire.beginTransmission(addr);
    err = Wire.endTransmission();
    String t = "";
    if (err == 0)
    {
      t = "I2C device found at address 0x";
      if (addr < 16)
        t += "0";
      t += String(addr, HEX);
      log(t);
      nDevices++;

      if (addr == SSD1306_ADDRESS)
      {
        ssd1306_found = true;
        log("ssd1306 display found");
      }
      if (addr == AXP192_SLAVE_ADDRESS)
      {
        axp192_found = true;
        log("axp192 PMU found");
      }
    }
    else if (err == 4)
    {
      t = "Unknown error at address 0x";
      if (addr < 16)
        t += "0";
      t += String(addr, HEX);
      log(t);
    }
  }
  if (nDevices == 0)
    log("No I2C devices found");
  else
    log("done");
}

/**
 * Init the power manager chip
 *
 * axp192 power
    DCDC1 0.7-3.5V @ 1200mA max -> OLED // If you turn this off you'll lose comms to the axp192 because the OLED and the axp192 share the same i2c bus, instead use ssd1306 sleep mode
    DCDC2 -> unused
    DCDC3 0.7-3.5V @ 700mA max -> ESP32 (keep this on!)
    LDO1 30mA -> charges GPS backup battery // charges the tiny J13 battery by the GPS to power the GPS ram (for a couple of days), can not be turned off
    LDO2 200mA -> LORA
    LDO3 200mA -> GPS
 */

void axp192Init()
{
  if (axp192_found)
  {
    if (!axp.begin(Wire, AXP192_SLAVE_ADDRESS))
    {
      log("AXP192 Begin PASS");
    }
    else
    {
      log("AXP192 Begin FAIL");
    }
    // axp.setChgLEDMode(LED_BLINK_4HZ);
    log("AXP Status before switching power...");
    log("DCDC1: %s", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
    log("DCDC2: %s", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
    log("LDO2: %s", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
    log("LDO3: %s", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
    log("DCDC3: %s", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
    log("Exten: %s", axp.isExtenEnable() ? "ENABLE" : "DISABLE");

    axp.setPowerOutPut(AXP192_LDO2, AXP202_ON); // LORA radio
    axp.setPowerOutPut(AXP192_LDO3, AXP202_ON); // GPS main power
    axp.setPowerOutPut(AXP192_DCDC2, AXP202_ON);
    axp.setPowerOutPut(AXP192_EXTEN, AXP202_ON);
    axp.setPowerOutPut(AXP192_DCDC1, AXP202_ON);
    axp.setDCDC1Voltage(3300); // for the OLED power

    log("AXP Status after switching power...");
    log("DCDC1: %s", axp.isDCDC1Enable() ? "ENABLE" : "DISABLE");
    log("DCDC2: %s", axp.isDCDC2Enable() ? "ENABLE" : "DISABLE");
    log("LDO2: %s", axp.isLDO2Enable() ? "ENABLE" : "DISABLE");
    log("LDO3: %s", axp.isLDO3Enable() ? "ENABLE" : "DISABLE");
    log("DCDC3: %s", axp.isDCDC3Enable() ? "ENABLE" : "DISABLE");
    log("Exten: %s", axp.isExtenEnable() ? "ENABLE" : "DISABLE");

    pinMode(PMU_IRQ, INPUT_PULLUP);
    attachInterrupt(
        PMU_IRQ, [] {
          pmu_irq = true;
        },
        FALLING);

    axp.adc1Enable(AXP202_BATT_CUR_ADC1, 1);
    axp.enableIRQ(AXP202_VBUS_REMOVED_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_BATT_REMOVED_IRQ | AXP202_BATT_CONNECT_IRQ, 1);
    axp.clearIRQ();

    if (axp.isChargeing())
    {
      baChStatus = "Charging";
    }
  }
  else
  {
    log("AXP192 not found");
  }
}

// Perform power on init that we do on each wake from deep sleep
void initDeepSleep()
{
  bootCount++;
  wakeCause = esp_sleep_get_wakeup_cause();
  /*
    Not using yet because we are using wake on all buttons being low

    wakeButtons = esp_sleep_get_ext1_wakeup_status();       // If one of these buttons is set it was the reason we woke
    if (wakeCause == ESP_SLEEP_WAKEUP_EXT1 && !wakeButtons) // we must have been using the 'all buttons rule for waking' to support busted boards, assume button one was pressed
        wakeButtons = ((uint64_t)1) << buttons.gpios[0];
    */

  log("booted, wake cause %d (boot count %d)\n", wakeCause, bootCount);
}

void setup()
{
  Serial.begin(SERIAL_BAUD);
  // Make sure that the log buffer is initialized
  clearLog();

  // Read the configuration from the nvs partition
  config.read();

  SPIFFS.begin();

  initDeepSleep();

  Wire.begin(I2C_SDA, I2C_SCL);
  scanI2Cdevice();

  axp192Init();

  // Buttons & LED
  pinMode(BUTTON_PIN, INPUT_PULLUP);

#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
#endif

  // Hello
  log(APP_NAME " " APP_VERSION "\n");

  // Don't init display if we don't have one or we are waking headless due to a timer event
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER)
    ssd1306_found = false; // forget we even have the hardware

  if (ssd1306_found)
    screen_setup();

  // Access point setup
  if (config.wifiEnabled)
  {
    wifiSetup();
  }

  // Init GPS
  gps_setup();

  // Show logo on first boot after removing battery
  if (bootCount == 0)
  {
    screen_print(APP_NAME " " APP_VERSION, 0, 0);
    screen_show_logo();
    screen_update();
    delay(LOGO_DELAY);
  }

  // TTN setup
  if (!ttn_setup())
  {
    screen_print("[ERR] Radio module not found!\n");

    if (REQUIRE_RADIO)
    {
      delay(config.sleepDelayMs);
      screen_off();
      sleep_forever();
    }
  }
  else
  {
    ttn_register(callback);
    ttn_join();
    ttn_adr(config.loraUseADR);
  }
}

void loop()
{
  gps_loop();
  ttn_loop();
  screen_loop();

  if (packetSent)
  {
    packetSent = false;
    sleep();
  }

  // if user presses button for more than 3 secs, discard our network prefs and reboot (FIXME, use a debounce lib instead of this boilerplate)
  // for everything under 3 secs, toggle the wifi AP mode
  if (digitalRead(BUTTON_PIN) == LOW)
  {
    log("Select button pressed");
    ostime_t startTime = os_getTime();
    while (digitalRead(BUTTON_PIN) == LOW)
    {
      // Wait...
    }

    auto msSpent = (osticks2ms(os_getTime() - startTime));
    log("Button released after" + String(msSpent, DEC) + "ms");
    if (msSpent < 3000)
    {
      if (wifiEnabled())
      {
        wifiShutdown();
      }
      else
      {
        wifiSetup();
      }
    }
    else
    {
      // Discard the network prefs and reboot now
      screen_print("Erasing prefs");
      ttn_erase_prefs();
      delay(5000); // Give some time to read the screen
      ESP.restart();
    }
  }

  // Send every config.sendIntervalMs millis
  static uint32_t last = 0;
  static bool first = true;
  if (0 == last || millis() - last > config.sendIntervalMs)
  {
    if (trySend())
    {
      last = millis();
      first = false;
      log("TRANSMITTED");
    }
    else
    {
      if (first)
      {
        screen_print("Waiting GPS lock\n");
        first = false;
      }

#ifdef GPS_WAIT_FOR_LOCK
      if (millis() > GPS_WAIT_FOR_LOCK)
      {
        sleep();
      }
#endif

      // No GPS lock yet, let the OS put the main CPU in low power mode for 100ms (or until another interrupt comes in)
      // i.e. don't just keep spinning in loop as fast as we can.
      delay(100);
    }
  }
}
