/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "bolt_lock_manager.h"

#include "app_event.h"
#include "app_task.h"

using namespace chip;

BoltLockManager BoltLockManager::sLock;

void BoltLockManager::Init(StateChangeCallback callback)
{
	mStateChangeCallback = callback;

	k_timer_init(&mActuatorTimer, &BoltLockManager::ActuatorTimerEventHandler, nullptr);
	k_timer_user_data_set(&mActuatorTimer, this);

	InitializeCredentials(CredentialTypeEnum::kPin);
	InitializeCredentials(CredentialTypeEnum::kRfid);
	InitializeCredentials(CredentialTypeEnum::kFingerprint);
}

bool BoltLockManager::GetUser(uint16_t userIndex, EmberAfPluginDoorLockUserInfo &user) const
{
	/* userIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_NUM_USERS */
	user = mUsers[userIndex - 1];

	ChipLogProgress(Zcl, "Getting lock user %u: %s", static_cast<unsigned>(userIndex),
			user.userStatus == UserStatusEnum::kAvailable ? "available" : "occupied");

	return true;
}

bool BoltLockManager::SetUser(uint16_t userIndex, FabricIndex creator, FabricIndex modifier, const CharSpan &userName,
			      uint32_t uniqueId, UserStatusEnum userStatus, UserTypeEnum userType,
			      CredentialRuleEnum credentialRule, const CredentialStruct *credentials,
			      size_t totalCredentials)
{
	/* userIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_NUM_USERS */
	UserData &userData = mUserData[userIndex - 1];
	auto &user = mUsers[userIndex - 1];

	VerifyOrReturnError(userName.size() <= DOOR_LOCK_MAX_USER_NAME_SIZE, false);
	VerifyOrReturnError(totalCredentials <= CONFIG_LOCK_TOTAL_NUM_CREDENTIALS_PER_USER, false);

	Platform::CopyString(userData.mName, userName);

	size_t pinIdx{ 0 };
	size_t rfidIdx{ 0 };
	size_t fpIdx{ 0 };
	for (size_t i = 0; i < totalCredentials; ++i) {
		auto *currentCredentials = credentials + i;
		CredentialTypeEnum credentialType = static_cast<CredentialTypeEnum>(currentCredentials->credentialType);
		switch (credentialType) {
		case CredentialTypeEnum::kPin:
			if (pinIdx >= CONFIG_LOCK_NUM_CREDENTIALS_PER_USER) {
				return false;
			}
			memcpy(&userData.mCredentials.mPin[pinIdx], currentCredentials, sizeof(CredentialStruct));
			// Ideally this could just reference the address instead of copying the data, but we need a
			// sequential block of CredentialStruct to be compatible with chip::Span API. The same applies
			// in below cases.
			userData.mOccupiedCredentials[i] = userData.mCredentials.mPin[pinIdx];
			pinIdx++;
			break;
		case CredentialTypeEnum::kRfid:
			if (rfidIdx >= CONFIG_LOCK_NUM_CREDENTIALS_PER_USER) {
				return false;
			}
			memcpy(&userData.mCredentials.mRfid[rfidIdx], currentCredentials, sizeof(CredentialStruct));
			userData.mOccupiedCredentials[i] = userData.mCredentials.mRfid[rfidIdx];
			rfidIdx++;
			break;
		case CredentialTypeEnum::kFingerprint:
		case CredentialTypeEnum::kFingerVein:
			if (fpIdx >= CONFIG_LOCK_NUM_CREDENTIALS_PER_USER) {
				return false;
			}
			memcpy(&userData.mCredentials.mFingerPrint[fpIdx], currentCredentials,
			       sizeof(CredentialStruct));
			userData.mOccupiedCredentials[i] = userData.mCredentials.mFingerPrint[fpIdx];
			fpIdx++;
			break;
		default:
			return false;
		}
	}

	user.userName = CharSpan(userData.mName, userName.size());
	user.credentials = Span<const CredentialStruct>(userData.mOccupiedCredentials, totalCredentials);
	user.userUniqueId = uniqueId;
	user.userStatus = userStatus;
	user.userType = userType;
	user.credentialRule = credentialRule;
	user.creationSource = DlAssetSource::kMatterIM;
	user.createdBy = creator;
	user.modificationSource = DlAssetSource::kMatterIM;
	user.lastModifiedBy = modifier;

	ChipLogProgress(Zcl, "Setting lock user %u: %s", static_cast<unsigned>(userIndex),
			userStatus == UserStatusEnum::kAvailable ? "available" : "occupied");

	return true;
}

