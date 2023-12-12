/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "board_config.h"
#include "board_consts.h"
#include "led_util.h"
#include "led_widget.h"
#include "system_event.h"

enum class DeviceState : uint8_t { kDeviceDisconnected, kDeviceAdvertisingBLE, kDeviceConnectedBLE, kDeviceProvisioned };
enum class DeviceLeds : uint8_t { kStatusLED, kAppLED, kUserLED1, kUserLED2 };

using ButtonState = uint32_t;
using ButtonMask = uint32_t;
using LedStateHandler = void (*)();

class Board {
	using LedState = bool;

public:
	/**
	 * @brief initialize Board components such as LEDs and Buttons
	 *
	 * buttonCallback: User can register a callback for button interruption that can be used in the
	 * specific way.
	 * The callback enters two arguments:
	 * ButtonState as uint32_t which represents a button state (Pressed, Released)
	 * ButtonMask as uint32_t which represents a bitmask that shows indicates whether the button has been changed
	 *
	 * ledStateHandler: User can register a custom callback for status LED behaviour,
	 * and handle the indications of the device states in the specific way.
	 *
	 * @param buttonCallback the callback function for button interruption.
	 * @param ledStateHandler
	 * @return true if board components has been initialized successfully.
	 * @return false if an error occurred.
	 */
	bool Init(button_handler_t buttonHandler = nullptr, LedStateHandler ledStateHandler = nullptr);

	/**
	 * @brief Get the LED located on the board.
	 *
	 * @param led LEDWidget an enum value of the requested led.
	 * @return LEDWidget& a reference of the choosen LED.
	 */
	LEDWidget &GetLED(DeviceLeds led);

	/**
	 * @brief Update a device state to change LED indicator
	 *
	 * The device state should be changed after three interactions:
	 * - The device is disconnected from a network.
	 * - The device is connected via Bluetooth LE and commissioning process is in progress.
	 * - The device is provisioned to the Matter network and the connection is established.
	 *
	 * @param state the new state to be set.
	 */
	void UpdateDeviceState(DeviceState state);

	DeviceState GetDeviceState(){ return sInstance.mState; }

private:
	Board() = default;
	friend Board &GetBoard();
	static Board sInstance;

	/* LEDs */
	static void UpdateStatusLED();
	static void LEDStateUpdateHandler(LEDWidget &ledWidget);
	static void UpdateLedStateEventHandler(const SystemEvent &event);
	void ResetAllLeds();

	LEDWidget mStatusLED;
	LEDWidget mApplicationLED;
	k_timer mFunctionTimer;
	DeviceState mState = DeviceState::kDeviceDisconnected;
	LedStateHandler mLedStateHandler = UpdateStatusLED;
#if NUMBER_OF_LEDS == 3
	LEDWidget mUserLED1;
#elif NUMBER_OF_LEDS == 4
	LEDWidget mUserLED1;
	LEDWidget mUserLED2;
#endif

	/* Function Timer */
	void CancelTimer();
	void StartTimer(uint32_t timeoutInMs);
	static void FunctionTimerTimeoutCallback(k_timer *timer);
	static void FunctionTimerEventHandler(const SystemEvent &event);

	bool mFunctionTimerActive = false;
	SystemEventType mFunction;

	/* Buttons */
	static void ButtonEventHandler(ButtonState buttonState, ButtonMask hasChanged);
	static void FunctionHandler(const SystemEvent &event);
	static void StartBLEAdvertisementHandler(const SystemEvent &event);
	static void StartBLEAdvertisement();

	button_handler_t mButtonHandler = nullptr;
};

/**
 * @brief Get the Board instance
 *
 * Obtain the Board instance to initialize the module, get the LEDWidget object,
 * and update the device state.
 *
 * @return Board& instance for the board.
 */
inline Board &GetBoard()
{
	return Board::sInstance;
}
