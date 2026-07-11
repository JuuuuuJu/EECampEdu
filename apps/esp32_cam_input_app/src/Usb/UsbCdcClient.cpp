#include "UsbCdcClient.hpp"

#ifdef _WIN32
#include <windows.h>

#include <algorithm>

static std::string LastWin32Error() {
    DWORD code = GetLastError();
    if (code == 0) {
        return "unknown error";
    }

    LPSTR message = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);

    std::string result = size && message ? std::string(message, size) : "Win32 error " + std::to_string(code);
    if (message) {
        LocalFree(message);
    }
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
        result.pop_back();
    }
    return result;
}

UsbCdcClient::~UsbCdcClient() {
    Close();
}

bool UsbCdcClient::Open(const std::string& port_name, int baud_rate, std::string* error) {
    Close();

    std::string device = port_name;
    if (device.rfind("\\\\.\\", 0) != 0) {
        device = "\\\\.\\" + device;
    }

    HANDLE handle = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error) *error = LastWin32Error();
        return false;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        if (error) *error = LastWin32Error();
        CloseHandle(handle);
        return false;
    }

    dcb.BaudRate = static_cast<DWORD>(baud_rate);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(handle, &dcb)) {
        if (error) *error = LastWin32Error();
        CloseHandle(handle);
        return false;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 1;
    timeouts.ReadTotalTimeoutConstant = 1;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 250;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(handle, &timeouts);

    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    handle_ = handle;
    return true;
}

void UsbCdcClient::Close() {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool UsbCdcClient::IsOpen() const {
    return handle_ != nullptr;
}

bool UsbCdcClient::Write(const std::string& data, std::string* error) {
    if (!IsOpen()) {
        if (error) *error = "port is not open";
        return false;
    }

    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
        if (error) *error = LastWin32Error();
        return false;
    }
    if (written != data.size()) {
        if (error) *error = "short serial write";
        return false;
    }
    return true;
}

bool UsbCdcClient::ReadAvailable(std::string* out, std::string* error) {
    out->clear();
    if (!IsOpen()) {
        return true;
    }

    DWORD errors = 0;
    COMSTAT status = {};
    if (!ClearCommError(static_cast<HANDLE>(handle_), &errors, &status)) {
        if (error) *error = LastWin32Error();
        return false;
    }

    DWORD to_read = std::min<DWORD>(status.cbInQue, 4096);
    if (to_read == 0) {
        return true;
    }

    std::string buffer(to_read, '\0');
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), buffer.data(), to_read, &read, nullptr)) {
        if (error) *error = LastWin32Error();
        return false;
    }
    buffer.resize(read);
    *out = buffer;
    return true;
}

#else
UsbCdcClient::~UsbCdcClient() = default;

bool UsbCdcClient::Open(const std::string&, int, std::string* error) {
    if (error) *error = "USB CDC client is implemented for Windows COM ports in this demo";
    return false;
}

void UsbCdcClient::Close() {}
bool UsbCdcClient::IsOpen() const { return false; }
bool UsbCdcClient::Write(const std::string&, std::string* error) {
    if (error) *error = "USB CDC client is not available on this platform";
    return false;
}
bool UsbCdcClient::ReadAvailable(std::string*, std::string*) { return true; }
#endif
