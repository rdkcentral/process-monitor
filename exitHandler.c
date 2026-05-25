#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <dirent.h>
#include <time.h>

// MAY NOT WORK FOR BUSYBOX utilities, as stdio might've been reopened
#if 0
#define PRINT printf
#define PRINT_INFO printf
#define PRINT_NOISE printf
#define TEST_ME
#else
#define PRINT(...)
#define PRINT_INFO(...)
#define PRINT_NOISE(...)
#undef TEST_ME
#endif

#undef INSTALL_EXIT_HANDLER
#ifdef TEST_ME
unsigned long testpsstotal, testswappsstotal;
#endif

int getPSSandSwapPSS(unsigned pid, unsigned long *psstotal, unsigned long *swappsstotal)
{
    char mmapTmpArray[1024]; /* Used to read entries from /proc/pid/smaps...big enough to hold large entries */
    static unsigned skipToPss = 0, skipToSwapPss = 0, skipToRollover = 0;
    unsigned skippedToLearn = 0;

    sprintf(mmapTmpArray, "/proc/%u/smaps", pid);
    FILE *smap = fopen(mmapTmpArray, "r");
    if (smap) {
        unsigned lines_To_skip = skipToPss;
        unsigned skipped = 1; // Tracks current skips
        unsigned expect_pss = 1, expect_swappss = 0;
        //unsigned expect_pss, expect_swappss;

        unsigned pss, swappss;
        while (fgets(mmapTmpArray, 1024, smap)) {
#ifdef TEST_ME
            if (sscanf(mmapTmpArray, "Pss: %u kB", &pss)) {
                testpsstotal += pss;
            }
            else if (sscanf(mmapTmpArray, "SwapPss: %u kB", &swappss)) {
                testswappsstotal += swappss;
            }
#endif
            if (skipToRollover) { // Learnt the format
                if (++skipped > lines_To_skip) {
                    if (expect_pss) {
                        if (sscanf(mmapTmpArray, "Pss: %u kB", &pss)) {
                            PRINT_NOISE("Read Entry %s %u", mmapTmpArray, pss);
                            lines_To_skip = skipToSwapPss;
                            skipped = 1;
                            expect_swappss = 1;
                            expect_pss = 0;
                            *psstotal += pss;
                        }
                        else {
                            PRINT("Shouldn't get here..Error Reading Size from %s", mmapTmpArray);
                        }
                    }
                    else if (expect_swappss) {
                        if (sscanf(mmapTmpArray, "SwapPss: %u kB", &swappss)) {
                            PRINT_NOISE("Read Entry %s %u", mmapTmpArray, swappss);
                            skipped = 1;
                            expect_pss = 1;
                            expect_swappss = 0;
                            *swappsstotal += swappss;
                            lines_To_skip = skipToRollover;
                        }
                        else {
                            PRINT("Shouldn't get here..Error Reading Rss from %s", mmapTmpArray);
                        }
                    }
                } // if (++skipped > lines_To_skip)
                else {
                    PRINT_INFO("Skipping, skipped vs lines_To_skip %u:%u, line %s", skipped, lines_To_skip, mmapTmpArray);
                }
            } // if (skipToRollover) 
            else { // Learn here
                if (!skipToPss) {
                    if (sscanf(mmapTmpArray, "Pss: %u kB", &pss)) {
                        skipToPss = skippedToLearn + 1;
                        skippedToLearn = 0;
                        *psstotal += pss;
                        PRINT_INFO("Read Pss %u after skipping %u lines - %s", pss, skipToPss, mmapTmpArray);
                    }
                    else {
                        skippedToLearn++;
                        PRINT_NOISE("skippedForPss: %u, %s\n", skippedToLearn, mmapTmpArray);
                    }
                }
                else if (!skipToSwapPss) {
                    if (sscanf(mmapTmpArray, "SwapPss: %u kB", &swappss)) {
                        skipToSwapPss = skippedToLearn + 1;
                        skippedToLearn = 0;
                        *swappsstotal += swappss;
                        PRINT_INFO("Read SwapPss %u after skipping %u lines - %s", swappss, skipToSwapPss, mmapTmpArray);
                    }
                    else {
                        skippedToLearn++;
                        PRINT_NOISE("skippedForSwapPss: %u, %s\n", skippedToLearn, mmapTmpArray);
                    }
                }
                else if (!skipToRollover) {
                    if (sscanf(mmapTmpArray, "Pss: %u kB", &pss)) {
                        skipToRollover = skippedToLearn + 1;
                        skippedToLearn = 0;
                        *psstotal += pss;
                        expect_swappss = 1;
                        expect_pss = 0;
                        skipped = 1;
                        lines_To_skip = skipToSwapPss;
                        PRINT_INFO("Read Pss %u by rollover after skipping %u lines - %s", pss, skipToRollover, mmapTmpArray);
                    }
                    else {
                        skippedToLearn++;
                        PRINT_NOISE("skippedForRollover: %u, %s\n", skippedToLearn, mmapTmpArray);
                    }
                                }
                else {
                    PRINT("%s:%d: Shouldn't get here..read line %s\n", __FUNCTION__, __LINE__, mmapTmpArray);
                }
            }
        }
        fclose(smap);
    }
    else {
        PRINT("%s: Open failed, errno %d [%s]\n", mmapTmpArray, errno, strerror(errno));
        return 1;
    }
    return 0;
}

