#include "ControlsWin.hpp"

#include <cstdio>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>

static void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
    return ss.str();
}

void ControlsWin::draw(AppState& state) {
    ImGui::Begin("Controls");

    ImGui::Checkbox("Show ImGui demo", &state.show_imgui_demo);

    ImGui::SeparatorText("USB CDC");
    ImGui::InputText("Port", state.usb_port, sizeof(state.usb_port));
    ImGui::InputInt("Baud", &state.baud_rate);
    if (!state.IsUsbConnected()) {
        if (ImGui::Button("Connect")) {
            state.ConnectUsb();
        }
    } else {
        if (ImGui::Button("Disconnect")) {
            state.DisconnectUsb();
        }
    }
    ImGui::TextWrapped("Status: %s", state.usb_status.c_str());
    if (!state.last_command.empty()) {
        ImGui::TextWrapped("Last command: %s", state.last_command.c_str());
    }

    ImGui::SeparatorText("ESP2 Output");
    ImGui::InputText("ESP2 Port", state.output_port, sizeof(state.output_port));
    ImGui::InputInt("ESP2 Baud", &state.output_baud_rate);
    ImGui::Checkbox("Auto-forward RESULT", &state.auto_forward_output);
    if (!state.IsOutputConnected()) {
        if (ImGui::Button("Connect ESP2")) {
            state.ConnectOutput();
        }
    } else {
        if (ImGui::Button("Disconnect ESP2")) {
            state.DisconnectOutput();
        }
    }
    ImGui::TextWrapped("Output: %s", state.output_status.c_str());
    if (!state.last_output_command.empty()) {
        ImGui::TextWrapped("Last output: %s", state.last_output_command.c_str());
    }
    if (ImGui::Button("Output Up")) state.SendOutputGesture(0, "up");
    ImGui::SameLine();
    if (ImGui::Button("Output Down")) state.SendOutputGesture(1, "down");
    if (ImGui::Button("Output Right")) state.SendOutputGesture(2, "right");
    ImGui::SameLine();
    if (ImGui::Button("Output Left")) state.SendOutputGesture(3, "left");
    ImGui::SameLine();
    if (ImGui::Button("Output Null")) state.SendOutputGesture(4, "null");

    ImGui::SeparatorText("Camera Commands");
    if (ImGui::Button("Capture once")) {
        state.SendUsbCommand("c" + GetTimestamp(), "capture once");
    }
    ImGui::SameLine();
    if (ImGui::Button(state.stream_enabled ? "Stop stream" : "Start stream")) {
        state.stream_enabled = !state.stream_enabled;
        state.SendUsbCommand(state.stream_enabled ? "d1" : "d0", state.stream_enabled ? "stream on" : "stream off");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save frame")) {
        state.SendUsbCommand("w" + GetTimestamp(), "save frame to MSC storage");
    }

    if (ImGui::Button("List storage")) {
        state.SendUsbCommand("L", "list storage");
    }
    ImGui::SameLine();
    if (ImGui::Button("Expose USB drive")) {
        state.SendUsbCommand("usb", "mount MSC to PC");
    }
    HelpMarker("Expose USB drive switches the ESP32-S3 storage partition to USB MSC mode so the PC can read the captured files.");

    ImGui::SeparatorText("Sensor Settings");
    const char* formats[] = {"Grayscale", "RGB565", "YUV422", "JPEG"};
    if (ImGui::Combo("Pixel format", &state.pixel_format, formats, IM_ARRAYSIZE(formats))) {
        state.SendUsbCommand("f" + std::to_string(state.pixel_format), "set pixel format");
    }

    const char* sizes[] = {"96x96", "QQVGA", "QVGA", "VGA", "SVGA", "UXGA"};
    if (ImGui::Combo("Frame size", &state.frame_size, sizes, IM_ARRAYSIZE(sizes))) {
        state.SendUsbCommand("s" + std::to_string(state.frame_size), "set frame size");
    }

    ImGui::SeparatorText("Exposure & Gain");

    if (ImGui::Checkbox("Auto Exposure (AEC)", &state.aec_enabled)) {
        state.SendUsbCommand(std::string("e") + (state.aec_enabled ? "1" : "0"), "set auto exposure");
    }

    if (!state.aec_enabled) {
        if (ImGui::SliderInt("Exposure value", &state.aec_value, 0, 1200)) {
            state.SendUsbCommand("v" + std::to_string(state.aec_value), "set manual exposure");
        }
    }

    if (ImGui::Checkbox("Auto Gain (AGC)", &state.agc_enabled)) {
        state.SendUsbCommand(std::string("g") + (state.agc_enabled ? "1" : "0"), "set auto gain");
    }

    if (!state.agc_enabled) {
        if (ImGui::SliderInt("Gain value", &state.agc_value, 0, 30)) {
            state.SendUsbCommand("a" + std::to_string(state.agc_value), "set manual gain");
        }
    }

    if (ImGui::Checkbox("Auto White Balance (AWB)", &state.awb_enabled)) {
        state.SendUsbCommand(std::string("y") + (state.awb_enabled ? "1" : "0"), "set auto white balance");
    }

    ImGui::SeparatorText("Image Controls");

    if (ImGui::SliderInt("Brightness", &state.brightness, -2, 2)) {
        state.SendUsbCommand("b" + std::to_string(state.brightness), "set brightness");
    }
    if (ImGui::SliderInt("Contrast", &state.contrast, -2, 2)) {
        state.SendUsbCommand("t" + std::to_string(state.contrast), "set contrast");
    }
    if (ImGui::SliderInt("Saturation", &state.saturation, -2, 2)) {
        state.SendUsbCommand("x" + std::to_string(state.saturation), "set saturation");
    }

    if (ImGui::Checkbox("Horizontal mirror", &state.horizontal_mirror)) {
        state.SendUsbCommand(std::string("m") + (state.horizontal_mirror ? "1" : "0"), "set horizontal mirror");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Vertical flip", &state.vertical_flip)) {
        state.SendUsbCommand(std::string("p") + (state.vertical_flip ? "1" : "0"), "set vertical flip");
    }

    ImGui::SeparatorText("Input Prototype");
    ImGui::SliderFloat("Zoom knob", &state.camera_focus, 0.0f, 1.0f);
    HelpMarker("This keeps the input team's rotary-control placeholder visible. It is not wired to OV2640 optical focus because OV2640 is fixed-focus.");

    ImGui::End();
}
