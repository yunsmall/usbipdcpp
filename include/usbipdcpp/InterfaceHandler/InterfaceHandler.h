#pragma once

#include "usbipdcpp/Export.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/StringPool.h"

namespace usbipdcpp {
struct UsbIpIsoPacketDescriptor;
struct UsbEndpoint;
struct SetupPacket;
struct UsbInterface;

class TransferResponder;
class TransferHandle;


/**
 * @brief 继承 VirtualInterfaceHandler 类，不要继承这个类
 */
class USBIPDCPP_API AbstInterfaceHandler {
public:
    explicit AbstInterfaceHandler(UsbInterface &handle_interface) :
        handle_interface(handle_interface) {
    }

    /**
     * @brief 新的客户端连接时会调这个函数
     * @param responder 传输应答接口（提交应答/停止传输用）
     * @param ec 发生的ec
     */
    virtual void on_new_connection(TransferResponder &responder, error_code &ec) =0;

    /**
     * @brief 返回所属接口的只读引用
     *
     * 接口描述信息（接口号/端点等）一律经它访问，不要给本类加字段级
     * getter。跨接口描述符要引用其他接口时（如 UAC AC Header 的
     * baInterfaceNr），由装配方把对方 handler 关联过来后经
     * `handler->get_interface()` 取
     * @return 所属接口的 const 引用
     */
    [[nodiscard]] const UsbInterface &get_interface() const {
        return handle_interface;
    }

    /**
     * @brief 当发生错误、客户端detach、主动关闭服务器等情况需要完全终止传输时会调用这个函数。被调用后不可以再提交消息
     */
    virtual void on_disconnection(error_code &ec) =0;
    /**
     * @brief 所有seqnum都会调用这个函数，请确保只处理自己的seqnum
     * @param unlink_seqnum 想要取消的包序号
     * @param cmd_seqnum CMD_UNLINK 命令的序号（用于构造 RET_UNLINK）
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum);

    virtual ~AbstInterfaceHandler() = default;

protected:
    UsbInterface &handle_interface;
};

}
