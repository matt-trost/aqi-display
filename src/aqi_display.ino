/*
 *  Time-sensitive, low-power AQI query-and-display.
 *  Matt, Feb. 2019
 *
 *  Built off of the following sources:
 *
 *  ESP8266 JSON Decode of server response by Manoj R. Thkuar
 *  https://circuits4you.com/2019/01/11/nodemcu-esp8266-arduino-json-parsing-example/
 *  
 *  NIST Date-Time with ESP8266 by Ray Burnette
 *  https://www.hackster.io/rayburne/nist-date-time-with-esp8266-e8b9a9
 *
 *  Referenced for Deep Sleep Mode:
 *
 *  https://www.losant.com/blog/making-the-esp8266-low-powered-with-deep-sleep
 *  https://quadmeup.com/esp8266-esp-01-low-power-mode-run-it-for-months/
 *  https://www.espressif.com/sites/default/files/9b-esp8266-low_power_solutions_en_0.pdf
 */

// ESP8266 Libraries
#include <ESP8266WiFi.h>
#include <WiFiClient.h> 
#include <ESP8266HTTPClient.h>

// JSON parsing library
#include <ArduinoJson.h>

// Libraries for interfacing with the OLED
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED characteristics
// I used this one: https://www.banggood.com/0_91-Inch-128x32-IIC-I2C-Blue-OLED-LCD-Display-DIY-Oled-Module-SSD1306-Driver-IC-DC-3_3V-5V-p-1140506.html?cur_warehouse=CN
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels

// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
// On my ESP8266 board, SDA = 4 = D2 and SCL = 5 = D1
// My display doesn't have a RESET pin, so I set this to -1
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Wifi credentials
const char* kWifiName = "YOUR_WIFI_NAME";  // CHANGE ME!
const char* kWifiPass = "YOUR_WIFI_PASSWORD";  // CHANGE ME!
 
// Web Server address to read/write from 
// Go to https://aqicn.org/data-platform/token/#/ to get your personal token.
// The @XXXX number is the index of the station whose data you want to read.
// To find your station's index, go to your desired station on aqicn.org,
// then view source and search for "idx" until you see some JSON that lists the
// idx as a 4 digit number.
const char *kAqiHost = "http://api.waqi.info/feed/@YOUR_4_DIGIT_STATION_INDEX_HERE/?token=YOUR_TOKEN_HERE";  // CHANGE ME!
const char *kTimeHost = "time.nist.gov"; // directs to least-loaded NIST server
// note that we should not make more than 1 (successful) NIST query every 60 seconds

// uncomment this to see Serial printouts.
//#define DEBUG

enum State {
  CHECK_AQI,
  AQI_SUCCESS,
  AQI_FAILURE,
  CHECK_TIME,
  TIME_SUCCESS,
  NIGHT_TIME,
  DAY_TIME,
  TIME_FAILURE,
  REBOOT
};

// constants
const uint32_t kMaxAqiFailures = 25;
const uint32_t kMaxTimeFailures = 20;
const uint32_t kMaxWifiFailures = 52;
const uint32_t kBaseAqiFailureDelaySec = 4;
const uint32_t kTimeFailureDelaySec = 8;
const uint32_t kWifiFailureDelayMs = 500;
const uint32_t kTooManyFailuresDelaySec = 3600; // 60 minutes
const uint32_t kDaytimeUpdateIntervalSec = 1200; // 20 minutes
const uint32_t kNighttimeUpdateIntervalSec = 3600 ; // 60 minutes
// note that the ESP can deep sleep for a maximum of 71min since
// its time in microseconds argument is a 32-bit integer.

// globals
uint32_t aqi_error_cnt = 0;
uint32_t time_error_cnt = 0;
uint32_t utc_time_in_sec = 0; // UTC time, number of seconds since midnight.
State state = CHECK_AQI;

// forward declarations
void display_num(uint32_t num);
void query_aqi();
void query_time();


