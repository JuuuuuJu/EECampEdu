#include "AppState.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

static int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<uint8_t> DecodeBase64(const std::string& input) {
    std::vector<uint8_t> out;
    int val = 0;
    int bits = -8;
    for (unsigned char uc : input) {
        char c = static_cast<char>(uc);
        if (std::isspace(uc)) {
            continue;
        }
        if (c == '=') {
            break;
        }
        int decoded = Base64Value(c);
        if (decoded < 0) {
            continue;
        }
        val = (val << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

static bool ParseFrameHeader(const std::string& header, int* format, int* width, int* height, int* bytes) {
    return std::sscanf(header.c_str(), "---START_IMAGE:%d:%d:%d:%d---", format, width, height, bytes) == 4;
}

void AppState::Init() {
    usb_status = "Disconnected";
    last_command.clear();
    usb_rx_log.clear();
    last_frame_header.clear();
    cdc_parse_buffer.clear();
    received_image_count = 0;
    latest_frame_rgba.clear();
    latest_frame_width = 0;
    latest_frame_height = 0;
    latest_frame_format = -1;
    latest_frame_dirty = false;
}

bool AppState::ConnectUsb() {
    std::string error;
    if (usb_client.Open(usb_port, baud_rate, &error)) {
        usb_status = "Connected to " + std::string(usb_port);
        AppendUsbLog("[PC] Connected\n");
        return true;
    }

    usb_status = "Connect failed: " + error;
    AppendUsbLog("[PC] " + usb_status + "\n");
    return false;
}

void AppState::DisconnectUsb() {
    if (usb_client.IsOpen()) {
        usb_client.Close();
        AppendUsbLog("[PC] Disconnected\n");
    }
    usb_status = "Disconnected";
    stream_enabled = false;
}

bool AppState::IsUsbConnected() const {
    return usb_client.IsOpen();
}

void AppState::SendUsbCommand(const std::string& command, const std::string& label) {
    last_command = label + ": " + command;
    if (!usb_client.IsOpen()) {
        AppendUsbLog("[PC] Cannot send; USB CDC is disconnected.\n");
        return;
    }

    std::string payload = command;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }

    std::string error;
    if (!usb_client.Write(payload, &error)) {
        AppendUsbLog("[PC] Send failed: " + error + "\n");
        return;
    }
    AppendUsbLog("[TX] " + payload);
}

void AppState::PollUsb() {
    if (!usb_client.IsOpen()) {
        return;
    }

    std::string chunk;
    std::string error;
    if (!usb_client.ReadAvailable(&chunk, &error)) {
        AppendUsbLog("[PC] Read failed: " + error + "\n");
        return;
    }
    if (chunk.empty()) {
        return;
    }

    AppendUsbLog(chunk);
    cdc_parse_buffer += chunk;
    constexpr size_t kMaxParseBytes = 2 * 1024 * 1024;
    if (cdc_parse_buffer.size() > kMaxParseBytes) {
        cdc_parse_buffer.erase(0, cdc_parse_buffer.size() - kMaxParseBytes);
    }
    ParseCdcFrames();
}

void AppState::AppendUsbLog(const std::string& text) {
    usb_rx_log += text;
    constexpr size_t kMaxLogBytes = 12000;
    if (usb_rx_log.size() > kMaxLogBytes) {
        usb_rx_log.erase(0, usb_rx_log.size() - kMaxLogBytes);
    }
}

void AppState::ParseCdcFrames() {
    const std::string start_marker = "---START_IMAGE:";
    const std::string end_marker = "---END_IMAGE---";

    while (true) {
        size_t start = cdc_parse_buffer.find(start_marker);
        if (start == std::string::npos) {
            if (cdc_parse_buffer.size() > 4096) {
                cdc_parse_buffer.erase(0, cdc_parse_buffer.size() - 4096);
            }
            return;
        }
        if (start > 0) {
            cdc_parse_buffer.erase(0, start);
            start = 0;
        }

        size_t header_end = cdc_parse_buffer.find("---", start + start_marker.size());
        if (header_end == std::string::npos) {
            return;
        }
        header_end += 3;

        int format = -1;
        int width = 0;
        int height = 0;
        int byte_count = 0;
        std::string header = cdc_parse_buffer.substr(0, header_end);
        if (!ParseFrameHeader(header, &format, &width, &height, &byte_count)) {
            cdc_parse_buffer.erase(0, header_end);
            continue;
        }

        size_t payload_start = header_end;
        while (payload_start < cdc_parse_buffer.size() && (cdc_parse_buffer[payload_start] == '\r' || cdc_parse_buffer[payload_start] == '\n')) {
            payload_start += 1;
        }

        size_t end = cdc_parse_buffer.find(end_marker, payload_start);
        if (end == std::string::npos) {
            return;
        }

        std::string encoded = cdc_parse_buffer.substr(payload_start, end - payload_start);
        std::vector<uint8_t> payload = DecodeBase64(encoded);
        StoreDecodedFrame(format, width, height, payload);

        std::ostringstream oss;
        oss << "format=" << format << " width=" << width << " height=" << height
            << " bytes=" << byte_count << " decoded=" << payload.size();
        last_frame_header = oss.str();
        received_image_count += 1;

        cdc_parse_buffer.erase(0, end + end_marker.size());
    }
}

void AppState::StoreDecodedFrame(int format, int width, int height, const std::vector<uint8_t>& payload) {
    latest_frame_format = format;
    latest_frame_width = width;
    latest_frame_height = height;
    latest_frame_dirty = false;

    if (width <= 0 || height <= 0) {
        return;
    }

    if (format == 0) {
        const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (payload.size() < expected) {
            return;
        }
        latest_frame_rgba.resize(expected * 4);
        for (size_t i = 0; i < expected; ++i) {
            uint8_t y = payload[i];
            latest_frame_rgba[i * 4 + 0] = y;
            latest_frame_rgba[i * 4 + 1] = y;
            latest_frame_rgba[i * 4 + 2] = y;
            latest_frame_rgba[i * 4 + 3] = 255;
        }
        latest_frame_dirty = true;
        return;
    }

    if (format == 1) {
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (payload.size() < pixels * 2) {
            return;
        }
        latest_frame_rgba.resize(pixels * 4);
        for (size_t i = 0; i < pixels; ++i) {
            uint16_t packed = static_cast<uint16_t>(payload[i * 2]) | (static_cast<uint16_t>(payload[i * 2 + 1]) << 8);
            uint8_t r = static_cast<uint8_t>(((packed >> 11) & 0x1F) * 255 / 31);
            uint8_t g = static_cast<uint8_t>(((packed >> 5) & 0x3F) * 255 / 63);
            uint8_t b = static_cast<uint8_t>((packed & 0x1F) * 255 / 31);
            latest_frame_rgba[i * 4 + 0] = r;
            latest_frame_rgba[i * 4 + 1] = g;
            latest_frame_rgba[i * 4 + 2] = b;
            latest_frame_rgba[i * 4 + 3] = 255;
        }
        latest_frame_dirty = true;
        return;
    }

    latest_frame_rgba.clear();
}