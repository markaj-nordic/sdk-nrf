/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

enum class AppEventType : uint8_t { None = 0, TemperatureButton };

enum class TemperatureButtonAction : uint8_t { Pushed, Released };

struct AppEvent {
	AppEvent(AppEventType type) { mType = type; }
	AppEvent() = default;

	union {
		struct {
			TemperatureButtonAction Action;
		} TemperatureButtonEvent;
	};

	AppEventType mType{ AppEventType::None };
};
