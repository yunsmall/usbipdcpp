#include "InterfaceHandler/InterfaceHandler.h"

#include <spdlog/spdlog.h>

namespace usbipdcpp {

void AbstInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    SPDLOG_DEBUG("Unimplement AbstInterfaceHandler::handle_unlink_seqnum: unlink_seqnum={}, cmd_seqnum={}", unlink_seqnum, cmd_seqnum);
}

}
