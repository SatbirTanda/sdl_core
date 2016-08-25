/*
 Copyright (c) 2016, Ford Motor Company
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following
 disclaimer in the documentation and/or other materials provided with the
 distribution.

 Neither the name of the Ford Motor Company nor the names of its contributors
 may be used to endorse or promote products derived from this software
 without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
 */
#include "policy/policy_manager_impl.h"

#include <algorithm>
#include <set>
#include <queue>
#include <iterator>
#include <limits>
#include "policy/policy_table.h"
#include "policy/pt_representation.h"
#include "policy/policy_helper.h"
#include "utils/file_system.h"
#include "utils/logger.h"
#include "utils/date_time.h"
#include "utils/json_utils.h"
#include "utils/make_shared.h"
#include "policy/cache_manager.h"
#include "policy/update_status_manager.h"
#include "config_profile/profile.h"
#include "utils/timer_task_impl.h"

policy::PolicyManager* CreateManager(const std::string& app_storage_folder,
                                     uint16_t attempts_to_open_policy_db,
                                     uint16_t open_attempt_timeout_ms
#ifdef ENABLE_LOG
                                     ,
                                     logger::Logger::Pimpl& logger
#endif
                                     ) {
  SET_SDL_LOGGER(logger);
  return new policy::PolicyManagerImpl(
      app_storage_folder, attempts_to_open_policy_db, open_attempt_timeout_ms);
}

namespace {
const uint32_t kDefaultRetryTimeoutInSec = 60u;
}  // namespace

