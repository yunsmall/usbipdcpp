#pragma once

#include <functional>
#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "Server.h"

namespace usbipdcpp {

/**
 * @brief Result of device bind/unbind operations.
 */
enum class DeviceOperationResult {
    Success,                ///< Operation completed successfully
    DeviceNotFound,         ///< Device was not found in the device list
    DeviceInUse,            ///< Device is currently in use and cannot be modified
    DeviceOpenFailed,       ///< Failed to open the device
    GetDescriptorFailed,    ///< Failed to get device descriptor
    GetConfigFailed,        ///< Failed to get configuration descriptor
    ClaimInterfaceFailed    ///< Failed to claim interface
};

class LibusbServer {
public:
    LibusbServer();

    /**
     * @brief Bind a physical USB device to make it available for export.
     *
     * This function retrieves device information and adds it to the available devices list.
     * The device is not opened until a client connects (lazy binding).
     * The device reference will be owned by this function.
     *
     * @param dev The libusb device to bind. If use_handle is false, this must not be nullptr.
     *            The function takes ownership of the device reference.
     * @param use_handle If true, use an existing device handle instead of opening a new one.
     *                   This is typically used on Android where the handle is obtained via
     *                   libusb_wrap_sys_device(). The handle is stored but interfaces are
     *                   claimed only when a client connects.
     * @param exist_handle An existing device handle to use when use_handle is true.
     *                     Must not be nullptr when use_handle is true.
     * @return DeviceOperationResult::Success on success, or an appropriate error code.
     */
    DeviceOperationResult bind_host_device(libusb_device *dev, bool use_handle = false,
                          libusb_device_handle *exist_handle = nullptr);

    /**
     * @brief Unbind a previously bound physical USB device.
     *
     * Releases all interfaces, reattaches kernel drivers, closes the device handle,
     * and removes the device from the available devices list. The device reference
     * will be released.
     *
     * @param device The libusb device to unbind. The function takes ownership of this reference.
     * @return DeviceOperationResult::Success on success,
     *         DeviceOperationResult::DeviceNotFound if not in available devices,
     *         DeviceOperationResult::DeviceInUse if currently in use.
     */
    DeviceOperationResult unbind_host_device(libusb_device *device);

    /**
     * @brief Remove a dead device from the device lists.
     *
     * This function should not be called with a busid that is still in use.
     * It only removes libusb devices, not other device types.
     *
     * @param busid The bus ID of the device to remove.
     * @return DeviceOperationResult::Success if found and removed,
     *         DeviceOperationResult::DeviceNotFound if not found.
     */
    DeviceOperationResult try_remove_dead_device(const std::string &busid);

    /**
     * @brief Refresh the list of available devices.
     *
     * Removes devices from the available list that are no longer present in the system.
     */
    void refresh_available_devices();

    /**
     * @brief Start the server listening on the specified endpoint.
     *
     * Also starts the hotplug monitor if enabled and supported.
     *
     * @param ep The TCP endpoint to listen on.
     */
    void start(asio::ip::tcp::endpoint &ep);

    /**
     * @brief Stop the server.
     *
     * Stops the hotplug monitor, closes all device handles, releases interfaces,
     * and reattaches kernel drivers.
     */
    void stop();

    ~LibusbServer();

    /**
     * @brief Print detailed information about a USB device to stdout.
     *
     * Displays device name, bus ID, VID/PID, USB version, device class, and speed.
     * Also shows whether the device is exported, available, or unbound.
     *
     * @param dev The libusb device to print information about.
     */
    void print_device(libusb_device *dev);

    /**
     * @brief List all USB devices connected to the host.
     *
     * Prints detailed information about each device to stdout.
     */
    void list_host_devices();

    /**
     * @brief Get the underlying Server instance.
     *
     * @return Reference to the internal Server object.
     */
    Server &get_server() {
        return server;
    }

    /**
     * @brief Get the manufacturer and product names of a USB device.
     *
     * @param device The libusb device to get names from.
     * @return A pair containing {manufacturer, product} names. Returns "Unknown Manufacturer"
     *         or "Unknown Product" if the corresponding string descriptor is not available.
     */
    static std::pair<std::string, std::string> get_device_names(libusb_device *device);

    /**
     * @brief Find a libusb device by its bus ID.
     *
     * @param busid The bus ID to search for (e.g., "1-2.3").
     * @return A libusb_device pointer if found, nullptr otherwise. The caller must call
     *         libusb_unref_device() when done with the device.
     */
    static libusb_device *find_by_busid(const std::string &busid);

    /**
     * @brief Set whether hotplug monitoring is enabled.
     *
     * Hotplug monitoring is enabled by default. On Android without root privileges,
     * hotplug is not supported and should be disabled before starting the server.
     *
     * @param enabled true to enable hotplug monitoring, false to disable.
     */
    void set_hotplug_enabled(bool enabled) {
        hotplug_enabled_by_user_ = enabled;
    }

    /**
     * @brief Check if hotplug monitoring is enabled.
     *
     * @return true if hotplug monitoring is enabled, false otherwise.
     */
    bool is_hotplug_enabled() const {
        return hotplug_enabled_by_user_;
    }

protected:
    Server server;

# ifndef USBIPDCPP_ENABLE_BUSY_WAIT
    std::atomic<bool> should_exit_libusb_event_thread = false;

    //不可在这个线程发送网络包
    std::thread libusb_event_thread;
# endif

    // 热插拔相关
    libusb_hotplug_callback_handle hotplug_handle_ = 0;
    bool hotplug_enabled_ = false;
    bool hotplug_enabled_by_user_ = true;  // 用户设置的开关，默认启用

    void start_hotplug_monitor();
    void stop_hotplug_monitor();

    void handle_device_arrived(libusb_device *device);
    void handle_device_left(const std::string &busid);

    static int LIBUSB_CALL hotplug_callback(
        libusb_context *ctx,
        libusb_device *device,
        libusb_hotplug_event event,
        void *user_data);
};
}
