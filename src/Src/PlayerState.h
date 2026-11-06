#pragma once
#include <string>

//! Which of the player's states (arkPlayerState*) the engine is running, read from the states' own vtables.
//!
//! The player controller runs a state machine of ~90 arkPlayerState subclasses (Sneaking, Choke, Assassination,
//! CarryCorpse, ...). Rather than guess where the controller keeps its current state, every slot of every one of
//! those vtables is pointed at a tiny counting stub that jumps on to the original: a state whose per-frame method
//! is being called is the state that is running. The vtables are found by RTTI (exe 1.77.9.0) and only their
//! entries are rewritten -- no function is patched, and the objects keep the same vtable pointer.
namespace PlayerState
{
    void Install();                  //!< patch the vtables (idempotent); from the first Present
    void OnPresent();                //!< per frame: per-slot deltas, timeline, research dumps
    bool Installed();

    //! Research: log every (state, slot) that ticked over the next `frames` frames, with counts.
    void Probe(int frames);
    //! Research: one log line per frame for `frames` frames: states that ticked, camera, body yaw, sim, fp draws.
    void Timeline(int frames);

    std::string Stats();             //!< heartbeat: the states that ticked last frame

    //! Is the player inside a scripted, animation-driven state whose camera the view should NOT follow -- a
    //! takedown (Choke, DropChoke, Assassination, the fatalities, SyncAnim) or a Slash (see AnimTakeover)? A state is RUNNING when its per-tick
    //! update (vtable slot 13, shared by every arkPlayerState) was called: measured, Sneaking:2,13,24 becomes
    //! Choke:2,13,24 on the frame the choke starts and back again when it ends, while slot 6 is polled on dozens of
    //! states every frame whether they run or not.
    bool AnimActive();
    //! Same, but also true when a scripted state's update ran in THIS frame's simulation, before the Present that
    //! latches AnimActive. For the render hooks: the first frame of a takedown already carries its camera pop
    //! (measured: 0.67 m in that frame), and a detector one frame late lets it through.
    bool AnimActiveNow();
    const char* AnimName();          //!< the scripted state running, or "" when none
    //! 1 (default): while a scripted state runs, the arms and the weapon are drawn as the animation has them
    //! (GpuTrace::SetAnimSuspend); 0: they stay on the controllers.
    void SetAnimArms(bool on);
    //! During Shadow Walk the arms are drawn as the game animates them (1, default) or follow the controllers (0).
    void SetShadowArms(bool on);
    bool AnimArms();
    //! A running scripted state that also owns the arms and the weapon and ignores the mouse (a takedown proper).
    //! AnimActive alone may be a view-only state: Slash, whose kill variant jolts the camera 40 deg.
    bool AnimTakeover();
    //! A cutscene in the engine (the Cinematic state, the development notes) is running: the view follows the head, the servo
    //! holds its target without injecting, the controllers have the cutscene's controls, the letterbox is not drawn.
    bool Cinematic();
    //! The servo must hold its target and inject nothing: a takeover, a cutscene, or a state marked so (a hard landing).
    bool AnimHoldServo();
    //! Every view state running (as AnimActiveNow counts them) is the player moving -- climbing onto something, sliding:
    //! the view takes only the head's rotation and the eye stays where play puts it.
    bool AnimLocoNow();
    //! The servo must measure the camera's yaw, not the body's (a slide: the mouse turns the camera, the body follows).
    bool ServoOnCamera();
    //! In a cutscene the arms are drawn as the game animates them (1) or stay on the controllers (0, default).
    void SetCineArms(bool on);
    bool CineArms();
    //! Config: which states count, as a mask -- 1 the view follows the head, 2 the animation takes over the arms,
    //! the weapon and the mouse, 4 a cutscene (see Cinematic), 8 the servo holds, 16 the player moving (only the
    //! rotation is the head's, see AnimLocoNow), 32 the servo on the camera's yaw, 0 none. Names as in the table
    //! ("Choke", "Slash", "Mantle", "Sliding", ...).
    bool SetAnimState(const char* name, int mask);
    //! Is this state (by name, e.g. "Powerwheel", "Sneaking") running -- its per-tick update ran in the last few
    //! simulation ticks? A frozen tick counter (pause menu) keeps the last answer.
    bool Running(const char* name);
    //! How many player states are running. 0 = there is no player being simulated (main menu, loading).
    int RunningCount();
    //! The running states' names, comma-separated ("-" = none). For the log.
    std::string RunningNames();
    //! Is any state running that is not one of these?
    bool RunningOutside(const char* const* names, int n);
    //! The object that state's methods were last called on (the player's instance of that arkPlayerState), or null
    //! if none was ever called. Not validated: the state objects live as long as the player.
    void* StateObject(const char* name);
    std::string AnimStates();
}
