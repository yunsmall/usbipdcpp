#pragma once

#include "usbipdcpp/usbipdcpp_core.h"

// 虚拟设备基类
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/VirtualDeviceTransferOperator.h"

// HID
#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/HidConstants.h"
#include "usbipdcpp/virtual_device/devices/AbsoluteMouseHandler.h"
#include "usbipdcpp/virtual_device/devices/RelativeMouseHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"
#include "usbipdcpp/virtual_device/devices/GamepadHandler.h"
#include "usbipdcpp/virtual_device/devices/DigitizerHandler.h"

// CDC ACM
#include "usbipdcpp/virtual_device/CdcAcmVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/CdcAcmConstants.h"

// MSC
#include "usbipdcpp/virtual_device/MscConstants.h"
#include "usbipdcpp/virtual_device/devices/MscBulkOnlyHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageBackend.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageIoTransfer.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageTransferOperator.h"
#include "usbipdcpp/virtual_device/storage_backends/RawImageBackend.h"
#include "usbipdcpp/virtual_device/storage_backends/MemoryBackend.h"

// UVC
#include "usbipdcpp/virtual_device/UvcConstants.h"
#include "usbipdcpp/virtual_device/UvcVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"
#include "usbipdcpp/virtual_device/video_sources/ColorBarSource.h"

// UAC
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"
#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"
#include "usbipdcpp/virtual_device/audio_sinks/WavFileSink.h"
