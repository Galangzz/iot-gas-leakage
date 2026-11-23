#pragma once

#include <stdint.h>

enum class GasStatus : uint8_t
{
  WARMUP,
  SAFE,
  ALARM
};

struct GasState
{
  GasStatus status;
  bool alarmActive;
};

inline bool thresholdsValid(int thresholdOn, int thresholdOff)
{
  return thresholdOff >= 0 && thresholdOn <= 4095 && thresholdOn > thresholdOff;
}

inline GasState nextGasState(GasState prev, int avg, bool warmingUp, int thresholdOn, int thresholdOff)
{
  if (warmingUp)
    return {GasStatus::WARMUP, false};

  bool alarm = prev.alarmActive;
  if (!alarm && avg > thresholdOn)
    alarm = true;
  else if (alarm && avg < thresholdOff)
    alarm = false;

  return {alarm ? GasStatus::ALARM : GasStatus::SAFE, alarm};
}

inline const char *statusName(GasStatus status)
{
  switch (status)
  {
  case GasStatus::WARMUP:
    return "WARMUP";
  case GasStatus::ALARM:
    return "ALARM";
  default:
    return "SAFE";
  }
}
