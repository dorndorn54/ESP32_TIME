#ifndef ROTARY_H
#define ROTARY_H

#include <Arduino.h>
#include <ESP32Encoder.h>

class RotaryEncoder {
    private:
        uint8_t pinSW;
        ESP32Encoder encoder;
        
        uint16_t btnState = 0;
        int32_t lastCount = 0;

    public:
        RotaryEncoder(uint8_t SW, uint8_t DT, uint8_t CLK) {
            pinSW = SW;
            encoder.attachHalfQuad(DT, CLK);
            encoder.setCount(0);

            pinMode(SW, INPUT_PULLUP);
        }
        
        bool is_clockwise(){
            int32_t count = encoder.getCount();
            if (count > lastCount) {
                lastCount = count;
                return true;
            }
            return false;
        }

        bool is_counterclockwise(){
            int32_t count = encoder.getCount();
            if (count < lastCount) {
                lastCount = count;
                return true;
            }
            return false;
        }

        bool is_button_pressed(){
            btnState = (btnState<<1) | !(digitalRead(pinSW));

            return (btnState == 0xFFF0);
        }

        int32_t get_last_count(){
            return lastCount;
        }

};

#endif // ROTARY_H