void setup() {  
  Serial.begin(115200);
  delay(100);

  // initializations
  // (think these are required because I assume globals are not reinitialized
  //  automatically upon waking from deep sleep)
  aqi_error_cnt = 0;
  time_error_cnt = 0;
  utc_time_in_sec = 0;
  state = CHECK_AQI;

  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
#ifdef DEBUG
    Serial.println(F("SSD1306 allocation failed"));
#endif
    for(;;); // Don't proceed, loop forever
  }

  // Show initial display buffer contents on the screen --
  // the library initializes this with an Adafruit splash screen.
  display.display();
  delay(500);
  display.clearDisplay(); // clear the buffer.
  
  // CONNECT TO WIFI
#ifdef DEBUG
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(kWifiName);
#endif

  uint32_t wifi_err_cnt = 0;
  WiFi.begin(kWifiName, kWifiPass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
#ifdef DEBUG
    Serial.print(".");
#endif
    wifi_err_cnt++;
    if (wifi_err_cnt > kMaxWifiFailures) {
#ifdef DEBUG
      Serial.println("");
      Serial.print("Wifi taking too long to connect, sleeping for ");
      Serial.print(kTooManyFailuresDelaySec);
      Serial.println(" seconds, then rebooting.");
#endif
      display.display(); // clear the adafruit splash screen
      ESP.deepSleep(kTooManyFailuresDelaySec * 1000000);
    }
  }

#ifdef DEBUG
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());   //You can get IP address assigned to ESP
#endif
}


// state machine
// (does not need to be fully connected because of reboots following deep sleep)
void loop() {
  if (state == CHECK_AQI) {
#ifdef DEBUG
    Serial.println("CHECK_AQI");
#endif
    query_aqi();
    return;
  }
  else if (state == AQI_SUCCESS) {
#ifdef DEBUG
    Serial.println("AQI_SUCCESS");
#endif
    aqi_error_cnt = 0;
    state = CHECK_TIME;
    return;
  }
  else if (state == AQI_FAILURE) {
    uint32_t aqi_failure_delay_sec = kBaseAqiFailureDelaySec;
    aqi_error_cnt++;
    if (aqi_error_cnt >= 10) {
      aqi_failure_delay_sec += (aqi_error_cnt - 9);
    }
#ifdef DEBUG
    Serial.print("AQI_FAILURE, waiting ");
    Serial.print(aqi_failure_delay_sec);
    Serial.println(" seconds.");
#endif
    if (aqi_error_cnt >= kMaxAqiFailures) {
#ifdef DEBUG
      Serial.println("Too many AQI failures, will reboot.");
#endif
      state = REBOOT;
    } else {
      delay(aqi_failure_delay_sec * 1000);
      state = CHECK_AQI;
    }
    return;
  }
  else if (state == CHECK_TIME) {
#ifdef DEBUG
    Serial.println("CHECK_TIME");
#endif
    query_time();
    return;
  }
  else if (state == TIME_SUCCESS) {
#ifdef DEBUG
    Serial.println("TIME_SUCCESS");
#endif
    time_error_cnt = 0;
    // night time if between 23:30 and 07:00 Beijing Time,
    // which is between 15:30 and 23:00 UTC.
    state = (utc_time_in_sec > (15*60*60 + 30*60) && utc_time_in_sec < (23*60*60)) ?
      NIGHT_TIME : DAY_TIME;
    return;
  }
  else if (state == DAY_TIME) {
#ifdef DEBUG
    Serial.print("DAY_TIME, sleeping ");
    Serial.print(kDaytimeUpdateIntervalSec);
    Serial.println(" seconds.");
#endif
    // note that upon waking from deep sleep, the ESP8266 will reboot and
    // start from setup(), thus we don't have to worry about the state here.
    ESP.deepSleep(kDaytimeUpdateIntervalSec * 1000000);
  }
  else if (state == NIGHT_TIME) {
    uint32_t sec_diff = (23*60*60) - utc_time_in_sec; // how long until 7am?
    uint32_t night_sleep_sec = (sec_diff > kNighttimeUpdateIntervalSec) ? kNighttimeUpdateIntervalSec : sec_diff;
#ifdef DEBUG
    Serial.print("NIGHT_TIME, sleeping ");
    Serial.print(night_sleep_sec);
    Serial.println(" seconds.");
#endif
    ESP.deepSleep(night_sleep_sec * 1000000);
  }
  else if (state == TIME_FAILURE) {
#ifdef DEBUG
    Serial.print("TIME_FAILURE, waiting ");
    Serial.print(kTimeFailureDelaySec);
    Serial.println(" seconds.");
#endif
    time_error_cnt++;
    if (time_error_cnt >= kMaxTimeFailures) {
#ifdef DEBUG
      Serial.println("Too many Time failures, will reboot.");
#endif
      state = REBOOT;
    } else {
      delay(kTimeFailureDelaySec * 1000);
      state = CHECK_TIME;
    }
    return;
  }
  else { // REBOOT (due to too many AQI or Time failures)
#ifdef DEBUG
    Serial.print("Too many AQI or time failures, waiting ");
    Serial.print(kTooManyFailuresDelaySec);
    Serial.println(" seconds, then rebooting.");
#endif
    ESP.deepSleep(kTooManyFailuresDelaySec * 1000000);
  }
}


