/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "board.h"
#include "task_executor.h"

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

Board Board::sInstance;

bool Board::Init(button_handler_t buttonHandler, LedStateHandler ledStateHandler)
{
#ifdef CONFIG_DK_LIBRARY
	/* Initialize LEDs */
	LEDWidget::InitGpio();
	LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);
	mStatusLED.Init(SYSTEM_STATE_LED);
	mApplicationLED.Init(APPLICATION_STATE_LED);
#if NUMBER_OF_LEDS == 3
	mUserLED1.Init(USER_LED_1);
#elif NUMBER_OF_LEDS == 4
	mUserLED1.Init(USER_LED_1);
	mUserLED2.Init(USER_LED_2);
#endif

	/* Initialize buttons */
	int ret = dk_buttons_init(ButtonEventHandler);
	if (ret) {
		LOG_ERR("dk_buttons_init() failed");
		return false;
	}

	/* Register an additional button handler for the user purposes */
	if (buttonHandler) {
		static struct button_handler handler = {
			.cb = buttonHandler,
		};
		dk_button_handler_add(&handler);
	}

	/* Initialize function timer */
	k_timer_init(&mFunctionTimer, &FunctionTimerTimeoutCallback, nullptr);
	k_timer_user_data_set(&mFunctionTimer, this);

	if (ledStateHandler) {
		mLedStateHandler = ledStateHandler;
	}

	mLedStateHandler();
#endif
	return true;
}

void Board::UpdateDeviceState(DeviceState state)
{
	if (mState != state) {
		mState = state;
		ResetAllLeds();
		mLedStateHandler();
	}
}

void Board::ResetAllLeds()
{
	sInstance.mStatusLED.Set(false);
	sInstance.mApplicationLED.Set(false);
#if NUMBER_OF_LEDS == 3
	sInstance.mUserLED1.Set(false);
#elif NUMBER_OF_LEDS == 4
	sInstance.mUserLED1.Set(false);
	sInstance.mUserLED2.Set(false);
#endif
}

void Board::LEDStateUpdateHandler(LEDWidget &ledWidget)
{
	SystemEvent ledEvent(SystemEventType::UpdateLedState);
	TaskExecutor::PostTask([ledEvent] { UpdateLedStateEventHandler(ledEvent); });
}

void Board::UpdateLedStateEventHandler(const SystemEvent &event)
{
	if (event.mType == SystemEventType::UpdateLedState) {
		event.UpdateLedStateEvent.LedWidget->UpdateState();
	}
}

void Board::UpdateStatusLED()
{
	/* Update the status LED.
	 *
	 * If IPv6 network and service provisioned, keep the LED On constantly.
	 *
	 * If the system has BLE connection(s) uptill the stage above, THEN blink the LED at an even
	 * rate of 100ms.
	 *
	 * Otherwise, blink the LED for a very short time. */
	switch (sInstance.mState) {
	case DeviceState::kDeviceDisconnected:
		sInstance.mStatusLED.Blink(LedConsts::StatusLed::Disconnected::kOn_ms,
					   LedConsts::StatusLed::Disconnected::kOff_ms);

		break;
	case DeviceState::kDeviceConnectedBLE:
		sInstance.mStatusLED.Blink(LedConsts::StatusLed::BleConnected::kOn_ms,
					   LedConsts::StatusLed::BleConnected::kOff_ms);
		break;
	case DeviceState::kDeviceProvisioned:
		sInstance.mStatusLED.Set(true);
		break;
	default:
		break;
	}
}

LEDWidget &Board::GetLED(DeviceLeds led)
{
	switch (led) {
#if NUMBER_OF_LEDS == 4
	case DeviceLeds::kUserLED1:
		return mUserLED1;
	case DeviceLeds::kUserLED2:
		return mUserLED2;
#elif NUMBER_OF_LEDS == 3
	case DeviceLeds::kUserLED1:
		return mUserLED1;
#endif
	case DeviceLeds::kStatusLED:
		return mStatusLED;
	case DeviceLeds::kAppLED:
	default:
		return mApplicationLED;
	}
}

void Board::CancelTimer()
{
	k_timer_stop(&mFunctionTimer);
	mFunctionTimerActive = false;
}

void Board::StartTimer(uint32_t timeoutInMs)
{
	k_timer_start(&mFunctionTimer, K_MSEC(timeoutInMs), K_NO_WAIT);
	mFunctionTimerActive = true;
}

void Board::FunctionTimerTimeoutCallback(k_timer *timer)
{
	SystemEvent timerEvent(SystemEventType::Timer);
	timerEvent.TimerEvent.Timer = timer;
	TaskExecutor::PostTask([timerEvent] { FunctionTimerEventHandler(timerEvent); });
}

