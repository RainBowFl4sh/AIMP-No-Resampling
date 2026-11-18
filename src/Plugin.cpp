// AIMP Prevent Resampling - switches the output (Windows device format, ASIO, Voicemeeter engine or
// PipeWire on Linux) to the sample rate of the current track so nothing gets resampled.
// Built against AIMP SDK v6.00 (build 3083); compiles to a Win32/Win64 DLL and a Linux .so.
#include "Menu.h"
#include "OptionsFrame.h"
#include "apiPlugin.h"

#if defined(_MSC_VER)
#define AR_EXPORT  // exported via PreventResampling.def (undecorated name even for 32-bit __stdcall)
#elif defined(_WIN32)
#define AR_EXPORT __declspec(dllexport)  // MinGW: -Wl,--kill-at strips the @4
#else
#define AR_EXPORT __attribute__((visibility("default")))
#endif


namespace ar {

static Engine* g_engine = nullptr;
static OptionsFrame* g_frame = nullptr;
static QuickSwitch* g_quick = nullptr;  // AIMP's menus and hotkeys (F1)
static bool g_updated = false;  // first start of a newer version than the last one (shown after loading)

// Before 2.6.1 the settings file was always in %APPDATA%\AIMP (Linux: $XDG_CONFIG_HOME/AIMP), also for a
// portable AIMP with its own profile folder. If the profile folder has no settings file yet, the old one is
// copied there - not moved: an installed AIMP may still use it. A [Session] in it (the old version restarted
// AIMP, e.g. for this update) is taken over with the copy, so it is removed from the old file.
// Returns the old file if it was copied.
static tstring TakeOverLegacySettings(const tstring& path) {
    tstring legacyDir = LegacySettingsDir(false);
    if (legacyDir.empty()) return tstring();
    tstring legacy = legacyDir + AR_T(AR_SEP "PreventResampling.ini");
    Ini current;
    current.Open(path);
    if (current.Existed()) return tstring();
    Ini old;
    old.Open(legacy);  // the same file as 'path' would exist already
    if (!old.Existed() || !old.HasSection(Config::Section()) || !old.SaveAs(path)) return tstring();
    Ini left;
    left.Open(legacy);
    if (left.Int(AR_T("Session"), AR_T("Saved"), 0)) {
        left.Clear(AR_T("Session"));
        left.Save();
    }
    return legacy;
}

// After an update (AIMP restarts to load a new version, or the DLL was copied by hand) - once per version
static void ShowUpdatedNotice() {
    if (g_engine && !(g_frame && g_frame->IsOpen())) CurrentLanguage() = ResolveLanguage(g_engine->cfg.language);
    tstring text = Trf("%1 for AIMP was updated to version %2.", {AR_T(AR_PLUGIN_NAME), AR_T(AR_VERSION)}) + AR_NL AR_NL +
                   Trf("The changes are on the About tab: Preferences \u2192 Plugins \u2192 %1 \u2192 About.", {AR_T(AR_PLUGIN_NAME)});
    if (!ShowMessage(text)) Log(AR_T("Update notice could not be shown (no message dialog in this AIMP version)"));
}

// Texts outside the settings page follow the plugin's language (the page may show another one while it is open)
static void UseLanguage() {
    if (g_engine && !(g_frame && g_frame->IsOpen())) CurrentLanguage() = ResolveLanguage(g_engine->cfg.language);
}

// F1: AIMP's menu or a hotkey switches the plugin on or off; AIMP shows the new state for 2 s
static void Toggle(const TChar* how) {
    if (!g_engine) return;
    bool on = !g_engine->cfg.enabled;
    UseLanguage();
    g_engine->SetEnabled(on, how);
    QuickSwitch::Notify(Trf(on ? "%1 is on" : "%1 is off", {AR_T(AR_PLUGIN_NAME)}));
}

static void ShowChainWindow() {
    if (!g_engine) return;
    UseLanguage();
    ChainWindow::Open(g_engine, ChainWindow::MainWindow());
}

static void ShowSettings(int tab = -1) {
    auto svc = Service<IAIMPServiceOptionsDialog>(IID_IAIMPServiceOptionsDialog);
    if (!svc || !g_frame) return;
    g_frame->pendingTab = tab;  // used when AIMP creates the page
    svc->FrameShow(static_cast<IAIMPOptionsDialogFrame*>(g_frame), 1);
    // AIMP 6 opens the first tab again after creating the page, so the tab is set once more afterwards
    if (tab >= 0 && g_frame->IsOpen()) g_frame->ShowTab(tab);
    g_frame->pendingTab = -1;
}

// "Restart AIMP now" (bit-perfect mode) was pressed on the settings page: open it again on the same tab
// AIMP restarts (bit-perfect mode or the legacy method): the plugin's windows that are open now come back
// after the start, as they were - the settings page on the same tab, the audio chain at the same place
static void RememberWindowsForRestart() {
    Ini& ini = Ini::Get();
    tstring chain = ChainWindow::Place();
    if (!chain.empty()) ini.Set(AR_T("Restart"), AR_T("ReopenChain"), chain);
    if (g_frame && g_frame->IsOpen() && !ini.Has(AR_T("Restart"), AR_T("ReopenSettings")))
        ini.Set(AR_T("Restart"), AR_T("ReopenSettings"), (long long)g_frame->ActiveTab());
    ini.Save();
}

static void ReopenSettingsAfterRestart() {
    Ini& ini = Ini::Get();
    bool settings = ini.Has(AR_T("Restart"), AR_T("ReopenSettings"));
    bool chain = ini.Has(AR_T("Restart"), AR_T("ReopenChain"));
    if (!settings && !chain) return;
    int tab = settings ? (int)ini.Int(AR_T("Restart"), AR_T("ReopenSettings"), 0) : -1;
    tstring place = chain ? ini.Str(AR_T("Restart"), AR_T("ReopenChain"), tstring()) : tstring();
    ini.Remove(AR_T("Restart"), AR_T("ReopenSettings"));
    ini.Remove(AR_T("Restart"), AR_T("ReopenChain"));
    ini.Save();
    if (!g_engine) return;
    // the audio chain first: AIMP 4 shows its preferences modally
    if (chain) {
        Log(AR_T("Opening the audio chain again after the restart"));
        g_engine->Later([place] {
            if (!g_engine) return;
            UseLanguage();
            ChainWindow::Open(g_engine, ChainWindow::MainWindow(), place);
        });
    }
    if (settings) {
        Log(AR_T("Opening the settings again after the restart (tab ") + Num(tab) + AR_T(")"));
        g_engine->Later([tab] { ShowSettings(tab); });
    }
}

static bool g_terminating;  // AIMP_MSG_EVENT_TERMINATING arrived

class Hook : public ComObject<IAIMPMessageHook> {
protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPMessageHook); }

