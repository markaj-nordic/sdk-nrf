/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

enum class AppEventType : uint8_t { None = 0, NUSCommand, LockEvent, ThreadWiFiSwitch };
enum class SwitchButtonAction : uint8_t { Pressed = 0, Released };

struct AppEvent {
	AppEvent(AppEventType type) { mType = type; }
	AppEvent() = default;

	union {
		struct {
			void *Context;
		} LockEvent;
		struct {
			SwitchButtonAction ButtonAction;
		} ThreadWiFiSwitchEvent;
	};

	AppEventType mType{ AppEventType::None };
};
