#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ModbusMaster.h>
#include <SPIFFS.h>
#include <BluetoothSerial.h>

#define TFT_SCLK 18
#define TFT_MOSI 23
#define TFT_CS    5
#define TFT_DC    26
#define TFT_RST   27
#define TFT_BL    25

#define BTN_UP    13
#define BTN_DOWN  32
#define BTN_OK    33

#define RS485_DE  4
#define RS485_RE  14
#define RS485_RX  16
#define RS485_TX  17

#define SENSOR_SLAVE_ID       1
#define REGISTER_COUNT        7
#define SENSOR_START_REGISTER 0x0000

#define BT_DEVICE_NAME   "Agripulse-BT"
#define LOG_PATH         "/log.csv"
#define CSV_HEADER       "timestamp,N,P,K,pH,EC,Moisture,Temp"
#define BUTTON_DEBOUNCE_MS 220
#define SCAN_DURATION_MS 5500
#define SCAN_RETRY_DELAY_MS 250

constexpr bool ENABLE_BLUETOOTH = true;

struct SoilReading {
  float nitrogen = 0.0f;
  float phosphorus = 0.0f;
  float potassium = 0.0f;
  float ph = 0.0f;
  float ec = 0.0f;
  float moisture = 0.0f;
  float temperature = 0.0f;
};

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
BluetoothSerial SerialBT;
ModbusMaster modbus;
Preferences prefs;
uint32_t sensorBaud = 4800;

SoilReading lastReading;
String lastStatus = "Ready";

int menuIndex = 0;
const char *menuItems[] = {"Start scanning", "History"};
const int menuCount = 2;
bool historyView = false;
bool readingView = false;
int historyOffset = 0;
bool accessPointStarted = false;

void setBacklight(bool enabled) {
  digitalWrite(TFT_BL, enabled ? HIGH : LOW);
}

void showMessage(const String &message) {
  lastStatus = message;
  tft.fillRect(0, 120, 128, 40, ST7735_BLACK);
  tft.setCursor(4, 122);
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setTextSize(1);
  tft.print(message.substring(0, 18));
}

String fmt(float v) {
  return String(v, 1);
}

void renderMenu() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN, ST7735_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 4);
  tft.println("AgriPulse");

  for (int i = 0; i < menuCount; ++i) {
    int y = 35 + i * 26;
    uint16_t background = i == menuIndex ? ST7735_BLUE : ST7735_BLACK;
    uint16_t text = i == menuIndex ? ST7735_WHITE : ST7735_GREEN;

    tft.fillRoundRect(6, y - 2, 116, 22, 4, background);
    tft.setTextColor(text, background);
    tft.setTextSize(1);
    tft.setCursor(14, y);
    tft.print(menuItems[i]);
  }

  tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);
  tft.setCursor(4, 110);
  tft.setTextSize(1);
  tft.println("Up/Down/OK");

  showMessage(lastStatus);
}

void renderReading(const SoilReading &reading) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, 5);
  tft.print("Nitrogen");
  tft.setCursor(98, 5);
  tft.print(fmt(reading.nitrogen));

  tft.setCursor(2, 22);
  tft.print("Phosphorus");
  tft.setCursor(98, 22);
  tft.print(fmt(reading.phosphorus));

  tft.setCursor(2, 39);
  tft.print("Potassium");
  tft.setCursor(98, 39);
  tft.print(fmt(reading.potassium));

  tft.setCursor(2, 56);
  tft.print("pH:");
  tft.setCursor(98, 56);
  tft.print(fmt(reading.ph));

  tft.setCursor(2, 73);
  tft.print("EC:");
  tft.setCursor(98, 73);
  tft.print(fmt(reading.ec));

  tft.setCursor(2, 90);
  tft.print("Moisture");
  tft.setCursor(98, 90);
  tft.print(fmt(reading.moisture));

  tft.setCursor(2, 107);
  tft.print("Temperature");
  tft.setCursor(98, 107);
  tft.print(fmt(reading.temperature));
}

void saveReadingToPreferences(const SoilReading &reading) {
  prefs.begin("agri", false);
  prefs.putFloat("N", reading.nitrogen);
  prefs.putFloat("P", reading.phosphorus);
  prefs.putFloat("K", reading.potassium);
  prefs.putFloat("pH", reading.ph);
  prefs.putFloat("EC", reading.ec);
  prefs.putFloat("MOIST", reading.moisture);
  prefs.putFloat("TEMP", reading.temperature);
  prefs.end();
}