public:
    void WINAPI CoreMessage(DWORD msg, INT32 param1, void*, HRESULT*) override {
        if (!g_engine) return;
        if (msg == (DWORD)AIMP_MSG_EVENT_STREAM_START || msg == (DWORD)AIMP_MSG_EVENT_PLAYER_STATE) {
            if (msg == (DWORD)AIMP_MSG_EVENT_STREAM_START) g_engine->OnStreamStart();
            else g_engine->OnPlayerState(param1);
            // The audio chain follows at once (there is no Refresh button), after the plugin has handled the track.
            // Only while it is shown and not while AIMP closes: AIMP 4 does not finish closing while a main-thread
            // task of the plugin is still queued (it stops playback during shutdown).
            if (!g_terminating && (ChainWindow::Current() || (g_frame && g_frame->IsOpen())))
                g_engine->Later([] {
                    if (ChainWindow::Current()) ChainWindow::Current()->Refresh();
                    if (g_frame && g_frame->IsOpen()) g_frame->RefreshStats();
                });
        }
        else if (msg == (DWORD)AIMP_MSG_EVENT_TERMINATING) g_terminating = true;
        else if (msg == (DWORD)AIMP_MSG_EVENT_LOADED) {
            g_engine->bitPerfect.Apply(g_engine->cfg);
            g_engine->ResumeAfterRestart();
            ReopenSettingsAfterRestart();
            const Config& c = g_engine->cfg;
            if (c.updateCheck && Updater::Due(c.updateInterval)) g_engine->updater.Check(false, c.updateAutoInstall);
            if (g_updated) {
                g_updated = false;
                g_engine->Later(ShowUpdatedNotice);
            }
        }
        else if (msg == (DWORD)AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION) {  // about once a second while playing
            g_engine->Tick();  // B5: did an output change?
            if (ChainWindow::Current()) ChainWindow::Current()->Tick();
            if (g_frame && g_frame->IsOpen()) g_frame->RefreshStats();
        } else if (msg == (DWORD)AIMP_MSG_EVENT_LANGUAGE) {
            if (g_frame) g_frame->OnAimpLanguageChanged();
            UseLanguage();
            if (g_quick) g_quick->Retranslate();
        }
    }
};

