#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/InterfaceHandler/InterfaceHandler.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"

namespace usbipdcpp {

class VirtualDeviceHandler;
class StringPool;

/**
 * @brief 虚拟设备接口处理器基类：响应接口级标准/类请求与数据面传输
 *
 * @note 实现静态工厂 make_interface 时禁止在函数内调用 UsbInterface::with_handler：
 *       handler 构造持接口引用，而 make_interface 里的接口对象是局部/临时对象，
 *       返回（拷贝/移动）后引用立即悬垂（ECM 曾因在 make_interface 里绑定而崩溃）。
 *       make_interface 只返回描述符正确的 UsbInterface，handler 由调用方在
 *       设备创建完成（接口对象入 device->interfaces、地址稳定）后通过
 *       device->interfaces[i].with_handler<T>(...) 绑定。
 */
class USBIPDCPP_API VirtualInterfaceHandler : public AbstInterfaceHandler {
public:
    explicit VirtualInterfaceHandler(UsbInterface &handle_interface,
                                     std::unique_ptr<TransferOperator> op = nullptr) :
        AbstInterfaceHandler(handle_interface),
        transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {
    }

    // ========== 连接生命周期 API ==========

    /**
     * @brief 设置所属的 DeviceHandler
     * @param handler DeviceHandler 指针
     * @note 一个接口 handler 只应绑定一台设备；重复绑定到不同设备会被忽略
     */
    void set_device_handler(VirtualDeviceHandler *handler);

    /**
     * @brief 把本接口需要的字符串登记到设备字符串池
     * @param pool 所属设备的字符串描述符池
     *
     * setup_interface_handlers 在 set_device_handler 之后、on_setup_interface_handlers
     * 之前调用。基类实现登记 iInterface；派生类重写时必须先调用父类实现，再登记
     * 自己额外的字符串（如 ECM 的 iMACAddress）。实现需幂等：重复 setup 不能重复占索引。
     * 调父类之前可以先做索引同步等准备（UVC 的 VC/VS 共享 iInterface 就是先
     * sync_string_interface_from 对端，再调基类登记）。
     *
     * 登记额外字符串的写法：
     * @code
     * void register_strings(StringPool &pool) override {
     *     VirtualInterfaceHandler::register_strings(pool);
     *     if (extra_string_index == 0) {
     *         extra_string_index = pool.new_string(L"Vendor String");
     *     }
     * }
     * @endcode
     */
    virtual void register_strings(StringPool &pool);

    /** setup_interface_handlers 末尾回调，此时 device_handler 已设置、字符串已登记，子类可在此做初始化 */
    virtual void on_setup_interface_handlers() {
    }

    /**
     * @brief 新的客户端连接时会调这个函数
     * @param current_session
     * @param ec 发生的ec
     * @note 子类重写时必须调用父类实现，在函数开头调用，父类会设置session指针
     */
    void on_new_connection(TransferResponder &current_session, error_code &ec) override {
        responder = &current_session;
        // 新连接等价重新枚举：端点 halt 状态清零（USB 2.0 §9.4.5，
        // 复位/重新配置后端点回到未 halt 状态）
        for (auto &halted: endpoint_halted)
            halted.store(false, std::memory_order_relaxed);
    }

    /**
     * @brief 当发生错误、客户端detach、主动关闭服务器等情况需要完全终止传输时会调用这个函数。被调用后不可以再提交消息。
     * @note 子类重写时必须调用父类实现，在函数末尾调用，父类会清理session指针
     */
    void on_disconnection(error_code &ec) override {
        responder = nullptr;
    }

    // ========== 数据面回调（默认回 EPIPE，功能设备必须重写） ==========