namespace policy {

SDL_CREATE_LOGGER("Policy")

PolicyManagerImpl::PolicyManagerImpl(const std::string& app_storage_folder,
                                     uint16_t attempts_to_open_policy_db,
                                     uint16_t open_attempt_timeout_ms)
    : PolicyManager()
    , listener_(NULL)
    , cache_(new CacheManager(app_storage_folder,
                              attempts_to_open_policy_db,
                              open_attempt_timeout_ms))
    , retry_sequence_timeout_(kDefaultRetryTimeoutInSec)
    , retry_sequence_index_(0)
    , timer_retry_sequence_("Retry sequence timer",
                            new timer::TimerTaskImpl<PolicyManagerImpl>(
                                this, &PolicyManagerImpl::RetrySequence))
    , ignition_check(true) {}

void PolicyManagerImpl::set_listener(PolicyListener* listener) {
  listener_ = listener;
  update_status_manager_.set_listener(listener);
}

#ifdef USE_HMI_PTU_DECRYPTION
utils::SharedPtr<policy_table::Table> PolicyManagerImpl::Parse(
    const BinaryMessage& pt_content) {
  using namespace utils::json;
  std::string json(pt_content.begin(), pt_content.end());
  JsonValue::ParseResult parse_result = JsonValue::Parse(json);
  if (!parse_result.second) {
    return utils::SharedPtr<policy_table::Table>();
  }
  JsonValue& root_json = parse_result.first;
  return utils::MakeShared<policy_table::Table>(root_json);
}
#else
utils::SharedPtr<policy_table::Table> PolicyManagerImpl::ParseArray(
    const BinaryMessage& pt_content) {
  using namespace utils::json;
  std::string json(pt_content.begin(), pt_content.end());
  JsonValue::ParseResult parse_result = JsonValue::Parse(json);
  if (parse_result.second) {
    // For PT Update received from SDL Server.
    if (parse_result.first["data"].Size() != 0) {
      JsonValue data = parse_result.first["data"];
      return new policy_table::Table(data[0u]);
    } else {
      return new policy_table::Table(parse_result.first);
    }
  } else {
    return utils::SharedPtr<policy_table::Table>();
  }
  const JsonValue& root_json = parse_result.first;
  // For PT Update received from SDL Server.
  if (root_json.HasMember("data") && root_json["data"].Size()) {
    const JsonValueRef data = root_json["data"];
    // First Element in
    return utils::MakeShared<policy_table::Table>(data[0u]);
  }
  return utils::MakeShared<policy_table::Table>(root_json);
}
#endif

void PolicyManagerImpl::CheckTriggers() {
  SDL_AUTO_TRACE();
  const bool exceed_ignition_cycles = ExceededIgnitionCycles();
  const bool exceed_days = ExceededDays();

  SDL_DEBUG("\nDays exceeded: " << std::boolalpha << exceed_days
                                << "\nIgnition cycles exceeded: "
                                << std::boolalpha << exceed_ignition_cycles);

  if (exceed_ignition_cycles || exceed_days) {
    update_status_manager_.ScheduleUpdate();
  }
}

bool PolicyManagerImpl::LoadPT(const std::string& file,
                               const BinaryMessage& pt_content) {
  SDL_INFO("LoadPT of size " << pt_content.size());

#ifdef USE_HMI_PTU_DECRYPTION
  // Assuemes Policy Table was parsed, formatted, and/or decrypted by
  // the HMI after system request before calling OnReceivedPolicyUpdate
  // Parse message into table struct
  utils::SharedPtr<policy_table::Table> pt_update = Parse(pt_content);
#else
  // Message Received from server unecnrypted with PTU in first element
  // of 'data' array. No Parsing was done by HMI.
  utils::SharedPtr<policy_table::Table> pt_update = ParseArray(pt_content);
#endif
  if (!pt_update) {
    SDL_WARN("Parsed table pointer is 0.");
    update_status_manager_.OnWrongUpdateReceived();
    return false;
  }

  file_system::DeleteFile(file);

  if (!IsPTValid(pt_update, policy_table::PT_UPDATE)) {
    update_status_manager_.OnWrongUpdateReceived();
    return false;
  }

  update_status_manager_.OnValidUpdateReceived();
  cache_->SaveUpdateRequired(false);

  // Update finished, no need retry
  if (timer_retry_sequence_.is_running()) {
    SDL_INFO("Stop retry sequence");
    timer_retry_sequence_.Stop();
  }

  {
    sync_primitives::AutoLock lock(apps_registration_lock_);

    // Get current DB data, since it could be updated during awaiting of PTU
    utils::SharedPtr<policy_table::Table> policy_table_snapshot =
        cache_->GenerateSnapshot();
    if (!policy_table_snapshot) {
      SDL_ERROR("Failed to create snapshot of policy table");
      return false;
    }

    // Checking of difference between PTU and current policy state
    // Must to be done before PTU applying since it is possible, that functional
    // groups, which had been present before are absent in PTU and will be
    // removed after update. So in case of revoked groups system has to know
    // names and ids of revoked groups before they will be removed.
    CheckPermissionsChanges(pt_update, policy_table_snapshot);

    // Replace current data with updated
    if (!cache_->ApplyUpdate(*pt_update)) {
      SDL_WARN("Unsuccessful save of updated policy table.");
      return false;
    }

    if (pt_update->policy_table.module_config.certificate.is_initialized()) {
      listener_->OnCertificateUpdated(
          *(pt_update->policy_table.module_config.certificate));
    }

    std::map<std::string, StringArray> app_hmi_types;
    cache_->GetHMIAppTypeAfterUpdate(app_hmi_types);
    if (!app_hmi_types.empty()) {
      SDL_INFO("app_hmi_types is full calling OnUpdateHMIAppType");
      listener_->OnUpdateHMIAppType(app_hmi_types);
    } else {
      SDL_INFO("app_hmi_types empty" << pt_content.size());
    }
  }

  // If there was a user request for policy table update, it should be started
  // right after current update is finished
  if (update_status_manager_.IsUpdateRequired()) {
    StartPTExchange();
    return true;
  }

  RefreshRetrySequence();
  return true;
}

void PolicyManagerImpl::CheckPermissionsChanges(
    const utils::SharedPtr<policy_table::Table> pt_update,
    const utils::SharedPtr<policy_table::Table> snapshot) {
  SDL_INFO("Checking incoming permissions.");

  // Replace predefined policies with its actual setting, e.g. "123":"default"
  // to actual values of default section
  UnwrapAppPolicies(pt_update->policy_table.app_policies_section.apps);

  std::for_each(pt_update->policy_table.app_policies_section.apps.begin(),
                pt_update->policy_table.app_policies_section.apps.end(),
                CheckAppPolicy(this, pt_update, snapshot));
}

void PolicyManagerImpl::PrepareNotificationData(
    const policy_table::FunctionalGroupings& groups,
    const policy_table::Strings& group_names,
    const std::vector<FunctionalGroupPermission>& group_permission,
    Permissions& notification_data) {
  SDL_INFO("Preparing data for notification.");
  ProcessFunctionalGroup processor(groups, group_permission, notification_data);
  std::for_each(group_names.begin(), group_names.end(), processor);
}

void PolicyManagerImpl::GetServiceUrls(const std::string& service_type,
                                       EndpointUrls& end_points) {
  SDL_AUTO_TRACE();
  cache_->GetServiceUrls(service_type, end_points);
}

bool PolicyManagerImpl::RequestPTUpdate() {
  SDL_AUTO_TRACE();
  utils::SharedPtr<policy_table::Table> policy_table_snapshot =
      cache_->GenerateSnapshot();
  if (!policy_table_snapshot) {
    SDL_ERROR("Failed to create snapshot of policy table");
    return false;
  }

  IsPTValid(policy_table_snapshot, policy_table::PT_SNAPSHOT);

  const std::string message_string =
      policy_table_snapshot->ToJsonValue().ToJson(false);

  SDL_DEBUG("Snapshot contents is : " << message_string);

  BinaryMessage update(message_string.begin(), message_string.end());

  listener_->OnSnapshotCreated(update);

  // Need to reset update schedule since all currenly registered applications
  // were already added to the snapshot so no update for them required.
  update_status_manager_.ResetUpdateSchedule();

  return true;
}

std::string PolicyManagerImpl::GetLockScreenIconUrl() const {
  return cache_->GetLockScreenIconUrl();
}

void PolicyManagerImpl::StartPTExchange() {
  SDL_AUTO_TRACE();

  const bool update_required = update_status_manager_.IsUpdateRequired();

  if (update_status_manager_.IsAppsSearchInProgress() && update_required) {
    update_status_manager_.ScheduleUpdate();
    SDL_INFO(
        "Starting exchange skipped, since applications "
        "search is in progress.");
    return;
  }

  if (update_status_manager_.IsUpdatePending() && update_required) {
    update_status_manager_.ScheduleUpdate();
    SDL_INFO(
        "Starting exchange skipped, since another exchange "
        "is in progress.");
    return;
  }

  if (listener_ && listener_->CanUpdate()) {
    if (ignition_check) {
      CheckTriggers();
      ignition_check = false;
    }

    if (update_status_manager_.IsUpdateRequired()) {
      if (RequestPTUpdate() && !timer_retry_sequence_.is_running()) {
        // Start retry sequency
        timer_retry_sequence_.Start(NextRetryTimeout(), timer::kPeriodic);
      }
    }
  }
}

void PolicyManagerImpl::OnAppsSearchStarted() {
  SDL_AUTO_TRACE();
  update_status_manager_.OnAppsSearchStarted();
}

void PolicyManagerImpl::OnAppsSearchCompleted() {
  SDL_AUTO_TRACE();
  update_status_manager_.OnAppsSearchCompleted();
  if (update_status_manager_.IsUpdateRequired()) {
    StartPTExchange();
  }
}

void PolicyManagerImpl::OnAppRegisteredOnMobile(
    const std::string& application_id) {
  StartPTExchange();
  SendNotificationOnPermissionsUpdated(application_id);
}

const std::vector<std::string> PolicyManagerImpl::GetAppRequestTypes(
    const std::string policy_app_id) const {
  std::vector<std::string> request_types;
  cache_->GetAppRequestTypes(policy_app_id, request_types);
  return request_types;
}

const VehicleInfo PolicyManagerImpl::GetVehicleInfo() const {
  return cache_->GetVehicleInfo();
}

void PolicyManagerImpl::CheckPermissions(const PTString& app_id,
                                         const PTString& hmi_level,
                                         const PTString& rpc,
                                         const RPCParams& rpc_params,
                                         CheckPermissionResult& result) {
  SDL_INFO("CheckPermissions for " << app_id << " and rpc " << rpc << " for "
                                   << hmi_level << " level.");

  cache_->CheckPermissions(app_id, hmi_level, rpc, result);
}

bool PolicyManagerImpl::ResetUserConsent() {
  bool result = true;

  return result;
}

void PolicyManagerImpl::SendNotificationOnPermissionsUpdated(
    const std::string& application_id) {
  SDL_AUTO_TRACE();
  const std::string device_id = GetCurrentDeviceId(application_id);
  if (device_id.empty()) {
    SDL_WARN(
        "Couldn't find device info for application id "
        "'"
        << application_id << "'");
    return;
  }

  std::vector<FunctionalGroupPermission> app_group_permissions;
  GetPermissionsForApp(device_id, application_id, app_group_permissions);

  policy_table::FunctionalGroupings functional_groupings;
  cache_->GetFunctionalGroupings(functional_groupings);

  policy_table::Strings app_groups;
  std::vector<FunctionalGroupPermission>::const_iterator it =
      app_group_permissions.begin();
  std::vector<FunctionalGroupPermission>::const_iterator it_end =
      app_group_permissions.end();
  for (; it != it_end; ++it) {
    app_groups.push_back((*it).group_name);
  }

  Permissions notification_data;
  PrepareNotificationData(functional_groupings,
                          app_groups,
                          app_group_permissions,
                          notification_data);

  SDL_INFO("Send notification for application_id:" << application_id);

  std::string default_hmi;
  default_hmi = "NONE";

  listener()->OnPermissionsUpdated(
      application_id, notification_data, default_hmi);
}

bool PolicyManagerImpl::CleanupUnpairedDevices() {
  SDL_AUTO_TRACE();
  // For SDL-specific it doesn't matter
  return true;
}

DeviceConsent PolicyManagerImpl::GetUserConsentForDevice(
    const std::string& device_id) const {
  SDL_AUTO_TRACE();
  return kDeviceAllowed;
}

void PolicyManagerImpl::SetUserConsentForDevice(const std::string& device_id,
                                                bool is_allowed) {
  SDL_AUTO_TRACE();
  SDL_DEBUG("Device :" << device_id);
  DeviceConsent current_consent = GetUserConsentForDevice(device_id);
  bool is_current_device_allowed =
      DeviceConsent::kDeviceAllowed == current_consent ? true : false;
  if (DeviceConsent::kDeviceHasNoConsent != current_consent &&
      is_current_device_allowed == is_allowed) {
    const std::string consent = is_allowed ? "allowed" : "disallowed";
    SDL_INFO("Device is already " << consent << ".");
    return;
  }
}

bool PolicyManagerImpl::ReactOnUserDevConsentForApp(const std::string app_id,
                                                    bool is_device_allowed) {
  return true;
}

bool PolicyManagerImpl::GetInitialAppData(const std::string& application_id,
                                          StringArray* nicknames,
                                          StringArray* app_hmi_types) {
  SDL_AUTO_TRACE();
  const bool result = nicknames && app_hmi_types;
  if (result) {
    cache_->GetInitialAppData(application_id, *nicknames, *app_hmi_types);
  }
  return result;
}

void PolicyManagerImpl::AddDevice(const std::string& device_id,
                                  const std::string& connection_type) {
  SDL_INFO("SetDeviceInfo");
  SDL_DEBUG("Device :" << device_id);
}

void PolicyManagerImpl::SetDeviceInfo(const std::string& device_id,
                                      const DeviceInfo& device_info) {
  SDL_AUTO_TRACE();
  SDL_DEBUG("Device :" << device_id);
}

PermissionConsent PolicyManagerImpl::EnsureCorrectPermissionConsent(
    const PermissionConsent& permissions_to_check) {
  std::vector<FunctionalGroupPermission> current_user_consents;
  GetUserConsentForApp(permissions_to_check.device_id,
                       permissions_to_check.policy_app_id,
                       current_user_consents);

  PermissionConsent permissions_to_set;
  permissions_to_set.device_id = permissions_to_check.device_id;
  permissions_to_set.policy_app_id = permissions_to_check.policy_app_id;
  permissions_to_set.consent_source = permissions_to_check.consent_source;

  std::vector<FunctionalGroupPermission>::const_iterator it =
      permissions_to_check.group_permissions.begin();
  std::vector<FunctionalGroupPermission>::const_iterator it_end =
      permissions_to_check.group_permissions.end();

  for (; it != it_end; ++it) {
    std::vector<FunctionalGroupPermission>::const_iterator it_curr =
        current_user_consents.begin();
    std::vector<FunctionalGroupPermission>::const_iterator it_curr_end =
        current_user_consents.end();

    for (; it_curr != it_curr_end; ++it_curr) {
      if (it->group_alias == it_curr->group_alias &&
          it->group_id == it_curr->group_id) {
        permissions_to_set.group_permissions.push_back(*it);
      }
    }
  }

  return permissions_to_set;
}

void PolicyManagerImpl::CheckPendingPermissionsChanges(
    const std::string& policy_app_id,
    const std::vector<FunctionalGroupPermission>& current_permissions) {
  SDL_AUTO_TRACE();
  sync_primitives::AutoLock lock(app_permissions_diff_lock_);
  std::map<std::string, AppPermissions>::iterator it_pending =
      app_permissions_diff_.find(policy_app_id);
  if (app_permissions_diff_.end() == it_pending) {
    SDL_WARN(
        "No pending permissions had been found for appID: " << policy_app_id);
    return;
  }

  SDL_DEBUG("Pending permissions had been found for appID: " << policy_app_id);

  // Change appPermissionsConsentNeeded depending on unconsented groups
  // presence
  std::vector<policy::FunctionalGroupPermission>::const_iterator it_groups =
      current_permissions.begin();
  std::vector<policy::FunctionalGroupPermission>::const_iterator it_end_groups =
      current_permissions.end();

  for (; it_groups != it_end_groups; ++it_groups) {
    if (policy::kGroupUndefined == it_groups->state) {
      SDL_DEBUG(
          "Unconsented groups still present for appID: " << policy_app_id);
      it_pending->second.appPermissionsConsentNeeded = true;
      return;
    }
  }

  SDL_DEBUG(
      "Unconsented groups not present anymore for appID: " << policy_app_id);
  it_pending->second.appPermissionsConsentNeeded = false;
  return;
}

void PolicyManagerImpl::SetUserConsentForApp(
    const PermissionConsent& permissions) {
  SDL_AUTO_TRACE();
}

bool PolicyManagerImpl::GetDefaultHmi(const std::string& policy_app_id,
                                      std::string* default_hmi) const {
  SDL_AUTO_TRACE();
  const std::string device_id = GetCurrentDeviceId(policy_app_id);
  DeviceConsent device_consent = GetUserConsentForDevice(device_id);
  const std::string app_id = policy::kDeviceAllowed != device_consent
                                 ? kPreDataConsentId
                                 : policy_app_id;
  return cache_->GetDefaultHMI(app_id, *default_hmi);
}

bool PolicyManagerImpl::GetPriority(const std::string& policy_app_id,
                                    std::string* priority) const {
  SDL_AUTO_TRACE();
  if (!priority) {
    SDL_WARN("Input priority parameter is null.");
    return false;
  }

  return cache_->GetPriority(policy_app_id, *priority);
}

std::vector<UserFriendlyMessage> PolicyManagerImpl::GetUserFriendlyMessages(
    const std::vector<std::string>& message_code, const std::string& language) {
  return cache_->GetUserFriendlyMsg(message_code, language);
}

void PolicyManagerImpl::GetUserConsentForApp(
    const std::string& device_id,
    const std::string& policy_app_id,
    std::vector<FunctionalGroupPermission>& permissions) {
  SDL_AUTO_TRACE();

  FunctionalIdType group_types;
  if (!cache_->GetPermissionsForApp(device_id, policy_app_id, group_types)) {
    SDL_WARN("Can't get user permissions for app " << policy_app_id);
    return;
  }

  // Functional groups w/o alias ("user_consent_prompt") considered as
  // automatically allowed and it could not be changed by user
  FunctionalGroupNames group_names;
  if (!cache_->GetFunctionalGroupNames(group_names)) {
    SDL_WARN("Can't get functional group names");
    return;
  }

  FunctionalGroupNames::const_iterator it = group_names.begin();
  FunctionalGroupNames::const_iterator it_end = group_names.end();
  FunctionalGroupIDs auto_allowed_groups;
  for (; it != it_end; ++it) {
    if (it->second.first.empty()) {
      auto_allowed_groups.push_back(it->first);
    }
  }

  // For basic policy
  FunctionalGroupIDs all_groups = group_types[kTypeGeneral];
  FunctionalGroupIDs default_groups = group_types[kTypeDefault];
  FunctionalGroupIDs predataconsented_groups =
      group_types[kTypePreDataConsented];

  FunctionalGroupIDs allowed_groups;
  FunctionalGroupIDs no_auto = ExcludeSame(all_groups, auto_allowed_groups);

  if (cache_->IsDefaultPolicy(policy_app_id)) {
    allowed_groups = ExcludeSame(no_auto, default_groups);
  } else if (cache_->IsPredataPolicy(policy_app_id)) {
    allowed_groups = ExcludeSame(no_auto, predataconsented_groups);
  }
  FillFunctionalGroupPermissions(
      allowed_groups, group_names, kGroupAllowed, permissions);
}

void PolicyManagerImpl::GetPermissionsForApp(
    const std::string& device_id,
    const std::string& policy_app_id,
    std::vector<FunctionalGroupPermission>& permissions) {
  SDL_AUTO_TRACE();
  std::string app_id_to_check = policy_app_id;

  bool allowed_by_default = false;
  if (cache_->IsDefaultPolicy(policy_app_id)) {
    app_id_to_check = kDefaultId;
    allowed_by_default = true;
  } else if (cache_->IsPredataPolicy(policy_app_id) ||
             policy::kDeviceDisallowed == GetUserConsentForDevice(device_id)) {
    app_id_to_check = kPreDataConsentId;
    allowed_by_default = true;
  }

  FunctionalIdType group_types;
  if (!cache_->GetPermissionsForApp(device_id, app_id_to_check, group_types)) {
    SDL_WARN("Can't get user permissions for app " << policy_app_id);
    return;
  }

  // Functional groups w/o alias ("user_consent_prompt") considered as
  // automatically allowed and it could not be changed by user
  FunctionalGroupNames group_names;
  if (!cache_->GetFunctionalGroupNames(group_names)) {
    SDL_WARN("Can't get functional group names");
    return;
  }

  // The "default" and "pre_DataConsent" are auto-allowed groups
  // So, check if application in the one of these mode.
  if (allowed_by_default) {
    SDL_INFO("Get auto allowed groups");
    GroupType type =
        (kDefaultId == app_id_to_check ? kTypeDefault : kTypePreDataConsented);

    FillFunctionalGroupPermissions(
        group_types[type], group_names, kGroupAllowed, permissions);
  } else {
    // The code bellow allows to process application which
    // has specific permissions(not default and pre_DataConsent).

    // All groups for specific application
    FunctionalGroupIDs all_groups = group_types[kTypeGeneral];

    // In case of GENIVI all groups are allowed
    FunctionalGroupIDs common_allowed = all_groups;
    FillFunctionalGroupPermissions(
        common_allowed, group_names, kGroupAllowed, permissions);
  }
  return;
}

std::string& PolicyManagerImpl::GetCurrentDeviceId(
    const std::string& policy_app_id) const {
  SDL_INFO("GetDeviceInfo");
  last_device_id_ = listener()->OnCurrentDeviceIdUpdateRequired(policy_app_id);
  return last_device_id_;
}

void PolicyManagerImpl::SetSystemLanguage(const std::string& language) {}

void PolicyManagerImpl::SetSystemInfo(const std::string& ccpu_version,
                                      const std::string& wers_country_code,
                                      const std::string& language) {
  SDL_AUTO_TRACE();
}

void PolicyManagerImpl::OnSystemReady() {
  // Update policy table for the first time with system information
  if (cache_->IsPTPreloaded()) {
    listener()->OnSystemInfoUpdateRequired();
    return;
  }
}

uint32_t PolicyManagerImpl::GetNotificationsNumber(
    const std::string& priority) const {
  SDL_AUTO_TRACE();
  return cache_->GetNotificationsNumber(priority);
}

bool PolicyManagerImpl::ExceededIgnitionCycles() {
  return 0 == cache_->IgnitionCyclesBeforeExchange();
}

bool PolicyManagerImpl::IsPTValid(
    utils::SharedPtr<policy_table::Table> policy_table,
    policy_table::PolicyTableType type) const {
  policy_table->SetPolicyTableType(type);
  if (!policy_table->is_valid()) {
    SDL_ERROR("Policy table is not valid.");
    rpc::ValidationReport report("policy_table");
    policy_table->ReportErrors(&report);
    SDL_DEBUG("Errors: " << rpc::PrettyFormat(report));
    return false;
  }
  return true;
}

const PolicySettings& PolicyManagerImpl::get_settings() const {
  DCHECK(settings_);
  return *settings_;
}

bool PolicyManagerImpl::ExceededDays() {
  SDL_AUTO_TRACE();

  TimevalStruct current_time = date_time::DateTime::getCurrentTime();
  const int kSecondsInDay = 60 * 60 * 24;
  const int days = current_time.tv_sec / kSecondsInDay;

  return 0 == cache_->DaysBeforeExchange(days);
}

void PolicyManagerImpl::KmsChanged(int kilometers) {
  SDL_AUTO_TRACE();
  if (0 == cache_->KilometersBeforeExchange(kilometers)) {
    SDL_INFO("Enough kilometers passed to send for PT update.");
    update_status_manager_.ScheduleUpdate();
    StartPTExchange();
  }
}

void PolicyManagerImpl::IncrementIgnitionCycles() {
  cache_->IncrementIgnitionCycles();
}

std::string PolicyManagerImpl::ForcePTExchange() {
  update_status_manager_.ScheduleUpdate();
  StartPTExchange();
  return update_status_manager_.StringifiedUpdateStatus();
}

std::string PolicyManagerImpl::GetPolicyTableStatus() const {
  return update_status_manager_.StringifiedUpdateStatus();
}

uint32_t PolicyManagerImpl::NextRetryTimeout() {
  sync_primitives::AutoLock auto_lock(retry_sequence_lock_);
  SDL_DEBUG("Index: " << retry_sequence_index_);
  uint32_t next = 0u;
  if (retry_sequence_seconds_.empty() ||
      retry_sequence_index_ >= retry_sequence_seconds_.size()) {
    return next;
  }

  ++retry_sequence_index_;

  for (uint32_t i = 0u; i < retry_sequence_index_; ++i) {
    next += retry_sequence_seconds_[i];
    // According to requirement APPLINK-18244
    next += retry_sequence_timeout_;
  }

  // Return miliseconds
  return next * date_time::kMillisecondsInSecond;
}

void PolicyManagerImpl::RefreshRetrySequence() {
  sync_primitives::AutoLock auto_lock(retry_sequence_lock_);
  retry_sequence_timeout_ = cache_->TimeoutResponse();
  retry_sequence_seconds_.clear();
  cache_->SecondsBetweenRetries(retry_sequence_seconds_);
}

void PolicyManagerImpl::ResetRetrySequence() {
  sync_primitives::AutoLock auto_lock(retry_sequence_lock_);
  retry_sequence_index_ = 0;
  update_status_manager_.OnResetRetrySequence();
}

int PolicyManagerImpl::TimeoutExchange() {
  return retry_sequence_timeout_;
}

const std::vector<int> PolicyManagerImpl::RetrySequenceDelaysSeconds() {
  sync_primitives::AutoLock auto_lock(retry_sequence_lock_);
  return retry_sequence_seconds_;
}

void PolicyManagerImpl::OnExceededTimeout() {
  update_status_manager_.OnUpdateTimeoutOccurs();
}

void PolicyManagerImpl::OnUpdateStarted() {
  int update_timeout = TimeoutExchange();
  SDL_DEBUG("Update timeout will be set to (sec): " << update_timeout);
  update_status_manager_.OnUpdateSentOut(update_timeout);
  cache_->SaveUpdateRequired(true);
}

void PolicyManagerImpl::PTUpdatedAt(Counters counter, int value) {
  SDL_AUTO_TRACE();
  cache_->SetCountersPassedForSuccessfulUpdate(counter, value);
  cache_->ResetIgnitionCycles();
}

void PolicyManagerImpl::Increment(usage_statistics::GlobalCounterId type) {
  SDL_INFO("Increment without app id");
  cache_->Increment(type);
}

void PolicyManagerImpl::Increment(const std::string& app_id,
                                  usage_statistics::AppCounterId type) {
  SDL_DEBUG("Increment " << app_id << " AppCounter: " << type);
  cache_->Increment(app_id, type);
}

void PolicyManagerImpl::Set(const std::string& app_id,
                            usage_statistics::AppInfoId type,
                            const std::string& value) {
  SDL_INFO("Set " << app_id);
  cache_->Set(app_id, type, value);
}

void PolicyManagerImpl::Add(const std::string& app_id,
                            usage_statistics::AppStopwatchId type,
                            int32_t timespan_seconds) {
  SDL_INFO("Add " << app_id);
  cache_->Add(app_id, type, timespan_seconds);
}

bool PolicyManagerImpl::IsApplicationRevoked(const std::string& app_id) const {
  return cache_->IsApplicationRevoked(app_id);
}

bool PolicyManagerImpl::IsConsentNeeded(const std::string& app_id) {
  SDL_AUTO_TRACE();
  return false;
}

void PolicyManagerImpl::SetVINValue(const std::string& value) {}

AppPermissions PolicyManagerImpl::GetAppPermissionsChanges(
    const std::string& policy_app_id) {
  typedef std::map<std::string, AppPermissions>::iterator PermissionsIt;
  PermissionsIt app_id_diff = app_permissions_diff_.find(policy_app_id);
  AppPermissions permissions(policy_app_id);
  if (app_permissions_diff_.end() != app_id_diff) {
    permissions = app_id_diff->second;
  } else {
    permissions.appPermissionsConsentNeeded = IsConsentNeeded(policy_app_id);
    permissions.appRevoked = IsApplicationRevoked(policy_app_id);
    GetPriority(permissions.application_id, &permissions.priority);
  }
  return permissions;
}

void PolicyManagerImpl::RemovePendingPermissionChanges(
    const std::string& app_id) {
  app_permissions_diff_.erase(app_id);
}

bool PolicyManagerImpl::CanAppKeepContext(const std::string& app_id) const {
  return cache_->CanAppKeepContext(app_id);
}

bool PolicyManagerImpl::CanAppStealFocus(const std::string& app_id) const {
  return cache_->CanAppStealFocus(app_id);
}

void PolicyManagerImpl::MarkUnpairedDevice(const std::string& device_id) {}

std::string PolicyManagerImpl::RetrieveCertificate() const {
  SDL_AUTO_TRACE();
  return cache_->GetCertificate();
}

void PolicyManagerImpl::AddApplication(const std::string& application_id) {
  SDL_AUTO_TRACE();
  const std::string device_id = GetCurrentDeviceId(application_id);
  DeviceConsent device_consent = GetUserConsentForDevice(device_id);
  sync_primitives::AutoLock lock(apps_registration_lock_);

  if (IsNewApplication(application_id)) {
    AddNewApplication(application_id, device_consent);
    update_status_manager_.OnNewApplicationAdded();
  } else {
    PromoteExistedApplication(application_id, device_consent);
  }
}

void PolicyManagerImpl::RemoveAppConsentForGroup(
    const std::string& app_id, const std::string& group_name) {
  cache_->RemoveAppConsentForGroup(app_id, group_name);
}

bool PolicyManagerImpl::IsPredataPolicy(const std::string& policy_app_id) {
  SDL_INFO("IsPredataApp");
  return cache_->IsPredataPolicy(policy_app_id);
}

void PolicyManagerImpl::AddNewApplication(const std::string& application_id,
                                          DeviceConsent device_consent) {
  SDL_AUTO_TRACE();

  cache_->SetDefaultPolicy(application_id);
}

void PolicyManagerImpl::PromoteExistedApplication(
    const std::string& application_id, DeviceConsent device_consent) {
  // If device consent changed to allowed during application being
  // disconnected, app permissions should be changed also
  if (kDeviceAllowed == device_consent &&
      cache_->IsPredataPolicy(application_id)) {
    cache_->SetDefaultPolicy(application_id);
  }
}

bool PolicyManagerImpl::IsNewApplication(
    const std::string& application_id) const {
  return false == cache_->IsApplicationRepresented(application_id);
}

bool PolicyManagerImpl::ResetPT(const std::string& file_name) {
  cache_->ResetCalculatedPermissions();
  const bool result = cache_->ResetPT(file_name);
  if (result) {
    RefreshRetrySequence();
  }
  return result;
}

bool PolicyManagerImpl::CheckAppStorageFolder() const {
  SDL_AUTO_TRACE();
  const std::string app_storage_folder = get_settings().app_storage_folder();
  SDL_DEBUG("AppStorageFolder " << app_storage_folder);
  if (!file_system::DirectoryExists(app_storage_folder)) {
    SDL_WARN("Storage directory doesn't exist " << app_storage_folder);
    return false;
  }
  if (!(file_system::IsWritingAllowed(app_storage_folder) &&
        file_system::IsReadingAllowed(app_storage_folder))) {
    SDL_WARN("Storage directory doesn't have read/write permissions "
             << app_storage_folder);
    return false;
  }
  return true;
}

bool PolicyManagerImpl::InitPT(const std::string& file_name,
                               const PolicySettings* settings) {
  SDL_AUTO_TRACE();
  settings_ = settings;
  if (!CheckAppStorageFolder()) {
    SDL_ERROR("Can not read/write into AppStorageFolder");
    return false;
  }
  const bool ret = cache_->Init(file_name, settings);
  if (ret) {
    RefreshRetrySequence();
    update_status_manager_.OnPolicyInit(cache_->UpdateRequired());
  }
  return ret;
}

uint32_t PolicyManagerImpl::HeartBeatTimeout(const std::string& app_id) const {
  return cache_->HeartBeatTimeout(app_id);
}

void PolicyManagerImpl::SaveUpdateStatusRequired(bool is_update_needed) {
  cache_->SaveUpdateRequired(is_update_needed);
}

void PolicyManagerImpl::set_cache_manager(
    CacheManagerInterface* cache_manager) {
  cache_ = cache_manager;
}

void PolicyManagerImpl::RetrySequence() {
  SDL_INFO("Start new retry sequence");
  RequestPTUpdate();

  uint32_t timeout = NextRetryTimeout();

  if (!timeout && timer_retry_sequence_.is_running()) {
    timer_retry_sequence_.Stop();
    return;
  }

  timer_retry_sequence_.Start(timeout, timer::kPeriodic);
}

}  //  namespace policy