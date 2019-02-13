//Replace with your Wifi SSID; example: #define ssid "MyWifi"
#define ssid "$SSID$"

//Replace with your Wifi password; example: #define password "12345678"
#define password "$WIFI_PASSWORD$"

//Replace with a human-friendly host name. Must not contain spaces or special characters
#define hostname "esp32_room_presence"

//Replace with your MQTT Broker address; example: #define mqttHost IPAddress(192, 168, 1, 195)
#define mqttHost IPAddress(192, 168, 1, 195)

//Replace with your MQTT Broker port; example: #define mqttPort 1883
#define mqttPort $MQTT_PORT$

//Replace with your MQTT Broker user; example: #define mqttUser "homeassistant"
#define mqttUser "$MQTT_USER$"

//Replace with your MQTT Broker password; example: #define mqttPassword "12345678"
#define mqttPassword "$MQTT_PASSWORD"

//Replace with the room name where the node will be placed; example: #define room "living-room"
#define room "$ROOM_NAME$"

//Specify the LED pin. For most dev boards, this is GPIO2
#define LED_BUILTIN 2

// Logic level for turning the led on. Most boards use active low, meaning LED_ON should be set to 0
#define LED_ON 0

//Define the base topic for room detection. Usually "room_presence"
#define channel "room_presence"

//Define the topic for publishing availability
#define availabilityTopic "presence_nodes/" room

// Define bluetooth scan parameters
#define scanInterval 2 // Define the interval in seconds between scans
#define singleScanTime 3 // Define the duration of a single scan in seconds
#define activeScan true // Active scan uses more power, but get results faster
#define bleScanInterval 0x80 // (milliseconds) The time interval from when the Controller started its last LE scan until it begins the subsequent LE scan
#define bleScanWindow 0x10 // (milliseconds) The duration of the LE scan, shall be less than or equal to bleScanInterval; if interval and window are equal then perform continuous scan
#define advertisementDuration 3 // Define the duration in seconds that the device should advertise as a beacon to other sensors

// Maximum distance (in meters) to report. Devices that are calculated to be further than this distance in meters will not be reported
#define maxDistance 5
