#include <gtest/gtest.h>

#include "test_utils.h"

#include "network.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;


TEST(TestNetwork, ntoh_hton) {
    if (is_little_endian()) {
        ASSERT_EQ(ntoh((uint64_t)0x1234567801020304llu), (uint64_t)0x0403020178563412llu);
        ASSERT_EQ(ntoh((uint32_t)0x12345678u), (uint32_t)0x78563412u);
        ASSERT_EQ(ntoh((uint16_t)0x1234u), (uint16_t)0x3412u);
        ASSERT_EQ(ntoh((uint8_t)0x12u), (uint8_t)0x12u);

        ASSERT_EQ(hton((uint64_t)0x1234567801020304llu), (uint64_t)0x0403020178563412llu);
        ASSERT_EQ(hton((uint32_t)0x12345678u), (uint32_t)0x78563412u);
        ASSERT_EQ(hton((uint16_t)0x1234u), (uint16_t)0x3412);
        ASSERT_EQ(hton((uint8_t)0x12u), (uint8_t)0x12);
    }
    else {
        ASSERT_EQ(ntoh((uint64_t)0x1234567801020304llu), (uint64_t)0x1234567801020304llu);
        ASSERT_EQ(ntoh((uint32_t)0x12345678u), (uint32_t)0x12345678u);
        ASSERT_EQ(ntoh((uint16_t)0x1234u), (uint16_t)0x1234u);
        ASSERT_EQ(ntoh((uint8_t)0x12u), (uint8_t)0x12u);

        ASSERT_EQ(hton((uint64_t)0x1234567801020304llu), (uint64_t)0x0403020178563412llu);
        ASSERT_EQ(hton((uint32_t)0x12345678u), (uint32_t)0x12345678u);
        ASSERT_EQ(hton((uint16_t)0x1234u), (uint16_t)0x1234u);
        ASSERT_EQ(hton((uint8_t)0x12u), (uint8_t)0x12u);
    }
}

TEST(TestNetwork, vector_append) {
    data_type data1;
    vector_mem_order_append(data1, static_cast<std::uint32_t>(0x01030405u),
                            static_cast<std::uint16_t>(0x1517u),
                            array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                            data_type{0x10u, 0x21u, 0x34u, 0x57u},
                            static_cast<std::uint16_t>(0x0115u),
                            static_cast<std::uint8_t>(0x99u),
                            static_cast<std::uint64_t>(0x9998979695949392llu));
    if constexpr (is_little_endian()) {
        ASSERT_TRUE((
            data1==data_type{0x05u,0x04u,0x03u,0x01u,0x17u,0x15u,0x01u, 0x02u, 0x05u, 0x09u, 0x11u,0x10u, 0x21u, 0x34u,
            0x57u,0x15u,0x01u,0x99u,0x92u,0x93u,0x94u,0x95u,0x96u,0x97u,0x98u,0x99u}
        ));
    }
    else {
        ASSERT_TRUE((
            data1==data_type{ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u, 0x10u, 0x21u,
            0x34u, 0x57u, 0x01u, 0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
        ));
    }


    data_type data2;
    vector_append_to_net(data2, static_cast<std::uint32_t>(0x01030405u),
                         static_cast<std::uint16_t>(0x1517u),
                         array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                         data_type{0x10u, 0x21u, 0x34u, 0x57u},
                         static_cast<std::uint16_t>(0x0115u),
                         static_cast<std::uint8_t>(0x99u),
                         static_cast<std::uint64_t>(0x9998979695949392llu));
    ASSERT_TRUE((
        data2==data_type{ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u,0x10u, 0x21u,
        0x34u, 0x57u, 0x01u, 0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
    ));
}

TEST(TestNetwork, to_network) {
    auto array = to_network_array(static_cast<std::uint32_t>(0x01030405u),
                                  static_cast<std::uint16_t>(0x1517),
                                  array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                                  static_cast<std::uint16_t>(0x0115u),
                                  static_cast<std::uint8_t>(0x99u),
                                  static_cast<std::uint64_t>(0x9998979695949392llu));
    ASSERT_TRUE((
        array==decltype(array){ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u, 0x01u,
        0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
    ));

    auto vec = to_network_data(static_cast<std::uint32_t>(0x01030405u),
                               static_cast<std::uint16_t>(0x1517u),
                               array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                               data_type{0x10u, 0x21u, 0x34u, 0x57u},
                               static_cast<std::uint16_t>(0x0115u),
                               static_cast<std::uint8_t>(0x99u),
                               static_cast<std::uint64_t>(0x9998979695949392llu));
    ASSERT_TRUE((
        vec==data_type{ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u,0x10u, 0x21u,
        0x34u, 0x57u, 0x01u, 0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
    ));
}
