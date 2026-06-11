#ifndef WIFI_H_
#define WIFI_H_

#define MAX_SSID_LEN     32
#define MAX_PSK_LEN      64

extern char wifi_ssid[MAX_SSID_LEN];
extern char wifi_psk [MAX_PSK_LEN];

void wifi_callbacks_init(void);
void wifi_thread(void);

#endif /* WIFI_H_ */