void printMyStatFromLib()
{
    /* Use /proc/pid/stat to read name, flags, stime, utime */
    char pathTmp[PATH_MAX];
    unsigned long utime, stime;
    unsigned long vsize=0, rss;
    int pid = getpid();

    sprintf(pathTmp, "/proc/%d/stat", pid);
    FILE *fp = fopen(pathTmp, "r");
    if (fp) {
        if (5 <= fscanf(fp, "%*d %*c%s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %lu %lu",
                        pathTmp, &utime, &stime, &vsize, &rss)) {
                pathTmp[strlen(pathTmp)-1] = '\0';
        }
        fclose(fp);
        if (vsize) {
            unsigned long pss=0, swappss=0;
            getPSSandSwapPSS(pid, &pss, &swappss);
            fp = fopen("/tmp/exitHandler.txt", "a");
            if (fp) {
                char tbuff[32];
                char buff[PATH_MAX*4];
                time_t timenow = time(NULL);
                struct tm *tmNow = localtime(&timenow);
                if (0 == strftime(tbuff, sizeof(tbuff), "%Y_%m_%d %H_%M_%S", tmNow)) {
                    // Shouldn't fail unless mmapTmpArray is not big enough to hold
                    sprintf(tbuff, "%lu", timenow); // see if this is warned in 32 bit systems..
                }
#ifdef TEST_ME
                fwrite(buff, sprintf(buff, "%s: %d %s %lu %lu %lu, %lu[%lu], %lu[%lu]\n", tbuff, pid, pathTmp, utime, stime, rss*4, pss, testpsstotal, swappss, testswappsstotal), 1, fp);
#else
                fwrite(buff, sprintf(buff, "%s: %d %s %lu %lu %lu, %lu, %lu\n", tbuff, pid, pathTmp, utime, stime, rss*4, pss, swappss), 1, fp);
#endif
                PRINT("%d[%s]:\nutime %lu\nstime %lu\nvsize %lu\nrss %lu\npss %lu[%lu]\nswappss %lu[%lu]\n", pid, pathTmp, utime, stime, vsize/1024, rss*4, pss, testpsstotal, swappss, testswappsstotal);
                fclose(fp);
            }
		}
    }
	else {
		char buff[32];
		fp = fopen("/tmp/exitHandler.txt", "a");
		if (fp) {
			fwrite(buff, sprintf(buff, "%d %d\n", pid, errno), 1, fp);
			fclose(fp);
		}
	}
}

#if defined(INSTALL_EXIT_HANDLER)
__attribute__((constructor)) void registerAtExitFromLib(void)
#else
void registerAtExitFromLib(void)
#endif
{
    //PRINT("ATEXIT_MAX = %ld\n", sysconf(_SC_ATEXIT_MAX));
    if (atexit(printMyStatFromLib)) {
            PRINT("%s: Error atexit()\n", __FUNCTION__);
    }
    else
            PRINT("%s: Registered atexit()\n", __FUNCTION__);
    return;
}

__attribute__((destructor)) void AtExitFromLib(void)
{
    //fwrite("\ndest\n", strlen("\ndest\n"), 1, stderr);
    //fwrite("dest2\n", strlen("dest2\n"), 1, stdout);
    printMyStatFromLib();
}