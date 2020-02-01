#include <Arduino.h>
#include <TimeLib.h>
#include "WifiConfig.h"
#include <NtpClientLib.h>
#include <ESP8266WiFi.h>
#include "EmonConfig.h"
#include <ArduinoOTA.h> // Библиотека для OTA-прошивки
#include <ArduinoJson.h>

DynamicJsonDocument doc(1024);

#ifndef YOUR_WIFI_SSID
#define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
#define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
#endif // !YOUR_WIFI_SSID

// #define OTA_HOSNAME "osmos_pump_timer"
#define OTA_HOSNAME "tenko"

#ifndef EMON_APIKEY
#define EMON_APIKEY "XXXXXXXXXXXXX"
#endif // !EMON_APIKEY

#define DAY_BEGIN 7
#define DAY_END 23

float temp_change_min = 0.1;

    String EMON_SEND_NODE_ID = OTA_HOSNAME;

// #define EMON_GET_NODE_ID "44" //bedroom_room_t
// // #define EMON_SEND_NODE_ID ""

const char *emon_get_temp_node_id = "44";
const char *emon_get_power_node_id = "84";
char node_id_to_get[3]; //2+1 просто переменная которая умеет хранить 2 знака

#define EMON_DOMAIN "udom.ua"
#define EMON_PATH "emoncms"
#define EMON_GET_DATA_TIMEOUT 5000 //ms
#define EMON_UPLOAD_PERIOD_MAX 300 //sec

#define ONBOARDLED D4 // Built in LED on ESP-12/ESP-07

#define FIRST_RELAY D1 //зеленый
#define SECOND_RELAY D2 //красный
#define POWER_RELAY D5 //желтый

#define SHOW_TIME_PERIOD 60       //sec
#define NTP_TIMEOUT 2000          // ms Response timeout for NTP requests //1500 говорят минимальное 2000
#define NTP_SYNC_PERIOD_MAX 43200 // 24*60*60/2  sec
#define LOOP_DELAY_MAX 30         // 24*60*60 sec

unsigned long t_sent, t_get = 0;
unsigned emon_upload_period = 120; //Upload period sec
unsigned emon_temp_check_period = 30;
unsigned emon_temp_check_period_max = 300;

bool power_overload;

int set_power = 0;
int set_power_prev = 0;

int ntp_sync_period = 63;
int loop_delay = 1;

float temp1, temp1_prev;

unsigned long time_last_data_sent, time_last_data_get, time_last_emon_data = 0;
unsigned emon_data_check_period = 10;

int8_t timeZone = 2;
int8_t minutesTimeZone = 0;
const PROGMEM char *ntpServer = "ua.pool.ntp.org"; //"europe.pool.ntp.org"; //"ua.pool.ntp.org"; //"time.google.com"; //"ua.pool.ntp.org";//"pool.ntp.org";
//pool1.ntp.od.ua

bool wifiFirstConnected = false;
bool FirstStart = true;
String ip;

WiFiClient Client;

void onSTAConnected(WiFiEventStationModeConnected ipInfo)
{
  Serial.printf("Connected to %s\r\n", ipInfo.ssid.c_str());
}

// Start NTP only after IP network is connected
void onSTAGotIP(WiFiEventStationModeGotIP ipInfo)
{
  Serial.printf("Got IP: %s\r\n", ipInfo.ip.toString().c_str());
  Serial.printf("Connected: %s\r\n", WiFi.status() == WL_CONNECTED ? "yes" : "no");
  digitalWrite(ONBOARDLED, LOW); // Turn on LED
  wifiFirstConnected = true;
}

// Manage network disconnection
void onSTADisconnected(WiFiEventStationModeDisconnected event_info)
{
  Serial.printf("Disconnected from SSID: %s\n", event_info.ssid.c_str());
  Serial.printf("Reason: %d\n", event_info.reason);
  digitalWrite(ONBOARDLED, HIGH); // Turn off LED
  //NTP.stop(); // NTP sync can be disabled to avoid sync errors
  WiFi.reconnect();
}

