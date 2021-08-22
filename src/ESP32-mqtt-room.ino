/*
	 ESP32 Bluetooth Low Energy presence detection, for use with MQTT.

	 Project and documentation are available on GitHub at https://jptrsn.github.io/ESP32-mqtt-room/

   Some giants upon whose shoulders the project stands -- major thanks to:

   pcbreflux for the original version of this code, as well as the eddystone handlers https://github.com/pcbreflux

   Andreis Speiss for his work on YouTube and his invaluable github at https://github.com/sensorsiot.

	 Sidddy for the implementation of Mi Flora plant sensor support. https://github.com/sidddy/flora

   Based on Neil Kolban example for IDF: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/tests/BLE%20Tests/SampleScan.cpp
   Ported to Arduino ESP32 by Evandro Copercini
*/
#include <WiFi.h>
extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/timers.h"
}
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

#include <AsyncTCP.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "BLEBeacon.h"
#include "BLEEddystoneTLM.h"
#include "BLEEddystoneURL.h"
#include "Settings.h"

#ifdef htuSensorTopic
	#define tempTopic htuSensorTopic "/temperature"
	#define humidityTopic htuSensorTopic "/humidity"
	#include "sensors/sensor_htu21d.h"
#endif

#ifdef batteryTopic
	#define batteryPinValueTopic batteryTopic "/pinvalue"
	#define batteryPinValueAverageTopic batteryTopic "/pinvalueavg"
	#define batteryVoltageTopic batteryTopic "/voltage"
	#define batteryChargeLevelTopic batteryTopic "/chargelevel"

	#include <Pangodream_18650_CL.h>
	#define ADC_PIN 35
	#define CONV_FACTOR 1.7
	#define READS 20
	
	Pangodream_18650_CL BL(ADC_PIN, CONV_FACTOR, READS);
#endif

static const int scanTime = singleScanTime;
static const int waitTime = scanInterval;
static const uint16_t beaconUUID = 0xFEAA;
#ifdef TxDefault
static const int defaultTxPower = TxDefault;
#else
static const int defaultTxPower = -72;
#endif
#define ENDIAN_CHANGE_U16(x) ((((x)&0xFF00)>>8) + (((x)&0xFF)<<8))

WiFiClient espClient;
AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;
bool updateInProgress = false;
String localIp;
byte retryAttempts = 0;
unsigned long last = 0;
BLEScan* pBLEScan;
TaskHandle_t BLEScan;

String getProximityUUIDString(BLEBeacon beacon) {
  std::string serviceData = beacon.getProximityUUID().toString().c_str();
  int serviceDataLength = serviceData.length();
  String returnedString = "";
  int i = serviceDataLength;
  while (i > 0)
  {
    if (serviceData[i-1] == '-') {
      i--;
    }
    char a = serviceData[i-1];
    char b = serviceData[i-2];
    returnedString += b;
    returnedString += a;

    i -= 2;
  }

  return returnedString;
}

float calculateDistance(int rssi, int txPower) {

	float distFl;

  if (rssi == 0) {
      return -1.0;
  }

  if (!txPower) {
      // somewhat reasonable default value
      txPower = defaultTxPower;
  }

	if (txPower > 0) {
		txPower = txPower * -1;
	}

  const float ratio = rssi * 1.0 / txPower;
  if (ratio < 1.0) {
      distFl = pow(ratio, 10);
  } else {
      distFl = (0.89976) * pow(ratio, 7.7095) + 0.111;
  }

	return round(distFl * 100) / 100;

}

#ifdef htuSensorTopic

void reportSensorValues() {
	if (htuSensorIsConnected()) {
		char temp[8];
		char humidity[8];

		dtostrf(getTemp(), 0, 1, temp); 						// convert float to string with one decimal place precision
		dtostrf(getHumidity(), 0, 1, humidity);			// convert float to string with one decimal place precision

		if (mqttClient.publish(tempTopic, 0, 0, temp) == true) {
			Serial.printf("Temperature %s sent\t", temp);
		}

		if (mqttClient.publish(humidityTopic, 0, 0, humidity) == true) {
			Serial.printf("Humidity %s sent\n\r", humidity);
		}
	}
}

#endif

#ifdef batteryTopic

