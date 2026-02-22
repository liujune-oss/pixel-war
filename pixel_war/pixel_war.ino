/*
 * Pixel War (像素大战) - ESP32-S3 Version
 * ----------------------------------------
 * 基于 Pixel Caddy 硬件平台的像素射击游戏
 *
 * 游戏规则:
 * - 敌人从灯带远端生成 (红/绿/蓝三色)
 * - 敌人向玩家方向移动
 * - 按对应颜色按键发射子弹
 * - 同色相撞: 双方消失，得分+1
 * - 异色相撞: 子弹变成敌人
 * - 敌人到达位置0: 游戏结束
 * ----------------------------------------
 */

#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1

#include "AudioPlayer.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Preferences.h>
#include <WiFi.h>

// ================= [NEW] BLE Libraries =================
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "secrets.h"

// ================= 1. 硬件引脚 (ESP32-S3) =================
#define PIN_LED_STRIP 8 // D9 (GPIO 8) - WS2812 数据线
#define PIN_BTN_RED 3   // D2 (GPIO 3) - 红色按键
#define PIN_BTN_GREEN 2 // D1 (GPIO 2) - 绿色按键
#define PIN_BTN_BLUE 4  // D3 (GPIO 4) - 蓝色按键
#define PIN_BUZZER 1    // D0 (GPIO 1) - 蜂鸣器
#define PIN_BATTERY 5   // D4 (GPIO 5) - 电池电压 ADC

// ================= 2. LED 灯带配置 =================
#define NUM_LEDS 180             // 灯带总灯数
#define PLAYER_POS 0             // 玩家位置 (近端)
#define SPAWN_POS (NUM_LEDS - 1) // 敌人生成位置 (远端)

Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_STRIP, NEO_GRB + NEO_KHZ800);

// ================= [NEW] Software Power Limiter =================
void stripShowSafe() {
  uint8_t *pixels = strip.getPixels();
  uint32_t totalSum = 0;
  uint16_t numBytes = NUM_LEDS * 3; // NEO_GRB

  // 1. 统计当前缓冲区的所有亮度值
  for (uint16_t i = 0; i < numBytes; i++) {
    totalSum += pixels[i];
  }

  // 2. 估算电流 (mA)
  // 假设全白 (765) = 60mA -> 1个单位值 ≈ 0.0784mA
  // 加上 ESP32 基础功耗约 100mA
  float estimatedCurrent = (totalSum * 0.0784) + 100;

  const float MAX_CURRENT_MA = 2000.0; // 限制在 2000mA (安全值)

  // 3. 如果超标，计算缩放比例并应用
  if (estimatedCurrent > MAX_CURRENT_MA) {
    float scale = MAX_CURRENT_MA / estimatedCurrent;
    for (uint16_t i = 0; i < numBytes; i++) {
      pixels[i] = (uint8_t)(pixels[i] * scale);
    }
  }

  strip.show();
}

// 颜色定义
const uint32_t COLOR_RED = 0xFF0000;
const uint32_t COLOR_GREEN = 0x00FF00;
const uint32_t COLOR_BLUE = 0x0000FF;
const uint32_t COLOR_WHITE = 0x404040;
const uint32_t COLOR_DIM = 0x050505;
const uint32_t COLOR_OFF = 0x000000;

// 游戏核心参数
const int MAX_PIXELS = NUM_LEDS; // 同屏最大像素对象数
const String FW_VERSION = "v1.2.0";

// ================= [NEW] Game Engine Architecture =================
enum GameMode { MODE_LIGHT_BEAM = 0 };
GameMode currentAppMode = MODE_LIGHT_BEAM;

// ================= 3. 游戏数据结构 =================
struct GamePixel {
  int position; // 0-255
  int color;    // 1=红, 2=绿, 3=蓝
  bool isEnemy; // true=敌人, false=子弹
  bool active;  // 是否激活
};

GamePixel pixels[MAX_PIXELS];

// 状态机
enum GameState {
  STATE_IDLE,      // 待机/开始画面
  STATE_PLAYING,   // 游戏进行中
  STATE_GAME_OVER, // 游戏结束
  STATE_SETTINGS   // 设置菜单
};
GameState currentState = STATE_IDLE;
bool isPaused = false;
bool isDemoMode = false;

// ================= 4. 全局变量 =================
Preferences prefs;
AsyncWebServer server(80);
AudioPlayer audio(PIN_BUZZER);

// WiFi OTA
bool isOTAMode = false;
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

