#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <string.h>
#include <esp_wifi.h>
#include <EEPROM.h>
#include "config.h"

// Set your Wi-Fi credentials
const byte DNS_PORT = 53;
const char* ssid = "OG Star Tracker";  //change to your SSID
const char* password = "jugal2001";    //change to your password, must be 8+ characters
//If you are using AP mode, you can access the website using the below URL
const String website_name = "www.tracker.com";
const int dither_intensity = 5;
//Time b/w two rising edges should be 133.3333 ms
//66.666x2  ms
//sidereal rate = 0.00416 deg/s
//for 80Mhz APB (TIMER frequency)
#ifdef STEPPER_0_9
const uint64_t c_SIDEREAL_PERIOD = 2666666;
const uint32_t c_SLEW_SPEED = SLEW_SPEED;
const int arcsec_per_step = 2;
#else
const uint64_t c_SIDEREAL_PERIOD = 5333333;
const uint32_t c_SLEW_SPEED = SLEW_SPEED / 2;
const int arcsec_per_step = 4;
#endif

int slew_speed = 0, num_exp = 0, len_exp = 0, dither_on = 0, focal_length = 0, pixel_size = 0, steps_per_10pixels = 0;
float arcsec_per_pixel = 0.0;
unsigned long old_millis = 0, blink_millis = 0;
uint64_t exposure_delay = 0;

//state variables
bool s_slew_active = false, s_sidereal_active = false;  //change sidereal state to false if you want tracker to be OFF on power-up
enum interv_states { ACTIVE,
                     DELAY,
                     DITHER,
                     INACTIVE };
volatile enum interv_states s_interv = INACTIVE;

//2 bytes occupied by each int
//eeprom addresses
#define DITHER_ADDR 1
#define FOCAL_LEN_ADDR 3
#define PIXEL_SIZE_ADDR 5
#define DITHER_PIXELS 30  //how many pixels to dither

WebServer server(80);
DNSServer dnsServer;
hw_timer_t* timer0 = NULL;  //for sidereal rate
hw_timer_t* timer1 = NULL;  //for intervalometer control

void IRAM_ATTR timer0_ISR() {
  //sidereal ISR
  digitalWrite(AXIS1_STEP, !digitalRead(AXIS1_STEP));  //toggle step pin at required frequency
}

void IRAM_ATTR timer1_ISR() {
  //intervalometer ISR
  if (s_interv == ACTIVE) {
    num_exp--;
    if (num_exp % 3 == 0)  //once in every 3 images
    {
      s_interv = DITHER;
      digitalWrite(INTERV_PIN, LOW);  //stop capture
      timerStop(timer1);              //pause the timer, wait for dither to finish in main loop
    } else if (num_exp == 0) {
      disableIntervalometer();
      num_exp = 0;
      len_exp = 0;
    } else {
      timerWrite(timer1, exposure_delay);
      digitalWrite(INTERV_PIN, LOW);
      s_interv = DELAY;
    }
  } else if (s_interv == DELAY) {
    timerWrite(timer1, 0);
    digitalWrite(INTERV_PIN, HIGH);
    s_interv = ACTIVE;
  }
}

