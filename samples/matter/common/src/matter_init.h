/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <platform/PlatformManager.h>
#include <platform/nrfconnect/FactoryDataProvider.h>

namespace nordic
{
namespace matter
{
	CHIP_ERROR InitChipServer(chip::DeviceLayer::PlatformManager::EventHandlerFunct chipEventHandler);
	CHIP_ERROR StartChipServer();
	CHIP_ERROR WaitForReadiness();

} // namespace matter
} // namespace nordic
