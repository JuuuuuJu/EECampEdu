#pragma once

#include "Usb/UsbCdcClient.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class AppState {
public:
    void Init();
    void PollUsb();
    void UpdateContinuousInference();
    bool ConnectUsb();
    void DisconnectUsb();
    void SendUsbCommand(const std::string& command, const std::string& label);
    bool IsUsbConnected() const;

    bool ConnectOutput();
    void DisconnectOutput();
    bool IsOutputConnected() const;
    void SendOutputGesture(int gesture, const std::string& name);
    void SendOutputAction(const std::string& action);
    const char* ModelClassName(int index) const;
    const char* OutputActionName(int index) const;

    static constexpr int kModelClassCount = 6;
    static constexpr int kOutputActionCount = 7;

    float camera_focus = 0.0f;
    bool aec_enabled = true;
    int aec_value = 300;
    bool agc_enabled = true;
    int agc_value = 0;
    bool awb_enabled = true;
    int brightness = 0;
    int contrast = 0;
    int saturation = 0;
    bool horizontal_mirror = false;
    int frame_size = 3;      // Firmware protocol: 0=96x96, 1=QQVGA, 2=QVGA, 3=VGA, 4=SVGA, 5=UXGA.
    int pixel_format = 3;   // Camera command protocol: 0=grayscale, 1=RGB565, 2=YUV422, 3=JPEG.
    bool stream_enabled = false;
    bool continuous_inference_enabled = false;
    int continuous_inference_interval_ms = 3000;
    int continuous_inference_timeout_ms = 8000;
    std::string continuous_inference_status;
    bool vertical_flip = false;
    bool show_imgui_demo = true;

    char usb_port[64] = "COM6";
    int baud_rate = 115200;
    std::string usb_status;
    std::string last_command;
    std::string usb_rx_log;
    std::string last_frame_header;
    int received_image_count = 0;

    char output_port[64] = "COM7";
    int output_baud_rate = 115200;
    bool auto_forward_output = true;
    int output_action_for_class[kModelClassCount] = {0, 1, 2, 3, 4, 5};
    std::string output_status;
    std::string last_output_command;

    std::vector<uint8_t> latest_frame_rgba;
    int latest_frame_width = 0;
    int latest_frame_height = 0;
    int latest_frame_format = -1;
    bool latest_frame_dirty = false;

private:
    UsbCdcClient usb_client;
    UsbCdcClient output_client;
    std::string cdc_parse_buffer;
    std::string cdc_text_buffer;
    bool cdc_receiving_image_frame = false;
    bool continuous_inference_pending = false;
    std::chrono::steady_clock::time_point last_continuous_inference_send{};
    std::chrono::steady_clock::time_point last_continuous_inference_complete{};

    void AppendUsbLog(const std::string& text);
    void ProcessCdcText(const std::string& text);
    void MarkInferenceFinished(const std::string& reason);
    void ForwardResultLineToOutput(const std::string& line);
    void ParseCdcFrames();
    void StoreDecodedFrame(int format, int width, int height, const std::vector<uint8_t>& payload);
};