const String html =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "    <title>OG Star Tracker Control</title>\n"
  "    <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
  "    <style>\n"
  "        body {\n"
  "            background-color: lightcoral;\n"
  "            text-align: center;\n"
  "            font-family: \"Arial\";\n"
  "        }\n"
  "\n"
  "        button {\n"
  "            background-color: white;\n"
  "            color: black;\n"
  "            border: none;\n"
  "            padding: 15px 32px;\n"
  "            text-align: center;\n"
  "            text-decoration: none;\n"
  "            display: inline-block;\n"
  "            font-size: 16px;\n"
  "            margin: 4px 2px;\n"
  "            cursor: pointer;\n"
  "        }\n"
  "\n"
  "        select {\n"
  "            font-size: 16px;\n"
  "            padding: 5px;\n"
  "        }\n"
  "\n"
  "        input[type='number'] {\n"
  "            font-size: 16px;\n"
  "            padding: 5px;\n"
  "            width: 50%;\n"
  "        }\n"
  "\n"
  "        label {\n"
  "            display: inline-block;\n"
  "            text-align: left;\n"
  "            margin: 10px;\n"
  "            font-size: 20px;\n"
  "        }\n"
  "\n"
  "        #status {\n"
  "            font-size: 24px;\n"
  "            margin: 20px;\n"
  "        }\n"
  "    </style>\n"
  "    <script>\n"
  "        function sendRequest(url) {\n"
  "            var xhr = new XMLHttpRequest();\n"
  "            xhr.onreadystatechange = function() {\n"
  "                if (this.readyState == 4 && this.status == 200) {\n"
  "                    document.getElementById('status').innerHTML = this.responseText;\n"
  "                }\n"
  "            };\n"
  "            xhr.open('GET', url, true);\n"
  "            xhr.send();\n"
  "        }\n"
  "\n"
  "        setInterval(function() {\n"
  "            sendRequest('/status');\n"
  "        }, 20000);\n"
  "\n"
  "        function sendSlewRequest(url) {\n"
  "            var speed = document.getElementById('slew-select').value;\n"
  "            var slewurl = url + '?speed=' + speed;\n"
  "            sendRequest(slewurl);\n"
  "        }\n"
  "\n"
  "        function sendCaptureRequest() {\n"
  "            var exposure = document.getElementById('exposure').value.trim();\n"
  "            var numExposures = document.getElementById('num-exposures').value.trim();\n"
  "            var focalLength = document.getElementById('focal_len').value.trim();\n"
  "            var pixSize = Math.floor(parseFloat(document.getElementById('pixel_size').value.trim()) * 100);\n"
  "            \n"
  "            var ditherEnabled = document.getElementById('dither_on').checked ? 1 : 0;\n"
  "            var intervalometerUrl = '/start?exposure=' + exposure + '&numExposures=' + numExposures + '&focalLength=' + focalLength + '&pixSize=' + pixSize + '&ditherEnabled=' + ditherEnabled;\n"
  "            sendRequest(intervalometerUrl);\n"
  "        }\n"
  "    </script>\n"
  "</head>\n"
  "<body>\n"
  "  <h1>OG Star Tracker Control</h1>\n"
  "    \n"
  "    <label>Sidereal Tracking:</label><br>\n"
  "    <button onclick=\"sendRequest('/on')\">ON</button>\n"
  "    <button onclick=\"sendRequest('/off')\">OFF</button><br>\n"
  "    <label>Slew Control:</label><br>\n"
  "    <label>Speed:</label>\n"
  "    <select id='slew-select'>\n"
  "        <option value='1'>1</option>\n"
  "        <option value='2'>2</option>\n"
  "        <option value='3'>3</option>\n"
  "        <option value='4'>4</option>\n"
  "        <option value='5'>5</option>\n"
  "    </select><br>\n"
  "    <button onclick=\"sendSlewRequest('/left')\">&#8592;</button>\n"
  "    <button onclick=\"sendSlewRequest('/right')\">&#8594;</button><br>\n"
  "    <label>Intervalometer Control:</label><br>\n"
  "    <input type='number' id='exposure' placeholder='Exposure length (s)'>\n"
  "    <input type='number' id='num-exposures' placeholder='Number of Exposures'><br><br>\n"
  "    <details>\n"
  "        <summary>Dither Settings</summary>\n"
  "        <!-- Content inside the collapsible section -->\n"
  "                <label style=\"font-size: 100%\">Dithering Enable:</label>\n"
  "                <input type=\"checkbox\" id=\"dither_on\" %dither%>\n"
  "            </label>\n"
  "            <div class=\"number-container\">\n"
  "                <input type=\"number\" id=\"focal_len\" placeholder='Focal Length (mm)' value='%focallen%'>\n"
  "                <label style=\"font-size: 80%\" for=\"focal_len\" class=\"number-label\">Ex. 135</label>\n"
  "                <input type=\"number\" id=\"pixel_size\" placeholder='Pixel Size (um)' value='%pixsize%' step=\"0.01\">\n"
  "                <label style=\"font-size: 80%\" for=\"pixel_size\" class=\"number-label\">Ex. 3.91</label>\n"
  "            </div>\n"
  "    </details><br>\n"
  "    <button onclick=\"sendCaptureRequest()\">Start capture</button>\n"
  "    <button onclick=\"sendRequest('/abort')\">Abort capture</button><br>\n"
  "    <label>STATUS:</label><br>\n"
  "    <p id='status'></p>\n"
  "</body>\n"
  "</html>";

// Handle requests to the root URL ("/")
void handleRoot() {
  String formattedHtmlPage = String(html);
  formattedHtmlPage.replace("%dither%", (dither_on ? "checked" : ""));
  formattedHtmlPage.replace("%focallen%", String(focal_length).c_str());
  formattedHtmlPage.replace("%pixsize%", String((float)pixel_size / 100, 2).c_str());
  server.send(200, "text/html", formattedHtmlPage);
}

void handleOn() {
  s_sidereal_active = true;
  timerAlarmEnable(timer0);
  server.send(200, "text/plain", "Tracking ON");
}

void handleOff() {
  s_sidereal_active = false;
  timerAlarmDisable(timer0);
  server.send(200, "text/plain", "Tracking OFF");
}

