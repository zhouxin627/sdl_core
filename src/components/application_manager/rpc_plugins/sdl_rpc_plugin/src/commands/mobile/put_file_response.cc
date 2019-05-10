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

#include "sdl_rpc_plugin/commands/mobile/put_file_response.h"

#include <bits/stdint-uintn.h>
#include <log4cxx/helpers/objectptr.h>
#include <log4cxx/logger.h>
#include <memory>

#include "application_manager/application.h"
#include "application_manager/application_manager.h"
#include "application_manager/commands/command_impl.h"
#include "application_manager/smart_object_keys.h"
#include "interfaces/MOBILE_API.h"
#include "smart_objects/smart_object.h"
#include "utils/logger.h"

namespace application_manager {
class HMICapabilities;
namespace rpc_service {
class RPCService;
}  // namespace rpc_service
}  // namespace application_manager
namespace policy {
class PolicyHandlerInterface;
}  // namespace policy

namespace sdl_rpc_plugin {
using namespace application_manager;

namespace commands {

PutFileResponse::PutFileResponse(
    const application_manager::commands::MessageSharedPtr& message,
    ApplicationManager& application_manager,
    app_mngr::rpc_service::RPCService& rpc_service,
    app_mngr::HMICapabilities& hmi_capabilities,
    policy::PolicyHandlerInterface& policy_handler)
    : CommandResponseImpl(message,
                          application_manager,
                          rpc_service,
                          hmi_capabilities,
                          policy_handler) {}

PutFileResponse::~PutFileResponse() {}

void PutFileResponse::Run() {
  LOG4CXX_AUTO_TRACE(logger_);
  uint32_t app_id =
      (*message_)[strings::params][strings::connection_key].asUInt();
  ApplicationSharedPtr app = application_manager_.application(app_id);
  if (!app) {
    LOG4CXX_ERROR(logger_, "Application not registered");
    SendResponse(false, mobile_apis::Result::APPLICATION_NOT_REGISTERED);
    return;
  }

  SendResponse((*message_)[strings::msg_params][strings::success].asBool());
}

}  // namespace commands

}  // namespace sdl_rpc_plugin
