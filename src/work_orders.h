#pragma once

#include "httplib.h"

namespace dfcapture {

void register_work_order_routes(httplib::Server& server);

} // namespace dfcapture
