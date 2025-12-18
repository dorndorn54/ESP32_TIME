#ifndef OUTPUT_PIN_H
#define OUTPUT_PIN_H

#include <Arduino.h>

class outputPin {
private:
    uint8_t pinNumber;

public:
    outputPin(uint8_t pin) : pinNumber(pin) {
        pinMode(pinNumber, OUTPUT);
    }

    void setHigh() {
        digitalWrite(pinNumber, HIGH);
    }

    void setLow() {
        digitalWrite(pinNumber, LOW);
    }

    void toggle() {
        digitalWrite(pinNumber, !digitalRead(pinNumber));
    }

};

#endif