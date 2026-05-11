#pragma once

#include "app_config.h"

#include <Windows.h>

#include <functional>
#include <vector>

namespace voicestick {

class SettingsDialog {
public:
    SettingsDialog(HINSTANCE instance, HWND parent, AppConfig config);
    ~SettingsDialog();

    void Show();

    std::function<void(AppConfig)> on_config_changed;

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    INT_PTR HandleMessage(UINT message, WPARAM w_param, LPARAM l_param);
    LPCDLGTEMPLATE BuildDialogTemplate();
    void RebuildUi();
    void DestroyControls();
    void BuildControls();
    void LoadConfigIntoControls();
    void SaveSettings();
    void UpdateProviderVisibility();
    void ApplyTrialApiKey();
    void ChooseDebugDirectory();
    bool IsLabelControl(HWND control) const;
    int Dp(int px) const;

    HINSTANCE instance_;
    HWND parent_;
    HWND hwnd_ = nullptr;
    AppConfig config_;
    UINT dpi_ = 96;

    HWND provider_combo_ = nullptr;
    HWND api_key_edit_ = nullptr;
    HWND apply_trial_button_ = nullptr;
    HWND resource_combo_ = nullptr;
    HWND hotwords_edit_ = nullptr;
    HWND llm_base_url_edit_ = nullptr;
    HWND llm_api_key_edit_ = nullptr;
    HWND llm_model_edit_ = nullptr;
    HWND debug_audio_check_ = nullptr;
    HWND debug_dir_edit_ = nullptr;
    HWND resource_label_ = nullptr;
    HFONT ui_font_ = nullptr;
    std::vector<BYTE> dialog_template_;
    std::vector<HWND> all_controls_;
    std::vector<HWND> label_controls_;

    static constexpr int kClientWidth = 520;
    static constexpr int kClientHeight = 500;
    static constexpr UINT kIdProviderCombo = 2001;
    static constexpr UINT kIdApiKeyEdit = 2002;
    static constexpr UINT kIdResourceCombo = 2003;
    static constexpr UINT kIdHotwordsEdit = 2004;
    static constexpr UINT kIdLlmBaseUrlEdit = 2005;
    static constexpr UINT kIdLlmApiKeyEdit = 2006;
    static constexpr UINT kIdLlmModelEdit = 2007;
    static constexpr UINT kIdDebugAudio = 2008;
    static constexpr UINT kIdDebugDirEdit = 2009;
    static constexpr UINT kIdChooseDir = 2010;
    static constexpr UINT kIdSave = 2011;
    static constexpr UINT kIdCancel = 2012;
    static constexpr UINT kIdApplyTrialApiKey = 2013;
};

} // namespace voicestick
