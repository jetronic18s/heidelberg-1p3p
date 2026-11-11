#ifndef CRASH_INFO_H
    #define CRASH_INFO_H
    #include <string>

    // Reason and backtrace of the last crash, kept in RTC memory. The serial
    // console shares UART0 with RS485, so the panic output is lost otherwise.
    // Decode the addresses with addr2line and the ELF of the same build.
    std::string crashInfoText();
#endif