// Gear button in the plugin list: opens the plugin's settings page
class SettingsDialog : public ComObject<IAIMPExternalSettingsDialog> {
protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPExternalSettingsDialog); }

public:
    void WINAPI Show(HWND) override { ShowSettings(); }
};

class Plugin : public ComObject<IAIMPPlugin> {
    Ptr<IAIMPServiceMessageDispatcher> disp_;
    Ptr<IAIMPMessageHook> hook_;
    Ptr<IAIMPOptionsDialogFrame> frame_;
    Ptr<IAIMPExternalSettingsDialog> settings_;

public:
    Plugin() {
        SettingsDialog* d = new SettingsDialog();
        d->AddRef();
        *settings_.Out() = d;
    }

    // According to the SDK, IAIMPExternalSettingsDialog is queried on the plugin object itself
    HRESULT __unknwncall QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (ppv && EqualGUID(riid, IID_IAIMPExternalSettingsDialog) && settings_) {
            settings_->AddRef();
            *ppv = settings_.Get();
            return S_OK;
        }
        return ComObject<IAIMPPlugin>::QueryInterface(riid, ppv);
    }

    PChar WINAPI InfoGet(INT32 id) override {
        switch (id) {
            // Must match the name of the settings page exactly (links the plugin list entry to it)
            case AIMP_PLUGIN_INFO_NAME: return const_cast<PChar>(AR_T(AR_PLUGIN_NAME));
            case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<PChar>(AR_T(AR_AUTHOR));
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:
                return const_cast<PChar>(AR_T("v" AR_VERSION " \u2013 switches the output sample rate to the track's rate (no resampling)"));
            case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:
                return const_cast<PChar>(AR_T("Version " AR_VERSION ". Supports WASAPI shared, Voicemeeter (engine rate and "
                                              "A1-A5 devices) and PipeWire on Linux; ASIO, WASAPI exclusive and DirectSound "
                                              "by changing AIMP's own rate without a restart. For AIMP 4.70, 5 and 6. Disabled after installation - "
                                              "enable it in Preferences -> Plugins -> " AR_PLUGIN_NAME "."));
        }
        return nullptr;
    }
    DWORD WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }

    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        Core() = core;
        Ptr<IAIMPString> profile;
        tstring dir;
        if (Succeeded(core->GetPath(AIMP_CORE_PATH_PROFILE, profile.Out())) && profile) {
            dir = FromString(profile.Get());
            if (!dir.empty() && dir.back() != AR_T('\\') && dir.back() != AR_T('/')) dir += AR_T("/");
            Logger::Get().SetFile(dir + AR_T("PreventResampling.log"));
        }

        auto* e = new Engine();
        e->player = Service<IAIMPServicePlayer>(IID_IAIMPServicePlayer);
        e->threads = Service<IAIMPServiceThreads>(IID_IAIMPServiceThreads);
        disp_ = Service<IAIMPServiceMessageDispatcher>(IID_IAIMPServiceMessageDispatcher);
        if (!e->player || !e->threads || !disp_) {
            // e.g. AIMP 3.60: its SDK has no threads service (AIMP 4 or newer is required)
            Log(tstring(AR_T("AIMP services not available:")) + (e->player ? AR_T("") : AR_T(" player")) +
                (e->threads ? AR_T("") : AR_T(" threads")) + (disp_ ? AR_T("") : AR_T(" message dispatcher")) +
                AR_T(" - this plugin needs AIMP 4.70 or newer"));
            delete e;
            disp_.Reset();
            Core() = nullptr;
            return E_FAIL;
        }
        // Settings next to AIMP.ini in AIMP's profile folder (the forum rules ask for it; a portable AIMP
        // keeps them in its own folder). Without a profile folder: %APPDATA%\AIMP as before 2.6.1.
        tstring settingsDir = dir;
        if (settingsDir.empty()) {
            settingsDir = LegacySettingsDir();
            if (!settingsDir.empty()) settingsDir += AR_T(AR_SEP);
        }
        tstring takenOver = dir.empty() ? tstring() : TakeOverLegacySettings(settingsDir + AR_T("PreventResampling.ini"));
        Ini::Get().Open(settingsDir + AR_T("PreventResampling.ini"));
        if (!Ini::Get().Existed()) {
            // Settings of versions before 2.5 lived in AIMP's own configuration - remove them so a
            // new installation really starts with the defaults (plugin disabled)
            auto c = Service<IAIMPServiceConfig>(IID_IAIMPServiceConfig);
            if (c) {
                for (const TChar* k : {AR_T("PreventResampling"), AR_T("AutoRate")}) {
                    auto ks = MakeString(k);
                    if (ks) c->Delete(ks.Get());
                }
                c->FlushCache();
            }
        }
        e->cfg.Load();
        CurrentLanguage() = ResolveLanguage(e->cfg.language);
        {
            tstring aimpLang;
            AimpLanguage(&aimpLang);
            Log(AR_T("AIMP language: ") + (aimpLang.empty() ? tstring(AR_T("?")) : aimpLang) + AR_T(" -> plugin language: ") +
                FromUtf8(LanguageName((Language)CurrentLanguage())) +
                (e->cfg.language == LanguageSetting::Auto ? tstring(AR_T(" (automatic)")) : tstring()));
        }
        bool existed = Ini::Get().Existed();
        if (!existed) e->cfg.Save();  // new installation: write all defaults (plugin off)
        // Version of the last start: newer now = an update (versions before 2.6.0 did not store it)
        tstring lastRun = Ini::Get().Str(AR_T("Updates"), AR_T("RunVersion"));
        g_updated = existed && (lastRun.empty() || CompareVersions(ToUtf8(lastRun), AR_VERSION) < 0);
        if (lastRun != AR_T(AR_VERSION)) {
            Ini::Get().Set(AR_T("Updates"), AR_T("RunVersion"), AR_T(AR_VERSION));
            Ini::Get().Save();
        }
        e->LoadStats();
        g_engine = e;
        e->Start();

        Hook* h = new Hook();
        h->AddRef();
        *hook_.Out() = h;
        disp_->Hook(hook_.Get());

        OptionsFrame* f = new OptionsFrame(e);
        f->AddRef();
        *frame_.Out() = f;
        g_frame = f;
        core->RegisterExtension(IID_IAIMPServiceOptionsDialog, frame_.Get());
        e->onStatsChanged = [] {
            if (g_frame && g_frame->IsOpen()) g_frame->RefreshStats();
            if (ChainWindow::Current()) ChainWindow::Current()->Refresh();
        };
        // F1: menus and hotkeys
        g_quick = new QuickSwitch();
        g_quick->isEnabled = [] { return g_engine && g_engine->cfg.enabled; };
        g_quick->toggle = [](const TChar* how) { Toggle(how); };
        g_quick->showChain = [] { ShowChainWindow(); };
        g_quick->showSettings = [] { ShowSettings(); };
        bool menus = g_quick->Register();
        e->onEnabledChanged = [] {
            if (g_quick) g_quick->Update();
            if (g_frame && g_frame->IsOpen()) g_frame->OnEnabledChanged();
            if (ChainWindow::Current()) ChainWindow::Current()->Refresh();
        };
        e->updater.downloadDir = settingsDir.substr(0, settingsDir.size() - 1);
        e->updater.onChanged = [] { if (g_frame && g_frame->IsOpen()) g_frame->RefreshAbout(); };
        e->beforeRestart = RememberWindowsForRestart;
        e->updater.onPackageOpened = [] { if (g_engine) g_engine->WatchForUpdate(); };

        Log(tstring(AR_T(AR_PLUGIN_NAME " " AR_VERSION " started (")) +
#ifdef _WIN32
            (sizeof(void*) == 8 ? AR_T("Windows x64") : AR_T("Windows x86")) +
#else
            AR_T("Linux") +
#endif
            AR_T(")"));
        Log(AR_T("Settings: ") + Ini::Get().Path() +
            (!takenOver.empty() ? AR_T(" (taken over from ") + takenOver + AR_T(")") : Ini::Get().Existed() ? tstring() : tstring(AR_T(" (new)"))));
        if (g_updated)
            Log(AR_T("Updated from ") + (lastRun.empty() ? tstring(AR_T("an older version")) : lastRun) + AR_T(" to " AR_VERSION));
        if (!e->cfg.enabled)
            Log(AR_T("Plugin is disabled - enable it in Preferences -> Plugins -> " AR_PLUGIN_NAME));
        if (!menus) Log(AR_T("AIMP offers no menus or hotkeys for plugins - switch the plugin on its settings page"));
        e->aimpSync.profileDir = dir;
        e->aimpSync.LogDiagnostics(e->player.Get());
        e->aimpSync.LoadResume();
