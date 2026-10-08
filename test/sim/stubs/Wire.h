#pragma once
#include <Arduino.h>
struct TwoWire { bool begin(int,int){return true;} };
inline TwoWire Wire;
