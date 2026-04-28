#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "InterfaceHandler/InterfaceHandler.h"
#include "protocol.h"

namespace usbipdcpp {

/**
 * @brief 端点请求队列，按端点地址管理传输请求（纯数据容器，不加锁）
 *
 * 用于管理每个端点的待处理 IN 传输请求。
 * 注意：所有方法都不加锁，调用者需自行管理互斥锁。
 */
class EndpointRequestQueue {
public:
    struct Request {
        std::uint32_t seqnum;
        std::uint32_t length;
        TransferHandle transfer;
    };

    /**
     * @brief 向指定端点入队请求
     * @note 调用者需已持有互斥锁
     */
    void enqueue(std::uint8_t ep_address, Request request) {
        queues_[ep_address].push_back(std::move(request));
    }

    /**
     * @brief 从指定端点出队请求
     * @note 调用者需已持有互斥锁
     */
    std::optional<Request> dequeue(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return std::nullopt;
        }
        auto req = std::move(it->second.front());
        it->second.pop_front();
        return req;
    }

    /**
     * @brief 从任何有请求的端点出队请求（返回端点地址和请求）
     * @return pair<端点地址, 请求>，如果所有队列都为空返回 nullopt
     * @note 调用者需已持有互斥锁
     */
    std::optional<std::pair<std::uint8_t, Request>> dequeue_any() {
        for (auto& [ep, queue] : queues_) {
            if (!queue.empty()) {
                auto req = std::move(queue.front());
                queue.pop_front();
                return std::make_pair(ep, std::move(req));
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 获取指定端点队列的首个请求（不出队）
     * @note 调用者需已持有互斥锁
     */
    Request* peek(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return nullptr;
        }
        return &it->second.front();
    }

    /**
     * @brief 检查指定端点队列是否为空
     * @note 调用者需已持有互斥锁
     */
    bool empty(std::uint8_t ep_address) const {
        auto it = queues_.find(ep_address);
        return it == queues_.end() || it->second.empty();
    }

    /**
     * @brief 按 seqnum 取消请求（用于 UNLINK）
     * @return 如果找到并移除了请求返回 true
     * @note 调用者需已持有互斥锁
     */
    bool cancel_by_seqnum(std::uint32_t unlink_seqnum) {
        for (auto& [ep, queue] : queues_) {
            auto it = std::find_if(queue.begin(), queue.end(),
                [unlink_seqnum](const Request& r) { return r.seqnum == unlink_seqnum; });
            if (it != queue.end()) {
                queue.erase(it);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 清空所有队列
     * @note 调用者需已持有互斥锁
     */
    void clear() {
        queues_.clear();
    }

private:
    std::unordered_map<std::uint8_t, std::deque<Request>> queues_;
};

class VirtualInterfaceHandler : public AbstInterfaceHandler {
public:
    explicit VirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        AbstInterfaceHandler(handle_interface), string_pool(string_pool) {

        string_interface = string_pool.new_string(L"Usbipdcpp Virtual Interface");
    }

    // ========== 连接生命周期 API ==========

    /**
     * @brief 设置所属的 DeviceHandler
     * @param handler DeviceHandler 指针
     */
    void set_device_handler(AbstDeviceHandler* handler) {
        device_handler = handler;
    }

    /**
     * @brief 新的客户端连接时会调这个函数
     * @param current_session
     * @param ec 发生的ec
     * @note 子类重写时必须调用父类实现，在函数开头调用，父类会设置session指针
     */
    void on_new_connection(Session &current_session, error_code &ec) override {
        session = &current_session;
    }

    /**
     * @brief 当发生错误、客户端detach、主动关闭服务器等情况需要完全终止传输时会调用这个函数。被调用后不可以再提交消息。
     * @note 子类重写时必须调用父类实现，在函数末尾调用，父类会清理session指针
     */
    void on_disconnection(error_code &ec) override {
        session = nullptr;
    }

    // ========== 内部实现（子类无需关心） ==========

    virtual void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                              std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                              TransferHandle transfer,
                              error_code &ec);
    virtual void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                   TransferHandle transfer,
                                   error_code &ec);
    virtual void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                     std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                     TransferHandle transfer,
                                     int num_iso_packets, error_code &ec);

    virtual void handle_non_standard_request_type_control_urb(std::uint32_t seqnum,
                                                              const UsbEndpoint &ep,
                                                              std::uint32_t transfer_flags,
                                                              std::uint32_t transfer_buffer_length,
                                                              const SetupPacket &setup,
                                                              TransferHandle transfer, std::error_code &ec);
    virtual void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum,
                                                                          const UsbEndpoint &ep,
                                                                          std::uint32_t transfer_flags,
                                                                          std::uint32_t transfer_buffer_length,
                                                                          const SetupPacket &setup,
                                                                          TransferHandle transfer,
                                                                          std::error_code &ec);

    // ========== 子类必须实现的虚函数 ==========

    virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) =0;
    virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                std::uint32_t *p_status) =0;

    virtual std::uint8_t request_get_interface(std::uint32_t *p_status) =0;
    virtual void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) =0;

    virtual std::uint16_t request_get_status(std::uint32_t *p_status) =0;
    virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) =0;

    /**
     * @brief this function is not necessary for all device,
     * HID device is required to implement this function
     * @param type
     * @param language_id
     * @param descriptor_length
     * @param p_status
     * @return
     */
    virtual data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                             std::uint16_t descriptor_length, std::uint32_t *p_status);

    virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) =0;
    virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                              std::uint32_t *p_status) =0;

    /**
     * @brief Only use for isochronous transfer, so give a default empty implement.
     * @param ep_address
     * @param p_status
     * @return
     */
    virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t *p_status) {
        return 0;
    }


    [[nodiscard]] virtual data_type get_class_specific_descriptor() =0;

    // ========== 工具函数 ==========

    [[nodiscard]] virtual std::uint8_t get_string_interface_value() const {
        return string_interface;
    }

    [[nodiscard]] virtual std::wstring get_string_interface() const {
        return string_pool.get_string(string_interface).value_or(L"");
    }

protected:
    Session *session = nullptr;
    AbstDeviceHandler* device_handler = nullptr;

    std::uint8_t string_interface;

    StringPool &string_pool;

    /**
     * @brief 保护 endpoint_requests_ 的互斥锁
     */
    mutable std::mutex endpoint_requests_mutex_;

    /**
     * @brief 通用端点请求队列，用于管理 IN 传输请求
     *
     * 子类可以直接使用此队列，不需要各自实现。
     * 操作时需持有 endpoint_requests_mutex_。
     */
    EndpointRequestQueue endpoint_requests_;
};

}