void handleLeft() {
  slew_speed = server.arg("speed").toInt();
  if (s_slew_active == false) {
    initSlew(c_DIRECTION);
    s_slew_active = true;
  }
  old_millis = millis();
  server.send(200, "text/plain", "Slewing");
}

void handleRight() {
  slew_speed = server.arg("speed").toInt();
  old_millis = millis();
  if (s_slew_active == false) {
    initSlew(!c_DIRECTION);  //reverse direction
    s_slew_active = true;
  }
  server.send(200, "text/plain", "Slewing");
}

void handleStartCapture() {
  if (s_interv == INACTIVE) {
    len_exp = server.arg("exposure").toInt();
    num_exp = server.arg("numExposures").toInt();
    dither_on = server.arg("ditherEnabled").toInt();
    focal_length = server.arg("focalLength").toInt();
    pixel_size = server.arg("pixSize").toInt();

    if ((len_exp == 0 || num_exp == 0)) {
      server.send(200, "text/plain", "Invalid Intervalometer Settings!");
      return;
    } else if (dither_on && (focal_length == 0 || pixel_size == 0)) {
      server.send(200, "text/plain", "Invalid Dither Settings!");
      return;
    }
    updateEEPROM(dither_on, focal_length, pixel_size);
    arcsec_per_pixel = (((float)pixel_size / 100.0) / focal_length) * 206.265;        //div pixel size by 100 since we multiplied it by 100 in html page
    steps_per_10pixels = (int)(((arcsec_per_pixel * 10.0) / arcsec_per_step) + 0.5);  //add 0.5 to round up float to nearest int while casting
    Serial.println("steps per 10px: ");
    Serial.println(steps_per_10pixels);

    s_interv = ACTIVE;
    exposure_delay = ((len_exp - 3) * 2000);  // 3 sec delay
    initIntervalometer();
    server.send(200, "text/plain", "Capture ON");
  } else {
    server.send(200, "text/plain", "Capture Already ON");
  }
}

void handleAbortCapture() {
  if (s_interv == INACTIVE) {
    server.send(200, "text/plain", "Capture Already OFF");
  } else {
    disableIntervalometer();
    num_exp = 0;
    len_exp = 0;
    s_interv = INACTIVE;
    server.send(200, "text/plain", "Capture OFF");
  }
}

void handleStatusRequest() {
  if (s_interv != INACTIVE) {
    char status[60];
    sprintf(status, "%d Captures Remaining...", num_exp);
    server.send(200, "text/plain", status);
  } else
    server.send(204, "text/plain", "dummy");
}

void writeEEPROM(int address, int value) {
  byte high = value >> 8;
  byte low = value & 0xFF;
  EEPROM.write(address, high);
  EEPROM.write(address + 1, low);
}

int readEEPROM(int address) {
  byte high = EEPROM.read(address);
  byte low = EEPROM.read(address + 1);
  return ((high << 8) + low);
}

void updateEEPROM(int dither, int focal_len, int pix_size) {
  if (readEEPROM(DITHER_ADDR) != dither) {
    writeEEPROM(DITHER_ADDR, dither);
    //Serial.println("dither updated");
  }
  if (readEEPROM(FOCAL_LEN_ADDR) != focal_len) {
    writeEEPROM(FOCAL_LEN_ADDR, focal_len);
    //Serial.println("focal length updated");
  }
  if (readEEPROM(PIXEL_SIZE_ADDR) != pix_size) {
    writeEEPROM(PIXEL_SIZE_ADDR, pix_size);
    //Serial.println("pix size updated");
  }
  EEPROM.commit();
}
void setMicrostep(int microstep) {
  switch (microstep) {
    case 8:
      digitalWrite(MS1, LOW);
      digitalWrite(MS2, LOW);
      break;
    case 16:
      digitalWrite(MS1, HIGH);
      digitalWrite(MS2, HIGH);
      break;
    case 32:
      digitalWrite(MS1, HIGH);
      digitalWrite(MS2, LOW);
      break;
    case 64:
      digitalWrite(MS1, LOW);
      digitalWrite(MS2, HIGH);
      break;
  }
}
void initSlew(int dir) {
  timerAlarmDisable(timer0);
  digitalWrite(AXIS1_DIR, dir);
  setMicrostep(8);
  ledcSetup(0, (c_SLEW_SPEED * slew_speed), 8);
  ledcAttachPin(AXIS1_STEP, 0);
  ledcWrite(0, 127);  //50% duty pwm
}
void initSiderealTracking() {
  digitalWrite(AXIS1_DIR, c_DIRECTION);
  setMicrostep(16);
  timerAlarmWrite(timer0, c_SIDEREAL_PERIOD, true);
  if (s_sidereal_active == true)
    timerAlarmEnable(timer0);
  else
    timerAlarmDisable(timer0);
}
void initIntervalometer() {
  timer1 = timerBegin(1, 40000, true);
  timerAttachInterrupt(timer1, &timer1_ISR, true);
  timerAlarmWrite(timer1, (len_exp * 2000), true);  //2000 because prescaler cant be more than 16bit, = 1sec ISR freq
  timerAlarmEnable(timer1);
  digitalWrite(INTERV_PIN, HIGH);  // start the first capture
}
void disableIntervalometer() {
  digitalWrite(INTERV_PIN, LOW);
  timerAlarmDisable(timer1);
  timerDetachInterrupt(timer1);
  timerEnd(timer1);
}

