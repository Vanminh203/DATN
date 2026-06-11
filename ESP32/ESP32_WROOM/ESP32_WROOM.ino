#include <WiFi.h>
#include <PubSubClient.h>

//  WIFI 
const char* ssid = "RobotFollow";
const char* password = "123456789";

const unsigned long WIFI_RECONNECT_INTERVAL = 5000;
unsigned long lastWifiReconnectAttempt = 0;
bool wifiWasConnected = false;

//  MQTT 
const char* mqtt_server = "10.102.183.12";
const int mqtt_port = 1883;

#define MOVE_TOPIC         "control/move"
#define MODE_TOPIC         "control/mode"
#define SPEED_TOPIC        "control/speed"
#define ROBOT_STATE_TOPIC  "robot/state"

WiFiClient espClient;
PubSubClient client(espClient);

const unsigned long MQTT_RECONNECT_INTERVAL = 3000;
unsigned long lastMqttReconnectAttempt = 0;

//  UART RASPBERRY PI 
HardwareSerial PiSerial(2);
const int PI_UART_RX = 16;
const int PI_UART_TX = 17;
const int PI_UART_BAUD = 115200;

String uartLine = "";

//  MOTOR 
#define ENA 32
#define IN1 33
#define IN2 25
#define IN3 26
#define IN4 27
#define ENB 14

#define CH_A 0
#define CH_B 1
#define PWM_FREQ 1000
#define PWM_RES 8

//  ULTRASONIC 
#define TRIG_PIN 5
#define ECHO_PIN 18

float obstacleDistance = 999.0;
const float STOP_DISTANCE = 35.0;
const float SLOW_DISTANCE = 80.0;

//  CONTROL STATE 
String mode = "Auto";
String lastMoveCmd = "stop";

int requestedSpeed = 30;
int appliedSpeed = 0;

unsigned long lastPrintTime = 0;
const unsigned long PRINT_INTERVAL = 1000;

unsigned long lastStatePublishTime = 0;
const unsigned long STATE_PUBLISH_INTERVAL = 200;

//  TRACKING DATA FROM PI 
int track_found = 0;
int track_error_x = 0;
int track_bbox_area = 0;

unsigned long lastTrackPacketTime = 0;
const unsigned long TRACK_TIMEOUT_MS = 3000;

bool trackFrameStuck = false;

const int TRACK_DEAD_ZONE = 10;

//  AUTO CONTROL 
float Kp = 0.4;
float Kd = 0.35;
int last_error = 0;

//  FUNCTION DECLARATIONS 
void stopRobot(const char* reason = nullptr);

//  WIFI 
void setup_wifi() {
  Serial.println("Connecting WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid, password);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiWasConnected = false;
    Serial.println("\nWiFi connect failed. Will retry in loop.");
  }
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.println("WiFi reconnected!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
    return true;
  }

  if (wifiWasConnected) {
    Serial.println("WiFi lost!");
  }

  wifiWasConnected = false;
  stopRobot();

  if (client.connected()) {
    client.disconnect();
  }

  unsigned long now = millis();
  if (lastWifiReconnectAttempt == 0 || now - lastWifiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
    lastWifiReconnectAttempt = now;
    Serial.println("Trying to reconnect WiFi...");
    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid, password);
  }

  return false;
}

//  MOTOR 
void motorStop() {
  ledcWrite(CH_A, 0);
  ledcWrite(CH_B, 0);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void stopRobot(const char* reason) {
  appliedSpeed = 0;
  last_error = 0;
  lastMoveCmd = "stop";
  motorStop();

  if (reason != nullptr) {
    Serial.println(reason);
  }
}

void driveWheels(int leftPWM, int rightPWM) {
  leftPWM = constrain(leftPWM, -255, 255);
  rightPWM = constrain(rightPWM, -255, 255);

  ledcWrite(CH_A, abs(leftPWM));
  ledcWrite(CH_B, abs(rightPWM));

  if (leftPWM > 0) {
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
  } else if (leftPWM < 0) {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, HIGH);
  } else {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
  }

  if (rightPWM > 0) {
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, HIGH);
  } else if (rightPWM < 0) {
    digitalWrite(IN3, HIGH);
    digitalWrite(IN4, LOW);
  } else {
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, LOW);
  }
}

//  ULTRASONIC 
float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 999.0;

  return duration * 0.0343 / 2.0;
}

float readDistanceFiltered() {
  const int samples = 3;
  float sum = 0.0;
  int count = 0;

  for (int i = 0; i < samples; i++) {
    float d = readDistanceCM();
    if (d > 0 && d < 500) {
      sum += d;
      count++;
    }
    delay(5);
  }

  if (count == 0) return 999.0;
  return sum / count;
}

//  SPEED 
int mapSpeedManual(int userSpeed) {
  if (userSpeed <= 0) return 0;
  userSpeed = constrain(userSpeed, 1, 255);
  return map(userSpeed, 1, 255, 100, 255);
}