void Board::FunctionTimerEventHandler(const SystemEvent &event)
{
	/* If we reached here, the button was held past kFactoryResetTriggerTimeout, initiate factory reset */
	if (sInstance.mFunction == SystemEventType::SoftwareUpdate) {
		LOG_INF("Factory reset has been triggered. Release button within %ums to cancel.",
			FactoryResetConsts::kFactoryResetTriggerTimeout);

		/* Start timer for kFactoryResetCancelWindowTimeout to allow user to cancel, if required. */
		sInstance.StartTimer(FactoryResetConsts::kFactoryResetCancelWindowTimeout);
		sInstance.mFunction = SystemEventType::FactoryReset;

		/* Turn off all LEDs before starting blink to make sure blink is coordinated. */
		sInstance.ResetAllLeds();

		sInstance.mStatusLED.Blink(LedConsts::kBlinkRate_ms);
		sInstance.mApplicationLED.Blink(LedConsts::kBlinkRate_ms);
#if NUMBER_OF_LEDS == 3
		sInstance.mUserLED1.Blink(LedConsts::kBlinkRate_ms);
#elif NUMBER_OF_LEDS == 4
		sInstance.mUserLED1.Blink(LedConsts::kBlinkRate_ms);
		sInstance.mUserLED2.Blink(LedConsts::kBlinkRate_ms);
#endif
	} else if (sInstance.mFunction == SystemEventType::FactoryReset) {
		/* Actually trigger Factory Reset */
		sInstance.mFunction = SystemEventType::None;
		chip::Server::GetInstance().ScheduleFactoryReset();

	} else if (sInstance.mFunction == SystemEventType::AdvertisingStart) {
		/* The button was held past kAdvertisingTriggerTimeout, start BLE advertisement
		   if we have 2 buttons UI*/
#if NUMBER_OF_BUTTONS == 2
		StartBLEAdvertisement();
		sInstance.mFunction = SystemEventType::None;
#endif
	}
}

void Board::ButtonEventHandler(ButtonState buttonState, ButtonMask hasChanged)
{
	SystemEvent buttonEvent(SystemEventType::Button);

	if (BLUETOOTH_ADV_BUTTON_MASK & hasChanged) {
		buttonEvent.ButtonEvent.PinNo = BLUETOOTH_ADV_BUTTON;
		buttonEvent.ButtonEvent.Action = static_cast<uint8_t>((BLUETOOTH_ADV_BUTTON_MASK & buttonState) ?
									      SystemEventType::ButtonPushed :
									      SystemEventType::ButtonReleased);
		TaskExecutor::PostTask([buttonEvent] { StartBLEAdvertisementHandler(buttonEvent); });
	}

	if (FUNCTION_BUTTON_MASK & hasChanged) {
		buttonEvent.ButtonEvent.PinNo = FUNCTION_BUTTON;
		buttonEvent.ButtonEvent.Action =
			static_cast<uint8_t>((FUNCTION_BUTTON_MASK & buttonState) ? SystemEventType::ButtonPushed :
										    SystemEventType::ButtonReleased);
		TaskExecutor::PostTask([buttonEvent] { FunctionHandler(buttonEvent); });
	}
}

void Board::FunctionHandler(const SystemEvent &event)
{
	if (event.ButtonEvent.PinNo != FUNCTION_BUTTON) {
		return;
	}
	if (event.ButtonEvent.Action == static_cast<uint8_t>(SystemEventType::ButtonPushed)) {
		if (!sInstance.mFunctionTimerActive && sInstance.mFunction == SystemEventType::None) {
			sInstance.mFunction = SystemEventType::SoftwareUpdate;
			sInstance.StartTimer(FactoryResetConsts::kFactoryResetTriggerTimeout);
		}
	} else {
		/* If the button was released before factory reset got initiated, trigger a software update. */
		if (sInstance.mFunctionTimerActive && sInstance.mFunction == SystemEventType::SoftwareUpdate) {
			sInstance.CancelTimer();
			sInstance.mFunction = SystemEventType::None;
		} else if (sInstance.mFunctionTimerActive && sInstance.mFunction == SystemEventType::FactoryReset) {
			sInstance.ResetAllLeds();
			sInstance.CancelTimer();
			sInstance.mLedStateHandler();
			sInstance.mFunction = SystemEventType::None;
			LOG_INF("Factory reset has been canceled");
		}
	}
}

void Board::StartBLEAdvertisementHandler(const SystemEvent &event)
{
#ifndef CUSTOM_BLUETOOTH_ADVERTISING
#if NUMBER_OF_BUTTONS == 2 && !(SKIP_DEFERRED_BLE_ADV)
	if (event.ButtonEvent.PinNo == FUNCTION_BUTTON) {
		if (event.ButtonEvent.Action == static_cast<uint8_t>(SystemEventType::ButtonPushed)) {
			StartBLEAdvertisement();
		}
	} else if (event.ButtonEvent.Action == static_cast<uint8_t>(SystemEventType::ButtonPushed)) {
		sInstance.StartTimer(AdvertisingConsts::kAdvertisingTriggerTimeout);
		sInstance.mFunction = SystemEventType::AdvertisingStart;
	} else {
		if (sInstance.mFunction == SystemEventType::AdvertisingStart && sInstance.mFunctionTimerActive) {
			sInstance.CancelTimer();
			sInstance.mFunction = SystemEventType::None;
		}
	}
#else
	if (event.ButtonEvent.Action == static_cast<uint8_t>(SystemEventType::ButtonPushed)) {
		StartBLEAdvertisement();
	}
#endif
#endif /* CUSTOM_BLUETOOTH_ADVERTISING */
}

void Board::StartBLEAdvertisement()
{
	if (chip::Server::GetInstance().GetFabricTable().FabricCount() != 0) {
		LOG_INF("Matter service BLE advertising not started - device is already commissioned");
		return;
	}

	if (chip::DeviceLayer::ConnectivityMgr().IsBLEAdvertisingEnabled()) {
		LOG_INF("BLE advertising is already enabled");
		return;
	}

	if (chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow() !=
	    CHIP_NO_ERROR) {
		LOG_ERR("OpenBasicCommissioningWindow() failed");
	}
}