// 固定 IP 配置
IPAddress staticIP(10, 10, 10, 36);
IPAddress gateway(10, 10, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(10, 10, 10, 1);

// 游戏参数
int score = 0;
int highScore = 0;
int currentBrightness = 30;
int currentVolume = 30;
int difficulty = 1; // 难度等级 1-10

// 游戏速度控制
unsigned long lastEnemyMove = 0;
unsigned long lastBulletMove = 0;
unsigned long lastEnemySpawn = 0;
int enemyMoveInterval = 150;  // 敌人移动间隔 (ms)
int enemySpawnInterval = 800; // 波内敌人生成间隔 (ms)

// 波次系统
int waveEnemyCount = 3;          // 每波敌人数量
int waveEnemiesSpawned = 0;      // 当前波已生成的敌人数
int waveNumber = 0;              // 当前波次编号
bool waveActive = true;          // 当前是否在出敌人
unsigned long waveRestStart = 0; // 波间休息开始时间
int waveRestDuration = 3000;     // 波间休息时长 (ms)

// 按键防抖
unsigned long lastButtonPress = 0;
const int BUTTON_COOLDOWN = 150;
int lastStateRed = HIGH;
int lastStateGreen = HIGH;
int lastStateBlue = HIGH;

// 长按检测
unsigned long pressTimeRed = 0;
bool longPressHandledRed = false;

// 设置菜单
int settingsMode = 0; // 0=亮度, 1=音量, 2=难度
const int SETTINGS_MODE_COUNT = 3;
const int SETTING_STEP = 10;

// ================= 10. 全局变量 =================
// 屏保
unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT = 5 * 60 * 1000; // 5分钟
bool isScreenSaver = false;

// 动画效果
unsigned long gameOverAnimTimer = 0;
int gameOverAnimFrame = 0;

// ================= [NEW] BLE Objects & Logic =================
#define SERVICE_UUID "7a0247cb-0000-48a8-b611-3957ce9fb6a4"
#define CHAR_TX_UUID "7a0247cb-0001-48a8-b611-3957ce9fb6a4"
#define CHAR_RX_UUID "7a0247cb-0002-48a8-b611-3957ce9fb6a4"

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
BLECharacteristic *pRxCharacteristic = NULL;
int deviceConnectedCount = 0;
bool advertisingRestartRequested = false;
unsigned long bleFeedbackTimer = 0; // [NEW] Timer to show BLE visual feedback

// Forward declarations
void lightBeam_initGame();
void saveData();

// ================= [NEW] BLE Callbacks =================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnectedCount++;
    // Force Enable Notifications (Hack for Windows/some WebBLE)
    BLEDescriptor *pDesc =
        pTxCharacteristic->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    if (pDesc) {
      uint8_t on[] = {0x01, 0x00};
      pDesc->setValue(on, 2);
    }
    Serial.printf("Device Connected. Count: %d\n", deviceConnectedCount);
    advertisingRestartRequested = true;
  }
  void onDisconnect(BLEServer *pServer) {
    deviceConnectedCount--;
    if (deviceConnectedCount < 0)
      deviceConnectedCount = 0;
    advertisingRestartRequested = true;
    Serial.printf("Device Disconnected. Count: %d\n", deviceConnectedCount);
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() > 0) {
      Serial.printf("BLE RX: %s\n", value.c_str());

      // Parse Commands
      if (value == "RESTART") {
        lightBeam_initGame();
      } else if (value == "OTA") {
        isOTAMode = true;
        setupOTA();
      } else if (value == "PAUSE") {
        if (currentState == STATE_PLAYING) {
          isPaused = !isPaused;
          if (!isPaused) {
            // 恢复时重置计时器防止跳跃
            unsigned long now = millis();
            lastEnemyMove = now;
            lastBulletMove = now;
            lastEnemySpawn = now;
          }
        }
      } else if (value == "DEMO") {
        isDemoMode = true;
        lightBeam_initGame();
      } else if (value == "SYNC") {
        String json = "{\"type\":\"sync\",\"fw\":\"" + String(FW_VERSION) +
                      "\",\"m\":" + String((int)currentAppMode) +
                      ",\"brt\":" + String(currentBrightness) +
                      ",\"vol\":" + String(currentVolume) +
                      ",\"diff\":" + String(difficulty) +
                      ",\"p\":" + String(isPaused ? 1 : 0) +
                      ",\"d\":" + String(isDemoMode ? 1 : 0) + "}";
        pTxCharacteristic->setValue((uint8_t *)json.c_str(), json.length());
        pTxCharacteristic->notify();
      } else if (value.startsWith("B:")) {
        currentBrightness = value.substring(2).toInt();
        if (currentBrightness < 5)
          currentBrightness = 5;
        if (currentBrightness > 100)
          currentBrightness = 100;
        strip.setBrightness(currentBrightness * 255 / 100);

        // 实时灯光反馈
        currentState = STATE_SETTINGS;
        settingsMode = 0;
        bleFeedbackTimer = millis(); // [NEW] Stall normal rendering
        drawSettings();

        saveData();
      } else if (value.startsWith("V:")) {
        currentVolume = value.substring(2).toInt();
        if (currentVolume < 0)
          currentVolume = 0;
        if (currentVolume > 100)
          currentVolume = 100;
        audio.setVolume(currentVolume);

        // 实时声音反馈 (长蜂鸣声测试音量)
        currentState = STATE_SETTINGS;
        settingsMode = 1;
        bleFeedbackTimer = millis();
        drawSettings();

        audio.playBeep();

        saveData();
      } else if (value.startsWith("D:")) {
        difficulty = value.substring(2).toInt();
        if (difficulty < 1)
          difficulty = 1;
        if (difficulty > 14)
          difficulty = 14;

        currentState = STATE_SETTINGS;
        settingsMode = 2;
        bleFeedbackTimer = millis();
        drawSettings();

        audio.playBeep();
        saveData();
      } else if (value.startsWith("MODE:")) {
        int newMode = value.substring(5).toInt();
        if (newMode == 0) {
          currentAppMode = MODE_LIGHT_BEAM;
          currentState = STATE_IDLE; // Switch mode resets the game state
          lightBeam_initGame();
          audio.play1UP();
        }
      }
    }
  }
};