int mapSpeedAuto(int level) {
  if (level <= 0) return 0;
  level = constrain(level, 1, 5);
  return map(level, 1, 5, 100, 125);
}

int getSafeManualSpeed(int requested, const String& moveCmd, float distance) {
  if (requested <= 0) return 0;

  if (moveCmd == "forward") {
    if (distance <= STOP_DISTANCE) return 0;

    if (distance <= SLOW_DISTANCE) {
      int reduced = requested * 0.2;
      return max(reduced, 1);
    }
  }

  return requested;
}

//  MQTT STATE 
void publishRobotState() {
  if (!client.connected()) return;

  String json = "{";
  json += "\"mode\":\"" + mode + "\",";
  json += "\"move\":\"" + lastMoveCmd + "\",";
  json += "\"requestedSpeed\":" + String(requestedSpeed) + ",";
  json += "\"appliedSpeed\":" + String(appliedSpeed) + ",";
  json += "\"distance\":" + String(obstacleDistance, 1);
  json += "}";

  client.publish(ROBOT_STATE_TOPIC, json.c_str(), true);
}

//  MANUAL CONTROL 
void executeManualMove(const String& moveCmd) {
  if (moveCmd == "stop") {
    lastMoveCmd = "stop";
    stopRobot();
    publishRobotState();
    return;
  }

  int safeSpeed = getSafeManualSpeed(requestedSpeed, moveCmd, obstacleDistance);
  int pwm = mapSpeedManual(safeSpeed);

  if (pwm == 0) {
    stopRobot();
    publishRobotState();
    return;
  }

  appliedSpeed = safeSpeed;

  if (moveCmd == "forward") {
    driveWheels(pwm, pwm);
  } else if (moveCmd == "backward") {
    driveWheels(-pwm, -pwm);
  } else if (moveCmd == "left") {
    driveWheels(pwm, -pwm);
  } else if (moveCmd == "right") {
    driveWheels(-pwm, pwm);
  } else {
    stopRobot();
  }

  publishRobotState();
}

//  UART TRACKING 
bool parseTrackPacket(String packet) {
  packet.trim();

  if (!packet.startsWith("$TRACK,")) return false;
  if (!packet.endsWith("*")) return false;

  String rawPacket = packet;

  packet.remove(packet.length() - 1);
  packet = packet.substring(7);

  int commaPos[2];
  int commaCount = 0;

  for (int i = 0; i < packet.length(); i++) {
    if (packet[i] == ',') {
      if (commaCount < 2) {
        commaPos[commaCount] = i;
      }
      commaCount++;
    }
  }

  if (commaCount != 2) return false;

  int newFound = packet.substring(0, commaPos[0]).toInt();
  int newErrX = packet.substring(commaPos[0] + 1, commaPos[1]).toInt();
  int newBboxArea = packet.substring(commaPos[1] + 1).toInt();

  static String lastRawPacket = "";
  static int samePacketCount = 1;

  if (rawPacket == lastRawPacket && newFound == 1) {
    samePacketCount++;
  } else {
    samePacketCount = 1;
  }

  trackFrameStuck = (samePacketCount >= 2);
  lastRawPacket = rawPacket;
  track_found = newFound;
  track_error_x = newErrX;
  track_bbox_area = newBboxArea;
  lastTrackPacketTime = millis();

  if (trackFrameStuck) {
    stopRobot("STOP");
  }

  return true;
}

void readTrackingUART() {
  while (PiSerial.available()) {
    char ch = PiSerial.read();

    if (ch == '\r' || ch == '\n') continue;

    if (ch == '$') {
      uartLine = "$";
      continue;
    }

    if (uartLine.length() == 0) continue;

    uartLine += ch;

    if (ch == '*') {
      parseTrackPacket(uartLine);
      uartLine = "";
    }

    if (uartLine.length() > 120) {
      uartLine = "";
    }
  }
}

