#ifndef TIME_SYNC_H
    #define TIME_SYNC_H
    #include <ctime>
    #include <string>

    // Wall clock time from SNTP, German local time.
    void timeSyncStart(const std::string &server);
    // Apply a changed server from the config page.
    void timeSyncSetServer(const std::string &server);
    bool timeIsSet();
    // "10.11.2026 12:39:16", empty while the time is not set
    std::string formatLocalTime(time_t t);
    // "Time" row: current time and the server in use
    std::string timeStatusText();
#endif /* TIME_SYNC_H */
