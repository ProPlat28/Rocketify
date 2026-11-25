#include <rocketify.hpp> // Include the Rocketify library

rocketify(MainMenu) { // Modifies the Main Menu
    void onSettings() { // We replace the settings button with a popup
        create::popup( // We create a popup
            "Test",       // title
            "Hi! Test!",  // message
            "YES", "NO",  // buttons
            "classic"     // popup type: "classic" or "warning"
        );
    }
};