//  AUTO TRACKING 
void executeAutoTracking() {
  if (mode != "Auto") return;

  if (trackFrameStuck) {
    lastMoveCmd = "stop";
    stopRobot("STOP");
    return;
  }

  if (millis() - lastTrackPacketTime > TRACK_TIMEOUT_MS) {
    lastMoveCmd = "stop";
    stopRobot("STOP");
    return;
  }

  if (track_found == 0) {
    lastMoveCmd = "stop";
    stopRobot();
    return;
  }

  if (track_bbox_area >= 10000) {
    lastMoveCmd = "stop";
    stopRobot("STOP");
    return;
  }

  int err = track_error_x;
  if (abs(err) < TRACK_DEAD_ZONE) err = 0;

  // Nếu quá gần vật cản
  if (obstacleDistance <= 25.0) {
    if (abs(err) > 75) {
      int turnPWM = mapSpeedAuto(2);
      appliedSpeed = 2;
      last_error = err;

      if (err < 0) {
        lastMoveCmd = "left";
        driveWheels(turnPWM, -turnPWM);
      } else {
        lastMoveCmd = "right";
        driveWheels(-turnPWM, turnPWM);
      }
      return;
    }

    lastMoveCmd = "stop";
    stopRobot("STOP: ultrasonic safety");
    return;
  }

  int baseSpeed = 5;
  if (obstacleDistance <= 40.0) {
    baseSpeed = min(baseSpeed, 1);
  }

  if (track_bbox_area > 7000) {
    baseSpeed = min(baseSpeed, 2);
  } else if (track_bbox_area > 5000) {
    baseSpeed = min(baseSpeed, 3);
  }

  int derivative = err - last_error;
  float turnF = Kp * err + Kd * derivative;
  turnF *= min(1.0f, abs(err) / 150.0f);

  if (turnF < 0) turnF *= 0.75f;
  if (abs(err) > 80) turnF *= 0.8f;

  last_error = err;

  int turn = constrain((int)turnF, -3, 5);
  int leftLevel = constrain(baseSpeed - turn, 0, 5);
  int rightLevel = constrain(baseSpeed + turn, 0, 5);

  if (abs(err) < 25) {
    leftLevel = min(baseSpeed, 2);
    rightLevel = min(baseSpeed, 2);
    lastMoveCmd = "forward";
  } else if (err < 0) {
    lastMoveCmd = "left";
  } else if (err > 0) {
    lastMoveCmd = "right";
  } else {
    lastMoveCmd = "forward";
  }

  if (abs(err) < 40 && abs(err) >= 25) {
    leftLevel = constrain(leftLevel, 1, 2);
    rightLevel = constrain(rightLevel, 1, 2);
  }

  int pwmL = mapSpeedAuto(leftLevel);
  int pwmR = mapSpeedAuto(rightLevel);

  if (pwmL == 0 && pwmR == 0) {
    lastMoveCmd = "stop";
    stopRobot();
    return;
  }

  appliedSpeed = constrain((leftLevel + rightLevel + 1) / 2, 0, 5);
  driveWheels(pwmL, pwmR);
}

//  MQTT 
void callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  Serial.print("Topic: ");
  Serial.println(topicStr);
  Serial.print("Message: ");
  Serial.println(message);

  if (topicStr == MODE_TOPIC) {
    if (message == "Auto" || message == "Manual") {
      mode = message;
      lastMoveCmd = "stop";
      stopRobot();
      publishRobotState();
    }
    return;
  }

  if (topicStr == SPEED_TOPIC) {
    requestedSpeed = constrain(message.toInt(), 0, 255);

    if (mode == "Manual") {
      if (requestedSpeed == 0) {
        executeManualMove("stop");
      } else if (lastMoveCmd != "stop") {
        executeManualMove(lastMoveCmd);
      } else {
        publishRobotState();
      }
    } else {
      publishRobotState();
    }
    return;
  }

  if (topicStr == MOVE_TOPIC && mode == "Manual") {
    lastMoveCmd = message;
    executeManualMove(lastMoveCmd);
    return;
  }
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (client.connected()) return;

  unsigned long now = millis();
  if (lastMqttReconnectAttempt != 0 && now - lastMqttReconnectAttempt < MQTT_RECONNECT_INTERVAL) {
    return;
  }

  lastMqttReconnectAttempt = now;
  Serial.print("Connecting MQTT...");

  if (client.connect("ESP32_CAR")) {
    Serial.println("connected");
    client.subscribe(MOVE_TOPIC);
    client.subscribe(MODE_TOPIC);
    client.subscribe(SPEED_TOPIC);
    publishRobotState();
  } else {
    Serial.print("failed, rc=");
    Serial.println(client.state());
  }
}

//  SETUP 
void setup() {
  Serial.begin(115200);
  PiSerial.begin(PI_UART_BAUD, SERIAL_8N1, PI_UART_RX, PI_UART_TX);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  ledcSetup(CH_A, PWM_FREQ, PWM_RES);
  ledcSetup(CH_B, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENA, CH_A);
  ledcAttachPin(ENB, CH_B);

  motorStop();
  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(15);
  client.setSocketTimeout(3);

  Serial.println("ESP32 ready");
}

//  LOOP 
void loop() {
  if (!ensureWiFiConnected()) {
    delay(50);
    return;
  }

  reconnectMQTT();

  if (client.connected()) {
    client.loop();
  }

  readTrackingUART();
  obstacleDistance = readDistanceFiltered();

  if (mode == "Auto") {
    executeAutoTracking();
  } else if (lastMoveCmd != "stop") {
    executeManualMove(lastMoveCmd);
  }

  unsigned long now = millis();

  if (now - lastPrintTime >= PRINT_INTERVAL) {
    lastPrintTime = now;
  }

  if (now - lastStatePublishTime >= STATE_PUBLISH_INTERVAL) {
    lastStatePublishTime = now;
    publishRobotState();
  }

  delay(15);
}
