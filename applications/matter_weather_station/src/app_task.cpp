/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"
#include "board.h"
#include "task_executor.h"

#include "battery.h"
#include "buzzer.h"
#include "fabric_table_delegate.h"
#include "led_widget.h"
#include <platform/CHIPDeviceLayer.h>

#include <DeviceInfoProviderImpl.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/ota-requestor/OTATestEventTriggerDelegate.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>

#ifdef CONFIG_CHIP_WIFI
#include <app/clusters/network-commissioning/network-commissioning.h>
#include <platform/nrfconnect/wifi/NrfWiFiDriver.h>
#endif

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include "ota_util.h"
#endif

#include <dk_buttons_and_leds.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

using namespace ::chip;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app;

LOG_MODULE_DECLARE(app);

namespace
{
enum class FunctionTimerMode { kDisabled, kFactoryResetTrigger, kFactoryResetComplete };
enum class LedState { kAlive, kAdvertisingBle, kConnectedBle, kProvisioned };

#if CONFIG_AVERAGE_CURRENT_CONSUMPTION <= 0
#error Invalid CONFIG_AVERAGE_CURRENT_CONSUMPTION value set
#endif

constexpr size_t kMeasurementsIntervalMs = 3000;
constexpr uint8_t kTemperatureMeasurementEndpointId = 1;
constexpr int16_t kTemperatureMeasurementAttributeMaxValue = 0x7fff;
constexpr int16_t kTemperatureMeasurementAttributeMinValue = 0x954d;
constexpr int16_t kTemperatureMeasurementAttributeInvalidValue = 0x8000;
constexpr uint8_t kHumidityMeasurementEndpointId = 2;
constexpr uint16_t kHumidityMeasurementAttributeMaxValue = 0x2710;
constexpr uint16_t kHumidityMeasurementAttributeMinValue = 0;
constexpr uint16_t kHumidityMeasurementAttributeInvalidValue = 0xffff;
constexpr uint8_t kPressureMeasurementEndpointId = 3;
constexpr int16_t kPressureMeasurementAttributeMaxValue = 0x7fff;
constexpr int16_t kPressureMeasurementAttributeMinValue = 0x8001;
constexpr int16_t kPressureMeasurementAttributeInvalidValue = 0x8000;
constexpr uint8_t kPowerSourceEndpointId = 0;
constexpr int16_t kMinimalOperatingVoltageMv = 3200;
constexpr int16_t kMaximalOperatingVoltageMv = 4050;
constexpr int16_t kWarningThresholdVoltageMv = 3450;
constexpr int16_t kCriticalThresholdVoltageMv = 3250;
constexpr uint8_t kMinBatteryPercentage = 0;
/* Value is expressed in half percent units ranging from 0 to 200. */
constexpr uint8_t kMaxBatteryPercentage = 200;
/* Battery capacity in uAh */
constexpr uint32_t kBatteryCapacityUaH = 1350000;
/* Average device current consumption in uA */
constexpr uint32_t kDeviceAverageCurrentConsumptionUa = CONFIG_AVERAGE_CURRENT_CONSUMPTION;
/* Fully charged battery operation time in seconds */
constexpr uint32_t kFullBatteryOperationTime = kBatteryCapacityUaH / kDeviceAverageCurrentConsumptionUa * 3600;
/* It is recommended to toggle the signalled state with 0.5 s interval. */
constexpr size_t kIdentifyTimerIntervalMs = 500;

k_timer sMeasurementsTimer;
k_timer sIdentifyTimer;

/* NOTE! This key is for test/certification only and should not be available in production devices!
 * If CONFIG_CHIP_FACTORY_DATA is enabled, this value is read from the factory data.
 */
uint8_t sTestEventTriggerEnableKey[TestEventTriggerDelegate::kEnableKeyLength] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
										   0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
										   0xcc, 0xdd, 0xee, 0xff };
chip::DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;

/* Add identify for all endpoints */
Identify sIdentifyTemperature = { chip::EndpointId{ kTemperatureMeasurementEndpointId }, AppTask::OnIdentifyStart,
				  AppTask::OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kAudibleBeep };