void processSyncEvent(NTPSyncEvent_t ntpEvent)
{
  if (ntpEvent < 0)
  {
    Serial.printf("Time Sync error: %d\n", ntpEvent);
    if (ntpEvent == noResponse)
      Serial.println("NTP server not reachable");
    else if (ntpEvent == invalidAddress)
      Serial.println("Invalid NTP server address");
    else if (ntpEvent == errorSending)
      Serial.println("Error sending request");
    else if (ntpEvent == responseError)
      Serial.println("NTP response error");
  }
  else
  {
    if (ntpEvent == timeSyncd)
    {
      Serial.print("Got NTP time: ");
      Serial.println(NTP.getTimeDateString(NTP.getLastNTPSync()));
    }
  }
}

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPSyncEvent_t ntpEvent;            // Last triggered event

// **** GET EMON *********

//служебная функция для получения данных из emoncms в формате json
//используем для получения мощности
String get_emon_data(const char *node_id_to_get)
{

  String json;
  Serial.print("connect to Server ");
  Serial.println(EMON_DOMAIN);
  Serial.print("GET /emoncms/feed/timevalue.json?id=");
  Serial.println(node_id_to_get);

  if (Client.connect(EMON_DOMAIN, 80))
  {
    Serial.println("connected");
    Client.print("GET /emoncms/feed/timevalue.json?id="); //http://udom.ua/emoncms/feed/feed/timevalue.json?id=18
    Client.print(node_id_to_get);
    Client.println();

    unsigned long tstart = millis();
    while (Client.available() == 0)
    {
      if (millis() - tstart > EMON_GET_DATA_TIMEOUT)
      {
        Serial.println(" --- Client Timeout !");
        Client.stop();
        return "0";
      }
    }

    // Read all the lines of the reply from server and print them to Serial
    while (Client.available())
    {
      json = Client.readStringUntil('\r');
      Serial.print("json = ");
      Serial.println(json);
    }

    Serial.println();
    Serial.println("closing connection");
  }
  return json;
}

// запрашиваем и извлекаем данные из json с заданной периодичностью
// использукется толко для температуры, поскольку ее можно получать с большой задержкой
// а для мощности используем прямой запрос поскольку нужны только свежие данные
void get_and_parse_json_data(
    unsigned long &time_last_data_get, //когда последний раз проверялись данные
    float &dat,                        //извлекаемые данные
    unsigned long &time_last_emon_data //время последних данных котороые хранятся в emoncms
)
{

  // Serial.println();
  // Serial.print("time_last_data_get = ");
  // Serial.println(time_last_data_get);
  // Serial.print("emon_temp_check_period = ");
  // Serial.println(emon_temp_check_period);
  // Serial.print("millis() = ");
  // Serial.print(millis());
  // Serial.println();

  if (!time_last_data_get or ((millis() - time_last_data_get) > emon_temp_check_period * 1000))
  {
    String json = get_emon_data(emon_get_temp_node_id);
    deserializeJson(doc, json);

    dat = doc["value"];
    time_last_emon_data = doc["time"];

    Serial.println();
    Serial.print("dat = ");
    Serial.print(dat);

    long dt_last_dat = (now() - time_last_emon_data) / 1000; //разница во времени в секундах
    Serial.print(", time_last_emon_data = ");
    Serial.print(time_last_emon_data);
    Serial.print(", now = ");
    Serial.print(now());
    Serial.print(", dt_last_dat = ");
    Serial.print(dt_last_dat);
    Serial.println(" sec");

    time_last_data_get = millis();
  }
  else
  {
    Serial.print("data is fresh, next check in ");
    Serial.print((emon_temp_check_period * 1000 - (millis() - time_last_data_get)) / 1000);
    Serial.print(" sec");
    Serial.print(", emon_temp_check_period = ");
    Serial.println(emon_temp_check_period);
  }
}

// **** !GET EMON *********

