#ifndef AZURE_MQTT_H_
#define AZURE_MQTT_H_

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>

/* -----------------------------------------------------------------------
 * USER CONFIGURATION — fill these in before building
 * --------------------------------------------------------------------- */

/* Oracle Cloud MQTT bridge — plaintext, no TLS required on device.
 * Bridge forwards to Azure IoT Hub over TLS server-side.
 * For production replace with ESP32-WROVER and direct Azure connection. */
#define AZURE_IOT_HUB_HOSTNAME   "161.33.232.177"

/* Device ID registered in your IoT Hub */
#define AZURE_DEVICE_ID          "esp32-device-01"

/* SAS Token — only used for direct Azure connection (TLS mode).
 * Not required for plaintext bridge connection but kept for reference. */
#define AZURE_SAS_TOKEN "SharedAccessSignature sr=iot-hub-esp32-Ryan-Smith.azure-devices.net%2Fdevices%2Fesp32-device-01&sig=dUYE4NNWdgzFTPBah8Z%2FssE2rk3ZYL1sSnihWJBwS%2Fw%3D&se=1774250233"

/* Root CA cert — not used in plaintext bridge mode.
 * Kept for reference for production WROVER deployment. */
#define AZURE_ROOT_CA_CERT \
"-----BEGIN CERTIFICATE-----\r\n"\
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\r\n"\
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\r\n"\
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\r\n"\
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\r\n"\
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\r\n"\
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\r\n"\
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\r\n"\
"2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\r\n"\
"1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\r\n"\
"q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\r\n"\
"tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\r\n"\
"vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\r\n"\
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\r\n"\
"5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\r\n"\
"1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\r\n"\
"NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\r\n"\
"Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\r\n"\
"8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\r\n"\
"pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\r\n"\
"MrY=\r\n"\
"-----END CERTIFICATE-----\r\n"

/* -----------------------------------------------------------------------
 * MQTT connection settings
 * --------------------------------------------------------------------- */
/* Plaintext port for Oracle bridge — no TLS on device */
#define AZURE_MQTT_PORT      1883
#define AZURE_MQTT_CLIENT_ID AZURE_DEVICE_ID

/* Username/password not required for anonymous Oracle bridge connection */
#define AZURE_MQTT_USERNAME  ""
#define AZURE_MQTT_TOPIC  "devices/" AZURE_DEVICE_ID "/messages/events/$.ct=application%2Fjson&$.ce=utf-8"
#define AZURE_TLS_TAG        1

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
int  azure_mqtt_connect(void);
int  azure_mqtt_publish(const char *json);
void azure_mqtt_process(void);
void azure_mqtt_thread(void);
void azure_mqtt_thread_start(void);

#endif /* AZURE_MQTT_H_ */