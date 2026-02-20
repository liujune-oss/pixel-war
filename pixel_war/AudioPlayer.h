#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <Arduino.h>

// ESP32-S3 LEDC Channel Config
#define BUZZER_CHANNEL 0
#define BUZZER_TIMER_BIT 12 // 12-bit resolution (0-4095)
// Note: ESP32-S3 usually uses 5000Hz base freq

// Music Notes (Hz)
#define NOTE_B0 31
#define NOTE_C1 33
#define NOTE_CS1 35
#define NOTE_D1 37
#define NOTE_DS1 39
#define NOTE_E1 41
#define NOTE_F1 44
#define NOTE_FS1 46
#define NOTE_G1 49
#define NOTE_GS1 52
#define NOTE_A1 55
#define NOTE_AS1 58
#define NOTE_B1 62
#define NOTE_C2 65
#define NOTE_CS2 69
#define NOTE_D2 73
#define NOTE_DS2 78
#define NOTE_E2 82
#define NOTE_F2 87
#define NOTE_FS2 93
#define NOTE_G2 98
#define NOTE_GS2 104
#define NOTE_A2 110
#define NOTE_AS2 117
#define NOTE_B2 123
#define NOTE_C3 131
#define NOTE_CS3 139
#define NOTE_D3 147
#define NOTE_DS3 156
#define NOTE_E3 165
#define NOTE_F3 175
#define NOTE_FS3 185
#define NOTE_G3 196
#define NOTE_GS3 208
#define NOTE_A3 220
#define NOTE_AS3 233
#define NOTE_B3 247
#define NOTE_C4 262
#define NOTE_CS4 277
#define NOTE_D4 294
#define NOTE_DS4 311
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_FS4 370
#define NOTE_G4 392
#define NOTE_GS4 415
#define NOTE_A4 440
#define NOTE_AS4 466
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_CS5 554
#define NOTE_D5 587
#define NOTE_DS5 622
#define NOTE_E5 659
#define NOTE_F5 698
#define NOTE_FS5 740
#define NOTE_G5 784
#define NOTE_GS5 831
#define NOTE_A5 880
#define NOTE_AS5 932
#define NOTE_B5 988
#define NOTE_C6 1047
#define NOTE_CS6 1109
#define NOTE_D6 1175
#define NOTE_DS6 1245
#define NOTE_E6 1319
#define NOTE_F6 1397
#define NOTE_FS6 1480
#define NOTE_G6 1568
#define NOTE_GS6 1661
#define NOTE_A6 1760
#define NOTE_AS6 1865
#define NOTE_B6 1976
#define NOTE_C7 2093
#define NOTE_CS7 2217
#define NOTE_D7 2349
#define NOTE_DS7 2489
#define NOTE_E7 2637
#define NOTE_F7 2794
#define NOTE_FS7 2960
#define NOTE_G7 3136
#define NOTE_GS7 3322
#define NOTE_A7 3520
#define NOTE_AS7 3729
#define NOTE_B7 3951
#define NOTE_C8 4186
#define NOTE_CS8 4435
#define NOTE_D8 4699
#define NOTE_DS8 4978
#define REST 0

struct Note {
  int frequency;
  int duration;
};

// Mario Theme (Short)
const Note MELODY_MARIO[] = {
    {NOTE_E5, 100}, {REST, 10},     {NOTE_E5, 100}, {REST, 100}, {NOTE_E5, 100},
    {REST, 100},    {NOTE_C5, 100}, {NOTE_E5, 100}, {REST, 100}, {NOTE_G5, 100},
    {REST, 300},    {NOTE_G4, 100}, {REST, 300}};

// 1-UP Sound (Mario Life Up)
const Note MELODY_1UP[] = {{NOTE_E6, 100}, {NOTE_G6, 100}, {NOTE_E7, 100},
                           {NOTE_C7, 100}, {NOTE_D7, 100}, {NOTE_G7, 100}};

// Bad Sound (Low Pitch)
const Note MELODY_BAD[] = {{NOTE_C3, 150}, {NOTE_G2, 300}};

// Success (Short Beep)
const Note MELODY_BEEP[] = {{NOTE_A5, 50}};

// Hit Sound (子弹抵消 - 简短上升音)
const Note MELODY_HIT[] = {{NOTE_E6, 50}, {NOTE_G6, 50}};

class AudioPlayer {
private:
  int _pin;
  const Note *_currentMelody;
  int _noteCount;
  int _currentNoteIndex;
  unsigned long _lastNoteTime;
  bool _isPlaying;
  int _volumeDuty; // 0 - 2048 (50% of 4095)

public:
  AudioPlayer(int pin)
      : _pin(pin), _isPlaying(false), _currentMelody(NULL), _volumeDuty(2048) {
  } // 50% default

  void begin() {
    // Setup LEDC (PWM) - ESP32 Arduino Core 3.x API
    ledcAttach(_pin, 5000, BUZZER_TIMER_BIT);
    ledcWrite(_pin, 0); // Start silent
  }

  void setVolume(uint8_t percent) {
    // Map 0-100% to Duty Cycle 0 - 1024 (Max ~25% duty for best sound)
    // 太高的占空比会导致声音失真
    if (percent > 100)
      percent = 100;
    _volumeDuty = (1024 * percent) / 100; // 最大 1024 (25% 占空比)
  }

  void play(const Note *melody, int count) {
    _currentMelody = melody;
    _noteCount = count;
    _currentNoteIndex = 0;
    _isPlaying = true;
    _lastNoteTime = 0; // Trigger immediately
  }

  void playMario() { play(MELODY_MARIO, sizeof(MELODY_MARIO) / sizeof(Note)); }

  void play1UP() { play(MELODY_1UP, sizeof(MELODY_1UP) / sizeof(Note)); }

  void playBad() { play(MELODY_BAD, sizeof(MELODY_BAD) / sizeof(Note)); }

  void playBeep() { play(MELODY_BEEP, sizeof(MELODY_BEEP) / sizeof(Note)); }

  void playHit() { play(MELODY_HIT, sizeof(MELODY_HIT) / sizeof(Note)); }

  void stop() {
    _isPlaying = false;
    ledcWrite(_pin, 0);
  }

  // Must be called in main loop (Non-blocking)
  void update() {
    if (!_isPlaying)
      return;

    unsigned long now = millis();

    // Handle first note start
    if (_currentNoteIndex == 0 && _lastNoteTime == 0) {
      playCurrentNote();
      _lastNoteTime = now;
      return;
    }

    // Check if note duration has passed
    if (_currentNoteIndex < _noteCount) {
      const Note &note = _currentMelody[_currentNoteIndex];
      if (now - _lastNoteTime >= note.duration) {
        _currentNoteIndex++;
        if (_currentNoteIndex >= _noteCount) {
          stop();
        } else {
          playCurrentNote();
          _lastNoteTime = now;
        }
      }
    }
  }

private:
  void playCurrentNote() {
    if (_currentNoteIndex >= _noteCount)
      return;

    const Note &note = _currentMelody[_currentNoteIndex];
    if (note.frequency > 0) {
      // ESP32 Arduino Core 3.x: 使用 ledcChangeFrequency 设置频率
      ledcChangeFrequency(_pin, note.frequency, BUZZER_TIMER_BIT);
      // 设置占空比 (音量)
      ledcWrite(_pin, _volumeDuty);
    } else {
      ledcWrite(_pin, 0); // Silence for REST
    }
  }
};

#endif
