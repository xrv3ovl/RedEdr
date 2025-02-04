#include <stdio.h>
#include <windows.h>
#include <dbghelp.h>
#include <iostream>
#include <string.h>

#include "config.h"
#include "dllinjector.h"
#include "etwreader.h"
#include "logreader.h"
#include "kernelreader.h"
#include "webserver.h"
#include "dllreader.h"
#include "kernelinterface.h"
#include "pplmanager.h"
#include "logging.h"
#include "event_processor.h"
#include "event_aggregator.h"
#include "event_detector.h"
#include "process_resolver.h"
#include "mem_static.h"
#include "mem_dynamic.h"


/* manager.cpp: Knows and manages all subsystems (Input's)
 *   start, stop, restart components
 *   start, stop, restart components logging
 *   set new configuration (process trace etc.)
 */


void ResetEverything() {
    g_EventAggregator.ResetData();
    g_EventProcessor.ResetData();
    g_EventDetector.ResetData();
    g_ProcessResolver.ResetData();
    //g_MemStatic.ResetData();
    //g_MemDynamic.ResetData();
}


BOOL ManagerReload() {
    // DLL
    // -> Automatic upon connect of DLL (initiated by Kernel)

    // ETW
    // -> Automatic in ProcessCache
    
    // Kernel
    if (g_Config.do_kernelcallback || g_Config.do_dllinjection) {
        LOG_A(LOG_INFO, "Manager: Tell Kernel about new target: %s", g_Config.targetExeName.c_str());
        if (!EnableKernelDriver(g_Config.enabled,  g_Config.targetExeName)) {
            LOG_A(LOG_ERROR, "Manager: Could not communicate with kernel driver, aborting.");
            return FALSE;
        }
    }

    // PPL
    if (g_Config.do_etwti) {
        LOG_A(LOG_INFO, "Manager: Tell ETW-TI about new target: %s", g_Config.targetExeName.c_str());
        EnablePplProducer(g_Config.enabled, g_Config.targetExeName);
    }

    return TRUE;
}


BOOL ManagerStart(std::vector<HANDLE>& threads) {
    // Do kernel module stuff first, as it can fail hard
    // we can then just bail out without tearing down the other threads
    if (g_Config.do_kernelcallback || g_Config.do_dllinjection) {
        if (IsServiceRunning(g_Config.driverName)) {
            LOG_A(LOG_INFO, "Manager: RedEdr Driver already loaded");
        }
        else {
            LOG_A(LOG_INFO, "Manager: Load Kernel Driver");
            if (!LoadKernelDriver()) {
                LOG_A(LOG_ERROR, "RedEdr: Could not load driver");
                return FALSE;
            }
        }

        // Start the kernel server first
        // The kernel module will connect to it
        LOG_A(LOG_INFO, "Manager: Start kernel reader  thread");
        KernelReaderInit(threads);

        // Enable it
        LOG_A(LOG_INFO, "Manager: Tell Kernel to start collecting telemetry of: %s", g_Config.targetExeName.c_str());
        if (!EnableKernelDriver(1, g_Config.targetExeName)) {
            LOG_A(LOG_ERROR, "Manager: Could not communicate with kernel driver, aborting.");
            return FALSE;
        }
    }
    if (g_Config.do_etw) {
        LOG_A(LOG_INFO, "Manager: Start ETW reader thread");
        InitializeEtwReader(threads);
    }
    if (g_Config.do_mplog) {
        LOG_A(LOG_INFO, "Manager: Start MPLOG Reader");
        InitializeLogReader(threads);
    }
    if (g_Config.do_dllinjection || g_Config.debug_dllreader || g_Config.do_etwti) {
        LOG_A(LOG_INFO, "Manager: Start InjectedDll reader thread");
        DllReaderInit(threads);
    }
    if (g_Config.do_etwti) {
        LOG_A(LOG_INFO, "Manager: Start ETW-TI reader");
        Sleep(500);
        InitPplService();
        EnablePplProducer(TRUE,g_Config.targetExeName);
    }

    return TRUE;
}


void ManagerShutdown() {
    g_EventAggregator.StopRecorder();

    if (g_Config.do_mplog) {
        LOG_A(LOG_INFO, "Manager: Stop log reader");
        LogReaderStopAll();
    }

    // Lets shut down ETW stuff first, its more important
    // ETW-TI
    if (g_Config.do_etwti) {
        LOG_A(LOG_INFO, "Manager: Stop ETWTI reader");
        EnablePplProducer(FALSE, NULL);
    }
    // ETW
    if (g_Config.do_etw) {
        LOG_A(LOG_INFO, "Manager: Stop ETW readers");
        EtwReaderStopAll();
    }

    // Make kernel module stop emitting events
    //    Disconnects KernelPipe client
    if (g_Config.do_kernelcallback || g_Config.do_dllinjection) {
        LOG_A(LOG_INFO, "Manager: Disable kernel driver");
        EnableKernelDriver(0, "");
    }

    // The following may crash?
    // Shutdown kernel reader
    if (g_Config.do_kernelcallback) {
        LOG_A(LOG_INFO, "Manager: Stop kernel reader");
        KernelReaderShutdown();
    }
    // Shutdown dll reader
    if (g_Config.do_dllinjection || g_Config.do_etwti) {
        LOG_A(LOG_INFO, "Manager: Stop DLL reader");
        DllReaderShutdown();
    }

    // Special case
    if (g_Config.debug_dllreader) {
        LOG_A(LOG_INFO, "Manager: Stop DLL reader");
        DllReaderShutdown();
    }

    // Web server
    if (g_Config.web_output) {
        LOG_A(LOG_INFO, "Manager: Stop web server");
        StopWebServer();
    }

    // Analyzer
    StopEventProcessor();
}
