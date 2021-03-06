/*

 Copyright (c) 2018, Ford Motor Company
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

#include "sdl_rpc_plugin/commands/mobile/on_button_press_notification.h"

#include "application_manager/application_impl.h"
#include "interfaces/MOBILE_API.h"
#include "utils/semantic_version.h"

namespace sdl_rpc_plugin {
using namespace application_manager;

namespace commands {

namespace mobile {

OnButtonPressNotification::OnButtonPressNotification(
    const application_manager::commands::MessageSharedPtr& message,
    ApplicationManager& application_manager,
    rpc_service::RPCService& rpc_service,
    HMICapabilities& hmi_capabilities,
    policy::PolicyHandlerInterface& policy_handler)
    : CommandNotificationImpl(message,
                              application_manager,
                              rpc_service,
                              hmi_capabilities,
                              policy_handler) {}

OnButtonPressNotification::~OnButtonPressNotification() {}

void OnButtonPressNotification::Run() {
  LOG4CXX_AUTO_TRACE(logger_);

  const uint32_t btn_id = static_cast<uint32_t>(
      (*message_)[strings::msg_params][hmi_response::button_name].asInt());

  const bool is_app_id_exists =
      (*message_)[strings::msg_params].keyExists(strings::app_id);
  ApplicationSharedPtr app;

  // CUSTOM_BUTTON notification
  if (static_cast<uint32_t>(mobile_apis::ButtonName::CUSTOM_BUTTON) == btn_id) {
    // app_id is mandatory for CUSTOM_BUTTON notification
    if (!is_app_id_exists) {
      LOG4CXX_ERROR(logger_, "CUSTOM_BUTTON OnButtonPress without app_id.");
      return;
    }

    app = application_manager_.application(
        (*message_)[strings::msg_params][strings::app_id].asUInt());

    // custom_button_id is mandatory for CUSTOM_BUTTON notification
    if (false == (*message_)[strings::msg_params].keyExists(
                     hmi_response::custom_button_id)) {
      LOG4CXX_ERROR(logger_,
                    "CUSTOM_BUTTON OnButtonPress without custom_button_id.");
      return;
    }

    if (!app) {
      LOG4CXX_ERROR(logger_, "Application doesn't exist.");
      return;
    }

    uint32_t custom_btn_id = 0;
    custom_btn_id =
        (*message_)[strings::msg_params][hmi_response::custom_button_id]
            .asUInt();

    if (false == app->IsSubscribedToSoftButton(custom_btn_id)) {
      LOG4CXX_ERROR(logger_,
                    "Application doesn't subscribed to this custom_button_id.");
      return;
    }

    // Send ButtonPress notification only in HMI_FULL or HMI_LIMITED mode
    const auto window_id = app->GetSoftButtonWindowID(custom_btn_id);
    app->hmi_level(mobile_apis::PredefinedWindows::DEFAULT_WINDOW);
    (*message_)[strings::msg_params][strings::window_id] = window_id;
    const auto window_hmi_level = app->hmi_level(window_id);
    if ((mobile_api::HMILevel::HMI_FULL != window_hmi_level) &&
        (mobile_api::HMILevel::HMI_LIMITED != window_hmi_level)) {
      LOG4CXX_WARN(logger_,
                   "CUSTOM_BUTTON OnButtonPress notification is allowed only "
                       << "in FULL or LIMITED hmi level");
      return;
    }

    SendButtonPress(app);
    return;
  }

  const std::vector<ApplicationSharedPtr>& subscribed_apps =
      application_manager_.applications_by_button(btn_id);

  std::vector<ApplicationSharedPtr>::const_iterator it =
      subscribed_apps.begin();
  for (; subscribed_apps.end() != it; ++it) {
    ApplicationSharedPtr subscribed_app = *it;
    if (!subscribed_app) {
      LOG4CXX_WARN(logger_, "Null pointer to subscribed app.");
      continue;
    }

    // Send ButtonPress notification only in HMI_FULL or HMI_LIMITED mode
    const mobile_apis::HMILevel::eType app_hmi_level =
        subscribed_app->hmi_level(
            mobile_apis::PredefinedWindows::DEFAULT_WINDOW);
    if ((mobile_api::HMILevel::HMI_FULL != app_hmi_level) &&
        (mobile_api::HMILevel::HMI_LIMITED != app_hmi_level)) {
      LOG4CXX_WARN(logger_,
                   "OnButtonPress notification is allowed only"
                       << "in FULL or LIMITED hmi level");
      continue;
    }
    // if "appID" is present, send it to named app only if its FULL or
    // LIMITED
    if (app.use_count() != 0) {
      if (app->app_id() == subscribed_app->app_id()) {
        SendButtonPress(subscribed_app);
      }
    } else if (subscribed_app->IsFullscreen()) {
      // if No "appID" - send it FULL apps only.
      SendButtonPress(subscribed_app);
    }
  }
}

void OnButtonPressNotification::SendButtonPress(ApplicationConstSharedPtr app) {
  if (!app) {
    LOG4CXX_ERROR(logger_, "OnButtonPress NULL pointer");
    return;
  }

  smart_objects::SmartObjectSPtr on_btn_press =
      std::make_shared<smart_objects::SmartObject>();

  if (!on_btn_press) {
    LOG4CXX_ERROR(logger_, "OnButtonPress NULL pointer");
    return;
  }

  (*on_btn_press)[strings::params][strings::connection_key] = app->app_id();

  (*on_btn_press)[strings::params][strings::function_id] =
      static_cast<int32_t>(mobile_apis::FunctionID::eType::OnButtonPressID);

  mobile_apis::ButtonName::eType btn_id =
      static_cast<mobile_apis::ButtonName::eType>(
          (*message_)[strings::msg_params][hmi_response::button_name].asInt());

  if (btn_id == mobile_apis::ButtonName::PLAY_PAUSE &&
      app->msg_version() < utils::rpc_version_5) {
    btn_id = mobile_apis::ButtonName::OK;
  }

  (*on_btn_press)[strings::msg_params][strings::button_name] = btn_id;
  (*on_btn_press)[strings::msg_params][strings::button_press_mode] =
      (*message_)[strings::msg_params][hmi_response::button_mode];

  if ((*message_)[strings::msg_params].keyExists(
          hmi_response::custom_button_id)) {
    (*on_btn_press)[strings::msg_params][strings::custom_button_id] =
        (*message_)[strings::msg_params][strings::custom_button_id];
  }

  if ((*message_)[strings::msg_params].keyExists(strings::window_id)) {
    (*on_btn_press)[strings::msg_params][strings::window_id] =
        (*message_)[strings::msg_params][strings::window_id];
  }

  message_ = on_btn_press;
  SendNotification();
}

}  // namespace mobile

}  // namespace commands

}  // namespace sdl_rpc_plugin
