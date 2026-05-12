#include "pair_device_dialog.h"

#include "ble_protocol.h"
#include "dpi_util.h"

#include <CommCtrl.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <utility>

namespace voicestick {

namespace {

constexpr UINT kDeviceListChangedMessage = WM_APP + 20;
constexpr UINT kPairingConnectedMessage = WM_APP + 21;
constexpr UINT kPairingSucceededMessage = WM_APP + 22;
constexpr UINT kPairingErrorMessage = WM_APP + 23;
constexpr UINT_PTR kPairingTimeoutTimerId = 2;
constexpr UINT_PTR kPairingFinalizeTimerId = 3;
constexpr UINT kPairingFinalizeDelayMs = 2500;
constexpr int kDeviceListId = 101;

HMENU ControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

std::string Utf8FromHstring(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string FormatBluetoothAddress(std::uint64_t address) {
    char buffer[18]{};
    snprintf(buffer, sizeof(buffer), "%02llX:%02llX:%02llX:%02llX:%02llX:%02llX",
             (address >> 40) & 0xff,
             (address >> 32) & 0xff,
             (address >> 24) & 0xff,
             (address >> 16) & 0xff,
             (address >> 8) & 0xff,
             address & 0xff);
    return buffer;
}

std::string FormatHresult(std::int32_t code) {
    char buffer[16]{};
    snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(code));
    return buffer;
}

std::wstring ScanStartFailureText(const winrt::hresult_error& error) {
    std::string message = "Turn on Bluetooth to scan (" + FormatHresult(error.code()) + ")";
    const auto detail = winrt::to_string(error.message());
    if (!detail.empty()) message += ": " + detail;
    auto text = winrt::to_hstring(message);
    return std::wstring(text.c_str());
}

bool CanReadAdvertisementAddressType() {
    static const bool available =
        winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs",
            L"BluetoothAddressType");
    return available;
}

BluetoothAddressKind AddressKindFromArgs(
    const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args) {
    using winrt::Windows::Devices::Bluetooth::BluetoothAddressType;
    // BluetoothAddressType requires Windows 10 v2004 (build 19041). Windows
    // 10 2019 builds can still pair through the address-only BLE path.
    if (CanReadAdvertisementAddressType()) {
        try {
            switch (args.BluetoothAddressType()) {
            case BluetoothAddressType::Public:
                return BluetoothAddressKind::kPublic;
            case BluetoothAddressType::Random:
                return BluetoothAddressKind::kRandom;
            default:
                return BluetoothAddressKind::kUnspecified;
            }
        } catch (...) {
        }
    }
    return BluetoothAddressKind::kUnspecified;
}

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) buffer->push_back(0);
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    if (!text) {
        AppendDialogWord(buffer, 0);
        return;
    }
    while (*text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

void InsertColumn(HWND list_view, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    SendMessageW(list_view, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&column));
}

void SetListViewText(HWND list_view, int item_index, int subitem_index, std::wstring* text) {
    LVITEMW item{};
    item.iSubItem = subitem_index;
    item.pszText = text->data();
    SendMessageW(list_view, LVM_SETITEMTEXTW, static_cast<WPARAM>(item_index),
                 reinterpret_cast<LPARAM>(&item));
}

} // namespace

PairDeviceDialog::PairDeviceDialog(HINSTANCE instance,
                                   HWND owner,
                                   std::vector<std::string> existing_device_ids,
                                   std::function<void(std::string, std::uint64_t, BluetoothAddressKind, std::string)> on_pair,
                                   std::function<void(std::string, std::optional<DeviceInfo>)> on_pair_completed)
    : instance_(instance),
      owner_(owner),
      existing_device_ids_(std::move(existing_device_ids)),
      on_pair_(std::move(on_pair)),
      on_pair_completed_(std::move(on_pair_completed)) {}

