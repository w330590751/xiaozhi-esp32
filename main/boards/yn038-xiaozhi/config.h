#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_METHOD_SIMPLEX

// INMP441 digital microphone
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

// MAX98357A I2S amplifier
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BOOT_BUTTON_GPIO GPIO_NUM_0

// Passive, read-only capture of the original environmental monitor's HT1621
// display bus. These pins never drive the old board.
#define HT1621_SNIFFER_ENABLED 1
#define HT1621_SNIFFER_GPIO_CS   GPIO_NUM_8
#define HT1621_SNIFFER_GPIO_WR   GPIO_NUM_9
#define HT1621_SNIFFER_GPIO_DATA GPIO_NUM_10

// 音箱（open-xiaoai speaker-brain）保存读数，环境问答由它回答，不依赖电脑。
// 优先走音箱的公网 IPv6（跨网段唯一可靠路径），失败再退回局域网 IPv4。
#define ENVIRONMENT_PUSH_URL "http://192.168.1.54:4399/env"
#define ENVIRONMENT_PUSH_URL_IPV6 "http://[240e:337:12:5800:5aea:1fff:fe2d:9a4a]:4399/env"
#define ENVIRONMENT_PUSH_INTERVAL_MS 3000

// 设备当前与音箱不在同一网段，推送会每 3 秒超时一次，先关闭（改回 1 即恢复）。
#define ENVIRONMENT_PUSH_ENABLED 0

// Legacy endpoint retained for the dormant dashboard parser; the active refresh task is local.
#define DASHBOARD_URL "http://10.0.0.185:8003/internal/dashboard"
#define DASHBOARD_REFRESH_MS 5000

#endif  // _BOARD_CONFIG_H_
