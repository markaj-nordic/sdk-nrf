/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

enum class AppEventType : uint8_t { None = 0, WindowButton };

enum class WindowButtonAction : uint8_t { Pushed, Released };

enum class WindowButton : uint8_t { OpenButton, CloseButton };

struct AppEvent {
	AppEvent(AppEventType type) { mType = type; }
	AppEvent() = default;

	union {
		struct {
			WindowButton Button;
			WindowButtonAction Action;
		} WindowButtonEvent;
	};

	AppEventType mType{ AppEventType::None };
};
