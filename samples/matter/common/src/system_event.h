/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

#include "led_widget.h"

enum class SystemEventType : uint8_t {
	None = 0,
	Button,
	ButtonPushed,
	ButtonReleased,
	Timer,
	UpdateLedState,
	SoftwareUpdate,
	FactoryReset,
	AdvertisingStart
};

struct SystemEvent {
	SystemEvent(SystemEventType type) { mType = type; }
	SystemEvent() = default;

	union {
		struct {
			uint8_t PinNo;
			uint8_t Action;
		} ButtonEvent;
		struct {
			k_timer *Timer;
		} TimerEvent;
		struct {
			LEDWidget *LedWidget;
		} UpdateLedStateEvent;
	};

	SystemEventType mType;
};
