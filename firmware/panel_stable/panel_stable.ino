#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <DNSServer.h>
#include <DoubleResetDetector.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>

#define DRD_TIMEOUT 2
#define DRD_ADDRESS 0
#define EEPROM_SIZE 512
#define HTTP_TIMEOUT_MS 20000
#define HTTP_MAX_RETRIES 3
#define CONFIG_EEPROM_START 4

struct SensorResult {
  String text;
  uint16_t color;
};

DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(32, 8, D8,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800);

String displayText = "";
int scrollX = matrix.width();
unsigned long lastScroll = 0;
const int scrollDelay = 150;
bool newDataReceived = false;

char sensorID1[20] = "";
char sensorID2[20] = "";
char customText[30] = "Texto personalizado";
char intervalChar[6] = "600";
unsigned long previousMillis = 0;
unsigned long interval = 600000;

SensorResult result1 = {"", matrix.Color(255, 255, 255)};
SensorResult result2 = {"", matrix.Color(255, 255, 255)};

WiFiManager wifiManager;

void logLine(const String& message) {
  Serial.println("[INFO] " + message);
}

void logValue(const String& label, const String& value) {
  Serial.println("[INFO] " + label + ": " + value);
}

bool isPrintableAscii(char c) {
  return c >= 32 && c <= 126;
}

void sanitizeCharBuffer(char* buffer, size_t size, const char* fallback) {
  bool invalid = false;
  bool hasVisibleChar = false;

  for (size_t i = 0; i < size; i++) {
    if (buffer[i] == '\0') {
      break;
    }
    if (!isPrintableAscii(buffer[i])) {
      invalid = true;
      break;
    }
    if (buffer[i] != ' ') {
      hasVisibleChar = true;
    }
  }

  if (invalid || !hasVisibleChar) {
    strncpy(buffer, fallback, size - 1);
    buffer[size - 1] = '\0';
  } else {
    buffer[size - 1] = '\0';
  }
}

bool isSensorConfigured(const char* sensorID) {
  return sensorID != nullptr && strlen(sensorID) > 0;
}

String wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void clearLEDPanel() {
  matrix.fillScreen(0);
  matrix.show();
}

void saveConfigToEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(CONFIG_EEPROM_START + 0, sensorID1);
  EEPROM.put(CONFIG_EEPROM_START + 20, sensorID2);
  EEPROM.put(CONFIG_EEPROM_START + 40, intervalChar);
  EEPROM.put(CONFIG_EEPROM_START + 60, customText);
  EEPROM.commit();
  EEPROM.end();
}

void loadConfigFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(CONFIG_EEPROM_START + 0, sensorID1);
  EEPROM.get(CONFIG_EEPROM_START + 20, sensorID2);
  EEPROM.get(CONFIG_EEPROM_START + 40, intervalChar);
  EEPROM.get(CONFIG_EEPROM_START + 60, customText);
  EEPROM.end();

  sanitizeCharBuffer(sensorID1, sizeof(sensorID1), "");
  sanitizeCharBuffer(sensorID2, sizeof(sensorID2), "");
  sanitizeCharBuffer(intervalChar, sizeof(intervalChar), "600");
  sanitizeCharBuffer(customText, sizeof(customText), "Texto personalizado");

  unsigned long parsedInterval = atol(intervalChar);
  if (parsedInterval == 0) {
    parsedInterval = 600;
    strncpy(intervalChar, "600", sizeof(intervalChar) - 1);
    intervalChar[sizeof(intervalChar) - 1] = '\0';
  }
  interval = parsedInterval * 1000UL;
}

uint16_t getColorForPMValue(float pmValue) {
  if (pmValue <= 12.0) {
    return matrix.Color(0, 255, 0);
  } else if (pmValue <= 35.4) {
    return matrix.Color(255, 255, 0);
  } else if (pmValue <= 55.4) {
    return matrix.Color(255, 165, 0);
  } else if (pmValue <= 150.0) {
    return matrix.Color(255, 69, 0);
  } else if (pmValue <= 300.0) {
    return matrix.Color(255, 0, 0);
  }
  return matrix.Color(128, 0, 128);
}

uint16_t getColorForTemperature(float tempValue) {
  if (tempValue < 10.0) {
    return matrix.Color(0, 0, 255);
  } else if (tempValue < 25.0) {
    return matrix.Color(0, 255, 0);
  } else if (tempValue < 32.0) {
    return matrix.Color(255, 255, 0);
  }
  return matrix.Color(255, 0, 0);
}

