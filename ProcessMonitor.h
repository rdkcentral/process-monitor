/*
 * Copyright 2024 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "Process.h"
#include <algorithm>
#include <condition_variable>
#include <json.hpp>
#include <list>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

class ProcessMonitor
{
public:
    ProcessMonitor();
    ~ProcessMonitor();

    ProcessMonitor(const ProcessMonitor &) = delete;
    ProcessMonitor &operator=(const ProcessMonitor &) = delete;

    bool Start();
    bool Stop();

    std::string GetJson();

private:
    bool setListenMode(bool enable) const;

    void receiveMessages();
    static void getProcessCommandLine(pid_t pid, std::string &commandLine);
    [[nodiscard]] static pid_t getParentPid(pid_t pid);
    void setSocketFilter() const;

    std::string getSystemdService(pid_t pid);

    void mergeExitHandlerData();

    bool setupMemPreload();
    void teardownMemPreload();

private:
    int mSocket;
    bool mListen;
    long mPageSize;

    bool mValid;

    nlohmann::json mProcessJson;

    std::thread mMessageReceiver;

    std::vector<processInfo> mRunningProcesses;
    std::vector<processInfo> mExitedProcesses;

    std::chrono::time_point<std::chrono::system_clock> mStart;
    std::chrono::time_point<std::chrono::system_clock> mEnd;

    bool mMemPreloadActive = false;
    bool mExitHandlerDataMerged = false;
};