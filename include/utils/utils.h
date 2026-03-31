#pragma once

#include <exception>

namespace usbipdcpp {
    inline void if_has_value_than_rethrow(std::exception_ptr e) {
        if (e)
            std::rethrow_exception(e);
    }
}
