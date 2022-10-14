/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/* ---- Light Bulb App Config ---- */

#include "board_util.h"

#define FUNCTION_BUTTON DK_BTN1
#define FUNCTION_BUTTON_MASK DK_BTN1_MSK

#if NUMBER_OF_BUTTONS == 2
#define BLE_ADVERTISEMENT_START_AND_LIGHTING_BUTTON DK_BTN2
#define BLE_ADVERTISEMENT_START_AND_LIGHTING_BUTTON_MASK DK_BTN2_MSK
#else
#define LIGHTING_BUTTON DK_BTN2
#define LIGHTING_BUTTON_MASK DK_BTN2_MSK
#define BLE_ADVERTISEMENT_START_BUTTON DK_BTN4
#define BLE_ADVERTISEMENT_START_BUTTON_MASK DK_BTN4_MSK
#endif

#define SYSTEM_STATE_LED DK_LED1
#define LIGHTING_STATE_LED DK_LED2
#if NUMBER_OF_BUTTONS == 4
#define FACTORY_RESET_SIGNAL_LED DK_LED3
#define FACTORY_RESET_SIGNAL_LED1 DK_LED4
#endif