Identify sIdentifyHumidity = { chip::EndpointId{ kHumidityMeasurementEndpointId }, AppTask::OnIdentifyStart,
			       AppTask::OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kAudibleBeep };
Identify sIdentifyPressure = { chip::EndpointId{ kPressureMeasurementEndpointId }, AppTask::OnIdentifyStart,
			       AppTask::OnIdentifyStop, Clusters::Identify::IdentifyTypeEnum::kAudibleBeep };

const device *sBme688SensorDev = DEVICE_DT_GET_ONE(bosch_bme680);
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
		LOG_ERR("ThreadStackMgr().InitThreadStack() failed");
		return err;
	}

#ifdef CONFIG_OPENTHREAD_MTD_SED
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_SleepyEndDevice);
#else
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
#endif
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ConnectivityMgr().SetThreadDeviceType() failed");
		return err;
	}

#elif defined(CONFIG_CHIP_WIFI)
	sWiFiCommissioningInstance.Init();
#else
	return CHIP_ERROR_INTERNAL;
#endif /* CONFIG_NET_L2_OPENTHREAD */

	/* Get references for RGB LED */
	mRedLED = &GetBoard().GetLED(DeviceLeds::kStatusLED);
	mGreenLED = &GetBoard().GetLED(DeviceLeds::kAppLED);
	mBlueLED = &GetBoard().GetLED(DeviceLeds::kUserLED1);

	if (!GetBoard().Init(nullptr, UpdateLedState)) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	if (!device_is_ready(sBme688SensorDev)) {
		LOG_ERR("BME688 sensor device not ready");
		return chip::System::MapErrorZephyr(-ENODEV);
	}

	int ret = BatteryMeasurementInit();
	if (ret) {
		LOG_ERR("Battery measurement init failed");
		return chip::System::MapErrorZephyr(ret);
	}

	ret = BatteryMeasurementEnable();
	if (ret) {
		LOG_ERR("Enabling battery measurement failed");
		return chip::System::MapErrorZephyr(ret);
	}

	ret = BatteryChargeControlInit();
	if (ret) {
		LOG_ERR("Battery charge control init failed");
		return chip::System::MapErrorZephyr(ret);
	}

	ret = BuzzerInit();
	if (ret) {
		LOG_ERR("Buzzer init failed");
		return chip::System::MapErrorZephyr(ret);
	}

#ifdef CONFIG_CHIP_OTA_REQUESTOR
	/* OTA image confirmation must be done before the factory data init. */
	OtaConfirmNewImage();
#endif

#ifdef CONFIG_MCUMGR_TRANSPORT_BT
	/* Initialize DFU over SMP */
	GetDFUOverSMP().Init();
	GetDFUOverSMP().ConfirmNewImage();
	GetDFUOverSMP().StartServer();
#endif

/* Get factory data */
#ifdef CONFIG_CHIP_FACTORY_DATA
	ReturnErrorOnFailure(mFactoryDataProvider.Init());
	SetDeviceInstanceInfoProvider(&mFactoryDataProvider);
	SetDeviceAttestationCredentialsProvider(&mFactoryDataProvider);
	SetCommissionableDataProvider(&mFactoryDataProvider);
	/* Read EnableKey from the factory data. */
	MutableByteSpan enableKey(sTestEventTriggerEnableKey);
	err = mFactoryDataProvider.GetEnableKey(enableKey);
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("mFactoryDataProvider.GetEnableKey() failed. Could not delegate a test event trigger");
		memset(sTestEventTriggerEnableKey, 0, sizeof(sTestEventTriggerEnableKey));
	}