void loadReadingFromPreferences() {
  prefs.begin("agri", false);
  lastReading.nitrogen = prefs.isKey("N") ? prefs.getFloat("N", 0.0f) : 0.0f;
  lastReading.phosphorus = prefs.isKey("P") ? prefs.getFloat("P", 0.0f) : 0.0f;
  lastReading.potassium = prefs.isKey("K") ? prefs.getFloat("K", 0.0f) : 0.0f;
  lastReading.ph = prefs.isKey("pH") ? prefs.getFloat("pH", 0.0f) : 0.0f;
  lastReading.ec = prefs.isKey("EC") ? prefs.getFloat("EC", 0.0f) : 0.0f;
  lastReading.moisture = prefs.isKey("MOIST") ? prefs.getFloat("MOIST", 0.0f) : 0.0f;
  lastReading.temperature = prefs.isKey("TEMP") ? prefs.getFloat("TEMP", 0.0f) : 0.0f;
  prefs.end();
}

bool sendHistoryOverBluetooth() {
  if (!SPIFFS.begin() || !SPIFFS.exists(LOG_PATH)) {
    SerialBT.println("ERROR,NO_HISTORY");
    return false;
  }

  File file = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!file) {
    SerialBT.println("ERROR,OPEN_FAILED");
    return false;
  }

  const size_t fileSize = file.size();
  SerialBT.print("FILE,log.csv,");
  SerialBT.println(static_cast<unsigned long>(fileSize));

  uint8_t buffer[128];
  size_t bytesRemaining = fileSize;
  while (bytesRemaining > 0) {
    const size_t bytesToRead = min(bytesRemaining, sizeof(buffer));
    const size_t bytesRead = file.read(buffer, bytesToRead);
    if (bytesRead == 0) {
      file.close();
      return false;
    }

    SerialBT.write(buffer, bytesRead);
    bytesRemaining -= bytesRead;
  }

  file.close();
  return true;
}

bool appendReadingToLog(const SoilReading &reading) {
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount SPIFFS");
    return false;
  }

  bool writeHeader = !SPIFFS.exists(LOG_PATH);
  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("Failed to open log file");
    return false;
  }

  if (writeHeader) {
    f.println(CSV_HEADER);
  }

  String line = "";
  line += String(millis());
  line += "," + String(reading.nitrogen, 1);
  line += "," + String(reading.phosphorus, 1);
  line += "," + String(reading.potassium, 1);
  line += "," + String(reading.ph, 1);
  line += "," + String(reading.ec, 1);
  line += "," + String(reading.moisture, 1);
  line += "," + String(reading.temperature, 1);

  f.println(line);
  f.close();
  return true;
}

String buildJson() {
  String json = "{\n";
  json += "  \"nitrogen\": " + String(lastReading.nitrogen, 1) + ",\n";
  json += "  \"phosphorus\": " + String(lastReading.phosphorus, 1) + ",\n";
  json += "  \"potassium\": " + String(lastReading.potassium, 1) + ",\n";
  json += "  \"ph\": " + String(lastReading.ph, 1) + ",\n";
  json += "  \"ec\": " + String(lastReading.ec, 1) + ",\n";
  json += "  \"moisture\": " + String(lastReading.moisture, 1) + ",\n";
  json += "  \"temperature\": " + String(lastReading.temperature, 1) + "\n";
  json += "}\n";
  return json;
}

bool parseHistoryLine(const String &line, SoilReading &reading) {
  if (line.length() == 0 || line.startsWith("timestamp")) {
    return false;
  }

  float values[REGISTER_COUNT];
  int fieldStart = 0;
  for (int field = 0; field < REGISTER_COUNT + 1; ++field) {
    int fieldEnd = line.indexOf(',', fieldStart);
    if (fieldEnd < 0) {
      fieldEnd = line.length();
    }

    if (field > 0) {
      values[field - 1] = line.substring(fieldStart, fieldEnd).toFloat();
    }

    fieldStart = fieldEnd + 1;
    if (fieldEnd == line.length()) {
      break;
    }
  }

  reading.nitrogen = values[0];
  reading.phosphorus = values[1];
  reading.potassium = values[2];
  reading.ph = values[3];
  reading.ec = values[4];
  reading.moisture = values[5];
  reading.temperature = values[6];
  return true;
}

