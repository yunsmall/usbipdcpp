#include "usbipdcpp/DeviceHandler/DeviceHandler.h"

#include <spdlog/spdlog.h>

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/InterfaceHandler/InterfaceHandler.h"

using namespace usbipdcpp;

void AbstDeviceHandler::trigger_session_stop() {
    std::lock_guard lock(session_mutex_);
    if (session)[[likely]] {
        session->immediately_stop();
    }
}
