#include <unity.h>
#include "gas_logic.h"

void setUp() {}
void tearDown() {}

void test_thresholds_valid_accepts_default() {
    TEST_ASSERT_TRUE(thresholdsValid(3350, 3300));
}

void test_thresholds_rejects_equal() {
    TEST_ASSERT_FALSE(thresholdsValid(3300, 3300));
}

void test_thresholds_rejects_inverted() {
    TEST_ASSERT_FALSE(thresholdsValid(3300, 3350));
}

void test_thresholds_rejects_out_of_range() {
    TEST_ASSERT_FALSE(thresholdsValid(4096, 4000));
    TEST_ASSERT_FALSE(thresholdsValid(3350, -1));
}

void test_alarm_turns_on_above_threshold_on() {
    GasState s = nextGasState({GasStatus::SAFE, false}, 3360, false, 3350, 3300);
    TEST_ASSERT_TRUE(s.alarmActive);
    TEST_ASSERT_TRUE(s.status == GasStatus::ALARM);
}

void test_alarm_stays_on_between_thresholds() {
    GasState s = nextGasState({GasStatus::ALARM, true}, 3340, false, 3350, 3300);
    TEST_ASSERT_TRUE(s.alarmActive);
}

void test_alarm_turns_off_below_threshold_off() {
    GasState s = nextGasState({GasStatus::ALARM, true}, 3290, false, 3350, 3300);
    TEST_ASSERT_FALSE(s.alarmActive);
    TEST_ASSERT_TRUE(s.status == GasStatus::SAFE);
}

void test_alarm_ignores_boundary_values() {
    GasState on = nextGasState({GasStatus::SAFE, false}, 3350, false, 3350, 3300);
    TEST_ASSERT_FALSE(on.alarmActive);
    GasState off = nextGasState({GasStatus::ALARM, true}, 3300, false, 3350, 3300);
    TEST_ASSERT_TRUE(off.alarmActive);
}

void test_warmup_suppresses_alarm() {
    GasState s = nextGasState({GasStatus::ALARM, true}, 4000, true, 3350, 3300);
    TEST_ASSERT_FALSE(s.alarmActive);
    TEST_ASSERT_TRUE(s.status == GasStatus::WARMUP);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_thresholds_valid_accepts_default);
    RUN_TEST(test_thresholds_rejects_equal);
    RUN_TEST(test_thresholds_rejects_inverted);
    RUN_TEST(test_thresholds_rejects_out_of_range);
    RUN_TEST(test_alarm_turns_on_above_threshold_on);
    RUN_TEST(test_alarm_stays_on_between_thresholds);
    RUN_TEST(test_alarm_turns_off_below_threshold_off);
    RUN_TEST(test_alarm_ignores_boundary_values);
    RUN_TEST(test_warmup_suppresses_alarm);
    return UNITY_END();
}