class SensorData {
public:
  String type;
  float value1;
  float value2;
  float value3;
  float value4;

  SensorData(String t, float v1 = 0, float v2 = 0, float v3 = 0, float v4 = 0) {
    type = t;
    value1 = v1;
    value2 = v2;
    value3 = v3;
    value4 = v4;
  }

  String getDisplayText() const {
    if (type == "SDS011") {
      return "PM10:" + String(value1, 1) + " PM2.5:" + String(value2, 1);
    } else if (type == "BME280") {
      return "Temp:" + String(value1, 1) + " Press:" + String(value2, 1) +
             " Hum:" + String(value3, 1) + " PressSL:" + String(value4, 1);
    } else if (type == "BMP280") {
      return "Temp:" + String(value1, 1) + " Press:" + String(value2, 1);
    }
    return "Sensor desconocido";
  }
};

SensorResult fetchData(const String& sensorID) {
  SensorResult result = {"Sin datos", matrix.Color(255, 255, 0)};

  if (sensorID.length() == 0) {
    result.text = "";
    result.color = matrix.Color(255, 255, 255);
    return result;
  }

  if (WiFi.status() != WL_CONNECTED) {
    logLine("WiFi no conectado, no se puede consultar el sensor " + sensorID);
    result.text = "No conectado";
    return result;
  }

  logValue("Consultando sensor", sensorID);
  logValue("Heap antes de request", String(ESP.getFreeHeap()));

  const char* host = "data.sensor.community";
  String uri = "/airrohr/v1/sensor/" + sensorID + "/";
  String payload = "";

  for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
    logValue("HTTP intento", String(attempt));

    WiFiClientSecure localClient;
    localClient.setInsecure();
    localClient.setTimeout(HTTP_TIMEOUT_MS);
    localClient.setBufferSizes(512, 512);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.useHTTP10(true);

    if (!http.begin(localClient, host, 443, uri, true)) {
      logLine("http.begin() fallo para " + sensorID);
      result.text = "HTTP Begin Error";
      delay(1200);
      yield();
      continue;
    }

    int httpCode = http.GET();
    logValue("HTTP code", String(httpCode));

    if (httpCode == HTTP_CODE_OK) {
      payload = http.getString();
      http.end();
      break;
    }

    logValue("HTTP error", http.errorToString(httpCode));
    logValue("WiFi status", wifiStatusToString(WiFi.status()));
    logValue("IP", WiFi.localIP().toString());
    logValue("RSSI", String(WiFi.RSSI()));
    http.end();

    result.text = "HTTP Error";
    if (httpCode < 0) {
      WiFi.disconnect(false);
      delay(500);
      WiFi.reconnect();
      delay(2000);
      yield();
    }
    delay(1500);
    yield();
  }

  if (payload.length() == 0) {
    logLine("Payload vacio o request fallida para sensor " + sensorID);
    if (result.text.length() == 0 || result.text == "Sin datos") {
      result.text = "HTTP Error";
    }
    return result;
  }

  logValue("Payload length", String(payload.length()));
  logValue("Heap despues de payload", String(ESP.getFreeHeap()));

  DynamicJsonDocument doc(6144);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    logValue("JSON error", error.c_str());
    result.text = "JSON Error";
    return result;
  }

  JsonArray data = doc.as<JsonArray>();
  JsonObject mostRecentSensor;
  String mostRecentTimestamp = "";

  for (JsonObject sensor : data) {
    yield();
    String currentSensorID = sensor["sensor"]["id"].as<String>();
    String timestamp = sensor["timestamp"].as<String>();
    if (currentSensorID == sensorID && timestamp > mostRecentTimestamp) {
      mostRecentTimestamp = timestamp;
      mostRecentSensor = sensor;
    }
  }

  if (mostRecentSensor.isNull()) {
    logLine("No se encontro una lectura valida para " + sensorID);
    result.text = "Buscando sensor...";
    return result;
  }

  String sensorType = mostRecentSensor["sensor"]["sensor_type"]["name"].as<String>();
  logValue("Tipo de sensor", sensorType);
  logValue("Timestamp", mostRecentTimestamp);

  SensorData sensorData(sensorType);

  if (sensorType == "SDS011") {
    float pm10 = 0;
    float pm25 = 0;

    for (JsonObject value : mostRecentSensor["sensordatavalues"].as<JsonArray>()) {
      String valueType = value["value_type"].as<String>();
      float parsedValue = value["value"].as<float>();
      if (valueType == "P1") {
        pm10 = parsedValue;
      } else if (valueType == "P2") {
        pm25 = parsedValue;
      }
    }

    sensorData = SensorData(sensorType, pm10, pm25);
    result.color = getColorForPMValue(pm25);
  } else if (sensorType == "BME280") {
    float temperature = 0;
    float pressure = 0;
    float humidity = 0;
    float pressureAtSeaLevel = 0;

    for (JsonObject value : mostRecentSensor["sensordatavalues"].as<JsonArray>()) {
      String valueType = value["value_type"].as<String>();
      float parsedValue = value["value"].as<float>();
      if (valueType == "temperature") temperature = parsedValue;
      else if (valueType == "pressure") pressure = parsedValue;
      else if (valueType == "humidity") humidity = parsedValue;
      else if (valueType == "pressure_at_sealevel") pressureAtSeaLevel = parsedValue;
    }

    sensorData = SensorData(sensorType, temperature, pressure, humidity, pressureAtSeaLevel);
    result.color = getColorForTemperature(temperature);
  } else if (sensorType == "BMP280") {
    float temperature = 0;
    float pressure = 0;

    for (JsonObject value : mostRecentSensor["sensordatavalues"].as<JsonArray>()) {
      String valueType = value["value_type"].as<String>();
      float parsedValue = value["value"].as<float>();
      if (valueType == "temperature") temperature = parsedValue;
      else if (valueType == "pressure") pressure = parsedValue;
    }

    sensorData = SensorData(sensorType, temperature, pressure);
    result.color = getColorForTemperature(temperature);
  } else {
    result.text = "Tipo no soportado";
    result.color = matrix.Color(255, 255, 255);
    return result;
  }

  result.text = sensorData.getDisplayText();
  logValue("Texto sensor", result.text);
  logValue("Heap final request", String(ESP.getFreeHeap()));
  return result;
}

