#include <gtest/gtest.h>

#include "test_utils.h"

#include "network.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;


TEST(TestNetwork, ntoh_hton) {
    if (is_little_endian()) {
        ASSERT_EQ(ntoh((uint32_t)0x12345678u), (uint32_t)0x78563412u);
        ASSERT_EQ(ntoh((uint16_t)0x1234u), (uint16_t)0x3412u);
        ASSERT_EQ(ntoh((uint8_t)0x12u), (uint8_t)0x12u);

        ASSERT_EQ(hton((uint32_t)0x12345678u), (uint32_t)0x78563412u);
        ASSERT_EQ(hton((uint16_t)0x1234u), (uint16_t)0x3412);
        ASSERT_EQ(hton((uint8_t)0x12u), (uint8_t)0x12);
    }
    else {
        ASSERT_EQ(ntoh((uint32_t)0x12345678u), (uint32_t)0x12345678u);
        ASSERT_EQ(ntoh((uint16_t)0x1234u), (uint16_t)0x1234u);
        ASSERT_EQ(ntoh((uint8_t)0x12u), (uint8_t)0x12u);

        ASSERT_EQ(hton((uint32_t)0x12345678u), (uint32_t)0x12345678u);
        ASSERT_EQ(hton((uint16_t)0x1234u), (uint16_t)0x1234u);
        ASSERT_EQ(hton((uint8_t)0x12u), (uint8_t)0x12u);
    }
}

TEST(TestNetwork, vector_append) {
    data_type data;
    vector_mem_order_append(data, 0x12345678u);
    ASSERT_EQ(data.size(), sizeof(uint32_t));
    if (is_little_endian()) {
        ASSERT_EQ(data[0], 0x78u);
        ASSERT_EQ(data[1], 0x56u);
        ASSERT_EQ(data[2], 0x34u);
        ASSERT_EQ(data[3], 0x12u);
    }
    else {
        ASSERT_EQ(data[0], 0x12u);
        ASSERT_EQ(data[1], 0x34u);
        ASSERT_EQ(data[2], 0x56u);
        ASSERT_EQ(data[3], 0x78u);
    }

    data_type data2;
    vector_append_to_net(data2,0x10023004u);
    ASSERT_EQ(data2.size(), sizeof(uint32_t));
    ASSERT_EQ(data2[0], 0x10u);
    ASSERT_EQ(data2[1], 0x02u);
    ASSERT_EQ(data2[2], 0x30u);
    ASSERT_EQ(data2[3], 0x04u);

}
