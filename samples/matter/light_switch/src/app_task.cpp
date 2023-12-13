/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "fabric_table_delegate.h"
#include "light_switch.h"
#include "task_executor.h"

#include <platform/CHIPDeviceLayer.h>

#include "board.h"
#include <app/clusters/identify-server/identify-server.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/SafeInt.h>

#include <system/SystemError.h>

#ifdef CONFIG_CHIP_WIFI
#include <app/clusters/network-commissioning/network-commissioning.h>
#include <platform/nrfconnect/wifi/NrfWiFiDriver.h>
#endif

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include "ota_util.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr uint32_t kDimmerTriggeredTimeout = 500;
constexpr uint32_t kDimmerInterval = 300;
constexpr EndpointId kLightSwitchEndpointId = 1;
constexpr EndpointId kLightEndpointId = 1;

k_timer sDimmerPressKeyTimer;
k_timer sDimmerTimer;

Identify sIdentify = { kLightEndpointId, AppTask::IdentifyStartHandler, AppTask::IdentifyStopHandler,
		       Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator };
bool sWasDimmerTriggered = false;
bool sIsNetworkProvisioned = false;
} /* namespace */

#ifdef CONFIG_CHIP_WIFI
app::Clusters::NetworkCommissioning::Instance
	sWiFiCommissioningInstance(0, &(NetworkCommissioning::NrfWiFiDriver::Instance()));
#endif

CHIP_ERROR AppTask::Init()
{
	/* Initialize CHIP stack */
	LOG_INF("Init CHIP stack");

	CHIP_ERROR err = chip::Platform::MemoryInit();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Platform::MemoryInit() failed");
		return err;
	}

	err = PlatformMgr().InitChipStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().InitChipStack() failed");
		return err;
	}

#if defined(CONFIG_NET_L2_OPENTHREAD)
	err = ThreadStackMgr().InitThreadStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ThreadStackMgr().InitThreadStack() failed: %s", ErrorStr(err));
		return err;
	}

#ifdef CONFIG_OPENTHREAD_MTD_SED
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_SleepyEndDevice);
#else
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
#endif /* CONFIG_OPENTHREAD_MTD_SED */
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ConnectivityMgr().SetThreadDeviceType() failed: %s", ErrorStr(err));
		return err;
	}

#elif defined(CONFIG_CHIP_WIFI)
	sWiFiCommissioningInstance.Init();
#else
	return CHIP_ERROR_INTERNAL;
#endif /* CONFIG_NET_L2_OPENTHREAD */

	LightSwitch::GetInstance().Init(kLightSwitchEndpointId);

	if (!GetBoard().Init(ButtonEventHandler)) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}
	/* Initialize application timers */
	k_timer_init(&sDimmerPressKeyTimer, AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sDimmerTimer, AppTask::UserTimerTimeoutCallback, nullptr);

#ifdef CONFIG_CHIP_OTA_REQUESTOR
	/* OTA image confirmation must be done before the factory data init. */
	OtaConfirmNewImage();
#endif

#ifdef CONFIG_MCUMGR_TRANSPORT_BT
	/* Initialize DFU over SMP */
	GetDFUOverSMP().Init();
	GetDFUOverSMP().ConfirmNewImage();
#endif

	/* Initialize CHIP server */
#if CONFIG_CHIP_FACTORY_DATA
	ReturnErrorOnFailure(mFactoryDataProvider.Init());
	SetDeviceInstanceInfoProvider(&mFactoryDataProvider);
	SetDeviceAttestationCredentialsProvider(&mFactoryDataProvider);
	SetCommissionableDataProvider(&mFactoryDataProvider);
#else
	SetDeviceInstanceInfoProvider(&DeviceInstanceInfoProviderMgrImpl());
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif

	static CommonCaseDeviceServerInitParams initParams;
	(void)initParams.InitializeStaticResourcesBeforeServerInit();

	ReturnErrorOnFailure(chip::Server::GetInstance().Init(initParams));
	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
	AppFabricTableDelegate::Init();

	/*
	 * Add CHIP event handler and start CHIP thread.
	 * Note that all the initialization code should happen prior to this point to avoid data races
	 * between the main and the CHIP threads.
	 */
	PlatformMgr().AddEventHandler(ChipEventHandler, 0);

	err = PlatformMgr().StartEventLoopTask();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().StartEventLoopTask() failed");
		return err;
	}

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		TaskExecutor::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}

void AppTask::ButtonPushHandler(const AppEvent &event)
{
	if (event.mType == AppEventType::Button) {
		switch (event.ButtonEvent.PinNo) {
		case APPLICATION_BUTTON:
			LOG_INF("Button has been pressed, keep in this state for at least 500 ms to change light sensitivity of bound lighting devices.");
			Instance().StartTimer(Timer::DimmerTrigger, kDimmerTriggeredTimeout);
			break;
		default:
			break;
		}
	}
}

void AppTask::ButtonReleaseHandler(const AppEvent &event)
{
	if (event.mType == AppEventType::Button) {
		switch (event.ButtonEvent.PinNo) {
		case FUNCTION_BUTTON:
#ifdef CONFIG_MCUMGR_TRANSPORT_BT
			GetDFUOverSMP().StartServer();
#else
			LOG_INF("Software update is disabled");
#endif
			break;
		case APPLICATION_BUTTON:
			if (!sWasDimmerTriggered) {
				LightSwitch::GetInstance().InitiateActionSwitch(LightSwitch::Action::Toggle);
			}
			Instance().CancelTimer(Timer::Dimmer);
			Instance().CancelTimer(Timer::DimmerTrigger);
			sWasDimmerTriggered = false;
			break;
		default:
			break;
		}
	}
}

