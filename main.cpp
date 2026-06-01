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

#include "ProcessMonitor.h"

#include <condition_variable>
#include <csignal>
#include <getopt.h>
#include <mutex>
#include <filesystem>
#include <fstream>

#include "Log.h"

std::mutex gMutex;
std::condition_variable gShutdown;

static std::string gOutputFile;
bool gCaptureMemData = false;
std::string gMemPreloadLib;

// Default to 30 seconds
static int gDurationSeconds = 30;

/**
 * @brief Display a help message for the tool
 */
static void displayUsage()
{
    printf("Usage: ProcessMonitor <option(s)>\n");
    printf("    Linux process monitor\n\n");
    printf("    -h, --help          Print this help and exit\n");
    printf("    -d, --duration      How long to capture data for (seconds)\n");
    printf("    -o  --output        File to save results to\n");
    printf("    -m, --mem <path>    Capture memory data (path to libexithandler.so)\n");
}

/**
 * @brief Parse the provided command line arguments
 */
static void parseArgs(const int argc, char **argv)
{
    struct option longopts[] = {{"help", no_argument, nullptr, (int)'h'},
                                {"duration", required_argument, nullptr, (int)'d'},
                                {"output", required_argument, nullptr, (int)'o'},
                                {"mem", required_argument, nullptr, (int)'m'},
                                {nullptr, 0, nullptr, 0}};

    opterr = 0;

    int option;
    int longindex;

    while ((option = getopt_long(argc, argv, "hd:o:m:", longopts, &longindex)) != -1)
    {
        switch (option)
        {
        case 'h':
            displayUsage();
            exit(EXIT_SUCCESS);
            break;
        case 'd':
            gDurationSeconds = std::atoi(optarg);
            if (gDurationSeconds < 0)
            {
                fprintf(stderr, "Error: Duration must be > 0\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'o':
            gOutputFile = std::string(optarg);
            break;
        case 'm':
            {
                std::string libPath = optarg ? std::string(optarg) : std::string{};
                if (libPath.empty())
                {
                    Log("ERROR: --mem requires a library path");
                    exit(EXIT_FAILURE);
                }
                gCaptureMemData = true;
                gMemPreloadLib = libPath;
                Log("Memory preload enabled with %s", gMemPreloadLib.c_str());
                break;
            }
        case '?':
            if (optopt == 'c')
                fprintf(stderr, "Warning: Option -%c requires an argument.\n", optopt);
            else if (isprint(optopt))
                fprintf(stderr, "Warning: Unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "Warning: Unknown option character `\\x%x'.\n", optopt);

            exit(EXIT_FAILURE);
            break;
        default:
            exit(EXIT_FAILURE);
            break;
        }
    }

    for (int i = optind; i < argc; i++)
    {
        printf("Warning: Non-option argument %s ignored\n", argv[i]);
    }
}

void signalHandler([[maybe_unused]] int signal)
{
    Log("Shutting down");
    gShutdown.notify_all();
}

int main(int argc, char **argv)
{
    // Parse arguments
    parseArgs(argc, argv);

    if (gOutputFile.empty())
    {
        fprintf(stderr, "Error: Must provide output file\n");
        return EXIT_FAILURE;
    }


    Log("Output file: %s", gOutputFile.c_str());
    Log("Capture duration: %d seconds", gDurationSeconds);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Start tracking events
    ProcessMonitor monitor;
    if (!monitor.Start())
    {
        return EXIT_FAILURE;
    }

    Log("Listening for process events");

    // Wait for shutdown signal or duration complete
    std::unique_lock<std::mutex> lock(gMutex);
    gShutdown.wait_for(lock, std::chrono::seconds(gDurationSeconds));

    monitor.Stop();

    auto json = monitor.GetJson();

    auto outputFile = std::filesystem::path(gOutputFile);

    // Create directory if it doesn't exist
    if (!std::filesystem::exists(outputFile.parent_path()))
    {
        std::filesystem::create_directory(outputFile.parent_path());
    }

    Log("Saving results to %s", gOutputFile.c_str());

    // Write results
    std::ofstream outputStream(outputFile, std::ios::trunc);
    if (outputStream)
    {
        outputStream << "let results = " << json << ";";
        return EXIT_SUCCESS;
    }
    else
    {
        Log("Failed to write to output file %s", gOutputFile.c_str());
        return EXIT_FAILURE;
    }
}