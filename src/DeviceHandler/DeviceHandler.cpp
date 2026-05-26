#include "DeviceHandler/DeviceHandler.h"

#include <spdlog/spdlog.h>

#include "Interface.h"
#include "constant.h"
#include "Device.h"
#include "Session.h"
#include "protocol.h"
#include "type.h"
#include "InterfaceHandler/InterfaceHandler.h"

using namespace usbipdcpp;

AbstDeviceHandler::AbstDeviceHandler(AbstDeviceHandler &&other) noexcept :
    handle_device(other.handle_device) {
}

void AbstDeviceHandler::trigger_session_stop() {
    std::lock_guard lock(session_mutex_);
    if (session)[[likely]] {
        session->immediately_stop();
    }
}