void AppTask::TimerEventHandler(const AppEvent &event)
{
	if (event.mType == AppEventType::Timer) {
		switch (event.TimerEvent.TimerType) {
		case Timer::DimmerTrigger:
			LOG_INF("Dimming started...");
			sWasDimmerTriggered = true;
			LightSwitch::GetInstance().InitiateActionSwitch(LightSwitch::Action::On);
			Instance().StartTimer(Timer::Dimmer, kDimmerInterval);
			Instance().CancelTimer(Timer::DimmerTrigger);
			break;
		case Timer::Dimmer:
			LightSwitch::GetInstance().DimmerChangeBrightness();
			break;
		default:
			break;
		}
	}
}

void AppTask::IdentifyStartHandler(Identify *)
{
	TaskExecutor::PostTask([] { GetBoard().GetLED(DeviceLeds::kAppLED).Blink(LedConsts::kIdentifyBlinkRate_ms); });
}

void AppTask::IdentifyStopHandler(Identify *)
{
	TaskExecutor::PostTask([] { GetBoard().GetLED(DeviceLeds::kAppLED).Set(false); });
}

void AppTask::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	bool isNetworkEnabled = false;

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
			GetBoard().UpdateDeviceState(DeviceState::kDeviceConnectedBLE);
		}

		break;
#if defined(CONFIG_NET_L2_OPENTHREAD)
	case DeviceEventType::kDnssdInitialized:
#if CONFIG_CHIP_OTA_REQUESTOR
		InitBasicOTARequestor();
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
		break;
	case DeviceEventType::kThreadStateChange:
		sIsNetworkProvisioned = ConnectivityMgr().IsThreadProvisioned() && ConnectivityMgr().IsThreadEnabled();
#elif defined(CONFIG_CHIP_WIFI)
	case DeviceEventType::kWiFiConnectivityChange:
		sIsNetworkProvisioned = ConnectivityMgr().IsWiFiStationProvisioned() && ConnectivityMgr().IsWiFiStationEnabled();
#if CONFIG_CHIP_OTA_REQUESTOR
		if (event->WiFiConnectivityChange.Result == kConnectivity_Established) {
			InitBasicOTARequestor();
		}
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
#endif
		if (sIsNetworkProvisioned) {
			GetBoard().UpdateDeviceState(DeviceState::kDeviceProvisioned);
		} else {
			GetBoard().UpdateDeviceState(DeviceState::kDeviceDisconnected);
		}
		break;
	default:
		break;
	}
}

void AppTask::ButtonEventHandler(ButtonState state, ButtonMask hasChanged)
{
	AppEvent buttonEvent(AppEventType::Button);

/* For nRF7002 DK the Light Switch operations are allowed only if the device was commissioned to the Matter network */
#if NUMBER_OF_BUTTONS == 2
	bool additionalCondition = sIsNetworkProvisioned;
#else
	bool additionalCondition = true;
#endif

	if ((APPLICATION_BUTTON_MASK & state & hasChanged) && additionalCondition) {
		buttonEvent.ButtonEvent.PinNo = APPLICATION_BUTTON;
		TaskExecutor::PostTask([buttonEvent] { ButtonPushHandler(buttonEvent); });
	} else if ((APPLICATION_BUTTON_MASK & hasChanged) && additionalCondition) {
		buttonEvent.ButtonEvent.PinNo = APPLICATION_BUTTON;
		TaskExecutor::PostTask([buttonEvent] { ButtonReleaseHandler(buttonEvent); });
	} else if (FUNCTION_BUTTON_MASK & hasChanged) {
		buttonEvent.ButtonEvent.PinNo = FUNCTION_BUTTON;
		TaskExecutor::PostTask([buttonEvent] { ButtonReleaseHandler(buttonEvent); });
	}
}

void AppTask::StartTimer(Timer timer, uint32_t timeoutMs)
{
	switch (timer) {
	case Timer::DimmerTrigger:
		k_timer_start(&sDimmerPressKeyTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::Dimmer:
		k_timer_start(&sDimmerTimer, K_MSEC(timeoutMs), K_MSEC(timeoutMs));
		break;
	default:
		break;
	}
}

void AppTask::CancelTimer(Timer timer)
{
	switch (timer) {
	case Timer::DimmerTrigger:
		k_timer_stop(&sDimmerPressKeyTimer);
		break;
	case Timer::Dimmer:
		k_timer_stop(&sDimmerTimer);
		break;
	default:
		break;
	}
}

void AppTask::UserTimerTimeoutCallback(k_timer *timer)
{
	if (!timer) {
		return;
	}
	AppEvent event(AppEventType::Timer);

	if (timer == &sDimmerPressKeyTimer) {
		event.TimerEvent.TimerType = Timer::DimmerTrigger;
	} else if (timer == &sDimmerTimer) {
		event.TimerEvent.TimerType = Timer::Dimmer;
	} else {
		return;
	}

	TaskExecutor::PostTask([event]() { TimerEventHandler(event); });
}