#else
	SetDeviceInstanceInfoProvider(&DeviceInstanceInfoProviderMgrImpl());
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif

	/* Initialize timers */
	k_timer_init(
		&sMeasurementsTimer, [](k_timer *) { TaskExecutor::PostTask([] { MeasurementsTimerHandler(); }); },
		nullptr);
	k_timer_init(
		&sIdentifyTimer, [](k_timer *) { TaskExecutor::PostTask([] { IdentifyTimerHandler(); }); }, nullptr);
	k_timer_start(&sMeasurementsTimer, K_MSEC(kMeasurementsIntervalMs), K_MSEC(kMeasurementsIntervalMs));

	/* Initialize CHIP server */
	static chip::CommonCaseDeviceServerInitParams initParams;
	static OTATestEventTriggerDelegate testEventTriggerDelegate{ ByteSpan(sTestEventTriggerEnableKey) };
	(void)initParams.InitializeStaticResourcesBeforeServerInit();

	initParams.testEventTriggerDelegate = &testEventTriggerDelegate;
	ReturnErrorOnFailure(chip::Server::GetInstance().Init(initParams));

	gExampleDeviceInfoProvider.SetStorageDelegate(&Server::GetInstance().GetPersistentStorage());
	chip::DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);
	AppFabricTableDelegate::Init();

	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

	/*
	 * Add CHIP event handler and start CHIP thread.
	 * Note that all the initialization code should happen prior to this point
	 * to avoid data races between the main and the CHIP threads.
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

void AppTask::MeasurementsTimerHandler()
{
	Instance().UpdateClustersState();
}

void AppTask::OnIdentifyStart(Identify *)
{
	TaskExecutor::PostTask([] { GetBoard().GetLED(DeviceLeds::kAppLED).Blink(LedConsts::kIdentifyBlinkRate_ms); });
	k_timer_start(&sIdentifyTimer, K_MSEC(kIdentifyTimerIntervalMs), K_MSEC(kIdentifyTimerIntervalMs));
}

void AppTask::OnIdentifyStop(Identify *)
{
	k_timer_stop(&sIdentifyTimer);
	BuzzerSetState(false);
}

void AppTask::IdentifyTimerHandler()
{
	BuzzerToggleState();
}

void AppTask::UpdateTemperatureClusterState()
{
	struct sensor_value sTemperature;
	EmberAfStatus status;
	int result = sensor_channel_get(sBme688SensorDev, SENSOR_CHAN_AMBIENT_TEMP, &sTemperature);
	if (result == 0) {
		/* Defined by cluster temperature measured value = 100 x temperature in degC with resolution of
		 * 0.01 degC. val1 is an integer part of the value and val2 is fractional part in one-millionth
		 * parts. To achieve resolution of 0.01 degC val2 needs to be divided by 10000. */
		int16_t newValue = static_cast<int16_t>(sTemperature.val1 * 100 + sTemperature.val2 / 10000);

		if (newValue > kTemperatureMeasurementAttributeMaxValue ||
		    newValue < kTemperatureMeasurementAttributeMinValue) {
			/* Read value exceeds permitted limits, so assign invalid value code to it. */
			newValue = kTemperatureMeasurementAttributeInvalidValue;
		}

		status = Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(
			kTemperatureMeasurementEndpointId, newValue);
		if (status != EMBER_ZCL_STATUS_SUCCESS) {
			LOG_ERR("Updating temperature measurement %x", status);
		}
	} else {
		LOG_ERR("Getting temperature measurement data from BME688 failed with: %d", result);
	}
}

void AppTask::UpdatePressureClusterState()
{
	struct sensor_value sPressure;
	EmberAfStatus status;
	int result = sensor_channel_get(sBme688SensorDev, SENSOR_CHAN_PRESS, &sPressure);
	if (result == 0) {
		/* Defined by cluster pressure measured value = 10 x pressure in kPa with resolution of 0.1 kPa.
		 * val1 is an integer part of the value and val2 is fractional part in one-millionth parts.
		 * To achieve resolution of 0.1 kPa val2 needs to be divided by 100000. */
		int16_t newValue = static_cast<int16_t>(sPressure.val1 * 10 + sPressure.val2 / 100000);

		if (newValue > kPressureMeasurementAttributeMaxValue ||
		    newValue < kPressureMeasurementAttributeMinValue) {
			/* Read value exceeds permitted limits, so assign invalid value code to it. */
			newValue = kPressureMeasurementAttributeInvalidValue;
		}

		status = Clusters::PressureMeasurement::Attributes::MeasuredValue::Set(kPressureMeasurementEndpointId,
										       newValue);
		if (status != EMBER_ZCL_STATUS_SUCCESS) {
			LOG_ERR("Updating pressure measurement %x", status);
		}
	} else {
		LOG_ERR("Getting pressure measurement data from BME688 failed with: %d", result);
	}
}

