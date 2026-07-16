#include "AppState.hpp"
#include "Image/JpegDecoder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

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

static const char* kModelClassNames[AppState::kModelClassCount] = {"up", "ok", "thumb", "palm", "rock", "stone"};
static const char* kOutputActionNames[AppState::kOutputActionCount] = {"up", "down", "left", "right", "clamp", "release", "none"};
static bool ParseFrameHeader(const std::string& header, int* format, int* width, int* height, int* bytes) {
    return std::sscanf(header.c_str(), "---START_IMAGE:%d:%d:%d:%d---", format, width, height, bytes) == 4;
}
static bool LooksLikeImagePayload(const std::string& text) {
    if (text.size() < 96) {
        return false;
    }
    size_t useful = 0;
    size_t base64ish = 0;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            continue;
        }
        ++useful;
        if (std::isalnum(c) || c == '+' || c == '/' || c == '=') {
            ++base64ish;
        }
    }
    return useful > 96 && base64ish * 100 / useful > 92;
}

void AppState::Init() {
    usb_status = "Disconnected";
    output_status = "Output disconnected";
    last_command.clear();
    last_output_command.clear();
    usb_rx_log.clear();
    last_frame_header.clear();
    cdc_parse_buffer.clear();
    cdc_text_buffer.clear();
    cdc_receiving_image_frame = false;
    received_image_count = 0;
    latest_frame_rgba.clear();
    latest_frame_width = 0;
    latest_frame_height = 0;
    latest_frame_format = -1;
    latest_frame_dirty = false;

    aec_enabled = true;
    aec_value = 300;
    agc_enabled = true;
    agc_value = 0;
    awb_enabled = true;
    brightness = 0;
    contrast = 0;
    saturation = 0;
    horizontal_mirror = false;
    vertical_flip = false;
    for (int i = 0; i < kModelClassCount; ++i) {
        output_action_for_class[i] = i;
    }
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
    cdc_receiving_image_frame = false;
}

bool AppState::IsUsbConnected() const {
    return usb_client.IsOpen();
}

bool AppState::ConnectOutput() {
    std::string error;
    if (output_client.Open(output_port, output_baud_rate, &error)) {
        output_status = "Connected to ESP2 on " + std::string(output_port);
        AppendUsbLog("[ESP2] " + output_status + "\n");
        return true;
    }

    output_status = "ESP2 connect failed: " + error;
    AppendUsbLog("[ESP2] " + output_status + "\n");
    return false;
}

void AppState::DisconnectOutput() {
    if (output_client.IsOpen()) {
        output_client.Close();
        AppendUsbLog("[ESP2] Disconnected\n");
    }
    output_status = "Output disconnected";
}

bool AppState::IsOutputConnected() const {
    return output_client.IsOpen();
}

const char* AppState::ModelClassName(int index) const {
    if (index >= 0 && index < kModelClassCount) {
        return kModelClassNames[index];
    }
    return "unknown";
}

const char* AppState::OutputActionName(int index) const {
    if (index >= 0 && index < kOutputActionCount) {
        return kOutputActionNames[index];
    }
    return "none";
}

void AppState::SendOutputAction(const std::string& action) {
    std::string command = "ACTION," + action + "\n";
    last_output_command = command;

    if (!output_client.IsOpen()) {
        output_status = "Output disconnected";
        return;
    }

    std::string error;
    if (!output_client.Write(command, &error)) {
        output_status = "ESP2 send failed: " + error;
        AppendUsbLog("[ESP2] " + output_status + "\n");
        return;
    }

    std::string ack;
    if (output_client.ReadAvailable(&ack, &error) && !ack.empty()) {
        output_status = ack;
        AppendUsbLog("[ESP2] " + ack);
    } else {
        output_status = "Sent " + command;
        AppendUsbLog("[ESP2 TX] " + command);
    }
}

