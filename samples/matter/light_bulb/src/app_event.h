/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

enum class AppEventType : uint8_t { None = 0, Lighting, Button };
struct AppEvent {
	AppEvent(AppEventType type) { mType = type; }
	AppEvent() = default;

	union {
		struct {
			uint8_t Action;
			int32_t Actor;
		} LightingEvent;
	};

	AppEventType mType{ AppEventType::None };
};
