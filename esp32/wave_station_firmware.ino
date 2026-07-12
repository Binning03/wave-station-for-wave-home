#include <Arduino.h>

// Arduino IDE의 자동 함수 프로토타입 생성과 애플리케이션 구현을 분리하기 위해
// 이 파일에는 setup()/loop() 진입점만 유지합니다.
void appSetup();
void appLoop();

void setup() {
  appSetup();
}

void loop() {
  appLoop();
}
