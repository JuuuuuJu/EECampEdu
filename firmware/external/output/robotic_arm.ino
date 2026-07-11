#include <ESP32Servo.h>

Servo baseServo;   // base, 0 - 180
Servo armServo;    // forth and back, 45 - 180
Servo pitchServo;  // height, 30 - 125
Servo clawServo;   // claw, 0 - 90

// Define PWM pin
const int basePin  = 18;
const int armPin   = 19;
const int pitchPin = 22;
const int clawPin  = 21;

// initial angle
int baseAngle = 90;
int armAngle = 90;
int pitchAngle = 90;
int clawAngle = 30;

void setup() {
  Serial.begin(115200);

  //Initiallizing Servos
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  baseServo.attach(basePin, 500, 2500);
  armServo.attach(armPin, 500, 2500);
  pitchServo.attach(pitchPin, 500, 2500);
  clawServo.attach(clawPin, 500, 2500);

  baseServo.write(baseAngle);  
  armServo.write(armAngle);
  pitchServo.write(pitchAngle);
  clawServo.write(clawAngle);
  
  Serial.println("Set");
}

void loop() {
  // 使用靜態 char 陣列作為緩衝區，保留原本高效的記憶體操作
  static char buffer[16];
  static uint8_t idx = 0;

  // 檢查是否有序列埠資料進來
  while (Serial.available() > 0) {
    char c = Serial.read();

    // 遇到換行字元 (Enter) 時進行字串解析
    if (c == '\n' || c == '\r') {
      if (idx > 0) {
        buffer[idx] = '\0'; // 補上 C-string 的結尾字元
        
        // 解析指令：陣列第 0 格為命令字元，第 1 格開始為數值
        char motorChar = buffer[0];
        int value = atoi(buffer + 1); 
        
        // 限制寫入的基礎安全範圍
        value = constrain(value, 0, 180);

        // 依據字元執行對應動作
        switch (motorChar) {
          case 'B': 
          case 'b':
            baseServo.write(value);
            Serial.print("Base (360) speed/dir set to: "); 
            Serial.println(value);
            break;
            
          case 'A': 
          case 'a':
            if(value < 45) value = 45; 
            armAngle = value;
            armServo.write(armAngle);
            Serial.print("Arm angle set to: "); 
            Serial.println(armAngle);
            break;
            
          case 'P': 
          case 'p':
            if(value > 125) value = 125;
            if(value < 30) value = 30;   
            pitchAngle = value;
            pitchServo.write(pitchAngle);
            Serial.print("Pitch angle set to: "); 
            Serial.println(pitchAngle);
            break;
            
          case 'C': 
          case 'c':
            clawAngle = value;
            clawServo.write(clawAngle);
            Serial.print("Claw angle set to: "); 
            Serial.println(clawAngle);
            break;
            
          default:
            Serial.println("Error: Unknown command. Use B/A/P/C + Number.");
            break;
        }

        idx = 0;
      }
    } else {
      
      if (idx < sizeof(buffer) - 1) {
        buffer[idx++] = c;
      }
    }
  }
  delay(50);
}