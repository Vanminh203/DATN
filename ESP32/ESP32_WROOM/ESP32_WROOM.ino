#include <WiFi.h>
#include <PubSubClient.h>

//  WIFI 
const char* ssid = "Minh";
const char* password = "11111111";

//  MQTT 
const char* mqtt_server = "192.168.207.12";
const int   mqtt_port   = 1883;

#define MOVE_TOPIC         "control/move"
#define MODE_TOPIC         "control/mode"
#define SPEED_TOPIC        "control/speed"
#define ROBOT_STATE_TOPIC  "robot/state"

WiFiClient espClient;
PubSubClient client(espClient);

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

#define TRIG_PIN 5
#define ECHO_PIN 18

//  CONTROL STATE 
String mode = "Auto";
String lastMoveCmd = "stop";

int requestedSpeed = 150;   // tốc độ do web gửi xuống
int appliedSpeed   = 0;     // tốc độ thực tế sau khi qua safety

float obstacleDistance = 999.0;

// Bù vùng chết motor: slider chỉ cần nhích lên là xe chạy
const int MOTOR_START_PWM = 100;

unsigned long lastPrintTime = 0;
const unsigned long PRINT_INTERVAL = 2000;
// 
// WIFI
// 
void setup_wifi() {
  Serial.println("Connecting WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// 
// MOTOR LOW LEVEL
// 
void motorStop() {
  ledcWrite(CH_A, 0);
  ledcWrite(CH_B, 0);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void motorForwardPWM(int pwm) {
  ledcWrite(CH_A, pwm);
  ledcWrite(CH_B, pwm);

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void motorBackwardPWM(int pwm) {
  ledcWrite(CH_A, pwm);
  ledcWrite(CH_B, pwm);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void motorLeftPWM(int pwm) {
  ledcWrite(CH_A, pwm);
  ledcWrite(CH_B, pwm);

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void motorRightPWM(int pwm) {
  ledcWrite(CH_A, pwm);
  ledcWrite(CH_B, pwm);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

// 
// ULTRASONIC
// 
float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // timeout 30ms

  if (duration == 0) {
    return 999.0; // coi như không có vật cản gần
  }

  float distance = duration * 0.0343 / 2.0;
  return distance;
}

// Đọc trung bình vài mẫu để đỡ giật
float readDistanceFiltered() {
  const int samples = 3;
  float sum = 0.0;
  int validCount = 0;

  for (int i = 0; i < samples; i++) {
    float d = readDistanceCM();
    if (d > 0 && d < 500) {
      sum += d;
      validCount++;
    }
    delay(5);
  }

  if (validCount == 0) return 999.0;
  return sum / validCount;
}

// 
// SPEED MAPPING
// 
// Slider 0..255
// userSpeed = 0  -> PWM 0
// userSpeed > 0  -> PWM từ MOTOR_START_PWM đến 255
int mapSpeedToPWM(int userSpeed) {
  if (userSpeed <= 0) return 0;
  if (userSpeed > 255) userSpeed = 255;

  return map(userSpeed, 1, 255, MOTOR_START_PWM, 255);
}

// 
// SAFETY LAYER
// 
int getSafeSpeed(int requested, const String& moveCmd, float distance) {
  if (requested <= 0) return 0;

  // Chặn / giảm tốc khi đi tới
  // Nếu muốn cả left/right cũng bị ảnh hưởng theo cảm biến trước,
  // bạn có thể đổi điều kiện thành:
  // if (moveCmd == "forward" || moveCmd == "left" || moveCmd == "right")
  if (moveCmd == "forward") {
    if (distance <= 35.0) {
      return 0;
    }

    if (distance <= 70.0) {
      int reduced = (int)(requested * 0.3);
      if (reduced < 1) reduced = 1;
      return reduced;
    }
  }

  return requested;
} 
// MQTT STATE PUBLISH

void publishRobotState() {
  String json = "{";
  json += "\"mode\":\"" + mode + "\",";
  json += "\"move\":\"" + lastMoveCmd + "\",";
  json += "\"requestedSpeed\":" + String(requestedSpeed) + ",";
  json += "\"appliedSpeed\":" + String(appliedSpeed) + ",";
  json += "\"distance\":" + String(obstacleDistance, 1);
  json += "}";

  client.publish(ROBOT_STATE_TOPIC, json.c_str(), true);
} 
// EXECUTE MOVE 
void executeMove(String moveCmd) {
  // Nếu stop thì dừng ngay
  if (moveCmd == "stop") {
    appliedSpeed = 0;
    motorStop();
    publishRobotState();
    return;
  }

  int safeUserSpeed = getSafeSpeed(requestedSpeed, moveCmd, obstacleDistance);
  appliedSpeed = safeUserSpeed;

  int pwm = mapSpeedToPWM(safeUserSpeed);

  if (pwm == 0) {
    motorStop();
    publishRobotState();
    return;
  }

  if (moveCmd == "forward") motorForwardPWM(pwm);
  else if (moveCmd == "backward") motorBackwardPWM(pwm);
  else if (moveCmd == "left") motorLeftPWM(pwm);
  else if (moveCmd == "right") motorRightPWM(pwm);
  else motorStop();

  publishRobotState();
}

// 
// MQTT CALLBACK
// 
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Message: ");
  Serial.println(message);

  // MODE
  if (String(topic) == MODE_TOPIC) {
    mode = message;
    Serial.println("Mode = " + mode);

    // Nếu chuyển sang Auto mà chưa có logic auto thì dừng lại cho an toàn
    if (mode == "Auto") {
      lastMoveCmd = "stop";
      executeMove("stop");
    }

    publishRobotState();
    return;
  }

  // SPEED
  if (String(topic) == SPEED_TOPIC) {
    requestedSpeed = message.toInt();

    if (requestedSpeed < 0) requestedSpeed = 0;
    if (requestedSpeed > 255) requestedSpeed = 255;

    Serial.println("Requested speed = " + String(requestedSpeed));

    if (requestedSpeed == 0) {
      lastMoveCmd = "stop";
      executeMove("stop");
    } else {
      // Nếu đang chạy thì cập nhật tốc độ ngay theo hướng hiện tại
      if (lastMoveCmd != "stop") {
        executeMove(lastMoveCmd);
      } else {
        // Nếu đang đứng yên thì chỉ publish trạng thái
        appliedSpeed = 0;
        publishRobotState();
      }
    }
    return;
  }

  // MANUAL CONTROL
  if (String(topic) == MOVE_TOPIC && mode == "Manual") {
    lastMoveCmd = message;
    executeMove(lastMoveCmd);
    return;
  }
}

// 
// MQTT CONNECT
// 
void reconnect() {
  while (!client.connected()) {
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
      delay(2000);
    }
  }
}

// 
// SETUP
// 
void setup() {
  Serial.begin(115200);

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
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  obstacleDistance = readDistanceFiltered();

  if (millis() - lastPrintTime >= PRINT_INTERVAL) {
    lastPrintTime = millis();

    Serial.print("Distance = ");
    Serial.print(obstacleDistance);
    Serial.println(" cm");
  }

  if (lastMoveCmd != "stop") {
    executeMove(lastMoveCmd);
  }

  static unsigned long lastStatePublish = 0;
  if (millis() - lastStatePublish >= 200) {
    lastStatePublish = millis();
    publishRobotState();
  }

  delay(30);
}