// ================= [NEW] Battery Monitor =================
// 读取电池电压 (单位: mV) - 带滤波
int readBatteryVoltage() {
  // ESP32-S3 ADC: 12-bit (0-4095), 参考电压约 3.3V
  // 电压分压 1:1，实际电压 = ADC电压 × 2
  long sum = 0;
  const int samples = 8;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(PIN_BATTERY);
    delayMicroseconds(500); // 0.5ms 间隔
  }
  int adcValue = sum / samples;
  int batteryMV = (adcValue * 3300 * 2) / 4095;
  return batteryMV;
}

// 转换电压为百分比 - 5 位滑动窗口滤波
int mvHistory[5] = {0, 0, 0, 0, 0};
int mvHistoryIndex = 0;
bool mvHistoryFilled = false;

int getBatteryPercent() {
  int mv = readBatteryVoltage();

  mvHistory[mvHistoryIndex] = mv;
  mvHistoryIndex = (mvHistoryIndex + 1) % 5;
  if (mvHistoryIndex == 0)
    mvHistoryFilled = true;

  int count = mvHistoryFilled ? 5 : mvHistoryIndex;
  if (count == 0)
    count = 1;
  long sum = 0;
  for (int i = 0; i < count; i++) {
    sum += mvHistory[i];
  }
  int avgMv = sum / count;

  // [调试] 输出原始电压值
  static unsigned long lastDebugTime = 0;
  if (millis() - lastDebugTime > 10000) { // 每10秒输出一次
    lastDebugTime = millis();
    Serial.printf("[BATT DEBUG] Raw mV: %d, Avg mV: %d\n", mv, avgMv);
  }

  // [校准] 实际满电约为 3900mV (留有负载压降余量)，空电约 3300mV
  int percent;
  if (avgMv >= 3900)
    percent = 100;
  else if (avgMv <= 3300)
    percent = 0;
  else
    percent = (avgMv - 3300) * 100 / 600;

  return percent;
}

// ================= 5. 数据存取 =================
void loadData() {
  prefs.begin("pixelwar", false);
  highScore = prefs.getInt("highscore", 0);
  currentBrightness = prefs.getInt("brt", 30);
  currentVolume = prefs.getInt("vol", 30);
  difficulty = prefs.getInt("diff", 1);
  prefs.end();

  audio.setVolume(currentVolume);
}

void saveData() {
  prefs.begin("pixelwar", false);
  prefs.putInt("highscore", highScore);
  prefs.putInt("brt", currentBrightness);
  prefs.putInt("vol", currentVolume);
  prefs.putInt("diff", difficulty);
  prefs.end();
}

// ================= 6. 颜色辅助函数 =================
uint32_t getColorByType(int type) {
  switch (type) {
  case 1:
    return COLOR_RED;
  case 2:
    return COLOR_GREEN;
  case 3:
    return COLOR_BLUE;
  default:
    return COLOR_OFF;
  }
}

int getColorType(uint32_t color) {
  if (color == COLOR_RED)
    return 1;
  if (color == COLOR_GREEN)
    return 2;
  if (color == COLOR_BLUE)
    return 3;
  return 0;
}

