// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/metrics/desktop_task_switch_metric_recorder.h"

#include "ash/common/wm_shell.h"
#include "ash/shell.h"
#include "ash/wm/window_util.h"
#include "ui/wm/public/activation_client.h"

namespace ash {

DesktopTaskSwitchMetricRecorder::DesktopTaskSwitchMetricRecorder()
    : last_active_task_window_(nullptr) {
  Shell::GetInstance()->activation_client()->AddObserver(this);
}

DesktopTaskSwitchMetricRecorder::~DesktopTaskSwitchMetricRecorder() {
  Shell::GetInstance()->activation_client()->RemoveObserver(this);
}

void DesktopTaskSwitchMetricRecorder::OnWindowActivated(
    aura::client::ActivationChangeObserver::ActivationReason reason,
    aura::Window* gained_active,
    aura::Window* lost_active) {
  if (gained_active && wm::IsWindowUserPositionable(gained_active)) {
    if (last_active_task_window_ != gained_active &&
        reason == aura::client::ActivationChangeObserver::ActivationReason::
                      INPUT_EVENT) {
      WmShell::Get()->RecordUserMetricsAction(UMA_DESKTOP_SWITCH_TASK);
    }
    last_active_task_window_ = gained_active;
  }
}

}  // namespace ash