void reportBatteryValues() {

	int batteryPinValue = analogRead(ADC_PIN); // reference ADC_PIN instead?
	char pinValue[5];
	itoa(batteryPinValue, pinValue, 10);
	Serial.print("Value from pin: ");
	Serial.println(batteryPinValue);
	if (mqttClient.publish(batteryPinValueTopic, 0, 0, pinValue) == true) {
		Serial.printf("Battery Pin Value %s sent\t", pinValue);
	}

  	int batteryAveragePinValue = BL.pinRead();
	char avgPinValue[5];
	itoa(batteryAveragePinValue, avgPinValue, 10);
  	Serial.print("Average value from pin: ");
  	Serial.println(batteryAveragePinValue);
  	if (mqttClient.publish(batteryPinValueAverageTopic, 0, 0, avgPinValue) == true) {
  		Serial.printf("Battery Average Pin Value %s sent\t", avgPinValue);
  	}
  
	float batteryVoltage = BL.getBatteryVolts();
	char voltage[5];
	dtostrf(batteryVoltage, 0, 2, voltage); // convert float to string with two decimal place precision
	Serial.print("Volts: ");
	Serial.println(batteryVoltage);
	if (mqttClient.publish(batteryVoltageTopic, 0, 0, voltage) == true) {
		Serial.printf("Battery Voltage %s sent\t", voltage);
	}

	int batteryChargeLevel = BL.getBatteryChargeLevel();
	char chargeLevel[5];
	itoa(batteryChargeLevel, chargeLevel, 10);
	Serial.print("Charge level: ");
	Serial.println(batteryChargeLevel);
	Serial.println("");
	if (mqttClient.publish(batteryChargeLevelTopic, 0, 0, chargeLevel) == true) {
		Serial.printf("Battery Charge Level %s sent\t", chargeLevel);
	}

}

#endif


bool sendTelemetry(int deviceCount = -1, int reportCount = -1) {
	StaticJsonDocument<256> tele;
	tele["room"] = room;
	tele["ip"] = localIp;
	tele["hostname"] = WiFi.getHostname();
	tele["scan_dur"] = scanTime;
	tele["wait_dur"] = waitTime;
	tele["max_dist"] = maxDistance;

	if (deviceCount > -1) {
		Serial.printf("devices_discovered: %d\n\r",deviceCount);
    tele["disc_ct"] = deviceCount;
	}

	if (reportCount > -1) {
		Serial.printf("devices_reported: %d\n\r",reportCount);
    tele["rept_ct"] = reportCount;
	}

	char teleMessageBuffer[258];
	serializeJson(tele, teleMessageBuffer);

	#ifdef htuSensorTopic
    reportSensorValues();
	#endif

	#ifdef batteryTopic
		reportBatteryValues();
	#endif


	if (mqttClient.publish(telemetryTopic, 0, 0, teleMessageBuffer) == true) {
		Serial.println("Telemetry sent");
		return true;
	} else {
		Serial.println("Error sending telemetry");
		return false;
	}

}

void connectToWifi() {
  Serial.println("Connecting to WiFi...");
	WiFi.begin(ssid, password);
	WiFi.setHostname(hostname);
}

void connectToMqtt() {
  Serial.println("Connecting to MQTT");
	if (WiFi.isConnected() && !updateInProgress) {
		mqttClient.setCredentials(mqttUser, mqttPassword);
		mqttClient.setClientId(hostname);
	  mqttClient.connect();
	} else {
		Serial.println("Cannot reconnect MQTT - WiFi error");
		handleWifiDisconnect();
	}
}

bool handleMqttDisconnect() {
	if (updateInProgress) {
		Serial.println("Not retrying MQTT connection - OTA update in progress");
		return true;
	}
	if (retryAttempts > 10) {
		Serial.println("Too many retries. Restarting");
		ESP.restart();
	} else {
		retryAttempts++;
	}
	if (WiFi.isConnected() && !updateInProgress) {
		Serial.println("Starting MQTT reconnect timer");
    if (xTimerReset(mqttReconnectTimer, 0) == pdFAIL) {
			Serial.println("failed to restart");
			xTimerStart(mqttReconnectTimer, 0);
		} else {
			Serial.println("restarted");
		}
  } else {
		Serial.print("Disconnected from WiFi; starting WiFi reconnect timiler\t");
		handleWifiDisconnect();
	}
}

