#pragma once

#include "usbipdcpp_core.h"

// 虚拟设备基类
#include "virtual_device/VirtualDeviceHandler.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/VirtualInterfaceHandler.h"
#include "virtual_device/VirtualDeviceTransferOperator.h"

// HID
#include "virtual_device/HidVirtualInterfaceHandler.h"
#include "virtual_device/HidConstants.h"
#include "virtual_device/devices/AbsoluteMouseHandler.h"
#include "virtual_device/devices/RelativeMouseHandler.h"
#include "virtual_device/devices/KeyboardHandler.h"
#include "virtual_device/devices/GamepadHandler.h"
#include "virtual_device/devices/DigitizerHandler.h"

// CDC ACM
#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"
#include "virtual_device/CdcAcmConstants.h"

// MSC
#include "virtual_device/MscConstants.h"
#include "virtual_device/devices/MscBulkOnlyHandler.h"
#include "virtual_device/storage_backends/StorageBackend.h"
#include "virtual_device/storage_backends/StorageIoTransfer.h"
#include "virtual_device/storage_backends/StorageTransferOperator.h"
#include "virtual_device/storage_backends/RawImageBackend.h"
#include "virtual_device/storage_backends/MemoryBackend.h"

// UVC
#include "virtual_device/UvcConstants.h"
#include "virtual_device/UvcVirtualInterfaceHandler.h"
#include "virtual_device/video_sources/VideoSource.h"
#include "virtual_device/video_sources/ColorBarSource.h"
