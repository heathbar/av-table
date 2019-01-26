/*
   MQTT RGB Light for Home-Assistant - NodeMCU (ESP8266)
   https://home-assistant.io/components/light.mqtt/

   Configuration (HA) : 
    light:
      platform: mqtt
      name: 'Office RGB light'
      state_topic: 'av-table/status'
      command_topic: 'av-table/switch'
      brightness_state_topic: 'av-table/brightness/status'
      brightness_command_topic: 'av-table/brightness/set'
      rgb_state_topic: 'av-table/rgb/status'
      rgb_command_topic: 'av-table/rgb/set'
      brightness_scale: 100
      optimistic: false
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <NeoPixelBus.h>
#include "secrets.h"

#define LEDCOUNT   52       // Number of LEDs used for serial
#define MQTT_VERSION MQTT_VERSION_3_1_1

const char* WIFI_SSID = MY_WIFI_SSID;
const char* WIFI_PASSWORD = MY_WIFI_PASS;

const PROGMEM char* MQTT_CLIENT_ID = "AVTABLE";
const PROGMEM char* MQTT_SERVER_IP = MY_MQTT_SERVER_IP;
const PROGMEM uint16_t MQTT_SERVER_PORT = 1883;

// MQTT: topics
// state
const PROGMEM char* MQTT_LIGHT_STATE_TOPIC = "av-table/status";
const PROGMEM char* MQTT_LIGHT_COMMAND_TOPIC = "av-table/switch";

// brightness
const PROGMEM char* MQTT_LIGHT_BRIGHTNESS_STATE_TOPIC = "av-table/brightness/status";
const PROGMEM char* MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC = "av-table/brightness/set";

// colors (rgb)
const PROGMEM char* MQTT_LIGHT_RGB_STATE_TOPIC = "av-table/rgb/status";
const PROGMEM char* MQTT_LIGHT_RGB_COMMAND_TOPIC = "av-table/rgb/set";

// payloads by default (on/off)
const PROGMEM char* LIGHT_ON = "ON";
const PROGMEM char* LIGHT_OFF = "OFF";

// variables used to store the state, the brightness and the color of the light
struct State {
    bool state;                     // Current mode: ON/OFF
    uint8_t targetBrightness;       // brightness
    uint8_t prevBrightness;         // previous brightness
    RgbColor targetColor;           // target (and after the fade, the current) color set by MQTT
    RgbColor prevColor;             // the previous color set by MQTT
    bool isFading;                  // whether or not we are fading between MQTT colors
    float fadeProgress;             // how far we have faded
    float fadeStep;                 // how much the fade progress should change each iteration
} state;

// buffer used to send/receive data with MQTT
const uint8_t MSG_BUFFER_SIZE = 20;
char msg_buffer[MSG_BUFFER_SIZE];

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
NeoPixelBus<NeoGrbFeature, NeoEsp8266Uart800KbpsMethod> strip(LEDCOUNT);

// function called to publish the state of the led (on/off)
void publishState() {
  if (state.state) {
    mqtt.publish(MQTT_LIGHT_STATE_TOPIC, LIGHT_ON, true);
  } else {
    mqtt.publish(MQTT_LIGHT_STATE_TOPIC, LIGHT_OFF, true);
  }
}

// function called to publish the brightness of the led (0-255)
void publishBrightness() {
  snprintf(msg_buffer, MSG_BUFFER_SIZE, "%d", state.targetBrightness);
  mqtt.publish(MQTT_LIGHT_BRIGHTNESS_STATE_TOPIC, msg_buffer, true);
}

// function called to publish the colors of the led (xx(x),xx(x),xx(x))
void publishRGBColor() {
  snprintf(msg_buffer, MSG_BUFFER_SIZE, "%d,%d,%d", state.targetColor.R, state.targetColor.G, state.targetColor.B);
  mqtt.publish(MQTT_LIGHT_RGB_STATE_TOPIC, msg_buffer, true);
}

// function called when a MQTT message arrived
void callback(char* p_topic, byte* p_payload, unsigned int p_length) {
  String payload;
  for (uint8_t i = 0; i < p_length; i++) {
    payload.concat((char)p_payload[i]);
  }
  Serial.println(payload);
  if (String(MQTT_LIGHT_COMMAND_TOPIC).equals(p_topic)) {
    // test if the payload is equal to "ON" or "OFF"
    if (payload.equals(String(LIGHT_ON))) {
      if (state.state != true) {
        state.state = true;
        fadeTo(state.prevColor, state.targetBrightness, 0.001);
        publishState();
        publishBrightness();
        publishRGBColor();
      }
    } else if (payload.equals(String(LIGHT_OFF))) {
      if (state.state != false) {
        state.state = false;
        fadeTo(RgbColor(0, 0, 0), state.targetBrightness, 0.001);
        publishState();
      }
    }
  } else if (String(MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC).equals(p_topic)) {
    uint8_t brightness = payload.toInt();
    if (brightness < 0 || brightness > 255) {
      return;
    }

    if (state.state) {
      fadeTo(state.targetColor, brightness);
      publishBrightness();  
    } else {
      state.prevBrightness = brightness;
    }

  } else if (String(MQTT_LIGHT_RGB_COMMAND_TOPIC).equals(p_topic)) {
    // get the position of the first and second commas
    uint8_t firstIndex = payload.indexOf(',');
    uint8_t lastIndex = payload.lastIndexOf(',');
    
    uint8_t rgb_red = payload.substring(0, firstIndex).toInt();
    if (rgb_red < 0 || rgb_red > 255) {
      return;
    }
    
    uint8_t rgb_green = payload.substring(firstIndex + 1, lastIndex).toInt();
    if (rgb_green < 0 || rgb_green > 255) {
      return;
    }
    
    uint8_t rgb_blue = payload.substring(lastIndex + 1).toInt();
    if (rgb_blue < 0 || rgb_blue > 255) {
      return;
    }

    if (state.state) {
      fadeTo(RgbColor(rgb_red, rgb_green, rgb_blue), state.targetBrightness);
      publishRGBColor();
    } else {
      state.prevColor = RgbColor(rgb_red, rgb_green, rgb_blue);
    }
  }
}

void fadeTo(RgbColor color, uint8_t brightness) {
    fadeTo(color, brightness, 0.003);
}

void fadeTo(RgbColor color, uint8_t brightness, float step) {
    state.prevColor = state.targetColor;
    state.targetColor = color;
    state.prevBrightness = state.targetBrightness;
    state.targetBrightness = brightness;
    state.isFading = true;
    state.fadeProgress = 0;
    state.fadeStep = step;
}

RgbColor calculateFade(RgbColor fromColor, RgbColor toColor, uint8_t fromBrightness, uint8_t toBrightness, float progress) {
    return applyBrightness(RgbColor(
        fromColor.R + (float)(toColor.R - fromColor.R)*progress,
        fromColor.G + (float)(toColor.G - fromColor.G)*progress,
        fromColor.B + (float)(toColor.B - fromColor.B)*progress
    ),
    (fromBrightness + (float)(toBrightness - fromBrightness) * progress));
}

RgbColor applyBrightness(RgbColor color, uint8_t brightness) {
  return RgbColor(
      color.R * brightness / 255,
      color.G * brightness / 255,
      color.B * brightness / 255
  );  
}

// function called to set the entire strip one color
void show(RgbColor color) {
    for (int i = 0; i < LEDCOUNT; i++) {
        strip.SetPixelColor(i, color);
    }
    strip.Show();
}

void setup() {
  Serial.begin(115200);
  strip.Begin();

  state.state = true;
  state.targetColor = RgbColor(0, 0, 0x32);
  state.targetBrightness = 255;

  // init the WiFi connection
  Serial.println();
  Serial.println();
  Serial.print("INFO: Connecting to ");
  WiFi.mode(WIFI_STA);
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("INFO: WiFi connected");
  Serial.print("INFO: IP address: ");
  Serial.println(WiFi.localIP());

  // init the MQTT connection
  mqtt.setServer(MQTT_SERVER_IP, MQTT_SERVER_PORT);
  mqtt.setCallback(callback);

}

void loop() {
  if (!mqtt.connected()) {
    reconnect();
  }

  RgbColor color;
  if (state.isFading) {
      state.fadeProgress += state.fadeStep;
      
      if (state.fadeProgress >= 1) {
          state.isFading = false;
          color = applyBrightness(state.targetColor, state.targetBrightness);
      } else {
          color = calculateFade(state.prevColor, state.targetColor, state.prevBrightness, state.targetBrightness, state.fadeProgress);
      }
  } else {
      color = applyBrightness(state.targetColor, state.targetBrightness);
  }
  
  show(color);
  mqtt.loop();

}

void reconnect() {
  // Loop until we're reconnected
  while (!mqtt.connected()) {
    Serial.println("INFO: Attempting MQTT connection...");
    // Attempt to connect
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      Serial.println("INFO: connected");
      
      // Once connected, publish an announcement...
      // publish the initial values
      publishState();
      publishBrightness();
      publishRGBColor();

      // ... and resubscribe
      mqtt.subscribe(MQTT_LIGHT_COMMAND_TOPIC);
      mqtt.subscribe(MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC);
      mqtt.subscribe(MQTT_LIGHT_RGB_COMMAND_TOPIC);
    } else {
      Serial.print("ERROR: failed, rc=");
      Serial.print(mqtt.state());
      Serial.println("DEBUG: try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