bool handleWifiDisconnect() {
	if (WiFi.isConnected()) {
		Serial.println("WiFi appears to be connected. Not retrying.");
		return true;
	}
	if (retryAttempts > 10) {
		Serial.println("Too many retries. Restarting");
		ESP.restart();
	} else {
		retryAttempts++;
	}
	if (mqttClient.connected()) {
		mqttClient.disconnect();
	}
	if (xTimerIsTimerActive(mqttReconnectTimer) != pdFALSE) {
		xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
	}

	if (xTimerReset(wifiReconnectTimer, 0) == pdFAIL) {
		Serial.println("failed to restart");
		xTimerStart(wifiReconnectTimer, 0);
		return false;
	} else {
		Serial.println("restarted");
		return true;
	}

}

void WiFiEvent(WiFiEvent_t event) {
    Serial.printf("[WiFi-event] event: %x\n\r", event);

		switch(event) {
	    case SYSTEM_EVENT_STA_GOT_IP:
					digitalWrite(LED_BUILTIN, !LED_ON);
	        Serial.print("IP address: \t");
	        Serial.println(WiFi.localIP());
					localIp = WiFi.localIP().toString().c_str();
					Serial.print("Hostname: \t");
					Serial.println(WiFi.getHostname());
	        connectToMqtt();
					if (xTimerIsTimerActive(wifiReconnectTimer) != pdFALSE) {
						Serial.println("Stopping wifi reconnect timer");
						xTimerStop(wifiReconnectTimer, 0);
					}
					retryAttempts = 0;
	        break;
	    case SYSTEM_EVENT_STA_DISCONNECTED:
					digitalWrite(LED_BUILTIN, LED_ON);
	        Serial.println("WiFi lost connection, resetting timer\t");
					handleWifiDisconnect();
					break;
			case SYSTEM_EVENT_WIFI_READY:
					Serial.println("Wifi Ready");
					handleWifiDisconnect();
					break;
			case SYSTEM_EVENT_STA_START:
					Serial.println("STA Start");
					tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, hostname);
					if (xTimerIsTimerActive(wifiReconnectTimer) != pdFALSE) {
						TickType_t xRemainingTime = xTimerGetExpiryTime( wifiReconnectTimer ) - xTaskGetTickCount();
						Serial.print("WiFi Time remaining: ");
						Serial.println(xRemainingTime);
					} else {
						Serial.println("WiFi Timer is inactive; resetting\t");
						handleWifiDisconnect();
					}
					break;
			case SYSTEM_EVENT_STA_STOP:
					Serial.println("STA Stop");
					handleWifiDisconnect();
					break;

    }
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
	retryAttempts = 0;

	if (mqttClient.publish(availabilityTopic, 0, 1, "CONNECTED") == true) {
		Serial.print("Success sending message to topic:\t");
		Serial.println(availabilityTopic);
	} else {
		Serial.println("Error sending message");
	}

	sendTelemetry();

}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
  handleMqttDisconnect();
}

