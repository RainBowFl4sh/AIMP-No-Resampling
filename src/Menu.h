// Quick switch (F1): the plugin on / off, the audio chain and the settings from AIMP's menus (main menu and
// tray icon), and the same as actions for hotkeys (Preferences -> Player -> Hot keys, group "Prevent Resampling"; AIMP 4, 5, 6 and Linux).
// No default keys: they could clash with AIMP's own keys or other plugins - the user picks them.
// Everything runs on AIMP's main thread.
#pragma once
#include <functional>

#include "UiCommon.h"
#include "apiActions.h"
#include "apiMenu.h"
#include "apiMessages.h"

namespace ar {

// Called by AIMP when a menu item is clicked, shown, or an action's hotkey is pressed
class ActionHandler : public ComObject<IAIMPActionEvent> {
    std::function<void()> fn_;

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPActionEvent); }

public:
    explicit ActionHandler(std::function<void()> f) : fn_(std::move(f)) {}
    void WINAPI OnExecute(IUnknown*) override { if (fn_) fn_(); }
    void Disconnect() { fn_ = nullptr; }
};

class QuickSwitch {
public:
    // Set by the plugin before Register()
    std::function<bool()> isEnabled;
    std::function<void(const TChar* how)> toggle;
    std::function<void()> showChain, showSettings;

private:
    struct Entry {
        Ptr<IAIMPPropertyList> obj;  // IAIMPAction or IAIMPMenuItem
        int captionProp;
        std::function<tstring()> caption;
        bool checkbox;
    };
    std::vector<Entry> entries_;
    std::vector<ActionHandler*> handlers_;
    int registered_ = 0;

    ActionHandler* Handler(std::function<void()> f) {
        auto* h = new ActionHandler(std::move(f));
        h->AddRef();
        handlers_.push_back(h);
        return h;
    }

    template <typename T>
    Ptr<T> Create(const GUID& iid) {
        Ptr<T> p;
        if (Core() && Succeeded(Core()->CreateObject(iid, p.OutV())) && p) return p;
        return Ptr<T>();
    }

    static Ptr<IAIMPPropertyList> Keep(IAIMPPropertyList* p) {
        Ptr<IAIMPPropertyList> r;
        if (p) {
            p->AddRef();
            *r.Out() = p;
        }
        return r;
    }

    bool Register(const GUID& service, IAIMPPropertyList* obj) {
        if (!obj || Failed(Core()->RegisterExtension(service, obj))) return false;
        registered_++;
        return true;
    }

    void AddAction(const TChar* id, std::function<tstring()> name, std::function<void()> run) {
        auto a = Create<IAIMPAction>(IID_IAIMPAction);
        if (!a) return;
        SetUiText(a.Get(), AIMP_ACTION_PROPID_ID, id);
        SetUiText(a.Get(), AIMP_ACTION_PROPID_NAME, name());
        SetUiText(a.Get(), AIMP_ACTION_PROPID_GROUPNAME, AR_T(AR_PLUGIN_NAME));
        a->SetValueAsObject(AIMP_ACTION_PROPID_EVENT, Handler(std::move(run)));
        if (Register(IID_IAIMPServiceActionManager, a.Get()))
            entries_.push_back({Keep(a.Get()), AIMP_ACTION_PROPID_NAME, std::move(name), false});
    }

    Ptr<IAIMPMenuItem> AddItem(IAIMPMenuItem* parent, const tstring& id, std::function<tstring()> caption,
                               std::function<void()> click, bool checkbox) {
        auto m = Create<IAIMPMenuItem>(IID_IAIMPMenuItem);
        if (!m || !parent) return Ptr<IAIMPMenuItem>();
        SetUiText(m.Get(), AIMP_MENUITEM_PROPID_ID, id);
        SetUiText(m.Get(), AIMP_MENUITEM_PROPID_CAPTION, caption());
        m->SetValueAsObject(AIMP_MENUITEM_PROPID_PARENT, parent);
        if (click) m->SetValueAsObject(AIMP_MENUITEM_PROPID_EVENT, Handler(std::move(click)));
        if (checkbox) {
            m->SetValueAsInt32(AIMP_MENUITEM_PROPID_STYLE, AIMP_MENUITEM_STYLE_CHECKBOX);
            m->SetValueAsInt32(AIMP_MENUITEM_PROPID_CHECKED, isEnabled && isEnabled() ? 1 : 0);
        }
        if (!Register(IID_IAIMPServiceMenuManager, m.Get())) return Ptr<IAIMPMenuItem>();
        entries_.push_back({Keep(m.Get()), AIMP_MENUITEM_PROPID_CAPTION, std::move(caption), checkbox});
        return m;
    }

