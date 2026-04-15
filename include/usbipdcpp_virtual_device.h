#pragma once

#include "usbipdcpp_core.h"

// 虚拟设备基类
#include "virtual_device/VirtualDeviceHandler.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/VirtualInterfaceHandler.h"

// 虚拟设备实现
#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"
#include "virtual_device/HidVirtualInterfaceHandler.h"