// ================= 7. 游戏逻辑 =================
void lightBeam_initGame() {
  score = 0;
  difficulty = 1;
  enemyMoveInterval = 150;
  enemySpawnInterval = 800;

  // 波次初始化
  waveNumber = 0;
  waveEnemyCount = 3;
  waveEnemiesSpawned = 0;
  waveActive = true;
  waveRestDuration = 3000;

  // 清空所有像素
  for (int i = 0; i < MAX_PIXELS; i++) {
    pixels[i].active = false;
  }

  currentState = STATE_PLAYING;
  isPaused = false;

  if (!isDemoMode) {
    audio.playBeep();
  }
}

int findFreePixelSlot() {
  for (int i = 0; i < MAX_PIXELS; i++) {
    if (!pixels[i].active)
      return i;
  }
  return -1;
}

void lightBeam_spawnEnemy() {
  int slot = findFreePixelSlot();
  if (slot < 0)
    return;

  // 检查生成位置是否已被占用
  for (int i = 0; i < MAX_PIXELS; i++) {
    if (pixels[i].active && pixels[i].position >= SPAWN_POS - 5) {
      return; // 太拥挤了，不生成
    }
  }

  pixels[slot].position = SPAWN_POS;
  pixels[slot].color = random(1, 4); // 1-3
  pixels[slot].isEnemy = true;
  pixels[slot].active = true;
}

void lightBeam_spawnBullet(int color) {
  int slot = findFreePixelSlot();
  if (slot < 0)
    return;

  // 检查发射位置是否已被占用
  for (int i = 0; i < MAX_PIXELS; i++) {
    if (pixels[i].active && pixels[i].position <= 3 && !pixels[i].isEnemy) {
      return; // 发射位置被占用
    }
  }

  pixels[slot].position = PLAYER_POS + 1;
  pixels[slot].color = color;
  pixels[slot].isEnemy = false;
  pixels[slot].active = true;

  audio.playBeep();
}

void lightBeam_checkCollisions() {
  for (int i = 0; i < MAX_PIXELS; i++) {
    if (!pixels[i].active)
      continue;

    for (int j = i + 1; j < MAX_PIXELS; j++) {
      if (!pixels[j].active)
        continue;

      // 检查是否相撞 (位置相同或交叉)
      int dist = abs(pixels[i].position - pixels[j].position);
      if (dist > 2)
        continue;

      // 一个是敌人，一个是子弹
      bool iEnemy = pixels[i].isEnemy;
      bool jEnemy = pixels[j].isEnemy;

      if (iEnemy == jEnemy)
        continue; // 同类不碰撞

      int enemyIdx = iEnemy ? i : j;
      int bulletIdx = iEnemy ? j : i;

      if (pixels[enemyIdx].color == pixels[bulletIdx].color) {
        // 同色: 双方消失
        pixels[enemyIdx].active = false;
        pixels[bulletIdx].active = false;
        score++;
        audio.playHit();

        // 每10分增加难度
        if (score % 10 == 0 && score > 0) {
          difficulty++;
          if (difficulty > 14)
            difficulty = 14;
          // 敌人移速加快 (最快10ms，与子弹同速)
          enemyMoveInterval = max(10, 150 - difficulty * 10);
          // 波次参数由波次系统自动调整
          audio.play1UP();
        }
      } else {
        // 异色: 子弹变敌人，寻找不重叠的空位
        pixels[bulletIdx].isEnemy = true;
        int targetPos = pixels[enemyIdx].position + 5;
        // 向远端搜索空闲位置，避免与其他像素重叠
        bool posOccupied = true;
        while (posOccupied && targetPos < NUM_LEDS) {
          posOccupied = false;
          for (int k = 0; k < MAX_PIXELS; k++) {
            if (k != bulletIdx && pixels[k].active &&
                abs(pixels[k].position - targetPos) < 3) {
              posOccupied = true;
              targetPos += 3;
              break;
            }
          }
        }
        pixels[bulletIdx].position = min(NUM_LEDS - 1, targetPos);
        audio.playBad();
      }
    }
  }
}

void lightBeam_updateEnemies() {
  for (int i = 0; i < MAX_PIXELS; i++) {
    if (!pixels[i].active || !pixels[i].isEnemy)
      continue;

    pixels[i].position--;

    // 检查是否到达玩家位置
    if (pixels[i].position <= PLAYER_POS) {
      currentState = STATE_GAME_OVER;
      if (score > highScore) {
        highScore = score;
        saveData();
      }
      gameOverAnimTimer = millis();
      gameOverAnimFrame = 0;
      audio.playBad();
      return;
    }
  }
}

