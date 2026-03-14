#include <gtest/gtest.h>

#include <gtest/gtest.h>
#include "../src/core/config.hpp"
#include <fstream>

TEST(ConfigTest, LoadFromFile) {
    auto result = luotsi::internal::Config::load_from_file(TEST_CONFIG_PATH);
    ASSERT_TRUE(result.has_value()) << "Failed to load config: " << result.error();
    luotsi::internal::Config config = result.value();
    EXPECT_EQ(config.log_level, "debug");
    ASSERT_EQ(config.nodes.size(), 1);
    EXPECT_EQ(config.nodes[0].id, "test_node");
    EXPECT_EQ(config.nodes[0].runtime.adapter, "stdio");
    EXPECT_EQ(config.nodes[0].runtime.command, "python3");
    ASSERT_EQ(config.nodes[0].routes.size(), 1);
    EXPECT_EQ(config.nodes[0].routes[0].trigger, "test_trigger");
    EXPECT_EQ(config.nodes[0].routes[0].target, "other_node");
}