void ditherRoutine() {

  int i = 0, j = 0;
  timerAlarmDisable(timer0);
  digitalWrite(AXIS1_DIR, random(2));  //dither in a random direction
  delay(500);
  Serial.println("Dither rndm direction:");
  Serial.println(random(2));

  for (i = 0; i < dither_intensity; i++) {
    for (j = 0; j < steps_per_10pixels; j++) {
      digitalWrite(AXIS1_STEP, !digitalRead(AXIS1_STEP));
      delay(10);
      digitalWrite(AXIS1_STEP, !digitalRead(AXIS1_STEP));
      delay(10);
    }
  }
  delay(1000);
  initSiderealTracking();
  delay(3000);  //settling time after dither
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);  //SIZE = 6 bytes, 2 bytes for each variable
  //fetch values from EEPROM
  dither_on = readEEPROM(DITHER_ADDR);
  focal_length = readEEPROM(FOCAL_LEN_ADDR);
  pixel_size = readEEPROM(PIXEL_SIZE_ADDR);

#ifdef AP
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAP(ssid, password);
  delay(500);
  Serial.println("Creating Wifi Network");

  //ANDROID 10 WORKAROUND==================================================
  //set new WiFi configurations
  WiFi.disconnect();
  Serial.println("reconfig WiFi...");
  /*Stop wifi to change config parameters*/
  esp_wifi_stop();
  esp_wifi_deinit();
  /*Disabling AMPDU RX is necessary for Android 10 support*/
  wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();  //We use the default config ...
  my_config.ampdu_rx_enable = 0;                              //... and modify only what we want.
  Serial.println("WiFi: Disabled AMPDU...");
  esp_wifi_init(&my_config);  //set the new config = "Disable AMPDU"
  esp_wifi_start();           //Restart WiFi
  delay(500);
  //ANDROID 10 WORKAROUND==================================================
#else
  WiFi.mode(WIFI_MODE_STA);  // Set ESP32 in station mode
  WiFi.begin(ssid, password);
  Serial.println("Connecting to Network in STA mode");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
#endif
  dnsServer.setTTL(300);
  dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
  dnsServer.start(DNS_PORT, website_name, WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/on", HTTP_GET, handleOn);
  server.on("/off", HTTP_GET, handleOff);
  server.on("/left", HTTP_GET, handleLeft);
  server.on("/right", HTTP_GET, handleRight);
  server.on("/start", HTTP_GET, handleStartCapture);
  server.on("/abort", HTTP_GET, handleAbortCapture);
  server.on("/status", HTTP_GET, handleStatusRequest);

  // Start the server
  server.begin();

#ifdef AP
  Serial.println(WiFi.softAPIP());
#else
  Serial.println(WiFi.localIP());
#endif
  pinMode(INTERV_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(AXIS1_STEP, OUTPUT);
  pinMode(AXIS1_DIR, OUTPUT);
  pinMode(EN12_n, OUTPUT);
  pinMode(MS1, OUTPUT);
  pinMode(MS2, OUTPUT);
  digitalWrite(AXIS1_STEP, LOW);
  digitalWrite(EN12_n, LOW);

  timer0 = timerBegin(0, 2, true);
  timerAttachInterrupt(timer0, &timer0_ISR, true);
  initSiderealTracking();
}

void loop() {
  if (s_slew_active) {
    //blink status led if mount is in slew mode
    if (millis() - blink_millis >= 150) {
      digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
      blink_millis = millis();
    }
  } else {
    //turn on status led if sidereal tracking is ON
    digitalWrite(STATUS_LED, (s_sidereal_active == true));
  }
  if ((s_slew_active == true) && (millis() - old_millis >= 1200)) {
    //slewing will stop if button is not pressed again within 1.2sec
    s_slew_active = false;
    ledcDetachPin(AXIS1_STEP);
    pinMode(AXIS1_STEP, OUTPUT);
    initSiderealTracking();
  }
  if (s_interv == DITHER) {
    disableIntervalometer();
    ditherRoutine();
    s_interv = ACTIVE;
    initIntervalometer();
  }
  server.handleClient();
  dnsServer.processNextRequest();
}
