#pragma once
struct WiFiClient {};
struct WiFiClientSecure : WiFiClient { void setInsecure(){} };