bool reportDevice(BLEAdvertisedDevice advertisedDevice) {

	// Serial.printf("\n\n");

	StaticJsonDocument<500> doc;

	String mac_address = advertisedDevice.getAddress().toString().c_str();
	mac_address.replace(":","");
	mac_address.toLowerCase();


	//Check scanned MAC Address against a list of allowed MAC Addresses

	if (allowedListCheck) {
		bool allowedListFound = false;
		for (uint32_t x = 0; x < allowedListNumberOfItems; x++) {
			if (mac_address == allowedList[x]) {
				allowedListFound = true;
			}
		}

		if (allowedListFound == false) {
			return false;
		}
	}
	// --------------


	// Serial.print("mac:\t");
	// Serial.println(mac_address);
	int rssi = advertisedDevice.getRSSI();
	float distance;

	doc["id"] = mac_address;
	doc["uuid"] = mac_address;
	doc["rssi"] = rssi;

	if (advertisedDevice.haveName()){
		String nameBLE = String(advertisedDevice.getName().c_str());
		// Serial.print("Name: ");
		// Serial.println(nameBLE);
		doc["name"] = nameBLE;

	} else {
		// doc["name"] = "unknown";
		// Serial.println("Device name unknown");
	}

	// Serial.printf("\n\r");
	// Serial.printf("Advertised Device: %s \n\r", advertisedDevice.toString().c_str());
	std::string strServiceData = advertisedDevice.getServiceData();
	 uint8_t cServiceData[100];
	 strServiceData.copy((char *)cServiceData, strServiceData.length(), 0);

	 if (advertisedDevice.getServiceDataUUID().equals(BLEUUID(beaconUUID))==true) {  // found Eddystone UUID
		// Serial.printf("is Eddystone: %d %s length %d\n", advertisedDevice.getServiceDataUUID().bitSize(), advertisedDevice.getServiceDataUUID().toString().c_str(),strServiceData.length());
	       // Update distance variable for Eddystone BLE devices
	    BLEBeacon oBeacon = BLEBeacon();
	    distance = calculateDistance(rssi, oBeacon.getSignalPower());
	    doc["distance"] = distance;


		if (cServiceData[0]==0x10) {
			 BLEEddystoneURL oBeacon = BLEEddystoneURL();
			 oBeacon.setData(strServiceData);
			 // Serial.printf("Eddystone Frame Type (Eddystone-URL) ");
			 // Serial.printf(oBeacon.getDecodedURL().c_str());
			 doc["url"] = oBeacon.getDecodedURL().c_str();

		} else if (cServiceData[0]==0x20) {
			 BLEEddystoneTLM oBeacon = BLEEddystoneTLM();
			 oBeacon.setData(strServiceData);
			 // Serial.printf("Eddystone Frame Type (Unencrypted Eddystone-TLM) \n");
			 // Serial.printf(oBeacon.toString().c_str());
		} else {
			// Serial.println("service data");
			for (int i=0;i<strServiceData.length();i++) {
				// Serial.printf("[%X]",cServiceData[i]);
			}
		}
		// Serial.printf("\n");
	} else {
		if (advertisedDevice.haveManufacturerData()==true) {
			std::string strManufacturerData = advertisedDevice.getManufacturerData();


			uint8_t cManufacturerData[100];
			strManufacturerData.copy((char *)cManufacturerData, strManufacturerData.length(), 0);

			if (strManufacturerData.length()==25 && cManufacturerData[0] == 0x4C  && cManufacturerData[1] == 0x00 ) {
				BLEBeacon oBeacon = BLEBeacon();
				oBeacon.setData(strManufacturerData);

				String proximityUUID = getProximityUUIDString(oBeacon);

				distance = calculateDistance(rssi, oBeacon.getSignalPower());

				// Serial.print("RSSI: ");
				// Serial.print(rssi);
				// Serial.print("\ttxPower: ");
				// Serial.print(oBeacon.getSignalPower());
				// Serial.print("\tDistance: ");
				// Serial.println(distance);

				int major = ENDIAN_CHANGE_U16(oBeacon.getMajor());
				int minor = ENDIAN_CHANGE_U16(oBeacon.getMinor());

				doc["major"] = major;
				doc["minor"] = minor;

				doc["uuid"] = proximityUUID;
				doc["id"] = proximityUUID + "-" + String(major) + "-" + String(minor);
				doc["txPower"] = oBeacon.getSignalPower();
				doc["distance"] = distance;

			} else {

				if (advertisedDevice.haveTXPower()) {
					distance = calculateDistance(rssi, advertisedDevice.getTXPower());
					doc["txPower"] = advertisedDevice.getTXPower();
				} else {
					distance = calculateDistance(rssi, defaultTxPower);
				}

				doc["distance"] = distance;

				// Serial.printf("strManufacturerData: %d \n\r",strManufacturerData.length());
				// TODO: parse manufacturer data

			}
		 } else {

			if (advertisedDevice.haveTXPower()) {
				distance = calculateDistance(rssi, advertisedDevice.getTXPower());
				doc["txPower"] = advertisedDevice.getTXPower();
				doc["distance"] = distance;
			} else {
				distance = calculateDistance(rssi, defaultTxPower);
				doc["distance"] = distance;
			}

			// Serial.printf("no Beacon Advertised ServiceDataUUID: %d %s \n\r", advertisedDevice.getServiceDataUUID().bitSize(), advertisedDevice.getServiceDataUUID().toString().c_str());
		 }
		}

		char JSONmessageBuffer[512];
		serializeJson(doc, JSONmessageBuffer);

		String publishTopic = String(channel) + "/" + room;

		if (mqttClient.connected()) {
			if (maxDistance == 0 || doc["distance"] < maxDistance) {
				if (mqttClient.publish((char *)publishTopic.c_str(), 0, 0, JSONmessageBuffer) == true) {

			    // Serial.print("Success sending message to topic: "); Serial.println(publishTopic);
					return true;

			  } else {
			    Serial.print("Error sending message: ");
					Serial.println(publishTopic);
			    Serial.print("Message: ");
					Serial.println(JSONmessageBuffer);
					return false;
			  }
			} else {
				Serial.printf("%s exceeded distance threshold %.2f\n\r", mac_address.c_str(), distance);
				return false;
			}

		} else {

			Serial.println("MQTT disconnected.");
			if (xTimerIsTimerActive(mqttReconnectTimer) != pdFALSE) {
				TickType_t xRemainingTime = xTimerGetExpiryTime( mqttReconnectTimer ) - xTaskGetTickCount();
				Serial.print("Time remaining: ");
				Serial.println(xRemainingTime);
			} else {
				handleMqttDisconnect();
			}
		}
		return false;
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {

	void onResult(BLEAdvertisedDevice advertisedDevice) {

		digitalWrite(LED_BUILTIN, LED_ON);
		vTaskDelay(10 / portTICK_PERIOD_MS);
		digitalWrite(LED_BUILTIN, !LED_ON);

	}

};

void scanForDevices(void * parameter) {
	while(1) {
		if (!updateInProgress && WiFi.isConnected() && (millis() - last > (waitTime * 1000) || last == 0)) {

			Serial.print("Scanning...\t");
			BLEScanResults foundDevices = pBLEScan->start(scanTime);
			int devicesCount = foundDevices.getCount();
	    Serial.printf("Scan done! Devices found: %d\n\r",devicesCount);

			int devicesReported = 0;
			if (mqttClient.connected()) {
			  for (uint32_t i = 0; i < devicesCount; i++) {
					bool included = reportDevice(foundDevices.getDevice(i));
					if (included) {
						devicesReported++;
					}
				}
				sendTelemetry(devicesCount, devicesReported);
				pBLEScan->clearResults();
			} else {
				Serial.println("Cannot report; mqtt disconnected");
				if (xTimerIsTimerActive(mqttReconnectTimer) != pdFALSE) {
					TickType_t xRemainingTime = xTimerGetExpiryTime( mqttReconnectTimer ) - xTaskGetTickCount();
					Serial.print("Time remaining: ");
					Serial.println(xRemainingTime);
				} else {
					handleMqttDisconnect();
				}
			}
			last = millis();
	  }
	}
}

void configureOTA() {
	ArduinoOTA
    .onStart([]() {
			Serial.println("OTA Start");
			pBLEScan->stop();
			updateInProgress = true;
			mqttClient.disconnect(true);
			xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
    })
    .onEnd([]() {
			updateInProgress = false;
			digitalWrite(LED_BUILTIN, !LED_ON);
      Serial.println("\n\rEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
			byte percent = (progress / (total / 100));
      Serial.printf("Progress: %u% \n\r", percent);
			digitalWrite(LED_BUILTIN, percent % 2);
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
			ESP.restart();
    });
	ArduinoOTA.setHostname(hostname);
	ArduinoOTA.setPort(8266);
  ArduinoOTA.begin();
}

void setup() {

  Serial.begin(115200);

	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, LED_ON);

  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));

  #ifdef htuSensorTopic
		sensor_setup();
	#endif

  WiFi.onEvent(WiFiEvent);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);

  mqttClient.setServer(mqttHost, mqttPort);
	mqttClient.setWill(availabilityTopic, 0, 1, "DISCONNECTED");
	mqttClient.setKeepAlive(60);

  connectToWifi();

	configureOTA();

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); //create new scan
  pBLEScan->setActiveScan(activeScan);
	pBLEScan->setInterval(bleScanInterval);
	pBLEScan->setWindow(bleScanWindow);

	xTaskCreatePinnedToCore(
		scanForDevices,
		"BLE Scan",
		4096,
		pBLEScan,
		1,
		&BLEScan,
		1);

}

void loop() {
	TIMERG0.wdt_wprotect=TIMG_WDT_WKEY_VALUE;
	TIMERG0.wdt_feed=1;
	TIMERG0.wdt_wprotect=0;
	ArduinoOTA.handle();
}