void setup()
{
  delay(1000);
  static WiFiEventHandler e1, e2, e3;

  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  delay(500);
  Serial.flush();
  WiFi.mode(WIFI_STA);
  WiFi.begin(YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);

  pinMode(ONBOARDLED, OUTPUT);   // Onboard LED
  digitalWrite(ONBOARDLED, LOW); // Switch on LED

  NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
    ntpEvent = event;
    syncEventTriggered = true;
  });

  e1 = WiFi.onStationModeGotIP(onSTAGotIP); // As soon WiFi is connected, start NTP Client
  e2 = WiFi.onStationModeDisconnected(onSTADisconnected);
  e3 = WiFi.onStationModeConnected(onSTAConnected);

  pinMode(FIRST_RELAY, OUTPUT);
  pinMode(SECOND_RELAY, OUTPUT);
  pinMode(POWER_RELAY, OUTPUT);

  ArduinoOTA.setHostname(OTA_HOSNAME); // Задаем имя сетевого порта
  //     ArduinoOTA.setPassword((const char *)"0000"); // Задаем пароль доступа для удаленной прошивки

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });
  ArduinoOTA.begin();
}

bool startNTP()
{
  Serial.println();
  Serial.println("*** startNTP ***");
  NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
  //NTP.begin("pool.ntp.org", 2, true);
  delay(3000); // there seems to be a 1 second delay before first sync will be attempted, delay 2 seconds allows request to be made and received
  int counter = 1;
  Serial.print("NTP.getLastNTPSync() = ");
  Serial.println(NTP.getLastNTPSync());
  while (!NTP.getLastNTPSync() && counter <= 3)
  {
    NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
    Serial.print("NTP CHECK: #");
    Serial.println(counter);
    counter += 1;
    delay(counter * 2000);
  };
  NTP.setInterval(ntp_sync_period); // in seconds
  if (now() < 100000)
  {
    return false;
  }
  else
  {
    return true;
  }
}

void TimeValidator()
{ //проверяем время, если неправильное - перезагружаемся

  Serial.println("TimeValidator");
  for (int ectr = 1; ectr < 4; ectr++)
  {
    ip = WiFi.localIP().toString();
    if (now() < 100000 and (ip != "0.0.0.0"))
    {
      bool isntpok = startNTP();
      if (isntpok)
      {
        return;
      }
      Serial.print("Wrong UNIX time: now() = ");
      //Serial.println(NTP.getTimeStr());
      Serial.println(now());
      Serial.print("ip = ");
      Serial.println(ip);
      Serial.print("ectr = ");
      Serial.println(ectr);
      Serial.print("delay ");
      Serial.print(10000 * ectr);
      Serial.println(" sec");
      delay(30000 * ectr);
    }
    else
    {
      return;
    }
  }
  Serial.println("**** restart **** "); //перезагружаемся только при 3-х ошибках подряд
  delay(2000);

  //            WiFi.forceSleepBegin(); wdt_reset(); ESP.restart(); while(1)wdt_reset();
  //            ESP.reset();
  ESP.restart();
}

// int get_required_power(thresh_temp6, thresh_temp3){
// }

