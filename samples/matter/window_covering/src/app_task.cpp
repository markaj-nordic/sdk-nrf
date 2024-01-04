/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "matter_init.h"
#include "task_executor.h"
#include "window_covering.h"

#include <app/clusters/identify-server/identify-server.h>
#include <app/server/OnboardingCodesUtil.h>

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include "ota_util.h"
#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
Identify sIdentify = { WindowCovering::Endpoint(), AppTask::IdentifyStartHandler, AppTask::IdentifyStopHandler,
		       Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator };

#define OPEN_BUTTON_MASK DK_BTN2_MSK
#define CLOSE_BUTTON_MASK DK_BTN3_MSK
} /* namespace */

void AppTask::IdentifyStartHandler(Identify *)
{
	TaskExecutor::PostTask([] {
		WindowCovering::Instance().GetLiftIndicator().SuppressOutput();
		GetBoard().GetLED(DeviceLeds::LED2).Blink(LedConsts::kIdentifyBlinkRate_ms);
	});
}

void AppTask::IdentifyStopHandler(Identify *)
{
	TaskExecutor::PostTask([] {
		GetBoard().GetLED(DeviceLeds::LED2).Set(false);
		WindowCovering::Instance().GetLiftIndicator().ApplyLevel();
	});
}

void AppTask::ButtonEventHandler(ButtonState state, ButtonMask hasChanged)
{
	if (OPEN_BUTTON_MASK & hasChanged) {
		WindowButtonAction action =
			(OPEN_BUTTON_MASK & state) ? WindowButtonAction::Pressed : WindowButtonAction::Released;
		TaskExecutor::PostTask([action] { OpenHandler(action); });
	}

	if (CLOSE_BUTTON_MASK & hasChanged) {
		WindowButtonAction action =
			(CLOSE_BUTTON_MASK & state) ? WindowButtonAction::Pressed : WindowButtonAction::Released;
		TaskExecutor::PostTask([action] { CloseHandler(action); });
	}
}

void AppTask::OpenHandler(const WindowButtonAction &action)
{
	if (action == WindowButtonAction::Pressed) {
		Instance().mOpenButtonIsPressed = true;
		if (Instance().mCloseButtonIsPressed) {
			Instance().ToggleMoveType();
		}
	} else {
		if (!Instance().mCloseButtonIsPressed) {
			if (!Instance().mMoveTypeRecentlyChanged) {
				WindowCovering::Instance().SetSingleStepTarget(OperationalState::MovingUpOrOpen);
			} else {
				Instance().mMoveTypeRecentlyChanged = false;
			}
		}
		Instance().mOpenButtonIsPressed = false;
	}
}

void AppTask::CloseHandler(const WindowButtonAction &action)
{
	if (action == WindowButtonAction::Pressed) {
		Instance().mCloseButtonIsPressed = true;
		if (Instance().mOpenButtonIsPressed) {
			Instance().ToggleMoveType();
		}
	} else {
		if (!Instance().mOpenButtonIsPressed) {
			if (!Instance().mMoveTypeRecentlyChanged) {
				WindowCovering::Instance().SetSingleStepTarget(OperationalState::MovingDownOrClose);
			} else {
				Instance().mMoveTypeRecentlyChanged = false;
			}
		}
		Instance().mCloseButtonIsPressed = false;
	}
}

void AppTask::ToggleMoveType()
{
	if (WindowCovering::Instance().GetMoveType() == WindowCovering::MoveType::LIFT) {
		WindowCovering::Instance().SetMoveType(WindowCovering::MoveType::TILT);
		LOG_INF("Window covering move: tilt");
	} else {
		WindowCovering::Instance().SetMoveType(WindowCovering::MoveType::LIFT);
		LOG_INF("Window covering move: lift");
	}
	mMoveTypeRecentlyChanged = true;
}

void AppTask::MatterEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	bool isNetworkProvisioned = false;

	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
#ifdef CONFIG_CHIP_NFC_COMMISSIONING
		if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Started) {
			if (NFCMgr().IsTagEmulationStarted()) {
				LOG_INF("NFC Tag emulation is already started");
			} else {
				ShareQRCodeOverNFC(
					chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
			}
		} else if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Stopped) {
			NFCMgr().StopTagEmulation();
		}
#endif
		if (ConnectivityMgr().NumBLEConnections() != 0) {
			GetBoard().UpdateDeviceState(DeviceState::DeviceConnectedBLE);
		}
		break;
	case DeviceEventType::kThreadStateChange:
		isNetworkProvisioned = ConnectivityMgr().IsThreadProvisioned() && ConnectivityMgr().IsThreadEnabled();
		if (isNetworkProvisioned) {
			GetBoard().UpdateDeviceState(DeviceState::DeviceProvisioned);
		} else {
			GetBoard().UpdateDeviceState(DeviceState::DeviceDisconnected);
		}
		break;
	case DeviceEventType::kDnssdInitialized:
#if CONFIG_CHIP_OTA_REQUESTOR
		InitBasicOTARequestor();
#endif
		break;
	default:
		break;
	}
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nordic::Matter::PrepareServer(
		MatterEventHandler, Nordic::Matter::InitData{ .mPostServerInitClbk = [] {
			WindowCovering::Instance().PositionLEDUpdate(WindowCovering::MoveType::LIFT);
			WindowCovering::Instance().PositionLEDUpdate(WindowCovering::MoveType::TILT);
			return CHIP_NO_ERROR;
		} }));

	if (!GetBoard().Init(ButtonEventHandler)) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	return Nordic::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		TaskExecutor::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