void lightBeam_updateBullets() {
  for (int i = 0; i < MAX_PIXELS; i++) {
    if (!pixels[i].active || pixels[i].isEnemy)
      continue;

    pixels[i].position++;

    // 子弹飞出屏幕
    if (pixels[i].position >= NUM_LEDS) {
      pixels[i].active = false;
    }
  }
}

// ================= 8. 显示函数 =================
void lightBeam_drawLEDs() {
  // 拖影效果: 每帧将所有LED亮度衰减，而非完全清除
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = (uint8_t)(c >> 16);
    uint8_t g = (uint8_t)(c >> 8);
    uint8_t b = (uint8_t)c;
    // 衰减到约80%亮度，产生较长拖尾效果
    r = r * 4 / 5;
    g = g * 4 / 5;
    b = b * 4 / 5;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }

  // 绘制所有像素
  for (int i = 0; i < MAX_PIXELS; i++) {
    if (!pixels[i].active)
      continue;
    if (pixels[i].position < 0 || pixels[i].position >= NUM_LEDS)
      continue;

    uint32_t color = getColorByType(pixels[i].color);
    strip.setPixelColor(pixels[i].position, color);

    // 敌人手动拖尾: 在移动方向后方绘制渐变尾巴
    if (pixels[i].isEnemy) {
      uint8_t cr = (uint8_t)(color >> 16);
      uint8_t cg = (uint8_t)(color >> 8);
      uint8_t cb = (uint8_t)color;
      const int tailLen = 4; // 拖尾长度
      for (int t = 1; t <= tailLen; t++) {
        int tailPos = pixels[i].position + t; // 敌人向左移动，尾巴在右边
        if (tailPos >= 0 && tailPos < NUM_LEDS) {
          int fade = 255 * (tailLen - t + 1) / (tailLen + 2);
          strip.setPixelColor(
              tailPos,
              strip.Color(cr * fade / 255, cg * fade / 255, cb * fade / 255));
        }
      }
    }
  }

  // 玩家位置指示 (白色闪烁)
  static unsigned long lastBlink = 0;
  static bool blinkOn = true;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    blinkOn = !blinkOn;
  }
  if (blinkOn) {
    strip.setPixelColor(PLAYER_POS, COLOR_WHITE);
  }

  stripShowSafe();
}

void lightBeam_drawIdleScreen() {
  static unsigned long lastAnim = 0;
  static int animPos = 0;

  if (millis() - lastAnim > 100) {
    lastAnim = millis();
    animPos = (animPos + 1) % NUM_LEDS;
  }

  strip.clear();

  // 彩虹波效果
  for (int i = 0; i < NUM_LEDS; i++) {
    int hue = ((i + animPos) * 256 / NUM_LEDS) % 256;
    int brightness = 50;

    // 简单的 HSV to RGB (只用色调)
    int r, g, b;
    if (hue < 85) {
      r = hue * 3;
      g = 255 - hue * 3;
      b = 0;
    } else if (hue < 170) {
      hue -= 85;
      r = 255 - hue * 3;
      g = 0;
      b = hue * 3;
    } else {
      hue -= 170;
      r = 0;
      g = hue * 3;
      b = 255 - hue * 3;
    }

    r = r * brightness / 255;
    g = g * brightness / 255;
    b = b * brightness / 255;

    strip.setPixelColor(i, strip.Color(r, g, b));
  }

  stripShowSafe();
}

void lightBeam_drawGameOver() {
  if (millis() - gameOverAnimTimer > 200) {
    gameOverAnimTimer = millis();
    gameOverAnimFrame++;
  }

  // 前4帧红色闪烁，之后显示分数
  if (gameOverAnimFrame < 8) {
    // 红色闪烁
    strip.clear();
    if (gameOverAnimFrame % 2 == 0) {
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, COLOR_RED);
      }
    }
    stripShowSafe();
    return;
  }

  // 显示分数 (算筹风格)
  strip.clear();

  int digits[4];
  int s = score;
  digits[0] = s / 1000;
  s %= 1000; // 千位
  digits[1] = s / 100;
  s %= 100;           // 百位
  digits[2] = s / 10; // 十位
  digits[3] = s % 10; // 个位

  // 颜色: 千=红, 百=绿, 十=蓝, 个=黄
  uint32_t digitColors[4] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, 0xFFAA00};
  int pos = 2; // 起始偏移

  for (int d = 0; d < 4; d++) {
    int digit = digits[d];
    int fives = digit / 5; // 几个5
    int ones = digit % 5;  // 余几个1
    uint32_t col = digitColors[d];

    // 画 "5" 的标记 (5个连续像素)
    for (int f = 0; f < fives; f++) {
      for (int p = 0; p < 5 && pos < NUM_LEDS; p++, pos++) {
        strip.setPixelColor(pos, col);
      }
      if (ones > 0 || f < fives - 1)
        pos += 2; // 组间间隔2像素
    }

    // 画 "1" 的标记 (1个像素)
    for (int o = 0; o < ones; o++) {
      if (pos < NUM_LEDS) {
        strip.setPixelColor(pos, col);
        pos++;
      }
      if (o < ones - 1)
        pos += 2; // 组间间隔2像素
    }

    // 白色分隔符 (最后一位后面不加)
    if (d < 3) {
      pos += 2; // 留空
      for (int w = 0; w < 3 && pos < NUM_LEDS; w++, pos++) {
        strip.setPixelColor(pos, COLOR_WHITE);
      }
      pos += 2; // 留空
    }
  }

  stripShowSafe();
}