void loop()
{

  if (FirstStart)
  {
    Serial.println();
    Serial.println("*** FirstStart ***");
    Serial.println();
    Serial.println(" *** demo ***");
    // delay(1000);
    // демонстрируем, что работает
    // digitalWrite(FIRST_RELAY, HIGH); delay(1000); digitalWrite(FIRST_RELAY, LOW);
    // digitalWrite(POWER_RELAY, HIGH); delay(1000); digitalWrite(POWER_RELAY, LOW);
  }

  digitalWrite(ONBOARDLED, LOW);

  ArduinoOTA.handle(); // Всегда готовы к прошивке

  static int i = 0;
  static unsigned long last_show_time = 0;

  if (wifiFirstConnected)
  {
    Serial.println("*** wifiFirstConnected ***");
    wifiFirstConnected = false;
    NTP.setInterval(63);            //60 * 5 + 3    //63 Changes sync period. New interval in seconds.
    NTP.setNTPTimeout(NTP_TIMEOUT); //Configure response timeout for NTP requests milliseconds
    startNTP();
    //NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
    NTP.getTimeDateString(); //dummy
  }

  if (syncEventTriggered)
  {
    processSyncEvent(ntpEvent);
    syncEventTriggered = false;
  }

  if ((millis() - last_show_time) > SHOW_TIME_PERIOD or FirstStart)
  {
    //Serial.println(millis() - last_show_time);
    last_show_time = millis();
    Serial.println();
    Serial.print("i = ");
    Serial.print(i);
    Serial.print(", ");
    Serial.print(NTP.getTimeDateString());
    Serial.print(" ");
    Serial.print(NTP.isSummerTime() ? "Summer Time. " : "Winter Time. ");
    Serial.print("WiFi is ");
    Serial.print(WiFi.isConnected() ? "connected" : "not connected");
    Serial.print(". ");
    Serial.print("Uptime: ");
    Serial.print(NTP.getUptimeString());
    Serial.print(" since ");
    Serial.println(NTP.getTimeDateString(NTP.getFirstSync()).c_str());

    Serial.printf("ESP8266 Chip id = %06X", ESP.getChipId());
    Serial.printf(", WiFi.status () = %d", WiFi.status());
    Serial.println(", WiFi.localIP() = " + WiFi.localIP().toString());
    //        Serial.printf ("Free heap: %u\n", ESP.getFreeHeap ());
    i++;
  }

  //TimeValidator();

  if (now() > 100000 and ip != "0.0.0.0" and ntp_sync_period < NTP_SYNC_PERIOD_MAX)
  { //постепенно увеличиваем период обновлений до суток
    ntp_sync_period += 63;
    Serial.print("ntp_sync_period = ");
    Serial.print(ntp_sync_period / 60);
    Serial.println(" minutes");
    NTP.setInterval(ntp_sync_period); // in seconds
    if (loop_delay < LOOP_DELAY_MAX)
    {
      loop_delay += 1; //sec //постепенно увеличиваем период цикла
    }
  }
  else if (now() < 100000 and ip != "0.0.0.0")
  {
    TimeValidator();
  }

  get_and_parse_json_data(time_last_data_get, temp1, time_last_emon_data);

//just for testing
// float r4 = random(0,10); 
//   Serial.print("r4: = ");
//   Serial.println(r4);

//   if (r4 > 7){
//     temp1 = random(15, 18);
//   }
//   else if (r4<4){
//     temp1 = 0;
//   }
//   else{
//     temp1 = 20;
//   }
  
  bool is_temp_change_big = abs(temp1 - temp1_prev) >= temp_change_min; //абсолютноеизменение температуры
  Serial.print("temp_change: ");
  Serial.print(temp1_prev);
  Serial.print(" -> ");
  Serial.print(temp1);
  Serial.print(", is_temp_change_big = ");
  Serial.print(is_temp_change_big);
  Serial.print(", temp_change_min = ");
  Serial.println(temp_change_min);

  Serial.println(OTA_HOSNAME);

  set_power = 0;

  bool is_day = hour() >= DAY_BEGIN and hour() < DAY_END;

  //включаем питание
  if (hour() * 60 + minute() > DAY_BEGIN * 60 + 30 and hour() < DAY_END) //насос выключаем через 30 мин после выкл тенов
  {
    Serial.println("**** POWER OFF  **** ");
    digitalWrite(POWER_RELAY, LOW);
  }
  else
  {
    Serial.println("**** POWER ON  **** ");
    digitalWrite(POWER_RELAY, HIGH);
  }

  if (is_day)
  {
    Serial.println("*** daytime ****");
    set_power = 0;
  }
  else if (hour() >= DAY_END) //>=23
  {
    Serial.println("*** night just started ****");
    if (temp1 < 18)
    {
      set_power = 6;
    }
    else if (temp1 < 18.5)
    {
      set_power = 3;
    }
    else
    {
      set_power = 0;
    }
  }
  else if (hour() < DAY_BEGIN - 3) //<4 
  {
    Serial.println("*** late night from 0 to 4 ****");
    if (temp1 < 18)
    {
      set_power = 6;
    }
    else if (temp1 < 18.5)
    {
      set_power = 3;
    }
    else
    {
      set_power = 0;
    }
  }
  else if (hour() < DAY_BEGIN - 1) // >=4 <6, то есть 4 и 5
  {
    Serial.println("*** from 4 to 6 ****");
    if (temp1 < 18.5)
    {
      set_power = 6;
    }
    else if (temp1 < 19)
    {
      set_power = 3;
    }
    else
    {
      set_power = 0;
    }
  }
  else if (hour() < DAY_BEGIN) //>=4 <7
  {
    Serial.println("*** just before wakeup ****");
    if (temp1 < 20)
    {
      set_power = 6;
    }
    else if (temp1 < 21)
    {
      set_power = 3;
    }
    else
    {
      set_power = 0;
    }
  }

  Serial.print("set_power = ");
  Serial.print(set_power);
  Serial.println(" kW");

  // set_power = 6; //TEST ONLY
  if (is_day or (is_temp_change_big)) // and set_power != set_power_prev
  {
    if (set_power == 3)
    {
      Serial.print("digitalWrite 3kW");
      digitalWrite(SECOND_RELAY, LOW);
      digitalWrite(FIRST_RELAY, HIGH);
    }
    else if (set_power == 6)
    {

      // проверяем, что можно включать 6 кВт
      String json = get_emon_data(emon_get_power_node_id);
      deserializeJson(doc, json);

      float power = doc["value"];

      Serial.println();
      Serial.print("power = ");
      Serial.print(power);
      Serial.println(" W");

      power_overload = false;
      Serial.print("set_power_prev = ");
      Serial.println(set_power_prev);
      Serial.print("power - set_power_prev * 1000 = ");
      Serial.println(power - set_power_prev * 1000);

      if (power - set_power_prev * 1000 > 4000)
      {
        power_overload = true;

        Serial.println();
        Serial.print("***** power overload!!! ******");
        Serial.println();
        Serial.println();

        //так при нуле мощности получим хотя бы 3 кВт, а при 3 как было 3 так и останется
        set_power = 3;
        Serial.print("digitalWrite 3kW");
        digitalWrite(SECOND_RELAY, LOW);
        digitalWrite(FIRST_RELAY, HIGH);
      }
      else
      {
        Serial.print("digitalWrite 6kW");
        digitalWrite(FIRST_RELAY, LOW);
        digitalWrite(SECOND_RELAY, HIGH);
      }
    }
    else
    {
      Serial.print("digitalWrite 0");
      digitalWrite(FIRST_RELAY, LOW);
      digitalWrite(SECOND_RELAY, LOW);
    }

    if (!power_overload and !is_day) //это чтобы включался сразу после 23 и не засчитывались параметры при перегрузке, поскольку при этом нет переключения
    {
      Serial.print("*** temp rewrite ***");
      set_power_prev = set_power;
      temp1_prev = temp1;
    }
  }

  if (Client.connect(EMON_DOMAIN, 80) && (millis() - t_get) > emon_upload_period * 1000)
  {
    t_get = millis();
    Serial.println("program is esp8266-ntp-tenko-timer");
    Serial.print("connect to Server to SEND data: ");
    Client.print("GET /");
    Client.print(EMON_PATH);
    Client.print("/input/post.json?apikey=");
    Client.print(EMON_APIKEY);
    Client.print("&node=");
    Client.print(EMON_SEND_NODE_ID);
    Client.print("&json={set_power:");
    Client.print(set_power);

    if (power_overload)
    {
      Client.print(",overload:1");
    }
    else
    {
      Client.print(",overload:0");
    }

    Client.print("}");
    Client.println();
    //          http://udom.ua/emoncms/input/post.json?node=tutu&fulljson={power1:100,power2:200,power3:300}

    unsigned long old = millis();
    while ((millis() - old) < 500) // 500ms timeout for 'ok' answer from server
    {
      while (Client.available())
      {
        Serial.write(Client.read());
      }
    }
    Client.stop();
    Serial.println("\nclosed");

    if (emon_upload_period < EMON_UPLOAD_PERIOD_MAX)
    {
      emon_upload_period += 1;
    }
  }

  //   digitalWrite(FIRST_RELAY, HIGH);
  // delay(1000);
  // digitalWrite(FIRST_RELAY, LOW);
  // digitalWrite(SECOND_RELAY, HIGH);
  // delay(1000);
  // digitalWrite(SECOND_RELAY, LOW);

  if (emon_temp_check_period < emon_temp_check_period_max)
  {
    emon_temp_check_period++;
  }

  Serial.print("loop_delay = ");
  Serial.print(loop_delay);
  Serial.print("(");
  Serial.print(LOOP_DELAY_MAX);
  Serial.println(") sec");
  Serial.println();
  digitalWrite(ONBOARDLED, HIGH); // Turn off LED
  delay(loop_delay * 1000);       //задержка большого цикла
  FirstStart = false;
}