bool BoltLockManager::GetCredential(uint16_t credentialIndex, CredentialTypeEnum credentialType,
				    EmberAfPluginDoorLockCredentialInfo &credential) const
{
	VerifyOrReturnError(credentialIndex > 0 && credentialIndex <= CONFIG_LOCK_NUM_CREDENTIALS_TYPE, false);

	auto *stored = GetCredentialsOfType(credentialType, credentialIndex);
	credential = *stored;

	ChipLogProgress(Zcl, "Getting lock credential %u: %s", static_cast<unsigned>(credentialIndex),
			credential.status == DlCredentialStatus::kAvailable ? "available" : "occupied");

	return true;
}

bool BoltLockManager::SetCredential(uint16_t credentialIndex, FabricIndex creator, FabricIndex modifier,
				    DlCredentialStatus credentialStatus, CredentialTypeEnum credentialType,
				    const ByteSpan &secret)
{
	VerifyOrReturnError(credentialIndex > 0 && credentialIndex <= CONFIG_LOCK_NUM_CREDENTIALS_TYPE, false);
	VerifyOrReturnError(secret.size() <= kMaxCredentialLength, false);

	auto *credentialData = GetCredentialsDataOfType(credentialType, credentialIndex);
	VerifyOrReturnError(credentialData, false);

	if (!secret.empty()) {
		memcpy(credentialData->mSecret.Alloc(secret.size()).Get(), secret.data(), secret.size());
	}

	EmberAfPluginDoorLockCredentialInfo *credentials = GetCredentialsOfType(credentialType, credentialIndex);
	VerifyOrReturnError(credentials, false);
	credentials->status = credentialStatus;
	credentials->credentialType = credentialType;
	credentials->credentialData = ByteSpan(credentialData->mSecret.Get(), secret.size());
	credentials->creationSource = DlAssetSource::kMatterIM;
	credentials->createdBy = creator;
	credentials->modificationSource = DlAssetSource::kMatterIM;
	credentials->lastModifiedBy = modifier;

	ChipLogProgress(Zcl, "Setting lock credential %u: %s", static_cast<unsigned>(credentialIndex),
			credentials->status == DlCredentialStatus::kAvailable ? "available" : "occupied");

	return true;
}

EmberAfPluginDoorLockCredentialInfo *BoltLockManager::GetCredentialsOfType(CredentialTypeEnum type,
									   uint16_t credentialIndex) const
{
	const EmberAfPluginDoorLockCredentialInfo *credentials{ nullptr };
	switch (type) {
	case CredentialTypeEnum::kPin:
		credentials = &mCredentialsPIN[credentialIndex - 1];
		break;
	case CredentialTypeEnum::kRfid:
		credentials = &mCredentialsRFID[credentialIndex - 1];
		break;
	case CredentialTypeEnum::kFingerprint:
	case CredentialTypeEnum::kFingerVein:
		credentials = &mCredentialsFinger[credentialIndex - 1];
		break;
	default:
		break;
	}
	return const_cast<EmberAfPluginDoorLockCredentialInfo *>(credentials);
}

BoltLockManager::CredentialData *BoltLockManager::GetCredentialsDataOfType(CredentialTypeEnum type,
									   uint16_t credentialIndex) const
{
	const CredentialData *credentialsData{ nullptr };
	switch (type) {
	case CredentialTypeEnum::kPin:
		credentialsData = &mSecrets.mPinData[credentialIndex - 1];
		break;
	case CredentialTypeEnum::kRfid:
		credentialsData = &mSecrets.mRfidData[credentialIndex - 1];
		break;
	case CredentialTypeEnum::kFingerprint:
	case CredentialTypeEnum::kFingerVein:
		credentialsData = &mSecrets.mFingerPrintData[credentialIndex - 1];
		break;
	default:
		break;
	}
	return const_cast<CredentialData *>(credentialsData);
}