void performSensorUpdate() {
  logLine("Iniciando actualizacion de sensores");
  result1 = fetchData(sensorID1);
  result2 = {"", matrix.Color(255, 255, 255)};

  if (isSensorConfigured(sensorID2)) {
    delay(1000);
    yield();
    result2 = fetchData(sensorID2);
  }

  displayText = String(customText);
  if (result1.text.length() > 0) {
    displayText += " - " + result1.text;
  }
  if (result2.text.length() > 0) {
    displayText += " - " + result2.text;
  }
  newDataReceived = true;

  logValue("Display text", displayText);
}

void resetCustomParameters() {
  logLine("Restableciendo configuracion WiFi y EEPROM");
  WiFi.disconnect(true);
  delay(1000);

  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  EEPROM.end();

  clearLEDPanel();

  strncpy(sensorID1, "", sizeof(sensorID1) - 1);
  sensorID1[sizeof(sensorID1) - 1] = '\0';
  strncpy(sensorID2, "", sizeof(sensorID2) - 1);
  sensorID2[sizeof(sensorID2) - 1] = '\0';
  strncpy(intervalChar, "600", sizeof(intervalChar) - 1);
  intervalChar[sizeof(intervalChar) - 1] = '\0';
  strncpy(customText, "Texto personalizado", sizeof(customText) - 1);
  customText[sizeof(customText) - 1] = '\0';
  interval = 600000;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  logLine("Arranque del firmware estable");
  logValue("Reset reason", ESP.getResetReason());
  logValue("Heap inicial", String(ESP.getFreeHeap()));

  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(40);

  loadConfigFromEEPROM();
  logValue("Sensor ID 1", String(sensorID1));
  logValue("Sensor ID 2", String(sensorID2));
  logValue("Texto personalizado", String(customText));
  logValue("Intervalo ms", String(interval));

  if (drd.detectDoubleReset()) {
    logLine("Doble reset detectado");
    wifiManager.resetSettings();
    resetCustomParameters();
    delay(200);
    ESP.restart();
  }

  WiFiManagerParameter custom_sensor_id("sensor_id", "Sensor PM", sensorID1, 20);
  WiFiManagerParameter custom_sensor_id_2("sensor_id_2", "Sensor Temp/Hum/Press", sensorID2, 20);
  WiFiManagerParameter custom_text_param("custom_text", "Texto Personalizado", customText, 30);
  WiFiManagerParameter custom_interval("interval", "Intervalo (segundos)", intervalChar, 6);

  wifiManager.addParameter(&custom_sensor_id);
  wifiManager.addParameter(&custom_sensor_id_2);
  wifiManager.addParameter(&custom_text_param);
  wifiManager.addParameter(&custom_interval);
  std::vector<const char *> menu = {"wifi", "exit"};
  wifiManager.setMenu(menu);
  wifiManager.setShowInfoUpdate(false);
  wifiManager.setConfigPortalTimeout(180);

  logLine("Lanzando WiFiManager");
  bool wifiOk = wifiManager.autoConnect("AA Panel Led");
  logValue("autoConnect", wifiOk ? "true" : "false");

  strncpy(sensorID1, custom_sensor_id.getValue(), sizeof(sensorID1) - 1);
  sensorID1[sizeof(sensorID1) - 1] = '\0';
  strncpy(sensorID2, custom_sensor_id_2.getValue(), sizeof(sensorID2) - 1);
  sensorID2[sizeof(sensorID2) - 1] = '\0';
  strncpy(customText, custom_text_param.getValue(), sizeof(customText) - 1);
  customText[sizeof(customText) - 1] = '\0';
  strncpy(intervalChar, custom_interval.getValue(), sizeof(intervalChar) - 1);
  intervalChar[sizeof(intervalChar) - 1] = '\0';

  sanitizeCharBuffer(sensorID1, sizeof(sensorID1), "");
  sanitizeCharBuffer(sensorID2, sizeof(sensorID2), "");
  sanitizeCharBuffer(intervalChar, sizeof(intervalChar), "600");
  sanitizeCharBuffer(customText, sizeof(customText), "Texto personalizado");

  interval = atol(intervalChar) * 1000UL;
  if (interval == 0) {
    interval = 600000;
  }

  saveConfigToEEPROM();

  logValue("WiFi status", wifiStatusToString(WiFi.status()));
  logValue("SSID", WiFi.SSID());
  logValue("IP", WiFi.localIP().toString());
  logValue("Heap post WiFi", String(ESP.getFreeHeap()));

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    displayText = "Conectado: " + WiFi.SSID();
    matrix.setTextColor(matrix.Color(0, 255, 0));
    newDataReceived = true;
    performSensorUpdate();
    previousMillis = millis();
  } else {
    displayText = "No conectado";
    matrix.setTextColor(matrix.Color(255, 0, 0));
    newDataReceived = true;
  }
}

