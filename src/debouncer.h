#ifndef DEBOUNCER_H
#define DEBOUNCER_H

#include <Arduino.h>

class Debouncer {
private:
    uint8_t pin;
    bool lastState;
    bool currentState;
    bool wasPressed;  // Make this a member variable, not static
    unsigned long lastDebounceTime;
    unsigned long debounceDelay;

public:
    Debouncer(uint8_t buttonPin, unsigned long delay = 20) {
        pin = buttonPin;
        debounceDelay = delay;
        pinMode(pin, INPUT_PULLUP);
        lastState = HIGH;
        currentState = HIGH;
        wasPressed = false;  // Initialize in constructor
        lastDebounceTime = 0;
    }

    bool isPressed() {
        bool reading = digitalRead(pin);
        
        // If reading changed, reset debounce timer
        if (reading != lastState) {
            lastDebounceTime = millis();
        }
        
        // If stable for debounceDelay, update current state
        if ((millis() - lastDebounceTime) > debounceDelay) {
            if (reading != currentState) {
                currentState = reading;
            }
        }
        
        lastState = reading;
        
        // Return true if pressed (LOW with INPUT_PULLUP)
        return (currentState == LOW);
    }

    bool justPressed() {
        bool pressed = isPressed();
        
        if (pressed && !wasPressed) {
            wasPressed = true;
            return true;  // Trigger only once on press
        }
        
        if (!pressed) {
            wasPressed = false;  // Reset when released
        }
        
        return false;  // Don't trigger while held down
    }
};

#endif