#ifdef _WIN32
        // After a restart by the plugin: AIMP's windows back to their state, place and focus
        if (!e->aimpSync.resume.windows.empty()) win::WindowRestorer::Get().Begin(e->aimpSync.resume.windows);
#endif
        return S_OK;
    }

    HRESULT WINAPI Finalize() override {
        Engine* e = g_engine;
        if (!e) return S_OK;
#ifdef _WIN32
        win::WindowRestorer::Get().End();
        win::LiveRate::Get().Cancel();  // a hidden switch of AIMP's rate that is still running
#endif
        if (disp_ && hook_) disp_->Unhook(hook_.Get());
        hook_.Reset();
        ChainWindow::CloseAll();
        e->onEnabledChanged = nullptr;
        if (g_quick) {
            g_quick->Unregister();
            delete g_quick;
            g_quick = nullptr;
        }
        if (frame_) Core()->UnregisterExtension(frame_.Get());
        g_frame = nullptr;
        frame_.Reset();
#ifdef _WIN32
        fx::ShutdownGdip();  // started by the picture on the About tab
#endif
        g_engine = nullptr;
        e->onStatsChanged = nullptr;
        e->updater.Cancel();
        e->PatchAimpIniAtExit();
        e->SaveStats();
        bool finished = e->Stop();
        Log(AR_T(AR_PLUGIN_NAME " stopped"));
        if (finished) delete e;  // otherwise leak on purpose, the worker is still running
        disp_.Reset();
        Core() = nullptr;
        return S_OK;
    }

    void WINAPI SystemNotification(INT32, IUnknown*) override {}
};

}  // namespace ar

#ifdef _WIN32
// Restart helper, called as: rundll32 PreventResampling.dll,RestartAimp <arguments>
extern "C" AR_EXPORT void CALLBACK RestartAimpW(HWND, HINSTANCE, LPWSTR cmdLine, int) {
    ar::win::RunRestartHelper(cmdLine);
}
#endif

extern "C" AR_EXPORT HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    ar::Plugin* p = new ar::Plugin();
    p->AddRef();
    *header = p;
    return S_OK;
}
