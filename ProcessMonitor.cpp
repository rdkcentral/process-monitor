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

#define _XOPEN_SOURCE 700

#include "ProcessMonitor.h"
#include "Log.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <fstream>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <cstring>

#define OP_LDH (BPF_LD | BPF_H | BPF_ABS)
#define OP_LDB (BPF_LD | BPF_B | BPF_ABS)
#define OP_LDW (BPF_LD | BPF_W | BPF_ABS)
#define OP_JEQ (BPF_JMP | BPF_JEQ | BPF_K)
#define OP_RET (BPF_RET | BPF_K)
#define BPF_ALLOW 0xffffffff
#define BPF_DENY 0

#define MOUNT_DIR "/media/apps"
#define ETC_DIR "/etc"

extern bool gCaptureMemData;
extern std::string gMemPreloadLib;

/**
 * @brief Recursively remove all files and subdirectories in a directory
 * 
 * @param path Path to the directory to clear
 * @return true if successful, false otherwise
 */
static bool clearDirectory(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == nullptr)
    {
        if (errno == ENOENT)
        {
            // Directory doesn't exist, nothing to clear
            Log("Directory %s does not exist, nothing to clear", path);
            return true;
        }
        Log("Failed to open directory %s: %s", path, strerror(errno));
        return false;
    }

    struct dirent *entry;
    bool success = true;

    while ((entry = readdir(dir)) != nullptr)
    {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        char fullPath[PATH_MAX];
        int ret = snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(fullPath))
        {
            Log("Path too long: %s/%s", path, entry->d_name);
            success = false;
            continue;
        }

        struct stat statbuf;
        if (lstat(fullPath, &statbuf) != 0)
        {
            Log("Failed to stat %s: %s", fullPath, strerror(errno));
            success = false;
            continue;
        }

        if (S_ISDIR(statbuf.st_mode))
        {
            // Recursively clear and remove subdirectory
            if (!clearDirectory(fullPath))
            {
                success = false;
                continue;
            }
            if (rmdir(fullPath) != 0)
            {
                Log("Failed to remove directory %s: %s", fullPath, strerror(errno));
                success = false;
            }
        }
        else
        {
            // Remove file
            if (unlink(fullPath) != 0)
            {
                Log("Failed to remove file %s: %s", fullPath, strerror(errno));
                success = false;
            }
        }
    }

    closedir(dir);
    return success;
}

/**
 * @brief Create a directory with all parent directories
 * 
 * @param path Path to the directory to create
 * @return true if successful, false otherwise
 */