    virtual void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                      std::uint32_t transfer_buffer_length, TransferHandle transfer, error_code &ec);
    virtual void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                           std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                           error_code &ec);
    virtual void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                             std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                             int num_iso_packets, error_code &ec);

    virtual void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                              std::uint32_t transfer_flags,
                                                              std::uint32_t transfer_buffer_length,
                                                              const SetupPacket &setup, TransferHandle transfer,
                                                              std::error_code &ec);
    virtual void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                          std::uint32_t transfer_flags,
                                                                          std::uint32_t transfer_buffer_length,
                                                                          const SetupPacket &setup,
                                                                          TransferHandle transfer, std::error_code &ec);

    // ========== 标准请求回调（默认实现：未实现即回成功/EPIPE，子类按需重写）==========
    // 默认实现都很简单，放头文件内联：头文件即文档，子类无需跳去 cpp 看默认行为。
    
    // 接口级 feature 请求一律 STALL：USB 2.0 Table 9-6 只给 Device 和 Endpoint
    // 定义了标准 feature selector，接口没有合法的 selector；§9.4 规定不支持
    // 或无效的请求必须在 Data/Status 阶段回 STALL。
    // 端点级 feature 由下面的 halt 状态机处理（§9.4.5）。

    virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
        (void)feature_selector;
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
    virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                std::uint32_t *p_status);

    virtual std::uint8_t request_get_interface(std::uint32_t *p_status) {
        // 返回当前 alternate setting（设备级在 SET_INTERFACE 成功时已更新）。
        // 对齐内核 f->get_alt：接口无 get_alt 回调时返回 0（只支持 alt 0）
        *p_status = 0;
        return handle_interface.current_altsetting;
    }
    virtual void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
        // 只接受设备定义里存在的 alt（endpoints 外层下标即 alt 号）：请求不存在的
        // alt 回 EPIPE，否则 GET_INTERFACE 返回的值与端点集合不一致（假装成功
        // 会让主机驱动读到矛盾的接口状态）。alt 0 是接口的默认设置，恒存在
        // （USB 2.0 §9.6.5）：无端点的接口（bNumEndpoints=0）endpoints 为空，
        // 但 SET_INTERFACE(0) 仍必须成功。对齐内核 composite：接口无 set_alt/
        // get_alt 回调时只支持 alt 0，请求其他 alt 回 STALL
        if (alternate_setting == 0 || alternate_setting < handle_interface.endpoints.size()) {
            *p_status = 0;
        }
        else {
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        }
    }

    virtual std::uint16_t request_get_status(std::uint32_t *p_status) {
        // GET_STATUS 回 0：主机栈（如 Windows）对状态查询有基础依赖，
        // 回 0 是"无特殊状态"的宽松响应（对齐内核 GET_STATUS 无 get_status
        // 回调时返回 0 的语义）
        *p_status = 0;
        return 0;
    }
    virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status);

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
                                             std::uint16_t descriptor_length, std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }

    virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
        (void)feature_selector;
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
    virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                              std::uint32_t *p_status);

    // ========== UNLINK ==========

    /**
     * @brief 尝试取消本接口挂起的 URB（不发应答）
     * @param unlink_seqnum 要取消的 URB seqnum
     * @return 是否真的取消了挂起的 URB（false 表示已完成或不在本接口）
     * @note 设备级 UNLINK 遍历所有接口汇总结果后只提交一条 RET_UNLINK；
     *       多接口设备（CDC/ECM/RNDIS 等）若每个接口各自应答，主机会收到
     *       多条相同 cmd_seqnum 的 RET_UNLINK，协议错乱
     */
    virtual bool cancel_pending_urb(std::uint32_t unlink_seqnum) {
        (void) unlink_seqnum;
        return false;
    }

    /**
     * @brief 取消挂起 URB 并提交 RET_UNLINK
     * @param unlink_seqnum 要取消的 URB seqnum
     * @param cmd_seqnum UNLINK 命令自身的 seqnum（应答里回填）
     * @note 设备级路径用 cancel_pending_urb 汇总后统一应答；直接调用本函数
     *       （单接口场景）时由接口自己应答
     */
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    /**
     * @brief Only use for isochronous transfer, so give a default empty implement.
     * @param ep_address
     * @param p_status
     * @return
     */
    virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t *p_status) {
        // 未实现：对齐 composite 对 SYNC_FRAME 的 STALL 行为
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return 0;
    }


    /// 类描述符（挂在配置描述符的接口描述符之后），默认空表示没有类描述符
    [[nodiscard]] virtual data_type get_class_specific_descriptor() {
        return {};
    }

    /**
     * @brief 端点级类特定描述符（紧跟在标准端点描述符之后）
     *
     * UAC/UVC 等设备类的端点需要类特定描述符；由接口 handler 按自己的类规则
     * 生成，设备级基类只负责拼接，避免基类反向依赖具体设备类。
     * @param ep 当前端点（地址、属性、包长）
     * @return 追加到该端点标准描述符之后的字节，默认空
     */
    [[nodiscard]] virtual data_type get_class_specific_endpoint_descriptor(const UsbEndpoint &ep) {
        return {};
    }

    /**
     * @brief class-specific 描述符是否放在所有 alternate setting 之后
     *
     * 默认 false：仅放 alt 0（UVC 等描述符较大的类，放所有 alt 会撑爆
     * 配置描述符 255 字节上限导致 Windows 截断解析失败）。
     * UAC 的 AS 接口描述符很小，且 alt 1 必须有格式描述符供驱动解析，需返回 true。
     */
    [[nodiscard]] virtual bool put_class_specific_descriptor_in_all_alts() const {
        return false;
    }

    // ========== TransferOperator ==========

    TransferOperator *get_transfer_operator() {
        return transfer_op_.get();
    }

    /**
     * @brief 查询端点是否处于 HALT 状态
     *
     * 数据面（VirtualDeviceHandler::dispatch_urb）据此回 STALL：真实设备上
     * halt 端点的 URB 直接以 -EPIPE 完成，不再由设备处理（USB 2.0 §9.4.5）
     * @param ep_address 端点地址（含方向位）
     */
    [[nodiscard]] bool is_endpoint_halted(std::uint8_t ep_address) const {
        return endpoint_halted[ep_address].load(std::memory_order_relaxed);
    }

    /**
     * @brief 清除本接口所有端点的 HALT 状态
     *
     * USB 2.0 §9.4.5：SetConfiguration() 和 SetInterface() 成功后端点 Halt
     * 必须清零，即使请求的配置/接口与当前相同。设备级在标准请求成功后调用。
     */
    void reset_endpoint_halt_state();

    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        // 注意：HID 等基于 GenericTransfer 的类（handle_control_urb、
        // handle_interrupt_transfer、send_input_report 里的
        // GenericTransfer::from_handle）硬性依赖 handle 是 GenericTransfer。
        // 默认 GenericTransferOperator 符合此假设；仅存储类（MSC 的
        // StorageTransferOperator，走 Bulk 不经 from_handle）例外。除非你
        // 清楚自己在做什么，否则不要给依赖 GenericTransfer 的接口换非
        // Generic 的 op，否则 from_handle 强转是未定义行为
        transfer_op_ = std::move(op);
    }

    // ========== 工具函数 ==========

    [[nodiscard]] virtual std::uint8_t get_string_interface_value() const {
        return string_interface;
    }

    /// 取 iInterface 指向的字符串；还没登记（未 setup）时返回 interface_string_to_register() 的默认名
    [[nodiscard]] virtual std::wstring get_string_interface() const;

    /**
     * @brief 登记到设备字符串池的接口名（iInterface 的内容）
     * @return 默认名字；需要自定义接口名的 handler（如 UAC 各接口、用户自定义 handler）重写本函数
     * @note 在 register_strings（setup_interface_handlers）时读取一次
     */
    [[nodiscard]] virtual std::wstring interface_string_to_register() const {
        return L"Usbipdcpp Virtual Interface";
    }

    /**
     * @brief 使 iInterface 与另一个 handler 一致（USBCCGP 移除 IAD 后，同功能接口靠 iInterface 分组）
     * @param other 另一个接口 handler
     * @note 必须在本 handler 登记字符串之前调用（register_strings 里、基类实现
     *       之前）。对方已登记则复制其索引；对方尚未登记（索引为 0）时保持原值，
     *       由先登记的一方分配、后登记的一方复制，因此双方都调用即可与顺序无关。
     *       登记之后调用会把自己已占用的索引丢掉，别的地方还在用该索引时就对不上了
     */
    void sync_string_interface_from(const VirtualInterfaceHandler &other);

protected:
    /**
     * @brief 取所属设备的字符串描述符表
     * @note 只能在 register_strings 之后调用（setup_interface_handlers 之后）
     * @return 设备字符串池
     */
    StringPool &get_string_pool() const;

    TransferResponder *responder = nullptr;
    VirtualDeviceHandler *device_handler = nullptr;
    std::unique_ptr<TransferOperator> transfer_op_;

    /// 端点 halt 状态（下标 = 端点地址含方向位）。数据面在会话接收线程读、
    /// 标准请求在同一线程写，但调度线程也可能路径经过 dispatch 之外的检查，
    /// 用 atomic 避免数据竞争
    std::array<std::atomic_bool, 256> endpoint_halted{};

    /// 接口字符串索引：setup 阶段由 register_strings 分配、sync_string_interface_from 复制，
    /// 导出后只读；0 表示还没登记到设备池
    std::uint8_t string_interface = 0;

    /// 设备字符串池（register_strings 时设置）；未登记时为空
    StringPool *string_pool = nullptr;
};

} // namespace usbipdcpp