PairDeviceDialog::~PairDeviceDialog() {
    StopScan();
    if (hwnd_) {
        EndDialog(hwnd_, IDCANCEL);
        hwnd_ = nullptr;
    }
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void PairDeviceDialog::Show() {
    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), owner_,
                            PairDeviceDialog::DialogProc, reinterpret_cast<LPARAM>(this));
}

void PairDeviceDialog::SetConnectedDevices(const std::vector<ConnectedDevice>& devices) {
    if (!hwnd_ || !pairing_device_id_.has_value()) return;
    const auto pending_device_id = *pairing_device_id_;
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const ConnectedDevice& device) {
        return device.id == pending_device_id;
    });
    if (it != devices.end()) {
        PostMessageW(hwnd_, kPairingConnectedMessage, 0, 0);
    }
}

void PairDeviceDialog::SetDeviceInfo(const DeviceInfo& info) {
    if (!hwnd_ || !pairing_device_id_.has_value() || *pairing_device_id_ != info.device_id ||
        info.firmware_version.empty()) {
        return;
    }
    PostMessageW(hwnd_, kPairingSucceededMessage, 0,
                 reinterpret_cast<LPARAM>(new DeviceInfo(info)));
}

void PairDeviceDialog::SetPairingError(const std::string& device_id, const std::string& message) {
    if (!hwnd_ || !pairing_device_id_.has_value() || *pairing_device_id_ != device_id) return;
    PostMessageW(hwnd_, kPairingErrorMessage, 0, reinterpret_cast<LPARAM>(new std::string(message)));
}

INT_PTR CALLBACK PairDeviceDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* dialog = reinterpret_cast<PairDeviceDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<PairDeviceDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);

        dialog->RebuildUi();

        RECT window_rect{};
        GetWindowRect(hwnd, &window_rect);
        const int window_width = window_rect.right - window_rect.left;
        const int window_height = window_rect.bottom - window_rect.top;
        RECT work_area = GetWorkAreaForWindow(hwnd);
        const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
        const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        dialog->StartScan();
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR PairDeviceDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case kDeviceListChangedMessage:
        RebuildList();
        return TRUE;
    case kPairingConnectedMessage:
        HandlePairingConnected();
        return TRUE;
    case kPairingSucceededMessage: {
        std::unique_ptr<DeviceInfo> info(reinterpret_cast<DeviceInfo*>(l_param));
        if (info) HandlePairingSucceeded(*info);
        return TRUE;
    }
    case kPairingErrorMessage: {
        std::unique_ptr<std::string> message(reinterpret_cast<std::string*>(l_param));
        if (message) HandlePairingError(*message);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w_param) == IDOK) {
            PairSelectedDevice();
            return TRUE;
        }
        if (LOWORD(w_param) == IDCANCEL) {
            Close();
            return TRUE;
        }
        return FALSE;
    case WM_TIMER:
        if (w_param == kPairingTimeoutTimerId) {
            HandlePairingTimeout();
            return TRUE;
        }
        if (w_param == kPairingFinalizeTimerId) {
            HandlePairingFinalize();
            return TRUE;
        }
        return FALSE;
    case WM_NOTIFY: {
        auto* notify = reinterpret_cast<NMHDR*>(l_param);
        if (notify && notify->idFrom == kDeviceListId && notify->code == NM_DBLCLK) {
            PairSelectedDevice();
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        Close();
        return TRUE;
    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(w_param);
        if (new_dpi != 0 && new_dpi != dpi_) {
            dpi_ = new_dpi;
            auto* rect = reinterpret_cast<const RECT*>(l_param);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RebuildUi();
            RebuildList();
        }
        return TRUE;
    }
    case WM_DESTROY:
        KillTimer(hwnd_, kPairingTimeoutTimerId);
        KillTimer(hwnd_, kPairingFinalizeTimerId);
        StopScan();
        hwnd_ = nullptr;
        status_label_ = nullptr;
        device_list_ = nullptr;
        pair_button_ = nullptr;
        cancel_button_ = nullptr;
        return TRUE;
    default:
        return FALSE;
    }
}