void AppTask::UpdateRelativeHumidityClusterState()
{
	struct sensor_value sHumidity;
	EmberAfStatus status;
	int result = sensor_channel_get(sBme688SensorDev, SENSOR_CHAN_HUMIDITY, &sHumidity);
	if (result == 0) {
		/* Defined by cluster humidity measured value = 100 x humidity in %RH with resolution of 0.01 %.
		 * val1 is an integer part of the value and val2 is fractional part in one-millionth parts.
		 * To achieve resolution of 0.01 % val2 needs to be divided by 10000. */
		uint16_t newValue = static_cast<int16_t>(sHumidity.val1 * 100 + sHumidity.val2 / 10000);

		if (newValue > kHumidityMeasurementAttributeMaxValue ||
		    newValue < kHumidityMeasurementAttributeMinValue) {
			/* Read value exceeds permitted limits, so assign invalid value code to it. */
			newValue = kHumidityMeasurementAttributeInvalidValue;
		}

		status = Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(
			kHumidityMeasurementEndpointId, newValue);
		if (status != EMBER_ZCL_STATUS_SUCCESS) {
			LOG_ERR("Updating relative humidity measurement %x", status);
		}
	} else {
		LOG_ERR("Getting humidity measurement data from BME688 failed with: %d", result);
	}
}

void AppTask::UpdatePowerSourceClusterState()
{
	EmberAfStatus status;
	int32_t voltage = BatteryMeasurementReadVoltageMv();
	/* Value is expressed in half percent units ranging from 0 to 200. */
	uint8_t batteryPercentage;
	uint32_t batteryTimeRemaining;
	Clusters::PowerSource::PowerSourceStatusEnum batteryStatus;
	Clusters::PowerSource::BatChargeLevelEnum batteryChargeLevel;
	bool batteryPresent;
	Clusters::PowerSource::BatChargeStateEnum batteryCharged;

	if (voltage < 0) {
		voltage = 0;
		batteryPercentage = 0;
		batteryStatus = Clusters::PowerSource::PowerSourceStatusEnum::kUnavailable;
		batteryPresent = false;

		LOG_ERR("Battery level measurement failed %d", voltage);
	} else {
		batteryStatus = Clusters::PowerSource::PowerSourceStatusEnum::kActive;
		batteryPresent = true;
	}

	if (voltage <= kMinimalOperatingVoltageMv) {
		batteryPercentage = kMinBatteryPercentage;
	} else if (voltage >= kMaximalOperatingVoltageMv) {
		batteryPercentage = kMaxBatteryPercentage;
	} else {
		batteryPercentage = kMaxBatteryPercentage * (voltage - kMinimalOperatingVoltageMv) /
				    (kMaximalOperatingVoltageMv - kMinimalOperatingVoltageMv);
	}

	batteryTimeRemaining = kFullBatteryOperationTime * batteryPercentage / kMaxBatteryPercentage;

	if (voltage < kCriticalThresholdVoltageMv) {
		batteryChargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kCritical;
	} else if (voltage < kWarningThresholdVoltageMv) {
		batteryChargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kWarning;
	} else {
		batteryChargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kOk;
	}

	if (BatteryCharged()) {
		batteryCharged = Clusters::PowerSource::BatChargeStateEnum::kIsCharging;
	} else {
		batteryCharged = Clusters::PowerSource::BatChargeStateEnum::kIsNotCharging;
	}

	status = Clusters::PowerSource::Attributes::BatVoltage::Set(kPowerSourceEndpointId, voltage);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery voltage failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatPercentRemaining::Set(kPowerSourceEndpointId, batteryPercentage);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery percentage failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatTimeRemaining::Set(kPowerSourceEndpointId, batteryTimeRemaining);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery time remaining failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatChargeLevel::Set(kPowerSourceEndpointId, batteryChargeLevel);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery charge level failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::Status::Set(kPowerSourceEndpointId, batteryStatus);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery status failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatPresent::Set(kPowerSourceEndpointId, batteryPresent);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery present failed %x", status);
	}

	status = Clusters::PowerSource::Attributes::BatChargeState::Set(kPowerSourceEndpointId, batteryCharged);
	if (status != EMBER_ZCL_STATUS_SUCCESS) {
		LOG_ERR("Updating battery charge failed %x", status);
	}
}