void drawSettings() {
  strip.clear();

  int value = 0;
  uint32_t color = COLOR_WHITE;

  if (settingsMode == 0) {
    // 亮度设置
    value = currentBrightness;
    color = COLOR_GREEN; // 亮度 = 绿色
  } else if (settingsMode == 1) {
    value = currentVolume;
    color = COLOR_BLUE; // 音量 = 蓝色
  } else {
    value = difficulty * 10;
    color = COLOR_RED; // 难度 = 红色
  }

  // 显示进度条 (前 value/10 个LED亮)
  int litLeds = value * NUM_LEDS / 100;
  for (int i = 0; i < litLeds; i++) {
    strip.setPixelColor(i, color);
  }

  stripShowSafe();
}

// ================= 9. WiFi OTA =================
void setupOTA() {
  strip.clear();
  strip.setPixelColor(0, COLOR_BLUE);
  stripShowSafe();

  WiFi.mode(WIFI_STA);

  // 使用固定 IP
  if (!WiFi.config(staticIP, gateway, subnet, dns)) {
    Serial.println("Static IP Failed!");
  }

  WiFi.begin(ssid, password);

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
    delay(500);
    strip.setPixelColor(0, (timeout % 2 == 0) ? COLOR_BLUE : COLOR_OFF);
    stripShowSafe();
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("pixelwar")) {
      Serial.println("mDNS started: pixelwar.local");
    }
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());

    // 显示连接成功 (绿色)
    for (int i = 0; i < 10; i++) {
      strip.setPixelColor(i, COLOR_GREEN);
    }
    stripShowSafe();
  } else {
    // 连接失败，启动 AP 模式
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("PixelWar OTA", "12345678");
    Serial.println("AP Mode: PixelWar OTA");

    // 显示 AP 模式 (红色)
    for (int i = 0; i < 10; i++) {
      strip.setPixelColor(i, COLOR_RED);
    }
    stripShowSafe();
  }

  ElegantOTA.begin(&server);
  server.begin();
  audio.play1UP();
}