// this function would ideally be generalized.
// currently it only supports 1-3 digit unsigned integers.
void display_num(uint32_t num) {
  display.clearDisplay();
  display.setTextSize(3);      // Normal 1:1 pixel scale
  display.setTextColor(WHITE); // Draw white text
  display.setCursor(18, 10);     // Don't start in the extreme corner
  display.cp437(true);         // Use full 256 char 'Code Page 437' font

  // AQIs don't exceed 3 digits (hopefully!)
  uint32_t nums[3];
  uint32_t numcpy = num;
  // initializing all to 999999 sentinel
  for (int i = 0; i <= 2; ++i) {
    nums[i] = 999999;
  }
  // parse!
  uint32_t idx = 0;
  while(numcpy > 0) {
    nums[idx] = numcpy % 10;
    numcpy = numcpy / 10;
    idx++;
  }

  // write to display buffer
  for (int i = 2; i >= 0; i--) {
    if (nums[i] != 999999) {
      display.write(nums[i]+ '0'); // convert from number to ASCII char rep
    } else {
      display.write('0');
    }
    display.write(' ');
  }

  // show buffer and delay 2 sec for good measure
  display.display();
  delay(2000);
#ifdef DEBUG
  Serial.println("end of display_num");
#endif
}


void query_time() {
#ifdef DEBUG
  Serial.print("Time Request Link=");
  Serial.println(kTimeHost);
#endif

  // Use WiFiClient class to create TCP connections
  WiFiClient client;
  const int httpPort = 13;

  if (!client.connect(kTimeHost, httpPort)) {
    state = TIME_FAILURE;
    return;
  } else {
    // This will send the request to the server
    client.print("HEAD / HTTP/1.1\r\nAccept: */*\r\nUser-Agent: Mozilla/4.0 (compatible; ESP8266 NodeMcu Lua;)\r\n\r\n");

    delay(1000); // give the server time to respond. In my setup, looks like at least 500ms is required.

    // Read all the lines of the reply from server and print them to Serial
    // expected line is like : JJJJJ YR-MO-DA HH:MM:SS TT L H msADV UTC(NIST) OTM
    // example: 58537 19-02-23 08:15:31 00 0 0 420.9 UTC(NIST) * 
    // see https://www.nist.gov/pml/time-and-frequency-division/services/internet-time-service-its
    if (!client.available()) {
#ifdef DEBUG
      Serial.println("response not received after 1 second, waiting 3 more seconds.");
#endif
      delay(3000);
      if (!client.available()) {
#ifdef DEBUG
        Serial.println("response still unavailable, counting as failure.");
#endif
        state = TIME_FAILURE;
        return;
      }
    }

    while(client.available()) {
      String line = client.readStringUntil('\r');
#ifdef DEBUG
      Serial.print("Response=");
      Serial.println(line);
#endif

      if (line.indexOf("Date") != -1) {
        Serial.print("=====>");
      } else {
        String time_string = line.substring(16, 24);
#ifdef DEBUG
        Serial.print("Time=");
        Serial.println(time_string); 
#endif

        // parse time_string into number of seconds.
        uint32_t hours = (time_string.charAt(0) - '0')*10 + (time_string.charAt(1) -'0');
        uint32_t minutes = (time_string.charAt(3) - '0')*10 + (time_string.charAt(4) -'0');
        uint32_t seconds = (time_string.charAt(6) - '0')*10 + (time_string.charAt(7) -'0');
        utc_time_in_sec = 0;
        utc_time_in_sec = (hours * 60 * 60) + (minutes * 60) + seconds;
#ifdef DEBUG
        Serial.print("utc_time_in_sec=");
        Serial.println(utc_time_in_sec);
#endif
      }
    }
  } 
  state = TIME_SUCCESS;
}