bool BoltLockManager::ValidatePIN(const Optional<ByteSpan> &pinCode, OperationErrorEnum &err) const
{
	/* Optionality of the PIN code is validated by the caller, so assume it is OK not to provide the PIN code. */
	if (!pinCode.HasValue()) {
		return true;
	}

	/* Check the PIN code */
	for (size_t index = 0; index < CONFIG_LOCK_NUM_CREDENTIALS_PER_USER; ++index) {
		auto *credentials = GetCredentialsOfType(CredentialTypeEnum::kPin, index);
		VerifyOrReturnError(credentials, false);

		if (credentials->status == DlCredentialStatus::kAvailable) {
			continue;
		}

		if (credentials->credentialData.data_equal(pinCode.Value())) {
			ChipLogDetail(Zcl, "Valid lock PIN code provided");
			return true;
		}
	}

	ChipLogDetail(Zcl, "Invalid lock PIN code provided");
	err = OperationErrorEnum::kInvalidCredential;

	return false;
}

void BoltLockManager::Lock(OperationSource source)
{
	VerifyOrReturn(mState != State::kLockingCompleted);
	SetState(State::kLockingInitiated, source);

	mActuatorOperationSource = source;
	k_timer_start(&mActuatorTimer, K_MSEC(kActuatorMovementTimeMs), K_NO_WAIT);
}

void BoltLockManager::Unlock(OperationSource source)
{
	VerifyOrReturn(mState != State::kUnlockingCompleted);
	SetState(State::kUnlockingInitiated, source);

	mActuatorOperationSource = source;
	k_timer_start(&mActuatorTimer, K_MSEC(kActuatorMovementTimeMs), K_NO_WAIT);
}

void BoltLockManager::ActuatorTimerEventHandler(k_timer *timer)
{
	/*
	 * The timer event handler is called in the context of the system clock ISR.
	 * Post an event to the application task queue to process the event in the
	 * context of the application thread.
	 */

	AppEvent event;
	event.Type = AppEventType::Timer;
	event.TimerEvent.Context = static_cast<BoltLockManager *>(k_timer_user_data_get(timer));
	event.Handler = BoltLockManager::ActuatorAppEventHandler;
	AppTask::Instance().PostEvent(event);
}

void BoltLockManager::ActuatorAppEventHandler(const AppEvent &event)
{
	BoltLockManager *lock = static_cast<BoltLockManager *>(event.TimerEvent.Context);

	if (!lock) {
		return;
	}

	switch (lock->mState) {
	case State::kLockingInitiated:
		lock->SetState(State::kLockingCompleted, lock->mActuatorOperationSource);
		break;
	case State::kUnlockingInitiated:
		lock->SetState(State::kUnlockingCompleted, lock->mActuatorOperationSource);
		break;
	default:
		break;
	}
}

void BoltLockManager::SetState(State state, OperationSource source)
{
	mState = state;

	if (mStateChangeCallback != nullptr) {
		mStateChangeCallback(state, source);
	}
}

void BoltLockManager::InitializeCredentials(CredentialTypeEnum credentialType)
{
	for (uint8_t idx = 1; idx <= CONFIG_LOCK_NUM_CREDENTIALS_TYPE; ++idx) {
		EmberAfPluginDoorLockCredentialInfo *credentials = GetCredentialsOfType(credentialType, idx);
		credentials->status = DlCredentialStatus::kAvailable;
		credentials->credentialType = credentialType;
		credentials->credentialData = ByteSpan();
		credentials->creationSource = DlAssetSource::kUnspecified;
		credentials->createdBy = 0;
		credentials->modificationSource = DlAssetSource::kUnspecified;
		credentials->lastModifiedBy = 0;
	}
}