void AppState::SendOutputGesture(int gesture, const std::string& name) {
    std::ostringstream command;
    command << "GESTURE," << gesture << "," << name << "\n";
    last_output_command = command.str();

    if (!output_client.IsOpen()) {
        output_status = "Output disconnected";
        return;
    }

    std::string error;
    if (!output_client.Write(command.str(), &error)) {
        output_status = "ESP2 send failed: " + error;
        AppendUsbLog("[ESP2] " + output_status + "\n");
        return;
    }

    std::string ack;
    if (output_client.ReadAvailable(&ack, &error) && !ack.empty()) {
        output_status = ack;
        AppendUsbLog("[ESP2] " + ack);
    } else {
        output_status = "Sent " + command.str();
        AppendUsbLog("[ESP2 TX] " + command.str());
    }
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

    const bool chunk_has_image_marker =
        chunk.find("---START_IMAGE:") != std::string::npos ||
        chunk.find("---END_IMAGE---") != std::string::npos;
    const bool chunk_looks_like_image_payload = LooksLikeImagePayload(chunk);
    if (!cdc_receiving_image_frame && !chunk_has_image_marker && !chunk_looks_like_image_payload) {
        AppendUsbLog(chunk);
        ProcessCdcText(chunk);
    }

    cdc_parse_buffer += chunk;
    constexpr size_t kMaxParseBytes = 4 * 1024 * 1024;
    if (cdc_parse_buffer.size() > kMaxParseBytes) {
        cdc_parse_buffer.erase(0, cdc_parse_buffer.size() - kMaxParseBytes);
        cdc_receiving_image_frame = false;
        AppendUsbLog("[PC] CDC parse buffer overflow; dropped old frame data.\n");
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

void AppState::ProcessCdcText(const std::string& text) {
    cdc_text_buffer += text;
    size_t newline = std::string::npos;
    while ((newline = cdc_text_buffer.find_first_of("\r\n")) != std::string::npos) {
        std::string line = cdc_text_buffer.substr(0, newline);
        cdc_text_buffer.erase(0, newline + 1);
        if (!line.empty()) {
            ForwardResultLineToOutput(line);
        }
    }
    if (cdc_text_buffer.size() > 1024) {
        cdc_text_buffer.erase(0, cdc_text_buffer.size() - 1024);
    }
}

void AppState::ForwardResultLineToOutput(const std::string& line) {
    if (!auto_forward_output || !output_client.IsOpen()) {
        return;
    }
    if (line.rfind("RESULT,", 0) != 0) {
        return;
    }

    size_t first = line.find(',');
    size_t second = line.find(',', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        return;
    }

    int gesture = -1;
    try {
        gesture = std::stoi(line.substr(first + 1, second - first - 1));
    } catch (...) {
        return;
    }

        if (gesture < 0 || gesture >= kModelClassCount) {
        output_status = "Skipped out-of-range gesture: " + std::to_string(gesture);
        AppendUsbLog("[ESP2] " + output_status + "\n");
        return;
    }

    const char* gesture_name = ModelClassName(gesture);
    const char* action = OutputActionName(output_action_for_class[gesture]);
    if (std::string(action) == "none") {
        output_status = "Skipped gesture mapped to none: " + std::to_string(gesture) + "(" + gesture_name + ")";
        AppendUsbLog("[ESP2] " + output_status + "\n");
        return;
    }

    AppendUsbLog("[ESP2 MAP] " + std::string(gesture_name) + " -> " + action + "\n");
    SendOutputAction(action);
}
void AppState::ParseCdcFrames() {
    const std::string start_marker = "---START_IMAGE:";
    const std::string end_marker = "---END_IMAGE---";

    while (true) {
        size_t start = cdc_parse_buffer.find(start_marker);
        if (start == std::string::npos) {
            // If the app connects in the middle of a streaming JPEG frame, the START marker may be missed.
            // Recover as soon as we see a JPEG base64 magic prefix and the END marker.
            const size_t jpeg_magic = cdc_parse_buffer.find("/9j/");
            const size_t recovered_end = cdc_parse_buffer.find(end_marker, jpeg_magic == std::string::npos ? 0 : jpeg_magic);
            if (jpeg_magic != std::string::npos && recovered_end != std::string::npos) {
                std::string encoded = cdc_parse_buffer.substr(jpeg_magic, recovered_end - jpeg_magic);
                std::vector<uint8_t> payload = DecodeBase64(encoded);
                StoreDecodedFrame(4, 1, 1, payload);

                std::ostringstream oss;
                oss << "recovered format=4 decoded=" << payload.size();
                last_frame_header = oss.str();
                received_image_count += 1;
                cdc_receiving_image_frame = false;
                AppendUsbLog("[RX_FRAME] " + last_frame_header + "\n");
                cdc_parse_buffer.erase(0, recovered_end + end_marker.size());
                continue;
            }

            cdc_receiving_image_frame = false;
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
            cdc_receiving_image_frame = true;
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
            cdc_receiving_image_frame = false;
            continue;
        }

        size_t payload_start = header_end;
        while (payload_start < cdc_parse_buffer.size() &&
               (cdc_parse_buffer[payload_start] == '\r' || cdc_parse_buffer[payload_start] == '\n')) {
            payload_start += 1;
        }

        size_t end = cdc_parse_buffer.find(end_marker, payload_start);
        if (end == std::string::npos) {
            cdc_receiving_image_frame = true;
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
        cdc_receiving_image_frame = false;
        AppendUsbLog("[RX_FRAME] " + last_frame_header + "\n");

        cdc_parse_buffer.erase(0, end + end_marker.size());
    }
}

void AppState::StoreDecodedFrame(int format, int width, int height, const std::vector<uint8_t>& payload) {
    latest_frame_format = format;
    latest_frame_width = width;
    latest_frame_height = height;
    latest_frame_dirty = false;

    if (width <= 0 || height <= 0) {
        AppendUsbLog("[PC] Invalid frame dimensions.\n");
        latest_frame_rgba.clear();
        return;
    }

    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);

    // ESP frame header protocol: 0=RGB565, 1=YUV422, 3=GRAYSCALE, 4=JPEG.
    // The UI command protocol is different: F 3 asks firmware to switch the camera to JPEG.
    if (format == 3) {
        const size_t expected = pixels;
        if (payload.size() < expected) {
            AppendUsbLog("[PC] Grayscale frame too small: decoded=" + std::to_string(payload.size()) +
                         " expected=" + std::to_string(expected) + "\n");
            latest_frame_rgba.clear();
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

    if (format == 0) {
        if (payload.size() < pixels * 2) {
            AppendUsbLog("[PC] RGB565 frame too small: decoded=" + std::to_string(payload.size()) +
                         " expected=" + std::to_string(pixels * 2) + "\n");
            latest_frame_rgba.clear();
            return;
        }
        latest_frame_rgba.resize(pixels * 4);
        for (size_t i = 0; i < pixels; ++i) {
            uint16_t packed = static_cast<uint16_t>(payload[i * 2]) |
                              (static_cast<uint16_t>(payload[i * 2 + 1]) << 8);
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

    if (format == 1) {
        if (payload.size() < pixels * 2) {
            AppendUsbLog("[PC] YUV422 frame too small: decoded=" + std::to_string(payload.size()) +
                         " expected=" + std::to_string(pixels * 2) + "\n");
            latest_frame_rgba.clear();
            return;
        }
        latest_frame_rgba.resize(pixels * 4);
        for (size_t i = 0; i < pixels; ++i) {
            // Show the Y channel as grayscale. Full YUV422 color conversion is not needed for preview diagnostics.
            uint8_t y = payload[(i / 2) * 4 + ((i % 2) == 0 ? 0 : 2)];
            latest_frame_rgba[i * 4 + 0] = y;
            latest_frame_rgba[i * 4 + 1] = y;
            latest_frame_rgba[i * 4 + 2] = y;
            latest_frame_rgba[i * 4 + 3] = 255;
        }
        latest_frame_dirty = true;
        return;
    }

    if (format == 4) {
        std::vector<uint8_t> decoded_rgba;
        int decoded_width = 0;
        int decoded_height = 0;
        std::string decode_error;
        if (!DecodeJpegToRgba(payload.data(), payload.size(), &decoded_rgba, &decoded_width, &decoded_height, &decode_error)) {
            latest_frame_rgba.clear();
            AppendUsbLog("[PC] JPEG decode failed: " + decode_error + "\n");
            return;
        }
        latest_frame_rgba = std::move(decoded_rgba);
        latest_frame_width = decoded_width;
        latest_frame_height = decoded_height;
        latest_frame_dirty = true;
        return;
    }

    latest_frame_rgba.clear();
    AppendUsbLog("[PC] Unsupported image frame format: " + std::to_string(format) + "\n");
}
