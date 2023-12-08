/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

/*
RED LED -> DK_LED1
GREEN LED -> DK_LED2
BLUE LED -> DK_LED3
*/

/* ---- Weather Station Example App Config ---- */

#define SYSTEM_STATE_LED DK_LED1 /* RED LED */
#define APPLICATION_STATE_LED DK_LED2 /* GREEN LED */
#define USER_LED_1 DK_LED3 /* BLUE LED */

#define BLUETOOTH_ADV_BUTTON DK_BTN1
#define FUNCTION_BUTTON DK_BTN1
