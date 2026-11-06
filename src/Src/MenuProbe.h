#pragma once

//! Research ([Logging] LogMenuData, 2026-11-01): what the game's Options screen sends to its Flash (Iggy) menus and
//! what those menus call back, written to the log -- the protocol a "VR" options tab would have to speak.
//!
//! Found statically (Dishonored_DO.exe): the options screen's tabs are an AS3 array `optionsData.sections` of
//! { label, gameCallbackOnClick = "OnOptionSectionSelected", screenClassName }, built from the screen's 4 section
//! objects (pause menu exe+0x564CC0, main menu exe+0x566CD0); the AS3 side calls OnOptionSectionSelected(index).
//! A section shown (exe+0x552A60) fills `optionsData.currentSection` { label, screenClassName, categories, settings },
//! each setting one of optionStepper / optionStepperBoolean / slider / dropBox / keyboardBinding / link. Values go
//! through arkIggyAS3Object::SetAS3Value<int / uint / float / bool / const char* / idStrId> (exe+0x2A770 / 0x2A840 /
//! 0x2A910 / 0x2AC90 / 0x2AD70 / 0x2AB80; this, const T* value, member), objects / arrays are made with a path
//! (exe+0x29490 / 0x28730: this, file, const char* path), and AS3 -> C++ calls arrive at arkIggyMain's
//! ExternalInterfaceHub (exe+0x35630: this, player, the call: name first). Not known yet: each widget's fields, what
//! `categories` holds, and the callbacks a changed value / Apply / Back make. This logs them.
namespace MenuProbe
{
    void Install(bool on);
    std::string Stats();
}
