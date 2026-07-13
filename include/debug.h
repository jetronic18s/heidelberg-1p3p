#ifndef DEBUG_H
#define DEBUG_H

#include <string>

void debugInit();
void debugLoop();
void debugSetTelnetEnabled(bool enabled);
void debugDisableConsoleLog();
int debugTelnetVprintf(const char *fmt, va_list args);

void debugWrite(const char *text);
void debugWrite(const std::string &text);
void debugWrite(char value);
void debugWrite(int value);
void debugWrite(unsigned int value);
void debugWrite(long value);
void debugWrite(unsigned long value);

void debugWriteLine(const char *text);
void debugWriteLine(const std::string &text);
void debugWriteLine(char value);
void debugWriteLine(int value);
void debugWriteLine(unsigned int value);
void debugWriteLine(long value);
void debugWriteLine(unsigned long value);

#define dbg(x) debugWrite(x);
#define dbgln(x) debugWriteLine(x);
#endif
