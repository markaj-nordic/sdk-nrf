/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

enum AppEventType : uint8_t { MeasurementsTimer, IdentifyTimer };

struct AppEvent {
	AppEvent(AppEventType type) { mType = type; }
	AppEvent() = default;

	AppEventType mType;
};
