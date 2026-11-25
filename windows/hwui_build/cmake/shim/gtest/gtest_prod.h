#pragma once
// gtest_prod.h stand-in: the friend declarations tests use; no test is built here.
#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name##_##test_name##_Test
