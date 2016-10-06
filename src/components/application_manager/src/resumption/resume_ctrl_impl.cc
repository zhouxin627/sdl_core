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
#include "application_manager/resumption/resume_ctrl_impl.h"

#include <fstream>
#include <algorithm>
#include <functional>

#include "application_manager/application_manager.h"

#include "utils/file_system.h"
#include "connection_handler/connection_handler_impl.h"
#include "application_manager/message_helper.h"
#include "connection_handler/connection.h"
#include "application_manager/commands/command_impl.h"
#include "policy/policy_manager_impl.h"
#include "application_manager/policies/policy_handler.h"
#include "application_manager/state_controller.h"
#include "utils/helpers.h"
#include "application_manager/resumption/resumption_data_db.h"
#include "application_manager/resumption/resumption_data_json.h"
#include "utils/make_shared.h"
#include "utils/timer_task_impl.h"

namespace resumption {
using namespace application_manager;

SDL_CREATE_LOGGER("Resumption")

ResumeCtrlImpl::ResumeCtrlImpl(ApplicationManager& application_manager)
    : event_engine::EventObserver(application_manager.event_dispatcher())
    , queue_lock_(false)
    , restore_hmi_level_timer_(
          "RsmCtrlRstore",
          new timer::TimerTaskImpl<ResumeCtrlImpl>(
              this, &ResumeCtrlImpl::ApplicationResumptiOnTimer))
    , save_persistent_data_timer_("RsmCtrlPercist",
                                  new timer::TimerTaskImpl<ResumeCtrlImpl>(
                                      this, &ResumeCtrlImpl::SaveDataOnTimer))
    , is_resumption_active_(false)
    , is_data_saved_(false)
    , launch_time_(time(NULL))
    , application_manager_(application_manager) {}
#ifdef BUILD_TESTS
void ResumeCtrlImpl::set_resumption_storage(
    utils::SharedPtr<ResumptionData> mock_storage) {
  resumption_storage_ = mock_storage;
}
#endif  // BUILD_TESTS

bool ResumeCtrlImpl::Init(resumption::LastState& last_state) {
  bool use_db = application_manager_.get_settings().use_db_for_resumption();
  if (use_db) {
    resumption_storage_.reset(new ResumptionDataDB(
        In_File_Storage, application_manager_.get_settings()));
    if (!resumption_storage_->Init()) {
      return false;
    }

    ResumptionDataDB* db =
        dynamic_cast<ResumptionDataDB*>(resumption_storage_.get());

    if (!db->IsDBVersionActual()) {
      SDL_INFO(
          "DB version had been changed. "
          "Rebuilding resumption DB.");

      smart_objects::SmartObject data;
      db->GetAllData(data);

      if (!db->RefreshDB()) {
        return false;
      }

      db->SaveAllData(data);
      db->UpdateDBVersion();
    }
  } else {
    resumption_storage_.reset(new ResumptionDataJson(
        last_state, application_manager_.get_settings()));
    if (!resumption_storage_->Init()) {
      SDL_DEBUG("Resumption storage initialisation failed");
      return false;
    }
  }
  LoadResumeData();
  save_persistent_data_timer_.Start(
      application_manager_.get_settings()
          .app_resumption_save_persistent_data_timeout(),
      timer::kPeriodic);
  return true;
}

ResumeCtrlImpl::~ResumeCtrlImpl() {}

void ResumeCtrlImpl::SaveAllApplications() {
  DataAccessor<ApplicationSet> accessor(application_manager_.applications());
  std::for_each(
      accessor.GetData().begin(),
      accessor.GetData().end(),
      std::bind1st(std::mem_fun(&ResumeCtrlImpl::SaveApplication), this));
}

void ResumeCtrlImpl::SaveApplication(ApplicationSharedPtr application) {
  SDL_AUTO_TRACE();

  DCHECK_OR_RETURN_VOID(application);
  SDL_INFO("application with appID " << application->app_id()
                                     << " will be saved");
  resumption_storage_->SaveApplication(application);
}

void ResumeCtrlImpl::on_event(const event_engine::Event& event) {
  SDL_DEBUG("Event received" << event.id());
}

bool ResumeCtrlImpl::RestoreAppHMIState(ApplicationSharedPtr application) {
  using namespace mobile_apis;
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(application, false);
  SDL_DEBUG("app_id : " << application->app_id() << "; policy_app_id : "
                        << application->policy_app_id());
  const std::string& device_mac = application->mac_address();
  smart_objects::SmartObject saved_app(smart_objects::SmartType_Map);
  bool result = resumption_storage_->GetSavedApplication(
      application->policy_app_id(), device_mac, saved_app);
  if (result) {
    DCHECK_OR_RETURN(application, false);
    if (saved_app.keyExists(strings::hmi_level)) {
      const HMILevel::eType saved_hmi_level =
          static_cast<mobile_apis::HMILevel::eType>(
              saved_app[strings::hmi_level].asInt());
      SDL_DEBUG("Saved HMI Level is : " << saved_hmi_level);
      return SetAppHMIState(application, saved_hmi_level, true);
    } else {
      result = false;
      SDL_ERROR("saved app data corrupted");
    }
  } else {
    SDL_ERROR("Application not saved");
  }
  return result;
}

bool ResumeCtrlImpl::SetupDefaultHMILevel(ApplicationSharedPtr application) {
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(application, false);
  mobile_apis::HMILevel::eType default_hmi =
      application_manager_.GetDefaultHmiLevel(application);
  return SetAppHMIState(application, default_hmi, false);
}

void ResumeCtrlImpl::ApplicationResumptiOnTimer() {
  SDL_AUTO_TRACE();
  sync_primitives::AutoLock auto_lock(queue_lock_);
  WaitingForTimerList::iterator it = waiting_for_timer_.begin();

  for (; it != waiting_for_timer_.end(); ++it) {
    ApplicationSharedPtr app = application_manager_.application(*it);
    if (!app) {
      SDL_ERROR("Invalid app_id = " << *it);
      continue;
    }
    StartAppHmiStateResumption(app);
  }
  is_resumption_active_ = false;
  waiting_for_timer_.clear();
  StartSavePersistentDataTimer();
}

void ResumeCtrlImpl::OnAppActivated(ApplicationSharedPtr application) {
  if (is_resumption_active_) {
    RemoveFromResumption(application->app_id());
  }
}

void ResumeCtrlImpl::RemoveFromResumption(uint32_t app_id) {
  SDL_AUTO_TRACE();
  queue_lock_.Acquire();
  waiting_for_timer_.remove(app_id);
  queue_lock_.Release();
}

bool ResumeCtrlImpl::SetAppHMIState(
    ApplicationSharedPtr application,
    const mobile_apis::HMILevel::eType hmi_level,
    bool check_policy) {
  using namespace mobile_apis;
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(application, false);
  SDL_TRACE(" app_id : " << application->app_id() << ", hmi_level : "
                         << hmi_level << ", check_policy : " << check_policy);
  const std::string& device_mac = application->mac_address();
  if (check_policy &&
      application_manager_.GetUserConsentForDevice(device_mac) !=
          policy::DeviceConsent::kDeviceAllowed) {
    SDL_ERROR("Resumption abort. Data consent wasn't allowed.");
    SetupDefaultHMILevel(application);
    return false;
  }
  application->set_is_resuming(true);
  application_manager_.state_controller().SetRegularState(application,
                                                          hmi_level);
  SDL_INFO("Application with policy id " << application->policy_app_id()
                                         << " got HMI level " << hmi_level);
  return true;
}

bool ResumeCtrlImpl::IsHMIApplicationIdExist(uint32_t hmi_app_id) {
  SDL_DEBUG("hmi_app_id :" << hmi_app_id);
  return resumption_storage_->IsHMIApplicationIdExist(hmi_app_id);
}

bool ResumeCtrlImpl::IsApplicationSaved(const std::string& policy_app_id,
                                        const std::string& device_id) {
  return -1 !=
         resumption_storage_->IsApplicationSaved(policy_app_id, device_id);
}

uint32_t ResumeCtrlImpl::GetHMIApplicationID(
    const std::string& policy_app_id, const std::string& device_mac) const {
  return resumption_storage_->GetHMIApplicationID(policy_app_id, device_mac);
}

bool ResumeCtrlImpl::RemoveApplicationFromSaved(
    ApplicationConstSharedPtr application) {
  const std::string& device_mac = application->mac_address();
  return resumption_storage_->RemoveApplicationFromSaved(
      application->policy_app_id(), device_mac);
}

void ResumeCtrlImpl::OnSuspend() {
  SDL_AUTO_TRACE();
  StopSavePersistentDataTimer();
  SaveAllApplications();
  resumption_storage_->OnSuspend();
  resumption_storage_->Persist();
}

void ResumeCtrlImpl::OnAwake() {
  ResetLaunchTime();
  StartSavePersistentDataTimer();
  return resumption_storage_->OnAwake();
}

void ResumeCtrlImpl::StartSavePersistentDataTimer() {
  SDL_AUTO_TRACE();
  if (!save_persistent_data_timer_.is_running()) {
    save_persistent_data_timer_.Start(
        application_manager_.get_settings()
            .app_resumption_save_persistent_data_timeout(),
        timer::kPeriodic);
  }
}

void ResumeCtrlImpl::StopSavePersistentDataTimer() {
  SDL_AUTO_TRACE();
  if (save_persistent_data_timer_.is_running()) {
    save_persistent_data_timer_.Stop();
  }
}

bool ResumeCtrlImpl::StartResumption(ApplicationSharedPtr application,
                                     const std::string& hash) {
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(application, false);
  SDL_DEBUG(" Resume app_id = " << application->app_id() << " hmi_app_id = "
                                << application->hmi_app_id() << " policy_id = "
                                << application->policy_app_id()
                                << " received hash = " << hash);
  SetupDefaultHMILevel(application);
  smart_objects::SmartObject saved_app;
  const std::string& device_mac = application->mac_address();
  bool result = resumption_storage_->GetSavedApplication(
      application->policy_app_id(), device_mac, saved_app);
  if (result) {
    const std::string& saved_hash = saved_app[strings::hash_id].asString();
    result = saved_hash == hash ? RestoreApplicationData(application) : false;
    application->UpdateHash();
    AddToResumptionTimerQueue(application->app_id());
  }
  return result;
}

bool ResumeCtrlImpl::StartResumptionOnlyHMILevel(
    ApplicationSharedPtr application) {
  // sync_primitives::AutoLock lock(resumtion_lock_);
  SDL_AUTO_TRACE();
  if (!application) {
    SDL_WARN("Application does not exist.");
    return false;
  }
  SDL_DEBUG("HMI level resumption requested for application id "
            << application->app_id() << "with hmi_app_id "
            << application->hmi_app_id() << ", policy_app_id "
            << application->policy_app_id());
  SetupDefaultHMILevel(application);
  const std::string& device_mac = application->mac_address();
  smart_objects::SmartObject saved_app;
  bool result = resumption_storage_->GetSavedApplication(
      application->policy_app_id(), device_mac, saved_app);
  if (result) {
    // sync_primitives::AutoUnlock unlock(lock);
    AddToResumptionTimerQueue(application->app_id());
  }
  SDL_INFO("StartResumptionOnlyHMILevel::Result = " << result);
  return result;
}

void ResumeCtrlImpl::StartAppHmiStateResumption(
    ApplicationSharedPtr application) {
  using namespace date_time;
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN_VOID(application);
  smart_objects::SmartObject saved_app;
  const std::string& device_mac = application->mac_address();
  bool result = resumption_storage_->GetSavedApplication(
      application->policy_app_id(), device_mac, saved_app);
  DCHECK_OR_RETURN_VOID(result);
  const uint32_t ign_off_count = saved_app[strings::ign_off_count].asUInt();
  bool restore_data_allowed = false;
  restore_data_allowed =
      CheckAppRestrictions(application, saved_app) &&
      ((0 == ign_off_count) || CheckIgnCycleRestrictions(saved_app));
  if (restore_data_allowed) {
    SDL_INFO("Resume application " << application->policy_app_id());
    RestoreAppHMIState(application);
    RemoveApplicationFromSaved(application);
  } else {
    SDL_INFO("Do not need to resume application "
             << application->policy_app_id());
  }
}
void ResumeCtrlImpl::ResetLaunchTime() {
  SDL_AUTO_TRACE();
  launch_time_ = time(NULL);
}

bool ResumeCtrlImpl::CheckPersistenceFilesForResumption(
    ApplicationSharedPtr application) {
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(application, false);
  SDL_DEBUG(" Resume app_id = " << application->app_id() << " policy_id = "
                                << application->policy_app_id());
  smart_objects::SmartObject saved_app;
  const std::string& device_mac = application->mac_address();
  bool result = resumption_storage_->GetSavedApplication(
      application->policy_app_id(), device_mac, saved_app);
  if (result) {
    if (saved_app.keyExists(strings::application_commands)) {
      if (!CheckIcons(application, saved_app[strings::application_commands])) {
        return false;
      }
    }
    if (saved_app.keyExists(strings::application_choice_sets)) {
      if (!CheckIcons(application,
                      saved_app[strings::application_choice_sets])) {
        return false;
      }
    }
  }
  return true;
}

bool ResumeCtrlImpl::CheckApplicationHash(ApplicationSharedPtr application,
                                          const std::string& hash) {
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(application, false);
  SDL_DEBUG("app_id : " << application->app_id() << " hash : " << hash);
  smart_objects::SmartObject saved_app;
  const std::string& device_mac = application->mac_address();
  bool result = resumption_storage_->GetSavedApplication(
      application->policy_app_id(), device_mac, saved_app);
  return result ? saved_app[strings::hash_id].asString() == hash : false;
}

void ResumeCtrlImpl::SaveDataOnTimer() {
  SDL_AUTO_TRACE();
  if (is_resumption_active_) {
    SDL_WARN("Resumption timer is active skip saving");
    return;
  }

  if (false == is_data_saved_) {
    SaveAllApplications();
    is_data_saved_ = true;
    if (!application_manager_.get_settings().use_db_for_resumption()) {
      resumption_storage_->Persist();
    }
  }
}

bool ResumeCtrlImpl::IsDeviceMacAddressEqual(
    ApplicationSharedPtr application, const std::string& saved_device_mac) {
  SDL_AUTO_TRACE();
  const std::string device_mac = application->mac_address();
  return device_mac == saved_device_mac;
}

bool ResumeCtrlImpl::RestoreApplicationData(ApplicationSharedPtr application) {
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(application, false);
  SDL_DEBUG("app_id : " << application->app_id());

  smart_objects::SmartObject saved_app(smart_objects::SmartType_Map);
  const std::string& device_mac = application->mac_address();
  bool result = resumption_storage_->GetSavedApplication(
      application->policy_app_id(), device_mac, saved_app);
  if (result) {
    if (saved_app.keyExists(strings::grammar_id)) {
      const uint32_t app_grammar_id = saved_app[strings::grammar_id].asUInt();
      application->set_grammar_id(app_grammar_id);
      AddFiles(application, saved_app);
      AddSubmenues(application, saved_app);
      AddCommands(application, saved_app);
      AddChoicesets(application, saved_app);
      SetGlobalProperties(application, saved_app);
      AddSubscriptions(application, saved_app);
      AddWayPointsSubscription(application, saved_app);
      result = true;
    } else {
      SDL_WARN("Saved data of application does not contain grammar_id");
      result = false;
    }
  } else {
    SDL_WARN("Application not saved");
  }
  return result;
}

void ResumeCtrlImpl::AddFiles(ApplicationSharedPtr application,
                              const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();

  if (saved_app.keyExists(strings::application_files)) {
    const smart_objects::SmartObject& application_files =
        saved_app[strings::application_files];
    for (size_t i = 0; i < application_files.length(); ++i) {
      const smart_objects::SmartObject& file_data = application_files[i];
      const bool is_persistent =
          file_data.keyExists(strings::persistent_file) &&
          file_data[strings::persistent_file].asBool();
      if (is_persistent) {
        AppFile file;
        file.is_persistent = is_persistent;
        file.is_download_complete =
            file_data[strings::is_download_complete].asBool();
        file.file_name = file_data[strings::sync_file_name].asString();
        file.file_type = static_cast<mobile_apis::FileType::eType>(
            file_data[strings::file_type].asInt());
        application->AddFile(file);
      }
    }
  } else {
    SDL_FATAL("application_files section is not exists");
  }
}

void ResumeCtrlImpl::AddSubmenues(ApplicationSharedPtr application,
                                  const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();

  if (saved_app.keyExists(strings::application_submenus)) {
    const smart_objects::SmartObject& app_submenus =
        saved_app[strings::application_submenus];
    for (size_t i = 0; i < app_submenus.length(); ++i) {
      const smart_objects::SmartObject& submenu = app_submenus[i];
      application->AddSubMenu(submenu[strings::menu_id].asUInt(), submenu);
    }
    ProcessHMIRequests(MessageHelper::CreateAddSubMenuRequestToHMI(
        application, application_manager_.GetNextHMICorrelationID()));
  } else {
    SDL_FATAL("application_submenus section is not exists");
  }
}

void ResumeCtrlImpl::AddCommands(ApplicationSharedPtr application,
                                 const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();

  if (saved_app.keyExists(strings::application_commands)) {
    const smart_objects::SmartObject& app_commands =
        saved_app[strings::application_commands];
    for (size_t i = 0; i < app_commands.length(); ++i) {
      const smart_objects::SmartObject& command = app_commands[i];

      application->AddCommand(command[strings::cmd_id].asUInt(), command);
    }
    ProcessHMIRequests(MessageHelper::CreateAddCommandRequestToHMI(
        application, application_manager_));
  } else {
    SDL_FATAL("application_commands section is not exists");
  }
}

void ResumeCtrlImpl::AddChoicesets(
    ApplicationSharedPtr application,
    const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();

  if (saved_app.keyExists(strings::application_choice_sets)) {
    const smart_objects::SmartObject& app_choice_sets =
        saved_app[strings::application_choice_sets];
    for (size_t i = 0; i < app_choice_sets.length(); ++i) {
      const smart_objects::SmartObject& choice_set = app_choice_sets[i];
      const int32_t choice_set_id =
          choice_set[strings::interaction_choice_set_id].asInt();
      application->AddChoiceSet(choice_set_id, choice_set);
    }
    ProcessHMIRequests(MessageHelper::CreateAddVRCommandRequestFromChoiceToHMI(
        application, application_manager_));
  } else {
    SDL_FATAL("There is no any choicesets");
  }
}

void ResumeCtrlImpl::SetGlobalProperties(
    ApplicationSharedPtr application,
    const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();

  if (saved_app.keyExists(strings::application_global_properties)) {
    const smart_objects::SmartObject& properties_so =
        saved_app[strings::application_global_properties];
    application->load_global_properties(properties_so);
    MessageHelper::SendGlobalPropertiesToHMI(application, application_manager_);
  }
}

void ResumeCtrlImpl::AddWayPointsSubscription(
    app_mngr::ApplicationSharedPtr application,
    const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();

  if (saved_app.keyExists(strings::subscribed_for_way_points)) {
    const smart_objects::SmartObject& subscribed_for_way_points_so =
        saved_app[strings::subscribed_for_way_points];
    if (true == subscribed_for_way_points_so.asBool()) {
      application_manager_.SubscribeAppForWayPoints(application->app_id());
    }
  }
}

void ResumeCtrlImpl::AddSubscriptions(
    ApplicationSharedPtr application,
    const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();
  if (saved_app.keyExists(strings::application_subscribtions)) {
    const smart_objects::SmartObject& subscribtions =
        saved_app[strings::application_subscribtions];

    if (subscribtions.keyExists(strings::application_buttons)) {
      const smart_objects::SmartObject& subscribtions_buttons =
          subscribtions[strings::application_buttons];
      mobile_apis::ButtonName::eType btn;
      for (size_t i = 0; i < subscribtions_buttons.length(); ++i) {
        btn = static_cast<mobile_apis::ButtonName::eType>(
            (subscribtions_buttons[i]).asInt());
        application->SubscribeToButton(btn);
      }
    }
    MessageHelper::SendAllOnButtonSubscriptionNotificationsForApp(
        application, application_manager_);

    if (subscribtions.keyExists(strings::application_vehicle_info)) {
      const smart_objects::SmartObject& subscribtions_ivi =
          subscribtions[strings::application_vehicle_info];
      VehicleDataType ivi;
      for (size_t i = 0; i < subscribtions_ivi.length(); ++i) {
        ivi = static_cast<VehicleDataType>((subscribtions_ivi[i]).asInt());
        application->SubscribeToIVI(ivi);
      }
      ProcessHMIRequests(MessageHelper::GetIVISubscriptionRequests(
          application, application_manager_));
    }
  }
}

bool ResumeCtrlImpl::CheckIgnCycleRestrictions(
    const smart_objects::SmartObject& saved_app) {
  SDL_AUTO_TRACE();
  bool result = true;

  if (!CheckDelayAfterIgnOn()) {
    SDL_INFO("Application was connected long after ign on");
    result = false;
  }

  if (!DisconnectedJustBeforeIgnOff(saved_app)) {
    SDL_INFO("Application was dissconnected long before ign off");
    result = false;
  }
  return result;
}

bool ResumeCtrlImpl::DisconnectedJustBeforeIgnOff(
    const smart_objects::SmartObject& saved_app) {
  using namespace date_time;
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(saved_app.keyExists(strings::time_stamp), false);

  const time_t time_stamp =
      static_cast<time_t>(saved_app[strings::time_stamp].asUInt());
  time_t ign_off_time =
      static_cast<time_t>(resumption_storage_->GetIgnOffTime());
  const uint32_t sec_spent_before_ign = labs(ign_off_time - time_stamp);
  SDL_DEBUG(
      "ign_off_time "
      << ign_off_time << "; app_disconnect_time " << time_stamp
      << "; sec_spent_before_ign " << sec_spent_before_ign
      << "; resumption_delay_before_ign "
      << application_manager_.get_settings().resumption_delay_before_ign());
  return sec_spent_before_ign <=
         application_manager_.get_settings().resumption_delay_before_ign();
}

bool ResumeCtrlImpl::CheckAppRestrictions(
    ApplicationConstSharedPtr application,
    const smart_objects::SmartObject& saved_app) {
  using namespace mobile_apis;
  using namespace helpers;
  SDL_AUTO_TRACE();
  DCHECK_OR_RETURN(saved_app.keyExists(strings::hmi_level), false);

#if defined(ENABLE_LOG)
  const bool is_media_app = application->is_media_application();
#endif
  const HMILevel::eType hmi_level =
      static_cast<HMILevel::eType>(saved_app[strings::hmi_level].asInt());
  const bool result = Compare<HMILevel::eType, EQ, ONE>(
                          hmi_level, HMILevel::HMI_FULL, HMILevel::HMI_LIMITED)
                          ? true
                          : false;
  SDL_DEBUG("is_media_app " << is_media_app << "; hmi_level " << hmi_level
                            << " result " << result);
  return result;
}

bool ResumeCtrlImpl::CheckIcons(ApplicationSharedPtr application,
                                smart_objects::SmartObject& obj) {
  using namespace smart_objects;
  SDL_AUTO_TRACE();
  const mobile_apis::Result::eType verify_images =
      MessageHelper::VerifyImageFiles(obj, application, application_manager_);
  return mobile_apis::Result::INVALID_DATA != verify_images;
}

bool ResumeCtrlImpl::CheckDelayAfterIgnOn() {
  using namespace date_time;
  SDL_AUTO_TRACE();
  const time_t curr_time = time(NULL);
  const time_t sdl_launch_time = launch_time();
  const uint32_t seconds_from_sdl_start = labs(curr_time - sdl_launch_time);
  const uint32_t wait_time =
      application_manager_.get_settings().resumption_delay_after_ign();
  SDL_DEBUG("curr_time " << curr_time << "; sdl_launch_time " << sdl_launch_time
                         << "; seconds_from_sdl_start "
                         << seconds_from_sdl_start << "; wait_time "
                         << wait_time);
  return seconds_from_sdl_start <= wait_time;
}

time_t ResumeCtrlImpl::launch_time() const {
  return launch_time_;
}

time_t ResumeCtrlImpl::GetIgnOffTime() {
  return resumption_storage_->GetIgnOffTime();
}

bool ResumeCtrlImpl::ProcessHMIRequest(smart_objects::SmartObjectSPtr request,
                                       bool use_events) {
  SDL_AUTO_TRACE();
  if (use_events) {
    const hmi_apis::FunctionID::eType function_id =
        static_cast<hmi_apis::FunctionID::eType>(
            (*request)[strings::function_id].asInt());

    const int32_t hmi_correlation_id =
        (*request)[strings::correlation_id].asInt();
    subscribe_on_event(function_id, hmi_correlation_id);
  }
  if (!application_manager_.ManageHMICommand(request)) {
    SDL_ERROR("Unable to send request");
    return false;
  }
  return true;
}

void ResumeCtrlImpl::ProcessHMIRequests(
    const smart_objects::SmartObjectList& requests) {
  for (smart_objects::SmartObjectList::const_iterator it = requests.begin(),
                                                      total = requests.end();
       it != total;
       ++it) {
    ProcessHMIRequest(*it, true);
  }
}

void ResumeCtrlImpl::AddToResumptionTimerQueue(const uint32_t app_id) {
  SDL_AUTO_TRACE();
  queue_lock_.Acquire();
  waiting_for_timer_.push_back(app_id);
  queue_lock_.Release();
  SDL_DEBUG("Application ID " << app_id << " have been added"
                                           " to resumption queue.");
  if (!is_resumption_active_) {
    is_resumption_active_ = true;
    restore_hmi_level_timer_.Start(
        application_manager_.get_settings().app_resuming_timeout(),
        timer::kSingleShot);
  }
}

void ResumeCtrlImpl::LoadResumeData() {
  SDL_AUTO_TRACE();
  smart_objects::SmartObject so_applications_data;
  resumption_storage_->GetDataForLoadResumeData(so_applications_data);
  size_t length = so_applications_data.length();
  for (size_t i = 0; i < length; ++i) {
    smart_objects::SmartObject& application = so_applications_data[i];
    if (IsAppDataResumptionExpired(application)) {
      const std::string device_id = application[strings::device_id].asString();
      const std::string app_id = application[strings::app_id].asString();
      SDL_INFO("Data resumption is expired.");
      SDL_DEBUG("Resumption data for application "
                << app_id << " and device id " << device_id
                << " will be dropped.");
      resumption_storage_->DropAppDataResumption(device_id, app_id);
      continue;
    }
  }
}

void ResumeCtrlImpl::OnAppRegistrationStart(const std::string& policy_app_id,
                                            const std::string& device_id) {
  SDL_AUTO_TRACE();
  if (IsApplicationSaved(policy_app_id, device_id)) {
    SDL_INFO(
        "Application is found in resumption "
        "data and will try to resume. Stopping resume data persistent timer");
    StopSavePersistentDataTimer();
  }
}

void ResumeCtrlImpl::OnAppRegistrationEnd() {
  SDL_AUTO_TRACE();
  StartSavePersistentDataTimer();
}

bool ResumeCtrlImpl::IsAppDataResumptionExpired(
    const smart_objects::SmartObject& application) const {
  const int32_t max_ign_off_count = 3;
  return max_ign_off_count <= application[strings::ign_off_count].asInt();
}

}  // namespce resumption