/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "led_widget.h"

#include <app/clusters/identify-server/identify-server.h>
#include <platform/CHIPDeviceLayer.h>

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	void UpdateClustersState();
	static void OnIdentifyStart(Identify *);
	static void OnIdentifyStop(Identify *);
	static void UpdateLedState();

private:
	CHIP_ERROR Init();

	void UpdateTemperatureClusterState();
	void UpdatePressureClusterState();
	void UpdateRelativeHumidityClusterState();
	void UpdatePowerSourceClusterState();

	static void MeasurementsTimerHandler();
	static void IdentifyTimerHandler();
	static void MatterEventHandler(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg);

	LEDWidget *mRedLED;
	LEDWidget *mGreenLED;
	LEDWidget *mBlueLED;
};
