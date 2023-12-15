/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "fabric_table_delegate.h"

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include "ota_util.h"
#endif

#include <app/server/OnboardingCodesUtil.h>
#include <platform/nrfconnect/FactoryDataProvider.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

namespace
{

#ifdef CONFIG_CHIP_WIFI
app::Clusters::NetworkCommissioning::Instance
	sWiFiCommissioningInstance(0, &(NetworkCommissioning::NrfWiFiDriver::Instance()));
#endif

#if CONFIG_CHIP_FACTORY_DATA
FactoryDataProvider<chip::DeviceLayer::InternalFlashFactoryData> sFactoryDataProvider;
#endif

K_MUTEX_DEFINE(sInitMutex);
K_CONDVAR_DEFINE(sInitCondVar);
CHIP_ERROR sInitResult;

CHIP_ERROR ConfigureThreadRole()
{
	using ThreadRole = ConnectivityManager::ThreadDeviceType;

	ThreadRole threadRole{ ThreadRole::kThreadDeviceType_MinimalEndDevice };
#ifdef CONFIG_OPENTHREAD_MTD_SED
#ifdef CONFIG_CHIP_THREAD_SSED
	threadRole = ThreadRole::kThreadDeviceType_SynchronizedSleepyEndDevice;
#else
	threadRole = ThreadRole::kThreadDeviceType_SleepyEndDevice;
#endif /* CONFIG_CHIP_THREAD_SSED */
#elif defined(CONFIG_OPENTHREAD_FTD)
	threadRole = ThreadRole::ConnectivityManager::kThreadDeviceType_Router;
#endif /* CONFIG_OPENTHREAD_MTD_SED */

	return ConnectivityMgr().SetThreadDeviceType(threadRole);
}

void DoInitChipServer(intptr_t arg)
{
	/* Initialize CHIP stack */
	LOG_INF("Init CHIP stack");

	sInitResult = chip::Platform::MemoryInit();
	if (sInitResult != CHIP_NO_ERROR) {
		LOG_ERR("Platform::MemoryInit() failed");
		return;
	}

	sInitResult = PlatformMgr().InitChipStack();
	if (sInitResult != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().InitChipStack() failed");
		return;
	}

#if defined(CONFIG_NET_L2_OPENTHREAD)
	sInitResult = ThreadStackMgr().InitThreadStack();
	if (sInitResult != CHIP_NO_ERROR) {
		LOG_ERR("ThreadStackMgr().InitThreadStack() failed: %s", ErrorStr(sInitResult));
		return;
	}

	sInitResult = ConfigureThreadRole();
	if (sInitResult != CHIP_NO_ERROR) {
		LOG_ERR("Cannot configure Thread role: %s", ErrorStr(sInitResult));
		return;
	}
#elif defined(CONFIG_CHIP_WIFI)
	sWiFiCommissioningInstance.Init();
#else
	sInitResult = CHIP_ERROR_INTERNAL;
	LOG_ERR("No valid L2 network backend selected");
	return;
#endif /* CONFIG_NET_L2_OPENTHREAD */

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
	sInitResult = sFactoryDataProvider.Init();
	if (sInitResult != CHIP_NO_ERROR) {
		LOG_ERR("chip::Server::Init() failed: %s", ErrorStr(sInitResult));
		return;
	}
	SetDeviceInstanceInfoProvider(&sFactoryDataProvider);
	SetDeviceAttestationCredentialsProvider(&sFactoryDataProvider);
	SetCommissionableDataProvider(&sFactoryDataProvider);
#else
	SetDeviceInstanceInfoProvider(&DeviceInstanceInfoProviderMgrImpl());
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif

	static chip::CommonCaseDeviceServerInitParams initParams;
	(void)initParams.InitializeStaticResourcesBeforeServerInit();

	sInitResult = chip::Server::GetInstance().Init(initParams);
	if (sInitResult != CHIP_NO_ERROR) {
		LOG_ERR("chip::Server::Init() failed: %s", ErrorStr(sInitResult));
		return;
	}
	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
	AppFabricTableDelegate::Init();

	sInitResult = PlatformMgr().AddEventHandler(reinterpret_cast<PlatformManager::EventHandlerFunct>(arg), 0);
	k_condvar_signal(&sInitCondVar);
}
} // namespace

namespace nordic
{
namespace matter
{
	CHIP_ERROR InitChipServer(PlatformManager::EventHandlerFunct chipEventHandler)
	{
		/* Schedule all CHIP initializations to the CHIP thread for better synchronization. */
		return chip::DeviceLayer::PlatformMgr().ScheduleWork(DoInitChipServer,
								     reinterpret_cast<intptr_t>(chipEventHandler));
	}

	CHIP_ERROR StartChipServer()
	{
		CHIP_ERROR err = PlatformMgr().StartEventLoopTask();
		if (err != CHIP_NO_ERROR) {
			LOG_ERR("PlatformMgr().StartEventLoopTask() failed");
			return err;
		}
		return CHIP_NO_ERROR;
	}

	CHIP_ERROR WaitForReadiness()
	{
		k_condvar_wait(&sInitCondVar, &sInitMutex, K_FOREVER);
		return sInitResult;
	}

} // namespace matter
} // namespace nordic