    // "Prevent Resampling" with its three entries below one of AIMP's menus
    void AddMenu(int builtIn, const tstring& id) {
        auto svc = Service<IAIMPServiceMenuManager>(IID_IAIMPServiceMenuManager);
        Ptr<IAIMPMenuItem> parent;
        if (!svc || Failed(svc->GetBuiltIn(builtIn, parent.Out())) || !parent) return;
        auto root = AddItem(parent.Get(), id, [] { return tstring(AR_T(AR_PLUGIN_NAME)); }, nullptr, false);
        if (!root) return;
        // the checkbox shows the current state whenever the menu opens (it may have changed on the settings page)
        root->SetValueAsObject(AIMP_MENUITEM_PROPID_EVENT_ONSHOW, Handler([this] { Update(); }));
        AddItem(root.Get(), id + AR_T(".Enabled"), [] { return Tr("Enabled"); }, [this] { if (toggle) toggle(AR_T("menu")); }, true);
        AddItem(root.Get(), id + AR_T(".Chain"), [] { return Tr("Audio chain..."); }, [this] { if (showChain) showChain(); }, false);
        AddItem(root.Get(), id + AR_T(".Settings"), [] { return Tr("Settings..."); }, [this] { if (showSettings) showSettings(); }, false);
    }

public:
    // Registers the actions and menus; false if this AIMP offers neither
    bool Register() {
        if (!Core()) return false;
        AddAction(AR_T("PreventResampling.Toggle"), [] { return Tr("Switch on / off"); }, [this] { if (toggle) toggle(AR_T("hotkey")); });
        AddAction(AR_T("PreventResampling.Chain"), [] { return Tr("Show the audio chain"); }, [this] { if (showChain) showChain(); });
        AddMenu(AIMP_MENUID_PLAYER_MAIN_OPTIONS, AR_T("PreventResampling.Menu"));
        AddMenu(AIMP_MENUID_PLAYER_TRAY, AR_T("PreventResampling.Tray"));
        return registered_ > 0;
    }

    int Count() const { return registered_; }

    void Unregister() {
        // children before their parents (registered after them)
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (Core() && it->obj) Core()->UnregisterExtension(it->obj.Get());
        entries_.clear();
        for (auto* h : handlers_) {  // AIMP may keep a reference a little longer: no calls into the plugin any more
            h->Disconnect();
            h->Release();
        }
        handlers_.clear();
        registered_ = 0;
    }

    // The checkboxes follow the plugin's state
    void Update() {
        bool on = isEnabled && isEnabled();
        for (auto& e : entries_)
            if (e.checkbox && e.obj) e.obj->SetValueAsInt32(AIMP_MENUITEM_PROPID_CHECKED, on ? 1 : 0);
    }

    // The captions in the plugin's current language
    void Retranslate() {
        for (auto& e : entries_)
            if (e.obj && e.caption) SetUiText(e.obj.Get(), e.captionProp, e.caption());
    }

    // AIMP's short text display ("running line" of the skin), hidden again after 2 s
    static void Notify(const tstring& text) {
        auto disp = Service<IAIMPServiceMessageDispatcher>(IID_IAIMPServiceMessageDispatcher);
        if (disp) disp->Send(AIMP_MSG_CMD_SHOW_NOTIFICATION, 0, const_cast<TChar*>(text.c_str()));
    }
};

}  // namespace ar