void loop() {
  drd.loop();

  if (WiFi.status() != WL_CONNECTED && !WiFi.softAPgetStationNum()) {
    displayText = "Configuracion activada";
    matrix.setTextColor(matrix.Color(255, 255, 0));

    if (millis() - lastScroll >= scrollDelay || newDataReceived) {
      if (newDataReceived) {
        matrix.fillScreen(0);
        scrollX = matrix.width();
        newDataReceived = false;
      }

      matrix.fillScreen(0);
      matrix.setCursor(scrollX, 0);
      matrix.print(displayText);
      matrix.show();
      lastScroll = millis();

      if (--scrollX < -((int)displayText.length() * 6)) {
        scrollX = matrix.width();
      }
    }

    delay(10);
    return;
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    performSensorUpdate();
  }

  if (millis() - lastScroll >= scrollDelay || newDataReceived) {
    if (newDataReceived) {
      matrix.fillScreen(0);
      scrollX = matrix.width();
      newDataReceived = false;
    }

    matrix.fillScreen(0);

    matrix.setTextColor(matrix.Color(255, 255, 255));
    matrix.setCursor(scrollX, 0);
    matrix.print(customText);

    matrix.setTextColor(result1.color);
    matrix.setCursor(scrollX + (String(customText).length() + 2) * 6, 0);
    matrix.print(result1.text);

    matrix.setTextColor(result2.color);
    matrix.setCursor(scrollX + ((String(customText).length() + result1.text.length() + 4) * 6), 0);
    matrix.print(result2.text);

    matrix.show();
    lastScroll = millis();

    if (--scrollX < -((int)displayText.length() * 6)) {
      scrollX = matrix.width();
    }
  }

  delay(10);
}
