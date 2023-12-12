/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

enum class AppEventType : uint8_t { None = 0, Button, Timer };

struct AppEvent {
	AppEvent(AppEventType type) { mType = type; }
	AppEvent() = default;
	union {
		struct {
			uint8_t PinNo;
			uint8_t Action;
		} ButtonEvent;
		struct {
			uint8_t TimerType;
		} TimerEvent;
	};

	AppEventType mType{ AppEventType::None };
};
