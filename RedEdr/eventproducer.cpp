#include "httplib.h" // Needs to be on top?

#include <iostream>
#include <sstream>
#include <vector>

#include <locale>
#include <codecvt>

#include "eventproducer.h"
#include "config.h"
#include "logging.h"
#include "utils.h"
#include "json.hpp"


/* Retrieves events from all subsystems:
     - ETW
     - ETWTI
     - Kernel
     - DLL
    as "text", convert to json-text, and buffer em. 

    It mostly makes sure all events are collected as fast
    as possible, with as little processing as possible.

    Analyzer will collect the events regularly.
*/

// Global
EventProducer g_EventProducer;


void EventProducer::do_output(std::wstring eventWstr) {
    // Convert to json and add it to the list
    //std::string json = ConvertLogLineToJsonEvent(eventWstr);

    std::string json = wstring_to_utf8(eventWstr);
    output_mutex.lock();
    output_entries.push_back(json);
    output_mutex.unlock();
    output_count++;

    // print it
    if (g_config.hide_full_output) {
        if (output_count >= 100) {
            if (output_count % 100 == 0) {
                std::wcout << L"O";
            }
        }
        else if (output_count >= 10) {
            if (output_count % 10 == 0) {
                std::wcout << L"o";
            }
        }
        else {
            std::wcout << L".";
        }
    }
    else {
        std::wcout << eventWstr << L"\n";
    }

    // Notify the analyzer thread
    cv.notify_one();
}


std::vector<std::string> EventProducer::GetEvents() {
    std::vector<std::string> newEvents;

    output_mutex.lock();
    newEvents = output_entries;
    output_entries.clear();
    output_mutex.unlock();

    return newEvents;
}


BOOL EventProducer::HasMoreEvents() {
    // Lock for now
    std::lock_guard<std::mutex> lock(output_mutex);

    if (output_entries.size() > 0) {
        return TRUE;
    }
    else {
        return false;
    }
}


void EventProducer::Stop() {
    done = TRUE;
    g_EventProducer.cv.notify_all();
}


void EventProducer::ResetData() {
    output_mutex.lock();
    output_entries.clear();
    output_mutex.unlock();
}


unsigned int EventProducer::GetCount() {
    return output_count;
}
