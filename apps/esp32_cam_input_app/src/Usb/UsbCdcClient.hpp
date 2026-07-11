#pragma once

#include <string>

class UsbCdcClient {
public:
    UsbCdcClient() = default;
    ~UsbCdcClient();

    UsbCdcClient(const UsbCdcClient&) = delete;
    UsbCdcClient& operator=(const UsbCdcClient&) = delete;

    bool Open(const std::string& port_name, int baud_rate, std::string* error);
    void Close();
    bool IsOpen() const;
    bool Write(const std::string& data, std::string* error);
    bool ReadAvailable(std::string* out, std::string* error);

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};
