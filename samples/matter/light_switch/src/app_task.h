/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "app_event.h"
#include "board.h"

#include <platform/CHIPDeviceLayer.h>

#if CONFIG_CHIP_FACTORY_DATA
#include <platform/nrfconnect/FactoryDataProvider.h>
#else
#include <platform/nrfconnect/DeviceInstanceInfoProviderImpl.h>
#endif

#ifdef CONFIG_MCUMGR_TRANSPORT_BT
#include "dfu_over_smp.h"
#endif

struct k_timer;
struct Identify;

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	void UpdateClusterState();

	static void IdentifyStartHandler(Identify *);
	static void IdentifyStopHandler(Identify *);

private:
	enum Timer : uint8_t { Function, DimmerTrigger, Dimmer };
	enum class Button : uint8_t {
		Function,
		Dimmer,
	};

	CHIP_ERROR Init();

	static void ButtonPushHandler(const AppEvent &event);
	static void ButtonReleaseHandler(const AppEvent &event);
	static void TimerEventHandler(const AppEvent &event);
	static void ButtonEventHandler(ButtonState state, ButtonMask hasChanged);

	static void StartTimer(Timer, uint32_t);
	static void CancelTimer(Timer);
	static void UserTimerTimeoutCallback(k_timer *timer);

	static void ChipEventHandler(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg);

#if CONFIG_CHIP_FACTORY_DATA
	chip::DeviceLayer::FactoryDataProvider<chip::DeviceLayer::InternalFlashFactoryData> mFactoryDataProvider;
#endif
};