void query_aqi() {
  HTTPClient http;    //Declare object of class HTTPClient
#ifdef DEBUG
  Serial.print("AQI Request Link=");
  Serial.println(kAqiHost);
#endif
  
  http.begin(kAqiHost);     //Specify request destination
  int httpCode = http.GET();            //Send the request
  String payload = http.getString();    //Get the response payload from server
 
#ifdef DEBUG
  Serial.print("Response Code="); //200 is OK
  Serial.println(httpCode);   //Print HTTP return code
  Serial.print("Returned data from Server=");
  Serial.println(payload);    //Print request response payload
#endif
  
  if(httpCode == 200) {
    // Allocate JsonBuffer
    // Use arduinojson.org/assistant to compute the capacity.
    // The assistant also generates appropriate parsing code.
    const size_t capacity = 2*JSON_ARRAY_SIZE(2) + 9*JSON_OBJECT_SIZE(1) + 3*JSON_OBJECT_SIZE(2) + 2*JSON_OBJECT_SIZE(3) + 2*JSON_OBJECT_SIZE(8) + 530;
    DynamicJsonBuffer jsonBuffer(capacity);
    //EXAMPLE JSON RESPONSE: {\"status\":\"ok\",\"data\":{\"aqi\":160,\"idx\":1393,\"attributions\":[{\"url\":\"http://www.xaepb.gov.cn/\",\"name\":\"Xi'an Environmental Protection Agency (西安市环境保护局)\"},{\"url\":\"https://waqi.info/\",\"name\":\"World Air Quality Index Project\"}],\"city\":{\"geo\":[34.2324,108.94],\"name\":\"Xiaozhai, Xian (西安小寨)\",\"url\":\"https://aqicn.org/city/china/xian/xiaozhai\"},\"dominentpol\":\"pm25\",\"iaqi\":{\"co\":{\"v\":12.7},\"no2\":{\"v\":31.1},\"o3\":{\"v\":16.3},\"pm10\":{\"v\":89},\"pm25\":{\"v\":160},\"so2\":{\"v\":3.1},\"w\":{\"v\":0.5},\"wg\":{\"v\":6.6}},\"time\":{\"s\":\"2019-02-22 22:00:00\",\"tz\":\"+08:00\",\"v\":1550872800},\"debug\":{\"sync\":\"2019-02-22T23:34:02+09:00\"}}}
    
    JsonObject& root = jsonBuffer.parseObject(payload);
    JsonObject& data = root["data"];
    uint32_t data_aqi = data["aqi"]; // ex: 160
#ifdef DEBUG
    Serial.print("AQI=");
    Serial.println(data_aqi);
#endif
    display_num(data_aqi);

    // Is this the time the AQI was updated? Might be useful in the future.
    // JsonObject& data_time = data["time"];
    // const char* data_time_s = data_time["s"]; //ex: "2019-02-22 22:00:00"
    
    http.end();  //Close connection
    state = AQI_SUCCESS;
    return;
  } else {
#ifdef DEBUG
    Serial.println("Error in response to AQI query.");
#endif
    http.end();  //Close connection
    state = AQI_FAILURE;
    return;
  }
}