// ================= 10. 按键处理 =================
void lightBeam_handleButtons() {
  int rRed = digitalRead(PIN_BTN_RED);
  int rGreen = digitalRead(PIN_BTN_GREEN);
  int rBlue = digitalRead(PIN_BTN_BLUE);
  unsigned long now = millis();

  bool anyKeyPressed = (rRed == LOW || rGreen == LOW || rBlue == LOW);

  if (anyKeyPressed) {
    lastActivityTime = now;
    if (isScreenSaver) {
      isScreenSaver = false;
      return;
    }
  }

  // 组合键: 红+绿 进入 OTA
  if (rRed == LOW && rGreen == LOW) {
    isOTAMode = true;
    setupOTA();
    while (digitalRead(PIN_BTN_RED) == LOW ||
           digitalRead(PIN_BTN_GREEN) == LOW) {
      delay(10);
    }
    return;
  }

  // 处理不同状态
  if (currentState == STATE_IDLE) {
    // 任意键开始游戏
    if (now - lastButtonPress > BUTTON_COOLDOWN) {
      if ((rRed == LOW && lastStateRed == HIGH) ||
          (rGreen == LOW && lastStateGreen == HIGH) ||
          (rBlue == LOW && lastStateBlue == HIGH)) {
        lastButtonPress = now;
        lightBeam_initGame();
      }
    }
  } else if (currentState == STATE_PLAYING) {
    if (now - lastButtonPress > BUTTON_COOLDOWN) {
      if (isDemoMode) {
        // [试玩模式] 按下任意键结束试玩并开始真正的游戏
        if ((rRed == LOW && lastStateRed == HIGH) ||
            (rGreen == LOW && lastStateGreen == HIGH) ||
            (rBlue == LOW && lastStateBlue == HIGH)) {
          lastButtonPress = now;
          isDemoMode = false;
          lightBeam_initGame();
        }
      } else if (!isPaused) {
        // [正常游戏情况] 发射子弹
        if (rRed == LOW && lastStateRed == HIGH) {
          lastButtonPress = now;
          pressTimeRed = now;
          longPressHandledRed = false;
          lightBeam_spawnBullet(1); // 红色子弹
        }
        if (rGreen == LOW && lastStateGreen == HIGH) {
          lastButtonPress = now;
          lightBeam_spawnBullet(2); // 绿色子弹
        }
        if (rBlue == LOW && lastStateBlue == HIGH) {
          lastButtonPress = now;
          lightBeam_spawnBullet(3); // 蓝色子弹
        }
      } else {
        // [暂停状态] 处理恢复和退出
        if (rGreen == LOW && lastStateGreen == HIGH) {
          // 暂停时按绿键：退出试玩 (DEMO) 并回到 IDLE
          lastButtonPress = now;
          currentState = STATE_IDLE;
          isPaused = false;
          isScreenSaver = false;
          lastActivityTime = millis();
        }
        if (rBlue == LOW && lastStateBlue == HIGH) {
          // 暂停时按蓝键：恢复游戏 (取消暂停)
          lastButtonPress = now;
          isPaused = false;
          // 恢复时重置计时器防止跳跃
          lastEnemyMove = now;
          lastBulletMove = now;
          lastEnemySpawn = now;
          audio.playBeep();
        }
      }
    }
    // 长按红键进入设置
    if (rRed == LOW && !longPressHandledRed && (now - pressTimeRed > 3000)) {
      longPressHandledRed = true;
      currentState = STATE_SETTINGS;
      settingsMode = 0;
      audio.play1UP();
      while (digitalRead(PIN_BTN_RED) == LOW)
        delay(10);
    }
  } else if (currentState == STATE_GAME_OVER) {
    // 任意键返回待机
    if (now - lastButtonPress > 1000) { // 1秒冷却防止误触
      if ((rRed == LOW && lastStateRed == HIGH) ||
          (rGreen == LOW && lastStateGreen == HIGH) ||
          (rBlue == LOW && lastStateBlue == HIGH)) {
        lastButtonPress = now;
        currentState = STATE_IDLE;
        audio.playBeep();
      }
    }
  }

  lastStateRed = rRed;
  lastStateGreen = rGreen;
  lastStateBlue = rBlue;
}

// ================= 11. 屏保检测 =================
void checkSleepTimeout() {
  if (!isScreenSaver && (millis() - lastActivityTime > SLEEP_TIMEOUT)) {
    isScreenSaver = true;
    strip.clear();
    stripShowSafe();
  }
}

// ================= 12. Setup & Loop =================
void setup() {
  Serial.begin(115200);
  Serial.println("Pixel War Starting...");

  // 初始化按键
  pinMode(PIN_BTN_RED, INPUT_PULLUP);
  pinMode(PIN_BTN_GREEN, INPUT_PULLUP);
  pinMode(PIN_BTN_BLUE, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);

  // 初始化音频
  audio.begin();

  // 初始化 LED 灯带
  strip.begin();
  strip.setBrightness(30);
  stripShowSafe();

  // ================= [NEW] 初始化 BLE 服务 =================
  BLEDevice::init("Pixel War");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // TX Characteristic (Notify)
  pTxCharacteristic = pService->createCharacteristic(
      CHAR_TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  pTxCharacteristic->addDescriptor(new BLE2902());

  // RX Characteristic (Write)
  pRxCharacteristic = pService->createCharacteristic(
      CHAR_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pService->start();

  // 广告设置
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(
      0x06); // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE Ready.");

  // 开机时检测组合键进入 OTA 模式
  if (digitalRead(PIN_BTN_RED) == LOW && digitalRead(PIN_BTN_GREEN) == LOW) {
    isOTAMode = true;
    setupOTA();
    return;
  }

  // 加载数据
  loadData();
  strip.setBrightness(currentBrightness * 255 / 100);

  // 随机数种子
  randomSeed(analogRead(0));

  // 启动动画
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, COLOR_GREEN);
    stripShowSafe();
    delay(2);
  }
  delay(200);
  strip.clear();
  stripShowSafe();

  audio.playMario();
  lastActivityTime = millis();

  Serial.println("Pixel War Ready!");
}

