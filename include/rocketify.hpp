// Rocketify — the easy way to write mods.
//
//   #include <rocketify.hpp>      (or <rocketify>)
//
//   rocketify(MainMenu) {                   // the place you modify
//       void onSettings() {                 // the SETTINGS button was clicked
//           create::popup(
//               "Test",                     // title
//               "Hi! Test!",                // message
//               "YES", "NO",                // buttons
//               "classic"                   // popup type: "classic" or "warning"
//           );
//       }
//   };
//
// Your function replaces what the button normally does.
#pragma once

#include "rocketify.h"

#include <deque>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

namespace rocketify {

namespace detail {

struct Popup {
    std::string title, message, type;
    std::vector<std::string> buttons;
};

class Place {
public:
    virtual ~Place() {}
    virtual const char* placeName() const = 0;
    virtual void dispatch(const std::string& event) = 0;
};

struct State {
    std::vector<Place*> places;
    std::string overrides;  // "MainMenu.onSettings;..."
    std::deque<Popup> popups;
    bool warned = false;
};

inline State& state() {
    static State s;
    return s;
}

template <class T>
struct Register {
    Register() {
        state().places.push_back(new T);
        T::template listOverrides<T>(state().overrides);
    }
};

// Shows the next waiting popup. The loader refuses (0) while another dialog is open.
inline void flush(const RocketifyApi* api) {
    State& s = state();
    if (s.popups.empty()) return;
    if (api->version < 3) {
        if (!s.warned) api->log("popups need a newer Rocketify");
        s.warned = true;
        s.popups.clear();
        return;
    }
    const Popup& p = s.popups.front();
    std::vector<const char*> buttons;
    for (const std::string& b : p.buttons) buttons.push_back(b.c_str());
    if (api->show_popup(p.title.c_str(), p.message.c_str(), buttons.data(), static_cast<int>(buttons.size()), p.type.c_str()))
        s.popups.pop_front();
}

inline void event(const RocketifyApi* api, const char* place, const char* name) {
    for (Place* p : state().places) {
        if (std::string(p->placeName()) != place) continue;
        try {
            p->dispatch(name);
        } catch (const std::exception& e) {
            api->log((std::string("exception in ") + place + "." + name + ": " + e.what()).c_str());
        } catch (...) {
            api->log((std::string("exception in ") + place + "." + name).c_str());
        }
    }
    flush(api);
}

// create::... inside every place
struct Create {
    static void popup(const std::string& title, const std::string& message) {
        popup(title, message, "OK", "classic");
    }

    // popup(title, message, button, [more buttons,] type)
    template <class... More>
    static void popup(const std::string& title, const std::string& message, const std::string& button, const More&... more) {
        static_assert(sizeof...(More) > 0, "popup: after the buttons comes the type, \"classic\" or \"warning\"");
        Popup p;
        p.title = title;
        p.message = message;
        p.buttons = {button, std::string(more)...};
        p.type = p.buttons.back();
        p.buttons.pop_back();
        state().popups.push_back(p);
    }
};

}  // namespace detail

// ---------------------------------------------------------------- places

// The main menu. Each function runs when that entry is clicked (mouse, keyboard or controller).
class MainMenu : public detail::Place {
public:
    using create = detail::Create;

    virtual void onPlay() {}
    virtual void onItemShop() {}
    virtual void onRocketPass() {}
    virtual void onGarage() {}
    virtual void onClub() {}
    virtual void onProfile() {}
    virtual void onSettings() {}
    virtual void onQuit() {}
    virtual void onMods() {}  // Rocketify's own entry
    virtual void onNews() {}          // the NEWS tab on the right
    virtual void onChallenges() {}    // VIEW CHALLENGES
    virtual void onFriendsLists() {}  // the friends button at the bottom right

    // A function the mod wrote belongs to its own type, the others still to MainMenu.
    template <class T>
    static void listOverrides(std::string& out) {
        if (!std::is_same<decltype(&T::onPlay), void (MainMenu::*)()>::value) out += "MainMenu.onPlay;";
        if (!std::is_same<decltype(&T::onItemShop), void (MainMenu::*)()>::value) out += "MainMenu.onItemShop;";
        if (!std::is_same<decltype(&T::onRocketPass), void (MainMenu::*)()>::value) out += "MainMenu.onRocketPass;";
        if (!std::is_same<decltype(&T::onGarage), void (MainMenu::*)()>::value) out += "MainMenu.onGarage;";
        if (!std::is_same<decltype(&T::onClub), void (MainMenu::*)()>::value) out += "MainMenu.onClub;";
        if (!std::is_same<decltype(&T::onProfile), void (MainMenu::*)()>::value) out += "MainMenu.onProfile;";
        if (!std::is_same<decltype(&T::onSettings), void (MainMenu::*)()>::value) out += "MainMenu.onSettings;";
        if (!std::is_same<decltype(&T::onQuit), void (MainMenu::*)()>::value) out += "MainMenu.onQuit;";
        if (!std::is_same<decltype(&T::onMods), void (MainMenu::*)()>::value) out += "MainMenu.onMods;";
        if (!std::is_same<decltype(&T::onNews), void (MainMenu::*)()>::value) out += "MainMenu.onNews;";
        if (!std::is_same<decltype(&T::onChallenges), void (MainMenu::*)()>::value) out += "MainMenu.onChallenges;";
        if (!std::is_same<decltype(&T::onFriendsLists), void (MainMenu::*)()>::value) out += "MainMenu.onFriendsLists;";
    }

    const char* placeName() const final { return "MainMenu"; }
    void dispatch(const std::string& e) final {
        if (e == "onPlay") onPlay();
        else if (e == "onItemShop") onItemShop();
        else if (e == "onRocketPass") onRocketPass();
        else if (e == "onGarage") onGarage();
        else if (e == "onClub") onClub();
        else if (e == "onProfile") onProfile();
        else if (e == "onSettings") onSettings();
        else if (e == "onQuit") onQuit();
        else if (e == "onMods") onMods();
        else if (e == "onNews") onNews();
        else if (e == "onChallenges") onChallenges();
        else if (e == "onFriendsLists") onFriendsLists();
    }
};

}  // namespace rocketify

// What the loader calls. Inline, so that any number of .cpp files can include this header.
extern "C" __declspec(dllexport) inline void Rocketify_OnTick(const RocketifyApi* api) {
    rocketify::detail::flush(api);
}
extern "C" __declspec(dllexport) inline void Rocketify_OnEvent(const RocketifyApi* api, const char* place, const char* name) {
    rocketify::detail::event(api, place, name);
}

extern "C" __declspec(dllexport) inline const char* Rocketify_Overrides(void) {
    return rocketify::detail::state().overrides.c_str();
}

template <class Tag>
struct RocketifyImpl;

// The tag is local to the file, so two files can modify the same place.
#define ROCKETIFY_PLACE2(PLACE, N)                                                          \
    namespace { struct RocketifyTag##N; }                                                   \
    template <> struct RocketifyImpl<RocketifyTag##N>;                                      \
    static ::rocketify::detail::Register<RocketifyImpl<RocketifyTag##N>> rocketifyReg##N;   \
    template <> struct RocketifyImpl<RocketifyTag##N> : public ::rocketify::PLACE
#define ROCKETIFY_PLACE1(PLACE, N) ROCKETIFY_PLACE2(PLACE, N)

// rocketify(Place) { ... };
#define rocketify(PLACE) ROCKETIFY_PLACE1(PLACE, __LINE__)