LPCDLGTEMPLATE PairDeviceDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog_template{};
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 260;
    dialog_template.cy = 190;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, L"Pair VoiceStick");
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void PairDeviceDialog::RebuildUi() {
    DestroyControls();

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(440), Dp(320)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    BuildContent();
}

void PairDeviceDialog::DestroyControls() {
    if (device_list_ && IsWindow(device_list_)) DestroyWindow(device_list_);
    if (status_label_ && IsWindow(status_label_)) DestroyWindow(status_label_);
    if (pair_button_ && IsWindow(pair_button_)) DestroyWindow(pair_button_);
    if (cancel_button_ && IsWindow(cancel_button_)) DestroyWindow(cancel_button_);
    device_list_ = nullptr;
    status_label_ = nullptr;
    pair_button_ = nullptr;
    cancel_button_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void PairDeviceDialog::BuildContent() {
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&common_controls);

    ui_font_ = CreateUiFont(dpi_);
    const HFONT font = ui_font_;
    const int margin = Dp(16);
    const int list_height = Dp(200);
    const int button_width = Dp(86);
    const int button_height = Dp(30);
    const int button_gap = Dp(10);
    const int client_width = Dp(440);
    const int button_y = margin + list_height + Dp(16);
    const int cancel_x = client_width - margin - button_width;
    const int pair_x = cancel_x - button_gap - button_width;

    device_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                       LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                       LVS_NOSORTHEADER,
                                   margin, margin, client_width - 2 * margin, list_height, hwnd_,
                                   ControlId(kDeviceListId), instance_, nullptr);
    SendMessageW(device_list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    const int scrollbar_width = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
    const int list_width = client_width - 2 * margin - scrollbar_width - Dp(6);
    InsertColumn(device_list_, 0, L"VoiceStick", Dp(150));
    InsertColumn(device_list_, 1, L"Signal", Dp(86));
    InsertColumn(device_list_, 2, L"Bluetooth Address",
                 list_width - Dp(150) - Dp(86));

    status_label_ = CreateWindowExW(0, L"STATIC", L"Scanning", WS_CHILD | WS_VISIBLE,
                                    margin, button_y + Dp(5), pair_x - margin - button_gap,
                                    Dp(22), hwnd_, nullptr, instance_, nullptr);
    pair_button_ = CreateWindowExW(0, L"BUTTON", L"Pair", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                   pair_x, button_y, button_width, button_height, hwnd_,
                                   ControlId(IDOK), instance_, nullptr);
    cancel_button_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     cancel_x, button_y, button_width, button_height, hwnd_,
                                     ControlId(IDCANCEL), instance_, nullptr);
    for (HWND control : {device_list_, status_label_, pair_button_, cancel_button_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void PairDeviceDialog::StartScan() {
    StopScan();
    {
        std::lock_guard lock(mutex_);
        devices_.clear();
    }
    RebuildList();
    SetWindowTextW(status_label_, L"Scanning");
    watcher_ = winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher();
    watcher_.ScanningMode(
        winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode::Active);
    received_token_ = watcher_.Received([this](const auto& watcher, const auto& args) {
        HandleAdvertisement(watcher, args);
    });
    try {
        watcher_.Start();
    } catch (const winrt::hresult_error& error) {
        try {
            watcher_.Received(received_token_);
        } catch (...) {
        }
        watcher_ = nullptr;
        const auto text = ScanStartFailureText(error);
        SetWindowTextW(status_label_, text.c_str());
        EnableWindow(pair_button_, FALSE);
    } catch (...) {
        try {
            watcher_.Received(received_token_);
        } catch (...) {
        }
        watcher_ = nullptr;
        SetWindowTextW(status_label_, L"Bluetooth scan failed.");
        EnableWindow(pair_button_, FALSE);
    }
}

void PairDeviceDialog::StopScan() {
    if (!watcher_) return;
    try {
        watcher_.Received(received_token_);
        watcher_.Stop();
    } catch (...) {
    }
    watcher_ = nullptr;
}

void PairDeviceDialog::HandleAdvertisement(
    const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher&,
    const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args) {
    const auto name = Utf8FromHstring(args.Advertisement().LocalName());
    auto device_id = BleProtocol::DeviceIdFromName(name);
    if (!device_id.has_value()) return;
    // Already-paired devices belong to the auto-reconnect path, not the
    // pairing list; surfacing them here lets the user accidentally pair the
    // same device a second time.
    if (IsExistingDevice(*device_id)) return;

    PairingDevice device;
    device.bluetooth_address = args.BluetoothAddress();
    device.address_kind = AddressKindFromArgs(args);
    device.name = name.empty() ? "VS-" + *device_id : name;
    device.device_id = *device_id;
    device.rssi = args.RawSignalStrengthInDBm();

    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(devices_.begin(), devices_.end(), [&](const PairingDevice& existing) {
            return existing.bluetooth_address == device.bluetooth_address;
        });
        if (it == devices_.end()) {
            devices_.push_back(device);
        } else {
            *it = device;
        }
        std::sort(devices_.begin(), devices_.end(), [](const PairingDevice& lhs, const PairingDevice& rhs) {
            return lhs.rssi > rhs.rssi;
        });
    }
    if (hwnd_) PostMessageW(hwnd_, kDeviceListChangedMessage, 0, 0);
}

void PairDeviceDialog::RebuildList() {
    if (!device_list_ || !status_label_) return;
    const int selected = static_cast<int>(SendMessageW(device_list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                                                       MAKELPARAM(LVNI_SELECTED, 0)));
    SendMessageW(device_list_, LVM_DELETEALLITEMS, 0, 0);

    std::vector<PairingDevice> devices;
    {
        std::lock_guard lock(mutex_);
        devices = devices_;
    }
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        std::string title = "VS-" + device.device_id;
        if (IsExistingDevice(device.device_id)) title += " (paired)";
        auto name = Utf16(title);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.iSubItem = 0;
        item.pszText = name.data();
        SendMessageW(device_list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

        auto signal = Utf16(std::to_string(device.rssi) + " dBm");
        SetListViewText(device_list_, static_cast<int>(index), 1, &signal);

        auto address = Utf16(FormatBluetoothAddress(device.bluetooth_address));
        SetListViewText(device_list_, static_cast<int>(index), 2, &address);
    }
    if (!devices.empty()) {
        const int selection = selected >= 0 && selected < static_cast<int>(devices.size()) ? selected : 0;
        LVITEMW item{};
        item.state = LVIS_SELECTED | LVIS_FOCUSED;
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(device_list_, LVM_SETITEMSTATE, static_cast<WPARAM>(selection),
                     reinterpret_cast<LPARAM>(&item));
        SendMessageW(device_list_, LVM_ENSUREVISIBLE, static_cast<WPARAM>(selection), FALSE);
    }

    const auto status = devices.empty() ? L"Scanning" : Utf16(std::to_string(devices.size()) + " found");
    SetWindowTextW(status_label_, status.c_str());
}

void PairDeviceDialog::PairSelectedDevice() {
    if (!device_list_) return;
    if (pairing_device_id_.has_value()) return;
    const int selected = static_cast<int>(SendMessageW(device_list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                                                       MAKELPARAM(LVNI_SELECTED, 0)));
    std::vector<PairingDevice> devices;
    {
        std::lock_guard lock(mutex_);
        devices = devices_;
    }
    if (selected < 0 || selected >= static_cast<int>(devices.size())) {
        SetWindowTextW(status_label_, L"Select a device");
        return;
    }
    BeginPairing(devices[static_cast<std::size_t>(selected)]);
}

void PairDeviceDialog::Close() {
    if (!hwnd_) return;
    EndDialog(hwnd_, IDCANCEL);
}

void PairDeviceDialog::BeginPairing(const PairingDevice& device) {
    pairing_device_id_ = device.device_id;
    StopScan();
    EnableWindow(device_list_, FALSE);
    EnableWindow(pair_button_, FALSE);
    SetWindowTextW(pair_button_, L"Pairing...");
    auto status = Utf16("Pairing VS-" + device.device_id + "...");
    SetWindowTextW(status_label_, status.c_str());
    SetTimer(hwnd_, kPairingTimeoutTimerId, 30000, nullptr);
    if (on_pair_) {
        on_pair_(device.device_id, device.bluetooth_address, device.address_kind, device.name);
    }
}

void PairDeviceDialog::HandlePairingConnected() {
    if (!pairing_device_id_.has_value() || pairing_finalized_) return;
    auto status = Utf16("Connected to VS-" + *pairing_device_id_ + ". Finishing up...");
    SetWindowTextW(status_label_, status.c_str());
    // Give the device a brief window to push device_info via state notification,
    // but treat the BLE link being up as success even if device_info never
    // arrives (a known WinRT quirk where the first post-subscribe notification
    // is sometimes not delivered to the value-changed handler).
    SetTimer(hwnd_, kPairingFinalizeTimerId, kPairingFinalizeDelayMs, nullptr);
}

void PairDeviceDialog::HandlePairingSucceeded(const DeviceInfo& info) {
    if (!pairing_device_id_.has_value() || *pairing_device_id_ != info.device_id) return;
    FinalizePairing(info);
}

void PairDeviceDialog::HandlePairingFinalize() {
    KillTimer(hwnd_, kPairingFinalizeTimerId);
    if (!pairing_device_id_.has_value() || pairing_finalized_) return;
    FinalizePairing(std::nullopt);
}

void PairDeviceDialog::FinalizePairing(std::optional<DeviceInfo> info) {
    if (!pairing_device_id_.has_value() || pairing_finalized_) return;
    pairing_finalized_ = true;
    KillTimer(hwnd_, kPairingTimeoutTimerId);
    KillTimer(hwnd_, kPairingFinalizeTimerId);
    const auto device_id = *pairing_device_id_;
    auto status = info && !info->firmware_version.empty()
                      ? Utf16("Paired VS-" + device_id + " firmware " + info->firmware_version)
                      : Utf16("Paired VS-" + device_id);
    SetWindowTextW(status_label_, status.c_str());
    if (on_pair_completed_) on_pair_completed_(device_id, std::move(info));
    Close();
}

void PairDeviceDialog::HandlePairingError(const std::string& message) {
    KillTimer(hwnd_, kPairingTimeoutTimerId);
    KillTimer(hwnd_, kPairingFinalizeTimerId);
    pairing_device_id_.reset();
    pairing_finalized_ = false;
    EnableWindow(device_list_, TRUE);
    EnableWindow(pair_button_, TRUE);
    SetWindowTextW(pair_button_, L"Retry");
    SetWindowTextW(status_label_, Utf16(message).c_str());
    StartScan();
}

void PairDeviceDialog::HandlePairingTimeout() {
    KillTimer(hwnd_, kPairingTimeoutTimerId);
    KillTimer(hwnd_, kPairingFinalizeTimerId);
    auto timed_out_device = pairing_device_id_;
    pairing_device_id_.reset();
    pairing_finalized_ = false;
    EnableWindow(device_list_, TRUE);
    EnableWindow(pair_button_, TRUE);
    SetWindowTextW(pair_button_, L"Pair");
    SetWindowTextW(status_label_, L"Pairing timed out");
    if (on_pair_timeout && timed_out_device) on_pair_timeout(*timed_out_device);
    StartScan();
}

bool PairDeviceDialog::IsExistingDevice(const std::string& device_id) const {
    return std::find(existing_device_ids_.begin(), existing_device_ids_.end(), device_id) !=
           existing_device_ids_.end();
}

std::wstring PairDeviceDialog::Utf16(const std::string& text) const {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

int PairDeviceDialog::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

} // namespace voicestick