void loop() {
  // 更新音频
  audio.update();

  // OTA 模式
  if (isOTAMode) {
    ElegantOTA.loop();
    return;
  }

  // 屏保检测
  checkSleepTimeout();
  if (isScreenSaver) {
    if (currentAppMode == MODE_LIGHT_BEAM) {
      lightBeam_handleButtons();
    }
    delay(100);
    return;
  }

  unsigned long now = millis();

  // ================= 10. 状态机与显示更新 =================
  if (bleFeedbackTimer > 0) {
    if (now - bleFeedbackTimer < 1200) {
      if (currentState == STATE_SETTINGS) {
        drawSettings();
      }
      return;
    } else {
      bleFeedbackTimer = 0;
      currentState = STATE_IDLE; // Feedback over, return to idle
    }
  }

  // 根据当前游戏模式分发逻辑
  switch (currentAppMode) {
  case MODE_LIGHT_BEAM:
    lightBeam_handleButtons();

    if (currentState == STATE_IDLE) {
      lightBeam_drawIdleScreen();
    } else if (currentState == STATE_PLAYING) {
      if (!isPaused) {
        // 波次系统: 生成敌人
        if (waveActive) {
          if (now - lastEnemySpawn > (unsigned long)enemySpawnInterval) {
            lastEnemySpawn = now;
            lightBeam_spawnEnemy();
            waveEnemiesSpawned++;

            if (waveEnemiesSpawned >= waveEnemyCount) {
              waveActive = false;
              waveRestStart = now;
            }
          }
        } else {
          if (now - waveRestStart > (unsigned long)waveRestDuration) {
            waveNumber++;
            waveEnemiesSpawned = 0;
            waveActive = true;
            waveEnemyCount = min(10, 3 + difficulty / 2);
            waveRestDuration = max(1000, 3000 - difficulty * 150);
          }
        }

        // 移动敌人
        if (now - lastEnemyMove > enemyMoveInterval) {
          lastEnemyMove = now;
          lightBeam_updateEnemies();
        }

        // 移动子弹
        if (now - lastBulletMove >= 10) {
          lastBulletMove = now;
          lightBeam_updateBullets();
        }

        // 碰撞检测
        lightBeam_checkCollisions();

        // ================= 试玩模式 AI =================
        if (isDemoMode && !isPaused) {
          int activeBullets = 0;
          int enemyPositions[MAX_PIXELS];
          int enemyColors[MAX_PIXELS];
          int enemyCount = 0;

          for (int i = 0; i < MAX_PIXELS; i++) {
            if (pixels[i].active) {
              if (!pixels[i].isEnemy)
                activeBullets++;
              else {
                enemyPositions[enemyCount] = pixels[i].position;
                enemyColors[enemyCount] = pixels[i].color;
                enemyCount++;
              }
            }
          }

          if (activeBullets < enemyCount) {
            for (int i = 0; i < enemyCount - 1; i++) {
              for (int j = 0; j < enemyCount - i - 1; j++) {
                if (enemyPositions[j] > enemyPositions[j + 1]) {
                  int tempP = enemyPositions[j];
                  enemyPositions[j] = enemyPositions[j + 1];
                  enemyPositions[j + 1] = tempP;
                  int tempC = enemyColors[j];
                  enemyColors[j] = enemyColors[j + 1];
                  enemyColors[j + 1] = tempC;
                }
              }
            }
            int targetColor = enemyColors[activeBullets];
            lightBeam_spawnBullet(targetColor);
          }
        }
      }
      lightBeam_drawLEDs();
    } else if (currentState == STATE_GAME_OVER) {
      lightBeam_drawGameOver();
      if (isDemoMode && (now - gameOverAnimTimer > 6000)) {
        lightBeam_initGame();
      }
    }
    break;
  }

  // ================= [NEW] BLE Telemetry Loop =================
  if (deviceConnectedCount > 0) {
    static unsigned long lastBleSync = 0;
    if (now - lastBleSync >= 1000) {
      lastBleSync = now;
      int batteryPct = getBatteryPercent();
      String json = "{\"m\":" + String((int)currentAppMode) +
                    ",\"s\":" + String(score) + ",\"k\":" + String(score) +
                    ",\"l\":" + String(difficulty) +
                    ",\"b\":" + String(batteryPct) +
                    ",\"p\":" + String(isPaused ? 1 : 0) +
                    ",\"d\":" + String(isDemoMode ? 1 : 0) + "}";
      pTxCharacteristic->setValue((uint8_t *)json.c_str(), json.length());
      pTxCharacteristic->notify();
    }
  }

  if (advertisingRestartRequested) {
    advertisingRestartRequested = false;
    BLEDevice::startAdvertising();
    Serial.println("Restarted Advertising");
  }

  delay(10); // 主循环延迟
}
