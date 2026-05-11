#include "settings_dialog.h"
#include "dpi_util.h"
#include "voice_stick_cloud_api_win.h"

#include <ShlObj.h>
#include <CommCtrl.h>
#include <Shellapi.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
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

std::wstring GetWindowText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<std::size_t>(len));
    return text;
}

std::string JoinHotwords(const std::vector<std::string>& hotwords) {
    std::string text;
    for (const auto& hotword : hotwords) {
        if (!text.empty()) text += "\r\n";
        text += hotword;
    }
    return text;
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateLeftLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, h, parent, nullptr, inst, nullptr);
}

HWND CreateEdit(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst,
                DWORD extra_style = 0) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra_style,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
}

HWND CreateMultilineEdit(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                               ES_AUTOVSCROLL | ES_WANTRETURN,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                           inst, nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                  UINT id, HINSTANCE inst, DWORD style = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

HWND CreateCombo(HWND parent, int x, int y, int w, int h, UINT id, HINSTANCE inst) {
    return CreateWindowExW(0, L"COMBOBOX", L"",
                           WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
}

} // namespace

SettingsDialog::SettingsDialog(HINSTANCE instance, HWND parent, AppConfig config)
    : instance_(instance), parent_(parent), config_(std::move(config)) {}

SettingsDialog::~SettingsDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void SettingsDialog::Show() {
    config_ = AppConfig::Load();

    if (hwnd_) {
        LoadConfigIntoControls();
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return;
    }

    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                            SettingsDialog::DialogProc, reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK SettingsDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* dialog = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<SettingsDialog*>(l_param);
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
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR SettingsDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kIdSave:
            SaveSettings();
            return TRUE;
        case kIdCancel:
            EndDialog(hwnd_, IDCANCEL);
            return TRUE;
        case kIdChooseDir:
            ChooseDebugDirectory();
            return TRUE;
        case kIdApplyTrialApiKey:
            ApplyTrialApiKey();
            return TRUE;
        case kIdProviderCombo:
            if (HIWORD(w_param) == CBN_SELCHANGE) {
                int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
                const auto& key = idx == 0 ? config_.voicestick_api_key : config_.volcengine_api_key;
                SetWindowTextW(api_key_edit_, Utf16(key).c_str());
                UpdateProviderVisibility();
            }
            return TRUE;
        case kIdApiKeyEdit:
            if (HIWORD(w_param) == EN_CHANGE) UpdateProviderVisibility();
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hwnd_, IDCANCEL);
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
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        const auto control = reinterpret_cast<HWND>(l_param);
        if (IsLabelControl(control)) {
            auto dc = reinterpret_cast<HDC>(w_param);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_DESTROY:
        hwnd_ = nullptr;
        provider_combo_ = nullptr;
        api_key_edit_ = nullptr;
        apply_trial_button_ = nullptr;
        resource_combo_ = nullptr;
        hotwords_edit_ = nullptr;
        llm_base_url_edit_ = nullptr;
        llm_api_key_edit_ = nullptr;
        llm_model_edit_ = nullptr;
        debug_audio_check_ = nullptr;
        debug_dir_edit_ = nullptr;
        resource_label_ = nullptr;
        all_controls_.clear();
        label_controls_.clear();
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

LPCDLGTEMPLATE SettingsDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog_template{};
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 300;
    dialog_template.cy = 210;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, L"VoiceStick Settings");
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void SettingsDialog::RebuildUi() {
    DestroyControls();

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    BuildControls();
    LoadConfigIntoControls();
}

void SettingsDialog::DestroyControls() {
    for (HWND control : all_controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    all_controls_.clear();
    label_controls_.clear();
    provider_combo_ = nullptr;
    api_key_edit_ = nullptr;
    apply_trial_button_ = nullptr;
    resource_combo_ = nullptr;
    hotwords_edit_ = nullptr;
    llm_base_url_edit_ = nullptr;
    llm_api_key_edit_ = nullptr;
    llm_model_edit_ = nullptr;
    debug_audio_check_ = nullptr;
    debug_dir_edit_ = nullptr;
    resource_label_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void SettingsDialog::BuildControls() {
    ui_font_ = CreateUiFont(dpi_);
    const HFONT font = ui_font_;

    auto remember = [&](HWND control) {
        if (control) all_controls_.push_back(control);
        return control;
    };
    auto remember_label = [&](HWND control) {
        if (control) label_controls_.push_back(control);
        return remember(control);
    };

    const int label_w = Dp(110);
    const int ctrl_x = Dp(130);
    const int ctrl_w = Dp(350);
    const int row_h = Dp(28);
    int y = Dp(20);

    remember_label(CreateLabel(hwnd_, L"Provider:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    provider_combo_ = remember(CreateCombo(hwnd_, ctrl_x, y, ctrl_w, Dp(200),
                                           kIdProviderCombo, instance_));
    SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"VoiceStick Cloud"));
    SendMessageW(provider_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Volcengine"));
    y += row_h + Dp(10);

    remember_label(CreateLabel(hwnd_, L"API Key:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    const int apply_btn_w = Dp(102);
    api_key_edit_ = remember(CreateEdit(hwnd_, ctrl_x, y, ctrl_w - apply_btn_w - Dp(8), Dp(24),
                                        kIdApiKeyEdit, instance_, ES_PASSWORD));
    apply_trial_button_ = remember(CreateButton(hwnd_, L"Apply Trial",
                                                ctrl_x + ctrl_w - apply_btn_w, y,
                                                apply_btn_w, Dp(24),
                                                kIdApplyTrialApiKey, instance_));
    y += row_h + Dp(10);

    resource_label_ = remember_label(CreateLabel(hwnd_, L"Resource ID:", Dp(10),
                                                 y + Dp(3), label_w, Dp(20), instance_));
    resource_combo_ = remember(CreateCombo(hwnd_, ctrl_x, y, ctrl_w, Dp(200),
                                           kIdResourceCombo, instance_));
    for (const auto& id : AppConfig::SupportedResourceIds()) {
        SendMessageW(resource_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(Utf16(id).c_str()));
    }
    y += row_h + Dp(16);

    remember_label(CreateLabel(hwnd_, L"Hotwords:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    hotwords_edit_ = remember(CreateMultilineEdit(hwnd_, ctrl_x, y, ctrl_w, Dp(74),
                                                  kIdHotwordsEdit, instance_));
    y += Dp(80);
    remember_label(CreateLeftLabel(hwnd_, L"Separate hotwords with commas or new lines.",
                                   ctrl_x, y, ctrl_w, Dp(16), instance_));
    y += Dp(26);

    remember_label(CreateLabel(hwnd_, L"LLM Base URL:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    llm_base_url_edit_ = remember(CreateEdit(hwnd_, ctrl_x, y, ctrl_w, Dp(24),
                                             kIdLlmBaseUrlEdit, instance_));
    y += row_h + Dp(10);

    remember_label(CreateLabel(hwnd_, L"LLM API Key:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    llm_api_key_edit_ = remember(CreateEdit(hwnd_, ctrl_x, y, ctrl_w, Dp(24),
                                            kIdLlmApiKeyEdit, instance_, ES_PASSWORD));
    y += row_h + Dp(10);

    remember_label(CreateLabel(hwnd_, L"LLM Model:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    llm_model_edit_ = remember(CreateEdit(hwnd_, ctrl_x, y, ctrl_w, Dp(24),
                                          kIdLlmModelEdit, instance_));
    y += row_h + Dp(16);

    remember_label(CreateLabel(hwnd_, L"Debug:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    debug_audio_check_ = remember(CreateButton(hwnd_, L"Save debug audio files", ctrl_x, y,
                                               ctrl_w, Dp(22), kIdDebugAudio, instance_,
                                               BS_AUTOCHECKBOX));
    y += row_h + Dp(10);

    remember_label(CreateLabel(hwnd_, L"Audio Folder:", Dp(10), y + Dp(3), label_w,
                               Dp(20), instance_));
    debug_dir_edit_ = remember(CreateEdit(hwnd_, ctrl_x, y, ctrl_w - Dp(80),
                                          Dp(24), kIdDebugDirEdit, instance_, ES_READONLY));
    remember(CreateButton(hwnd_, L"Browse...", ctrl_x + ctrl_w - Dp(75), y,
                          Dp(75), Dp(24), kIdChooseDir, instance_));
    y += row_h + Dp(20);

    const int btn_w = Dp(80);
    const int btn_h = Dp(30);
    remember(CreateButton(hwnd_, L"Save", Dp(kClientWidth - 200), y, btn_w, btn_h,
                          kIdSave, instance_));
    remember(CreateButton(hwnd_, L"Cancel", Dp(kClientWidth - 105), y, btn_w, btn_h,
                          kIdCancel, instance_));

    for (HWND control : all_controls_) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void SettingsDialog::LoadConfigIntoControls() {
    SendMessageW(provider_combo_, CB_SETCURSEL,
                 config_.asr_provider == AsrProvider::kVoiceStickCloud ? 0 : 1, 0);

    const auto& key = config_.asr_provider == AsrProvider::kVoiceStickCloud
                          ? config_.voicestick_api_key
                          : config_.volcengine_api_key;
    SetWindowTextW(api_key_edit_, Utf16(key).c_str());

    auto resource_wide = Utf16(config_.resource_id);
    int idx = static_cast<int>(SendMessageW(resource_combo_, CB_FINDSTRINGEXACT, -1,
                                            reinterpret_cast<LPARAM>(resource_wide.c_str())));
    SendMessageW(resource_combo_, CB_SETCURSEL, idx >= 0 ? idx : 0, 0);

    SetWindowTextW(hotwords_edit_, Utf16(JoinHotwords(config_.asr_hotwords)).c_str());
    SetWindowTextW(llm_base_url_edit_, Utf16(config_.llm_base_url).c_str());
    SetWindowTextW(llm_api_key_edit_, Utf16(config_.llm_api_key).c_str());
    SetWindowTextW(llm_model_edit_, Utf16(config_.llm_model).c_str());

    SendMessageW(debug_audio_check_, BM_SETCHECK, config_.debug_audio_cache ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(debug_dir_edit_, config_.debug_audio_directory.c_str());

    UpdateProviderVisibility();
}

void SettingsDialog::SaveSettings() {
    int provider_idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    AsrProvider new_provider = (provider_idx == 0) ? AsrProvider::kVoiceStickCloud
                                                   : AsrProvider::kVolcengine;

    auto api_key = Utf8(GetWindowText(api_key_edit_));
    if (new_provider == AsrProvider::kVoiceStickCloud) {
        config_.voicestick_api_key = api_key;
    } else {
        config_.volcengine_api_key = api_key;
    }
    config_.asr_provider = new_provider;
    config_.llm_base_url = Utf8(GetWindowText(llm_base_url_edit_));
    config_.llm_api_key = Utf8(GetWindowText(llm_api_key_edit_));
    config_.llm_model = Utf8(GetWindowText(llm_model_edit_));
    config_.asr_hotwords = ParseHotwordList(Utf8(GetWindowText(hotwords_edit_)));

    wchar_t resource_buf[256]{};
    int res_idx = static_cast<int>(SendMessageW(resource_combo_, CB_GETCURSEL, 0, 0));
    if (res_idx >= 0) {
        SendMessageW(resource_combo_, CB_GETLBTEXT, res_idx,
                     reinterpret_cast<LPARAM>(resource_buf));
        config_.resource_id = Utf8(resource_buf);
    }

    config_.debug_audio_cache = SendMessageW(debug_audio_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    auto dir = GetWindowText(debug_dir_edit_);
    if (!dir.empty()) config_.debug_audio_directory = dir;

    config_.Save();
    EndDialog(hwnd_, IDOK);
    if (on_config_changed) on_config_changed(config_);
}

void SettingsDialog::UpdateProviderVisibility() {
    int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    bool is_volcengine = (idx == 1);
    ShowWindow(resource_combo_, is_volcengine ? SW_SHOW : SW_HIDE);
    ShowWindow(resource_label_, is_volcengine ? SW_SHOW : SW_HIDE);
    const bool is_cloud = (idx == 0);
    const bool api_key_empty = GetWindowText(api_key_edit_).empty();
    ShowWindow(apply_trial_button_, is_cloud && api_key_empty ? SW_SHOW : SW_HIDE);
}

void SettingsDialog::ApplyTrialApiKey() {
    int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    if (idx != 0) return;

    EnableWindow(apply_trial_button_, FALSE);
    SetWindowTextW(apply_trial_button_, L"Applying...");
    UpdateWindow(apply_trial_button_);

    const std::string device_id = config_.paired_device_ids.empty()
                                      ? std::string()
                                      : config_.paired_device_ids.front();
    auto result = ApplyVoiceStickCloudTrialApiKey(config_.voicestick_cloud_url, device_id);

    SetWindowTextW(apply_trial_button_, L"Apply Trial");
    EnableWindow(apply_trial_button_, TRUE);

    if (!result.api_key.empty()) {
        SetWindowTextW(api_key_edit_, Utf16(result.api_key).c_str());
        UpdateProviderVisibility();
        return;
    }

    if (!result.url.empty()) {
        const auto wide_url = Utf16(result.url);
        auto* shell_result = ShellExecuteW(hwnd_, L"open", wide_url.c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(shell_result) <= 32) {
            MessageBoxW(hwnd_, L"Could not open the VoiceStick Cloud signup page.",
                        L"Could Not Apply Trial API Key", MB_ICONERROR | MB_OK);
        }
        UpdateProviderVisibility();
        return;
    }

    MessageBoxW(hwnd_, Utf16(result.error.empty()
                             ? "Could not apply a trial API Key."
                             : result.error).c_str(),
                L"Could Not Apply Trial API Key", MB_ICONERROR | MB_OK);
    UpdateProviderVisibility();
}

void SettingsDialog::ChooseDebugDirectory() {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_IFileDialog, reinterpret_cast<void**>(&dialog)))) {
        return;
    }
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

    auto current_dir = GetWindowText(debug_dir_edit_);
    if (!current_dir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(current_dir.c_str(), nullptr,
                                                  IID_IShellItem, reinterpret_cast<void**>(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                SetWindowTextW(debug_dir_edit_, path);
                CoTaskMemFree(path);
            }
            result->Release();
        }
    }
    dialog->Release();
}

bool SettingsDialog::IsLabelControl(HWND control) const {
    return std::find(label_controls_.begin(), label_controls_.end(), control) !=
           label_controls_.end();
}

int SettingsDialog::Dp(int px) const {
    return voicestick::ScalePx(px, dpi_);
}

} // namespace voicestick