bool loadHistoryRecord(int offset, SoilReading &reading) {
  if (!SPIFFS.begin(true) || !SPIFFS.exists(LOG_PATH)) {
    return false;
  }

  File file = SPIFFS.open(LOG_PATH, "r");
  if (!file) {
    return false;
  }

  int recordCount = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    SoilReading ignored;
    if (parseHistoryLine(line, ignored)) {
      ++recordCount;
    }
  }
  file.close();

  int target = recordCount - 1 - offset;
  if (target < 0) {
    return false;
  }

  file = SPIFFS.open(LOG_PATH, "r");
  if (!file) {
    return false;
  }

  int recordIndex = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    SoilReading current;
    if (parseHistoryLine(line, current)) {
      if (recordIndex == target) {
        reading = current;
        file.close();
        return true;
      }
      ++recordIndex;
    }
  }

  file.close();
  return false;
}

void renderHistory() {
  SoilReading reading;
  if (loadHistoryRecord(historyOffset, reading)) {
    lastStatus = "History scan";
    renderReading(reading);
    return;
  }

  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN, ST7735_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.println("History");
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setCursor(4, 30);
  tft.println("No saved scans");
  tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);
  tft.setCursor(4, 112);
  tft.println("OK = back");
  showMessage("No history");
}

uint8_t requestSensorRegisters() {
  modbus.clearResponseBuffer();
  modbus.clearTransmitBuffer();

  uint8_t result = modbus.readInputRegisters(SENSOR_START_REGISTER, REGISTER_COUNT);
  if (result != modbus.ku8MBSuccess) {
    modbus.clearResponseBuffer();
    modbus.clearTransmitBuffer();
    result = modbus.readHoldingRegisters(SENSOR_START_REGISTER, REGISTER_COUNT);
  }
  return result;
}

bool readSensorFromModbus() {
  uint32_t configuredBaud = sensorBaud;
  uint8_t result = requestSensorRegisters();
  uint32_t baudUsed = sensorBaud;
  if (result != modbus.ku8MBSuccess) {
    baudUsed = sensorBaud == 9600 ? 4800 : 9600;
    Serial2.updateBaudRate(baudUsed);
    result = requestSensorRegisters();
    if (result == modbus.ku8MBSuccess) {
      sensorBaud = baudUsed;
    } else {
      Serial2.updateBaudRate(sensorBaud);
    }
  }

  if (result == modbus.ku8MBSuccess) {
    SoilReading reading;
    reading.nitrogen = static_cast<float>(modbus.getResponseBuffer(0)) / 10.0f;
    reading.phosphorus = static_cast<float>(modbus.getResponseBuffer(1)) / 10.0f;
    reading.potassium = static_cast<float>(modbus.getResponseBuffer(2)) / 10.0f;
    reading.ph = static_cast<float>(modbus.getResponseBuffer(3)) / 10.0f;
    reading.ec = static_cast<float>(modbus.getResponseBuffer(4)) / 10.0f;
    reading.moisture = static_cast<float>(modbus.getResponseBuffer(5)) / 10.0f;
    reading.temperature = static_cast<float>(modbus.getResponseBuffer(6)) / 10.0f;

    lastReading = reading;
    saveReadingToPreferences(lastReading);
    lastStatus = "Sensor ready";
    Serial.print("Sensor reply at ");
    Serial.print(baudUsed);
    Serial.println(" baud");
    return true;
  }

  lastStatus = "Sensor read failed";
  Serial.print("Modbus read failed at ");
  Serial.print(configuredBaud);
  Serial.print(" and ");
  Serial.print(baudUsed);
  Serial.print(" baud, response code: ");
  Serial.println(result);
  return false;
}

bool scanSensorForDuration() {
  unsigned long scanStarted = millis();
  bool receivedReading = false;

  do {
    if (readSensorFromModbus()) {
      receivedReading = true;
    }
    if (millis() - scanStarted < SCAN_DURATION_MS) {
      delay(SCAN_RETRY_DELAY_MS);
    }
  } while (millis() - scanStarted < SCAN_DURATION_MS);

  lastStatus = receivedReading ? "Sensor ready" : "Sensor read failed";
  return receivedReading;
}

void initTft() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST7735_BLACK);
  tft.setTextWrap(false);
}

void initButtons() {
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
}

void initRs485() {
  pinMode(RS485_DE, OUTPUT);
  pinMode(RS485_RE, OUTPUT);
  digitalWrite(RS485_DE, LOW);
  digitalWrite(RS485_RE, LOW);

  Serial2.begin(sensorBaud, SERIAL_8N1, RS485_RX, RS485_TX);
  modbus.begin(SENSOR_SLAVE_ID, Serial2);
  modbus.preTransmission([]() {
    digitalWrite(RS485_DE, HIGH);
    digitalWrite(RS485_RE, HIGH);
  });
  modbus.postTransmission([]() {
    digitalWrite(RS485_DE, LOW);
    digitalWrite(RS485_RE, LOW);
  });
}

