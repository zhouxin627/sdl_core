#include "sdl_rpc_plugin/commands/mobile/set_cloud_app_properties_request.h"

#include <log4cxx/helpers/objectptr.h>
#include <log4cxx/logger.h>
#include <memory>

#include "application_manager/application.h"
#include "application_manager/application_manager.h"
#include "application_manager/commands/command_impl.h"
#include "application_manager/policies/policy_handler_interface.h"
#include "interfaces/MOBILE_API.h"
#include "utils/logger.h"

namespace application_manager {
class HMICapabilities;
namespace event_engine {
class Event;
}  // namespace event_engine
namespace rpc_service {
class RPCService;
}  // namespace rpc_service

class HMICapabilities;
namespace event_engine {
class Event;
}  // namespace event_engine
namespace rpc_service {
class RPCService;
}  // namespace rpc_service

class HMICapabilities;
namespace event_engine {
class Event;
}  // namespace event_engine
namespace rpc_service {
class RPCService;
}  // namespace rpc_service
}  // namespace application_manager

namespace sdl_rpc_plugin {
using namespace application_manager;

namespace commands {

SetCloudAppPropertiesRequest::SetCloudAppPropertiesRequest(
    const app_mngr::commands::MessageSharedPtr& message,
    app_mngr::ApplicationManager& application_manager,
    app_mngr::rpc_service::RPCService& rpc_service,
    app_mngr::HMICapabilities& hmi_capabilities,
    policy::PolicyHandlerInterface& policy_handler)
    : CommandRequestImpl(message,
                         application_manager,
                         rpc_service,
                         hmi_capabilities,
                         policy_handler) {}

SetCloudAppPropertiesRequest::~SetCloudAppPropertiesRequest() {}

void SetCloudAppPropertiesRequest::Run() {
  LOG4CXX_AUTO_TRACE(logger_);
  ApplicationSharedPtr app = application_manager_.application(connection_key());

  if (!app) {
    LOG4CXX_ERROR(logger_, "Application is not registered");
    SendResponse(false, mobile_apis::Result::APPLICATION_NOT_REGISTERED);
    return;
  }

  policy_handler_.OnSetCloudAppProperties(*message_);

  SendResponse(true, mobile_apis::Result::SUCCESS);
}

void SetCloudAppPropertiesRequest::on_event(
    const app_mngr::event_engine::Event& event) {
  LOG4CXX_INFO(logger_, "SetCloudAppPropertiesRequest on_event");
}

}  // namespace commands
}  // namespace sdl_rpc_plugin