static bool createDirectory(const char *path)
{
    char tmp[PATH_MAX];
    char *p = nullptr;
    size_t len;

    int ret = snprintf(tmp, sizeof(tmp), "%s", path);
    if (ret < 0 || ret >= (int)sizeof(tmp))
    {
        Log("Path too long: %s", path);
        return false;
    }

    len = strlen(tmp);
    if (tmp[len - 1] == '/')
    {
        tmp[len - 1] = 0;
    }

    for (p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            if (mkdir(tmp, 0755) != 0)
            {
                if (errno != EEXIST)
                {
                    Log("Failed to create directory %s: %s", tmp, strerror(errno));
                    return false;
                }
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0)
    {
        if (errno != EEXIST)
        {
            Log("Failed to create directory %s: %s", tmp, strerror(errno));
            return false;
        }
    }

    Log("Created directory: %s", path);
    return true;
}

/**
 * @brief Recursively copy a directory and its contents
 * 
 * @param src Source directory path
 * @param dest Destination directory path
 * @return true if successful, false otherwise
 */
static bool copyDirectory(const char *src, const char *dest)
{
    DIR *dir = opendir(src);
    if (dir == nullptr)
    {
        Log("Failed to open source directory %s: %s", src, strerror(errno));
        return false;
    }

    // Create destination directory
    if (!createDirectory(dest))
    {
        closedir(dir);
        return false;
    }

    struct dirent *entry;
    bool success = true;

    while ((entry = readdir(dir)) != nullptr)
    {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        char srcPath[PATH_MAX];
        char destPath[PATH_MAX];

        int ret = snprintf(srcPath, sizeof(srcPath), "%s/%s", src, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(srcPath))
        {
            Log("Source path too long: %s/%s", src, entry->d_name);
            success = false;
            continue;
        }

        ret = snprintf(destPath, sizeof(destPath), "%s/%s", dest, entry->d_name);
        if (ret < 0 || ret >= (int)sizeof(destPath))
        {
            Log("Destination path too long: %s/%s", dest, entry->d_name);
            success = false;
            continue;
        }

        struct stat statbuf;
        if (lstat(srcPath, &statbuf) != 0)
        {
            Log("Failed to stat %s: %s", srcPath, strerror(errno));
            success = false;
            continue;
        }

        if (S_ISDIR(statbuf.st_mode))
        {
            // Recursively copy subdirectory
            if (!copyDirectory(srcPath, destPath))
            {
                success = false;
            }
        }
        else if (S_ISLNK(statbuf.st_mode))
        {
            // Copy symlink
            char linkTarget[PATH_MAX];
            ssize_t linkLen = readlink(srcPath, linkTarget, sizeof(linkTarget) - 1);
            if (linkLen < 0)
            {
                Log("Failed to read symlink %s: %s", srcPath, strerror(errno));
                success = false;
                continue;
            }
            linkTarget[linkLen] = '\0';

            if (symlink(linkTarget, destPath) != 0)
            {
                Log("Failed to create symlink %s -> %s: %s", destPath, linkTarget, strerror(errno));
                success = false;
            }
        }
        else if (S_ISREG(statbuf.st_mode))
        {
            // Copy regular file
            int srcFd = open(srcPath, O_RDONLY | O_CLOEXEC);
            if (srcFd < 0)
            {
                Log("Failed to open source file %s: %s", srcPath, strerror(errno));
                success = false;
                continue;
            }

            int destFd = open(destPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, statbuf.st_mode);
            if (destFd < 0)
            {
                Log("Failed to open destination file %s: %s", destPath, strerror(errno));
                close(srcFd);
                success = false;
                continue;
            }

            char buffer[8192];
            ssize_t bytesRead;
            bool copySuccess = true;

            while ((bytesRead = read(srcFd, buffer, sizeof(buffer))) > 0)
            {
                ssize_t bytesWritten = write(destFd, buffer, bytesRead);
                if (bytesWritten != bytesRead)
                {
                    Log("Failed to write to %s: %s", destPath, strerror(errno));
                    copySuccess = false;
                    success = false;
                    break;
                }
            }

            if (bytesRead < 0)
            {
                Log("Failed to read from %s: %s", srcPath, strerror(errno));
                success = false;
            }

            close(srcFd);
            close(destFd);

            if (!copySuccess)
            {
                continue;
            }

            // Preserve timestamps
            struct timespec times[2];
            times[0] = statbuf.st_atim;
            times[1] = statbuf.st_mtim;
            if (utimensat(AT_FDCWD, destPath, times, 0) != 0)
            {
                Log("Warning: Failed to preserve timestamps for %s: %s", destPath, strerror(errno));
            }
        }
    }

    closedir(dir);

    if (success)
    {
        Log("Successfully copied directory %s to %s", src, dest);
    }

    return success;
}

/**
 * @brief Write content to a file
 * 
 * @param path Path to the file
 * @param content Content to write
 * @param append If true, append to file; if false, overwrite
 * @return true if successful, false otherwise
 */
static bool writeFile(const char *path, const char *content, bool append = false)
{
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    if (append)
    {
        flags |= O_APPEND;
    }
    else
    {
        flags |= O_TRUNC;
    }

    int fd = open(path, flags, 0644);
    if (fd < 0)
    {
        Log("Failed to open file %s for writing: %s", path, strerror(errno));
        return false;
    }

    size_t contentLen = strlen(content);
    ssize_t bytesWritten = write(fd, content, contentLen);

    if (bytesWritten < 0)
    {
        Log("Failed to write to file %s: %s", path, strerror(errno));
        close(fd);
        return false;
    }

    if ((size_t)bytesWritten != contentLen)
    {
        Log("Incomplete write to file %s: wrote %zd of %zu bytes", path, bytesWritten, contentLen);
        close(fd);
        return false;
    }

    if (fsync(fd) != 0)
    {
        Log("Warning: Failed to sync file %s: %s", path, strerror(errno));
    }

    close(fd);
    Log("Successfully wrote to file: %s", path);
    return true;
}

static bool isSamePathTarget(const char *pathA, const char *pathB)
{
    struct stat statA;
    struct stat statB;

    if (stat(pathA, &statA) != 0 || stat(pathB, &statB) != 0)
    {
        return false;
    }

    return (statA.st_dev == statB.st_dev) && (statA.st_ino == statB.st_ino);
}

/**
 * References:
 * * https://nick-black.com/dankwiki/index.php/The_Proc_Connector_and_Socket_Filters
 * * https://bewareofgeek.livejournal.com/2945.html
 */
ProcessMonitor::ProcessMonitor() : mSocket(0), mListen(false), mValid(false)
{
    mPageSize = sysconf(_SC_PAGESIZE);

    mSocket = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if (mSocket < 0)
    {
        Log("Failed to open netlink socket");
        return;
    }

    struct sockaddr_nl sa = {};
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = CN_IDX_PROC;
    sa.nl_pid = getpid();

    if (bind(mSocket, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        Log("Failed to bind netlink socket with error '%s'", strerror(errno));
        close(mSocket);
        mSocket = 0;
        return;
    }

    setSocketFilter();

    mValid = true;
}

ProcessMonitor::~ProcessMonitor()
{
    if (mListen)
    {
        Stop();
    }

    if (mSocket > 0)
    {
        if (close(mSocket) < 0)
        {
            Log("Failed to close socket with error '%s'", strerror(errno));
        }
    }
}

/**
 * @brief Start a thread to capture process exec/exit. Returns as soon as thread started, non-blocking
 *
 * @return True if capture thread started
 */
bool ProcessMonitor::Start()
{
    if (!mValid)
    {
        return false;
    }

    bool overlayJustActivated = false;
    if (gCaptureMemData && !mMemPreloadActive)
    {
        if (unlink("/tmp/exitHandler.txt") != 0 && errno != ENOENT)
        {
            Log("Warning: failed to clear /tmp/exitHandler.txt: %s", strerror(errno));
        }

        if (!setupMemPreload())
        {
            Log("Failed to set up memory preload");
            return false;
        }
        overlayJustActivated = true;
    }

    mStart = std::chrono::system_clock::now();
    mListen = setListenMode(true);

    if (mListen)
    {
        mMessageReceiver = std::thread(&ProcessMonitor::receiveMessages, this);

        if (mMessageReceiver.joinable())
        {
            return true;
        }

        Log("Message receiver thread failed to start");
        mListen = false;
    }
    else
    {
        Log("Failed to enable listen mode");
    }

    if (overlayJustActivated)
    {
        teardownMemPreload();
    }

    return false;
}

/**
 * Stop the currently running capture thread
 *
 * @return True if successfully stopped
 */
bool ProcessMonitor::Stop()
{
    mListen = false;
    mEnd = std::chrono::system_clock::now();
    if (mMessageReceiver.joinable())
    {
        mMessageReceiver.join();
    }

    setListenMode(false);
    teardownMemPreload();

    return true;
}

/**
 * @brief Enables kernel-level filtering on the socket with BPF to reduce userspace CPU load by only receiving
 * events we actually care about
 */
void ProcessMonitor::setSocketFilter() const
{
    // Best practice is to block everything first, then drain the socket completely. This prevents unwanted events
    // making it through whilst we're building and applying our filter
    // https://natanyellin.com/posts/ebpf-filtering-done-right/

    // Start by blocking all events
    struct sock_filter blockAll[] = {
        BPF_STMT(OP_RET, BPF_DENY),
    };

    struct sock_fprog blockAllProgram = {.len = 1, .filter = blockAll};

    if (setsockopt(mSocket, SOL_SOCKET, SO_ATTACH_FILTER, &blockAllProgram, sizeof blockAllProgram) < 0)
    {
        Log("Failed to set socket options with error %s", strerror(errno));
    }

    // Drain the socket
    char dummy[1];
    while (true)
    {
        ssize_t bytes = recv(mSocket, dummy, sizeof(dummy), MSG_DONTWAIT);
        if (bytes == -1)
        {
            break;
        }
    }

    // Write the actual filter we want to apply
    // This BPF filter allows exec/exit messages through only
    struct sock_filter processFilter[] =
        {BPF_STMT(OP_LDH, offsetof(struct nlmsghdr, nlmsg_type)),
         BPF_JUMP(OP_JEQ, htons(NLMSG_DONE), 1, 0), // Only allow complete netlink messages (block noop, error etc)
         BPF_STMT(OP_RET, BPF_DENY),
         BPF_STMT(OP_LDW, NLMSG_LENGTH(0) + offsetof(struct cn_msg, id) + offsetof(struct cb_id, idx)),
         BPF_JUMP(OP_JEQ, htonl(CN_IDX_PROC | CN_VAL_PROC), 1, 0), // Only allow messages from the process communicator
         BPF_STMT(OP_RET, BPF_DENY),
         BPF_STMT(OP_LDW, NLMSG_LENGTH(0) + offsetof(struct cn_msg, data) + offsetof(struct proc_event, what)),
         BPF_JUMP(OP_JEQ, htonl(proc_event::PROC_EVENT_EXEC), 2, 0), // Only allow exec/exit
         BPF_JUMP(OP_JEQ, htonl(proc_event::PROC_EVENT_EXIT), 1, 0),
         BPF_STMT(OP_RET, BPF_DENY),
         BPF_STMT(OP_RET, BPF_ALLOW)};

    struct sock_fprog processFilterProgram = {};
    processFilterProgram.len = sizeof processFilter / sizeof processFilter[0];
    processFilterProgram.filter = processFilter;

    if (setsockopt(mSocket, SOL_SOCKET, SO_ATTACH_FILTER, &processFilterProgram, sizeof processFilterProgram) < 0)
    {
        Log("Failed to set socket options with error %s", strerror(errno));
    }
}

/**
 * Sets the listening mode on the netlink socket to start/stop receiving events
 *
 * @param enable Whether to receive events over the netlink socket
 *
 * @return True if the listen mode was set successfully
 */
bool ProcessMonitor::setListenMode(bool enable) const
{
    struct iovec iov[3];

    char messageBuffer[NLMSG_LENGTH(0)];

    auto *messageHeader = reinterpret_cast<nlmsghdr *>(messageBuffer);
    struct cn_msg connectorMessage = {};
    enum proc_cn_mcast_op op;

    messageHeader->nlmsg_len = NLMSG_LENGTH(sizeof(connectorMessage) + sizeof(op));
    messageHeader->nlmsg_type = NLMSG_DONE;
    messageHeader->nlmsg_flags = 0;
    messageHeader->nlmsg_seq = 0;
    messageHeader->nlmsg_pid = getpid();

    iov[0].iov_base = messageBuffer;
    iov[0].iov_len = NLMSG_LENGTH(0);

    connectorMessage.id.idx = CN_IDX_PROC;
    connectorMessage.id.val = CN_VAL_PROC;
    connectorMessage.seq = 0;
    connectorMessage.ack = 0;
    connectorMessage.len = sizeof op;

    iov[1].iov_base = &connectorMessage;
    iov[1].iov_len = sizeof connectorMessage;

    if (enable)
    {
        op = PROC_CN_MCAST_LISTEN;
    }
    else
    {
        op = PROC_CN_MCAST_IGNORE;
    }

    iov[2].iov_base = &op;
    iov[2].iov_len = sizeof(op);

    if (writev(mSocket, iov, 3) < 0)
    {
        Log("Failed to set listen mode to %d with error '%s'", enable, strerror(errno));
        return false;
    }
    else
    {
        return true;
    }
}

/**
 * Loop to process incoming netlink messages and track process exec/exit
 */
void ProcessMonitor::receiveMessages()
{
    char buffer[mPageSize];

    struct msghdr header = {};
    struct sockaddr_nl address = {};

    struct iovec iov[1];
    iov[0].iov_base = buffer;
    iov[0].iov_len = sizeof buffer;

    header.msg_name = &address;
    header.msg_namelen = sizeof address;
    header.msg_iov = iov;
    header.msg_iovlen = 1;
    header.msg_control = nullptr;
    header.msg_controllen = 0;
    header.msg_flags = 0;

    while (mListen)
    {
        ssize_t len = TEMP_FAILURE_RETRY(recvmsg(mSocket, &header, 0));

        if (len < 0)
        {
            Log("Failed to receive message with error %d", errno);
        }

        if (address.nl_pid != 0)
        {
            continue;
        }

        // Got a message
        for (auto *message = reinterpret_cast<struct nlmsghdr *>(buffer); NLMSG_OK(message, len);
             message = NLMSG_NEXT(message, len))
        {
            auto *connectorMessage = static_cast<cn_msg *>(NLMSG_DATA(message));

            // We shouldn't need to worry about this anymore, since we filter with BPF but keep just in case I've
            // messed up the bpf filter
            if (connectorMessage->id.idx != CN_IDX_PROC || connectorMessage->id.val != CN_VAL_PROC)
            {
                // Ignore messages not from the connector subsystem
                continue;
            }

            // Got something we actually care about
            auto *ev = reinterpret_cast<struct proc_event *>(connectorMessage->data);

            switch (ev->what)
            {
            case proc_event::PROC_EVENT_EXEC:
            {
                // Process has started
                processInfo info{};

                info.pid = ev->event_data.exec.process_pid;
                info.parentPid = getParentPid(info.pid);
                info.grandparentPid = getParentPid(info.parentPid);

                // This isn't going to be 100% accurate but close enough
                info.startTime = std::chrono::system_clock::now();

                getProcessCommandLine(info.pid, info.commandLine);
                getProcessCommandLine(info.parentPid, info.parentCommandLine);
                getProcessCommandLine(info.grandparentPid, info.grandparentCommandLine);

                info.systemdServiceName = getSystemdService(info.pid);

                mRunningProcesses.emplace_back(info);
                break;
            }
            case proc_event::PROC_EVENT_EXIT:
            {
                // Process has exited

                pid_t pid = ev->event_data.exit.process_pid;

                auto process = std::find_if(mRunningProcesses.begin(), mRunningProcesses.end(),
                                            [pid](const processInfo &pi) { return pi.pid == pid; });

                // Are we tracking this process?
                if (process != mRunningProcesses.end())
                {
                    auto &p = (*process);

                    // Update process with the exit time and code
                    p.endTime = std::chrono::system_clock::now();
                    p.exitCode = ev->event_data.exit.exit_code;

                    // Move process from running vector to exited vector
                    mExitedProcesses.insert(mExitedProcesses.end(), std::make_move_iterator(process),
                                            std::make_move_iterator(std::next(process)));
                    mRunningProcesses.erase(process);
                }

                break;
            }
            default:
                break;
            }
        }
    }
}

/**
 * Given a PID, retrieve the command line of the processes from /proc/x/cmdline, correctly parsing it into a std string
 *
 * Commandline will be "Unknown" if this fails - can happen for extremely short-lived processes that have quit by the
 * time we get here
 *
 * @param pid PID to get command line for
 * @param[out] commandLine command line of the process.
 */
void ProcessMonitor::getProcessCommandLine(pid_t pid, std::string &commandLine)
{
    char procPath[PATH_MAX];

    sprintf(procPath, "/proc/%u/cmdline", pid);

    int fd = open(procPath, O_CLOEXEC | O_RDONLY);
    if (fd > 0)
    {
        char cmdline[PATH_MAX];

        ssize_t ret = read(fd, cmdline, PATH_MAX);
        if (ret > 0)
        {
            // Command line contains full string with args
            commandLine.assign(cmdline, ret);

            // Replace null chars with spaces
            std::replace(commandLine.begin(), std::prev(commandLine.end()), '\0', ' ');
            commandLine.erase(std::remove(std::prev(commandLine.end()), commandLine.end(), '\0'), commandLine.end());
        }
        else
        {
            // Failed to read from file, process has probably quit
            commandLine = "Unknown";
        }

        close(fd);
    }
    else
    {
        // Can't read cmdline file, process has probably quit
        commandLine = "Unknown";
    }
}

/**
 * Given a PID, return the PID of the parent process
 *
 * @param pid PID to find parent pid of
 * @return parent pid - 0 if unknown
 */
pid_t ProcessMonitor::getParentPid(pid_t pid)
{
    char procPath[PATH_MAX];

    sprintf(procPath, "/proc/%u/stat", pid);

    FILE *f = fopen(procPath, "r");
    if (f != nullptr)
    {
        pid_t parentPid;
        if (fscanf(f, "%*d %*s %*c %d", &parentPid) > 0)
        {
            fclose(f);
            return parentPid;
        }
        else
        {
            return 0;
        }
    }

    return 0;
}

/**
 * Build a JSON document containing the results, formatted in a suitable way for a vis.js dataset that can be displayed
 * in a timeline
 *
 * @return JSON string
 */
std::string ProcessMonitor::GetJson()
{
    if (gCaptureMemData) {
        mergeExitHandlerData();
    }
    Log("Generating process JSON");

    // Convert this into a form that vis.js timeline can understand
    nlohmann::json results;

    std::set<std::string> groups;
    std::set<std::string> systemdServices;

    nlohmann::json process;
    std::map<std::string, int> processExecutionCount;

    std::unordered_map<std::string, int> systemdServiceCount;

    for (auto &p : mExitedProcesses)
    {
        if (!p.commandLine.empty() && p.commandLine != "Unknown" && !p.parentCommandLine.empty() &&
            p.parentCommandLine != "Unknown")
        {
            process.clear();

            // Can't just use PID on its own as the id, since Linux will re-use PIDs eventually
            // Instead, id is built from start time and pid
            process["id"] = std::to_string(p.pid) + "_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(p.startTime.time_since_epoch()).count());
            process["pid"] = p.pid;
            process["content"] = p.GetStrippedName();
            process["title"] = p.GetStrippedCommandLine();
            process["group"] = p.GetGroupName();
            process["start"] =
                std::chrono::duration_cast<std::chrono::milliseconds>(p.startTime.time_since_epoch()).count();
            process["end"] = std::chrono::duration_cast<std::chrono::milliseconds>(p.endTime.time_since_epoch()).count();
            process["fullCommandLine"] = p.commandLine;
            process["parentCommandLine"] = p.parentCommandLine;
            process["grandparentCommandLine"] = p.grandparentCommandLine;
            process["exitCode"] = p.exitCode;
            process["systemdService"] = p.systemdServiceName;

            // Add memory statistics if available
            if (p.pss.has_value())
            {
                process["pss"] = *p.pss;
            }
            if (p.swapPss.has_value())
            {
                process["swapPss"] = *p.swapPss;
            }
            if (p.rss.has_value())
            {
                process["rss"] = *p.rss;
            }
            if (p.utime.has_value())
            {
                process["utime"] = *p.utime;
            }
            if (p.stime.has_value())
            {
                process["stime"] = *p.stime;
            }

            results["processes"].emplace_back(process);

            groups.insert(process["group"]);
            systemdServices.insert(p.systemdServiceName);

            processExecutionCount[p.GetStrippedName()] += 1;

            systemdServiceCount[p.systemdServiceName] += 1;
        }
    }

    for (const auto &g : groups)
    {
        nlohmann::json group;
        group["id"] = g;
        group["content"] = g;

        results["groups"].emplace_back(group);
    }

    for (const auto &freq : processExecutionCount)
    {
        nlohmann::json stats;
        stats["process"] = freq.first;
        stats["frequency"] = freq.second;

        results["stats"]["processes"].emplace_back(stats);
    }

    for (const auto &service : systemdServiceCount)
    {
        nlohmann::json stats;
        stats["serviceName"] = service.first;
        stats["frequency"] = service.second;

        results["stats"]["services"].emplace_back(stats);
    }

    results["start"] = std::chrono::duration_cast<std::chrono::milliseconds>(mStart.time_since_epoch()).count();
    results["end"] = std::chrono::duration_cast<std::chrono::milliseconds>(mEnd.time_since_epoch()).count();

    return results.dump();
}

/**
 * Given a PID, attempts to work out which systemd service it belongs to
 *
 * Will return "None" if it does not belong to a service
 * @param pid
 * @return
 */
std::string ProcessMonitor::getSystemdService(pid_t pid)
{
    char cgroupPath[PATH_MAX]{};
    sprintf(cgroupPath, "/proc/%u/cgroup", pid);

    std::ifstream cgroupFile(cgroupPath);

    if (!cgroupFile)
    {
        return "None";
    }

    std::string line;
    char serviceName[256]{};
    while (std::getline(cgroupFile, line))
    {
        // Example: 1:name=systemd:/system.slice/ip-setup-monitor.service
        if (sscanf(line.c_str(), "%*d:name=systemd:/system.slice/%255s", serviceName) > 0)
        {
            return {serviceName};
        }
    }

    return "None";
}

bool ProcessMonitor::setupMemPreload()
{
    if (!gCaptureMemData)
    {
        return true;
    }
    if (!mValid)
    {
        Log("ProcessMonitor is not valid; skipping memory preload setup");
        return false;
    }
    if (gMemPreloadLib.empty())
    {
        Log("Memory preload requested but no library path provided");
        return false;
    }

    Log("Preparing bind-mounted %s for memory preload", ETC_DIR);

    char mountEtcPath[PATH_MAX];
    int ret = snprintf(mountEtcPath, sizeof(mountEtcPath), "%s%s", MOUNT_DIR, ETC_DIR);
    Log("Mount etc path: %s", mountEtcPath);
    if (ret < 0 || ret >= (int)sizeof(mountEtcPath))
    {
        Log("Mount path too long");
        return false;
    }

    if (isSamePathTarget(ETC_DIR, mountEtcPath))
    {
        Log("Detected existing bind mount from %s to %s; attempting recovery", mountEtcPath, ETC_DIR);
        int staleUmountRc = std::system("/bin/umount /etc");
        if (staleUmountRc != 0)
        {
            if (staleUmountRc == -1)
            {
                Log("Failed to execute stale mount recovery umount: %s", strerror(errno));
            }
            else
            {
                Log("Stale mount recovery umount returned non-zero status: %d", staleUmountRc);
            }
            Log("Refusing to proceed while %s is still bind-mounted", ETC_DIR);
            return false;
        }
        Log("Recovered stale bind mount on %s", ETC_DIR);
    }

    // Clear existing mount directory
    if (!clearDirectory(mountEtcPath))
    {
        Log("Failed to clear %s", mountEtcPath);
        // Continue anyway as directory might not exist
    }

    // Remove the directory itself if it exists
    if (rmdir(mountEtcPath) != 0 && errno != ENOENT)
    {
        Log("Warning: Failed to remove %s: %s", mountEtcPath, strerror(errno));
    }

    // Create mount directory
    if (!createDirectory(mountEtcPath))
    {
        Log("Failed to create %s", mountEtcPath);
        return false;
    }
    Log("Successfully created mount directory: %s", mountEtcPath);

    // Copy /etc to mount directory
    if (!copyDirectory(ETC_DIR, mountEtcPath))
    {
        Log("Failed to copy %s to %s", ETC_DIR, mountEtcPath);
        clearDirectory(mountEtcPath);
        rmdir(mountEtcPath);
        return false;
    }
    Log("Successfully copied %s to %s", ETC_DIR, mountEtcPath);

    // Bind mount using mount-copybind
    char mountCmd[PATH_MAX * 2 + 64];
    ret = snprintf(mountCmd, sizeof(mountCmd), "/sbin/mount-copybind %s %s", mountEtcPath, ETC_DIR);
    if (ret < 0 || ret >= (int)sizeof(mountCmd))
    {
        Log("Mount command too long");
        clearDirectory(mountEtcPath);
        rmdir(mountEtcPath);
        return false;
    }

    Log("Running mount command: %s", mountCmd);
    int rc = std::system(mountCmd);
    if (rc != 0)
    {
        if (rc == -1)
        {
            Log("Failed to execute mount command: %s", strerror(errno));
        }
        else
        {
            Log("Mount command returned non-zero status: %d", rc);
        }
        clearDirectory(mountEtcPath);
        rmdir(mountEtcPath);
        return false;
    }
    Log("Successfully bind-mounted %s onto %s", mountEtcPath, ETC_DIR);

    // Write preload library to /etc/ld.so.preload
    char preloadPath[PATH_MAX];
    ret = snprintf(preloadPath, sizeof(preloadPath), "%s/ld.so.preload", ETC_DIR);
    if (ret < 0 || ret >= (int)sizeof(preloadPath))
    {
        Log("Preload path too long");
        // Cleanup: unmount before returning
        if (int umountRc = std::system("/bin/umount /etc"); umountRc != 0)
        {
            Log("Failed to umount %s during cleanup: status=%d", ETC_DIR, umountRc);
        }
        clearDirectory(mountEtcPath);
        rmdir(mountEtcPath);
        return false;
    }

    std::string preloadContent = gMemPreloadLib + "\n";
    if (!writeFile(preloadPath, preloadContent.c_str(), false))
    {
        Log("Failed to write preload library to %s", preloadPath);
        // Cleanup: unmount before returning
        if (int umountRc = std::system("/bin/umount /etc"); umountRc != 0)
        {
            Log("Failed to umount %s during cleanup: status=%d", ETC_DIR, umountRc);
        }
        clearDirectory(mountEtcPath);
        rmdir(mountEtcPath);
        return false;
    }

    mMemPreloadActive = true;
    Log("Memory preload enabled via %s", gMemPreloadLib.c_str());
    return true;
}

void ProcessMonitor::teardownMemPreload()
{
    if (!mMemPreloadActive)
    {
        return;
    }

    Log("Tearing down bind-mounted %s for memory preload", ETC_DIR);

    // Clear /etc/ld.so.preload while overlay is still mounted.
    char preloadPath[PATH_MAX];
    int ret = snprintf(preloadPath, sizeof(preloadPath), "%s/ld.so.preload", ETC_DIR);
    if (ret < 0 || ret >= (int)sizeof(preloadPath))
    {
        Log("Preload path too long");
    }
    else
    {
        if (!writeFile(preloadPath, "", false))
        {
            Log("Warning: failed to clear %s", preloadPath);
        }
    }

    // Unmount /etc
    Log("Unmounting %s", ETC_DIR);
    int umountRc = std::system("/bin/umount /etc");
    if (umountRc != 0)
    {
        if (umountRc == -1)
        {
            Log("Failed to execute umount command: %s", strerror(errno));
        }
        else
        {
            Log("Umount command returned non-zero status: %d", umountRc);
        }
        Log("Manual cleanup required for %s", ETC_DIR);
        return;
    }
    Log("Successfully unmounted %s", ETC_DIR);

    // Clean up mount directory
    char mountEtcPath[PATH_MAX];
        int mountPathRet = snprintf(mountEtcPath, sizeof(mountEtcPath), "%s/etc", MOUNT_DIR);
    if (mountPathRet < 0 || mountPathRet >= (int)sizeof(mountEtcPath))
    {
        Log("Mount path too long");
        mMemPreloadActive = false;
        return;
    }

    // Clear out the mount directory
    if (!clearDirectory(mountEtcPath))
    {
        Log("Failed to clear %s; manual cleanup required", mountEtcPath);
    }

    if (rmdir(mountEtcPath) != 0)
    {
        Log("Failed to remove %s: %s; manual cleanup required", mountEtcPath, strerror(errno));
    }
    else
    {
        Log("Successfully removed %s", mountEtcPath);
    }

    mMemPreloadActive = false;
    Log("Memory preload teardown complete");
}

void ProcessMonitor::mergeExitHandlerData()
{
    if (mExitHandlerDataMerged)
    {
        return;
    }

    std::ifstream file("/tmp/exitHandler.txt");
    if (!file.is_open())
    {
        Log("Could not open /tmp/exitHandler.txt for reading (may not exist yet)");
        mExitHandlerDataMerged = true;
        return;
    }

    Log("Parsing exit handler data from /tmp/exitHandler.txt");

    // Map to store exit handler entries by PID
    // Key: PID, Value: struct with timestamp and memory stats
    struct ExitHandlerEntry
    {
        std::chrono::time_point<std::chrono::system_clock> timestamp;
        std::string processName;
        unsigned long utime, stime, rss, pss, swapPss;
    };

    // Map: PID -> vector of entries (handle PID reuse)
    std::map<pid_t, std::vector<ExitHandlerEntry>> exitHandlerMap;

    std::string line;
    int parsedCount = 0;
    int errorCount = 0;

    while (std::getline(file, line))
    {
        if (line.empty())
        {
            continue;
        }

        // Parse format: "timestamp: pid processName utime stime rss, pss, swappss"
        // Example: "2024_01_15 10_30_45: 1234 /bin/ls 100 50 4096, 2048, 512"
        
        // Skip error format lines: "pid errno"
        if (line.find(':') == std::string::npos)
        {
            // Try error format: "pid errno"
            pid_t pid;
            int err;
            if (sscanf(line.c_str(), "%d %d", &pid, &err) == 2)
            {
                Log("Exit handler error entry: pid %d, errno %d", pid, err);
                errorCount++;
            }
            continue;
        }

        // Parse format: "timestamp: pid processName utime stime rss, pss, swappss"
        size_t colonPos = line.find(':');
        std::string timestampStr = line.substr(0, colonPos);
        std::string dataStr = line.substr(colonPos + 1);

        // Parse timestamp (format: "YYYY_MM_DD HH_MM_SS")
        struct tm tm = {};
        if (strptime(timestampStr.c_str(), "%Y_%m_%d %H_%M_%S", &tm) == nullptr)
        {
            Log("Failed to parse timestamp: %s", timestampStr.c_str());
            continue;
        }

        tm.tm_isdst = -1;

        auto timestamp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

        // Parse data: " pid processName utime stime rss, pss, swappss"
        pid_t pid;
        char processName[256];
        unsigned long utime, stime, rss, pss, swapPss;

        if (sscanf(dataStr.c_str(), " %d %255s %lu %lu %lu, %lu, %lu", &pid, processName, &utime, &stime, &rss, &pss, &swapPss) == 7)
        {
            ExitHandlerEntry entry;
            entry.timestamp = timestamp;
            entry.processName = processName;
            entry.utime = utime;
            entry.stime = stime;
            entry.rss = rss;
            entry.pss = pss;
            entry.swapPss = swapPss;

            exitHandlerMap[pid].push_back(entry);
            parsedCount++;
        }
        else
        {
            Log("Failed to parse exit handler data line: %s", line.c_str());
        }
    }
    file.close();
    if (file.bad())
    {
        Log("Error occurred while reading /tmp/exitHandler.txt");
    }
    Log("Parsed %d exit handler entries (%d errors)", parsedCount, errorCount);

    // Merge into mExitedProcesses
    int matchedCount = 0;

    for (auto& process : mExitedProcesses)
    {
        auto it = exitHandlerMap.find(process.pid);
        if (it == exitHandlerMap.end() || it->second.empty())
        {
            continue;
        }
        auto& entries = it->second;

        // Find entry with nearest timestamp to process.endTime
        ExitHandlerEntry* bestMatch = nullptr;
        auto minTimeDiff = std::chrono::seconds::max(); // Start with large value

        for (auto& entry : entries)
        {
            auto timeDiff = std::chrono::duration_cast<std::chrono::seconds>(
                process.endTime > entry.timestamp ?
                process.endTime - entry.timestamp :
                entry.timestamp - process.endTime);

            if (timeDiff < minTimeDiff)
            {
                minTimeDiff = timeDiff;
                bestMatch = &entry;
            }
        }

        if (bestMatch == nullptr)
        {
            continue;
        }

        // Optional name validation for large timestamp differences (fallback check)
        if (minTimeDiff > std::chrono::seconds(10))
        {
            std::string processBasename = process.GetStrippedName();
            std::string entryBasename = bestMatch->processName;

            // Extract basename from entry
            size_t slashPos = entryBasename.find_last_of('/');
            if (slashPos != std::string::npos)
            {
                entryBasename = entryBasename.substr(slashPos + 1);
            }

            // Extract basename from process name
            slashPos = processBasename.find_last_of('/');
            if (slashPos != std::string::npos)
            {
                processBasename = processBasename.substr(slashPos + 1);
            }

            // Fuzzy name match
            bool nameMatches = (processBasename.find(entryBasename) != std::string::npos) ||
                               (entryBasename.find(processBasename) != std::string::npos);

            if (!nameMatches)
            {
                Log("PID %d: large timestamp diff %lds, name mismatch ('%s' vs '%s'), skipping", process.pid, minTimeDiff.count(), processBasename.c_str(), entryBasename.c_str());
                continue;
            }
        }

        // Copy memory stats
        process.pss = bestMatch->pss;
        process.swapPss = bestMatch->swapPss;
        process.rss = bestMatch->rss;
        process.utime = bestMatch->utime;
        process.stime = bestMatch->stime;
        matchedCount++;
    }

    Log("Merged memory stats for %d processes", matchedCount);
    mExitHandlerDataMerged = true;
}