bool startAccessPoint() {
  SerialBT.begin(BT_DEVICE_NAME);
  delay(200);
  Serial.println("Bluetooth ready");
  Serial.print("BT device name: ");
  Serial.println(BT_DEVICE_NAME);
  lastStatus = "Bluetooth ready";
  return true;
}

void handleBluetoothCommands() {
  if (!SerialBT.available()) {
    return;
  }

  String command = SerialBT.readStringUntil('\n');
  command.trim();
  if (command.length() == 0) {
    return;
  }

  if (command.equalsIgnoreCase("SCAN")) {
    if (readSensorFromModbus()) {
      appendReadingToLog(lastReading);
      SerialBT.println(buildJson());
    } else {
      SerialBT.println("ERROR: Sensor read failed");
    }
    return;
  }

  if (command.equalsIgnoreCase("STATUS")) {
    SerialBT.println(buildJson());
    return;
  }

  if (command.equalsIgnoreCase("HISTORY")) {
    sendHistoryOverBluetooth();
    return;
  }

  if (command.equalsIgnoreCase("HELP")) {
    SerialBT.println("Commands: SCAN, STATUS, HISTORY, HELP; HISTORY sends FILE,name,size then file bytes");
    return;
  }

  SerialBT.println("Unknown command. Try: SCAN, STATUS, HISTORY, HELP");
}

void handleOnBoardButtons() {
  static unsigned long lastPress = 0;
  static bool pressHandled = false;
  const bool upPressed = digitalRead(BTN_UP) == LOW;
  const bool downPressed = digitalRead(BTN_DOWN) == LOW;
  const bool okPressed = digitalRead(BTN_OK) == LOW;

  if (!upPressed && !downPressed && !okPressed) {
    pressHandled = false;
    return;
  }

  if (pressHandled || millis() - lastPress < BUTTON_DEBOUNCE_MS) {
    return;
  }
  pressHandled = true;
  lastPress = millis();

  if (readingView) {
    if (okPressed) {
      readingView = false;
      renderMenu();
    }
    return;
  }

  if (historyView) {
    if (upPressed) {
      if (historyOffset > 0) {
        --historyOffset;
        renderHistory();
      } else {
        showMessage("Newest scan");
      }
      return;
    }

    if (downPressed) {
      ++historyOffset;
      if (!loadHistoryRecord(historyOffset, lastReading)) {
        --historyOffset;
        showMessage("Oldest scan");
      } else {
        renderHistory();
      }
      return;
    }

    if (okPressed) {
      historyView = false;
      historyOffset = 0;
      renderMenu();
      return;
    }
  }

  if (upPressed) {
    menuIndex = (menuIndex + menuCount - 1) % menuCount;
    renderMenu();
    return;
  }

  if (downPressed) {
    menuIndex = (menuIndex + 1) % menuCount;
    renderMenu();
    return;
  }

  if (okPressed) {
    if (menuIndex == 0) {
      readingView = true;
      lastStatus = "Scanning...";
      tft.fillScreen(ST7735_BLACK);
      tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
      tft.setTextSize(1);
      tft.setCursor(10, 55);
      tft.print("Scanning...");
      bool receivedReading = scanSensorForDuration();
      if (receivedReading) {
        appendReadingToLog(lastReading);
        if (SerialBT.hasClient()) {
          SerialBT.println(buildJson());
        }
      }
      renderReading(lastReading);
      if (!receivedReading) {
        showMessage("Sensor read failed");
      }
    } else if (menuIndex == 1) {
      historyView = true;
      historyOffset = 0;
      renderHistory();
    }
  }
}

void setup() {
  Serial.begin(115200);
  initTft();
  initButtons();
  initRs485();
  loadReadingFromPreferences();

  if (ENABLE_BLUETOOTH) {
    accessPointStarted = startAccessPoint();
  }
  if (!SPIFFS.begin(true)) {
    Serial.println("Warning: SPIFFS mount failed");
  }

  setBacklight(true);
  renderMenu();
  Serial.println(accessPointStarted ? "Setup complete; Bluetooth enabled" : "Setup complete; Bluetooth disabled");
}

void loop() {
  static uint32_t lastHeartbeat = 0;
  handleOnBoardButtons();
  handleBluetoothCommands();

  const uint32_t now = millis();
  if (now - lastHeartbeat >= 5000) {
    lastHeartbeat = now;
    Serial.println(accessPointStarted ? "Firmware running; Bluetooth enabled" : "Firmware running; Bluetooth disabled");
  }
}