void AppTask::UpdateClustersState()
{
	const int result = sensor_sample_fetch(sBme688SensorDev);

	if (result == 0) {
		UpdateTemperatureClusterState();
		UpdatePressureClusterState();
		UpdateRelativeHumidityClusterState();
	} else {
		LOG_ERR("Fetching data from BME688 sensor failed with: %d", result);
	}

	UpdatePowerSourceClusterState();
}

void AppTask::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
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
			GetBoard().UpdateDeviceState(DeviceState::kDeviceConnectedBLE);
		} else if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Started) {
			GetBoard().UpdateDeviceState(DeviceState::kDeviceAdvertisingBLE);
		} else {
			GetBoard().UpdateDeviceState(DeviceState::kDeviceDisconnected);
		}
		break;
#if defined(CONFIG_NET_L2_OPENTHREAD)
	case DeviceEventType::kDnssdInitialized:
#if CONFIG_CHIP_OTA_REQUESTOR
		InitBasicOTARequestor();
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
		break;
	case DeviceEventType::kThreadStateChange:
		isNetworkProvisioned = ConnectivityMgr().IsThreadProvisioned() && ConnectivityMgr().IsThreadEnabled();
#elif defined(CONFIG_CHIP_WIFI)
	case DeviceEventType::kWiFiConnectivityChange:
		isNetworkProvisioned = ConnectivityMgr().IsWiFiStationProvisioned() && ConnectivityMgr().IsWiFiStationEnabled();
#if CONFIG_CHIP_OTA_REQUESTOR
		if (event->WiFiConnectivityChange.Result == kConnectivity_Established) {
			InitBasicOTARequestor();
		}
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
#endif
		if (isNetworkProvisioned) {
			GetBoard().UpdateDeviceState(DeviceState::kDeviceProvisioned);
		}
		break;
	default:
		break;
	}
}

void AppTask::UpdateLedState()
{
	if (!Instance().mGreenLED || !Instance().mBlueLED || !Instance().mRedLED) {
		return;
	}

	switch (GetBoard().GetDeviceState()) {
	case DeviceState::kDeviceAdvertisingBLE:
		Instance().mBlueLED->Blink(LedConsts::StatusLed::Disconnected::kOn_ms,
					   LedConsts::StatusLed::Disconnected::kOff_ms);
	case DeviceState::kDeviceDisconnected:
		Instance().mGreenLED->Blink(LedConsts::StatusLed::Disconnected::kOn_ms,
					    LedConsts::StatusLed::Disconnected::kOff_ms);
		break;
	case DeviceState::kDeviceConnectedBLE:
		Instance().mBlueLED->Blink(LedConsts::StatusLed::BleConnected::kOn_ms,
					   LedConsts::StatusLed::BleConnected::kOff_ms);
		break;
	case DeviceState::kDeviceProvisioned:
		Instance().mRedLED->Blink(LedConsts::StatusLed::Disconnected::kOn_ms,
					  LedConsts::StatusLed::Disconnected::kOff_ms);
		Instance().mBlueLED->Blink(LedConsts::StatusLed::Disconnected::kOn_ms,
					   LedConsts::StatusLed::Disconnected::kOff_ms);
		break;
	default:
		break;
	}
}
